#include "eflash_mgr.h"
#include "eflash_ftl.h"
#include "eflash_sim.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>



#ifndef FTL_DEBUG
#define FTL_DEBUG(...) do {} while(0)  // Disable debug output
#endif

// Disable all printf debug output in eflash_mgr.c for better performance
#ifndef MGR_DEBUG
#define MGR_DEBUG(...) do {} while(0)
#define MGR_PRINTF(...) do {} while(0)
#endif

// --- Internal Helper Functions ---

// Forward declarations
static uint32_t get_total_node_count(void);
static void print_free_page_counts(const char *tag);
static int find_page_with_space(uint16_t *out_lpn, int *out_is_extended, int *out_ext_level, int *out_page_in_block);
static int extend_free_node_table(void);

// Helper: Get logical page number (LPN) for a free_node page
static uint16_t get_free_node_lpn(int page_index, int is_extended, int ext_level, int page_in_block) {
    if (!is_extended) {
        // Base level: LPN 8-11
        return SYS_FREE_LIST_BASE_LPN + page_index;
    } else {
        // Extended level: calculate from ext_free_node_addrs
        if (MGR->ext_free_node_addrs[ext_level] == 0xFFFFFFFF) {
            return PAGE_NONE;
        }
        uint16_t start_lpn = (uint16_t)(MGR->ext_free_node_addrs[ext_level] / EFLASH_PAGE_SIZE);
        return start_lpn + page_in_block;
    }
}

// Read count from free_node page via FTL layer (using logical address)
static int16_t read_node_count(uint16_t lpn) {
    if (lpn == PAGE_NONE) {
        printf("[READ_COUNT] ERROR: lpn is PAGE_NONE\n");
        return -1;
    }
    
    // Boundary check for LPN
    if (lpn >= EFLASH_TOTAL_PAGES) {
        printf("[READ_COUNT] ERROR: lpn=%d out of range (max=%d)\n", lpn, EFLASH_TOTAL_PAGES);
        return -1;
    }
    
    uint8_t buf[USER_DATA_SIZE];
    int ret = eflash_ftl_read(lpn, buf);
    if (ret != 0) {
        printf("[READ_COUNT] ERROR: eflash_ftl_read(lpn=%d) failed, ret=%d\n", lpn, ret);
        return -1;
    }
    
    // count stored as uint16_t at beginning of user data area after meta region (2 bytes)
    uint16_t offset = META_SIZE;  // Skip 48-byte meta
    int16_t count = (int16_t)(buf[offset] | (buf[offset + 1] << 8));
    
    // MGR_PRINTF("[READ_COUNT] lpn=%d, count=%d\n", lpn, count);  // Disabled for performance
    return count;
}

// Write count to free_node page
static void write_node_count(uint16_t phys_page, uint16_t count) {
    uint8_t buf[EFLASH_PAGE_SIZE];
    
    // Initialize to all 0xFF (erased state)
    memset(buf, 0xFF, EFLASH_PAGE_SIZE);
    
    // count stored as uint16_t at beginning of user data area after meta region (2 bytes)
    uint16_t offset = META_SIZE;
    buf[offset] = count & 0xFF;
    buf[offset + 1] = (count >> 8) & 0xFF;
    
    // Calculate ECC for entire page (covers user data + metadata excluding ECC part)
    // ECC protection range: USER_DATA_SIZE + META_SIZE - 5
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
    uint8_t *ecc_ptr = buf + USER_DATA_SIZE + META_SIZE - 5;
    bch_generate(&bch_3bit, buf, protected_len, ecc_ptr);
    
    // Flash must be erased before writing
    eflash_hw_erase(phys_page);
    eflash_hw_prog(phys_page, buf);
}

// Read link info from the last free_node page of an extension block
static int read_ext_link(uint32_t ext_logical_addr, free_node_link_t *link) {
    if (ext_logical_addr == 0xFFFFFFFF) {
        return -1;
    }
    
    // The link is stored at the end of the last page in the 4-page extension block
    uint16_t last_page_lpn = (uint16_t)(ext_logical_addr / EFLASH_PAGE_SIZE) + FREE_NODE_EXT_PAGES - 1;
    
    uint8_t buf[USER_DATA_SIZE];
    if (eflash_ftl_read(last_page_lpn, buf) != 0) {
        return -1;
    }
    
    // Link is at the end of user data area (last 6 bytes)
    uint16_t link_offset = USER_DATA_SIZE - FREE_NODE_LINK_SIZE;
    memcpy(link, buf + META_SIZE + link_offset, FREE_NODE_LINK_SIZE);
    
    if (link->magic != LINK_FREE_NODE_MAGIC) {
        return -1;  // Invalid magic
    }
    
    return 0;
}

// Write link info to the last free_node page of an extension block
static int write_ext_link(uint32_t ext_logical_addr, const free_node_link_t *link) {
    if (ext_logical_addr == 0xFFFFFFFF) {
        return -1;
    }
    
    // The link is stored at the end of the last page in the 4-page extension block
    uint16_t last_page_lpn = (uint16_t)(ext_logical_addr / EFLASH_PAGE_SIZE) + FREE_NODE_EXT_PAGES - 1;
    
    uint8_t buf[USER_DATA_SIZE];
    if (eflash_ftl_read(last_page_lpn, buf) != 0) {
        return -1;
    }
    
    // Write link at the end of user data area
    uint16_t link_offset = USER_DATA_SIZE - FREE_NODE_LINK_SIZE;
    memcpy(buf + META_SIZE + link_offset, link, FREE_NODE_LINK_SIZE);
    
    // Write through FTL layer (FTL will calculate ECC internally)
    return eflash_ftl_write(last_page_lpn, buf);
}

// Read free_node at specified index via FTL layer (using logical address)
static free_node_t read_free_node(uint16_t lpn, uint16_t index) {
    free_node_t node = {0xFFFFFFFF, 0xFFFF};  // Default return invalid node
    
    if (lpn == PAGE_NONE || index >= FREE_NODES_PER_PAGE) {
        return node;  // Invalid LPN or out of bounds
    }
    
    uint8_t buf[USER_DATA_SIZE];
    if (eflash_ftl_read(lpn, buf) != 0) {
        return node;  // Read failed
    }
    
    // Node data starts after meta + 2-byte count header
    uint16_t offset = META_SIZE + FREE_NODE_HEADER_SIZE + index * sizeof(free_node_t);
    
    // Boundary check
    if (offset + sizeof(free_node_t) > USER_DATA_SIZE) {
        return node;  // Beyond page range
    }
    
    memcpy(&node, buf + offset, sizeof(free_node_t));
    return node;
}

// Write entire free_node page via FTL layer (using logical address)
// This function writes the entire page buffer through FTL
static int write_free_node_page(uint16_t lpn, const uint8_t *page_buf) {
    if (lpn == PAGE_NONE || !page_buf) {
        return -1;
    }
    
    // Write through FTL layer (handles physical page allocation and Radix Tree update)
    return eflash_ftl_write(lpn, page_buf);
}

// Find and remove node with specified logical address from free_node table, return its size
static uint32_t remove_node_from_table(uint32_t target_logical_addr) {
    // Search in base level pages
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
        int16_t count = read_node_count(lpn);
        if (count < 0) continue;  // Skip invalid pages
        
        for (int16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(lpn, j);
            
            if (node.addr == target_logical_addr) {
                // Read entire page via FTL
                uint8_t buf[USER_DATA_SIZE];
                if (eflash_ftl_read(lpn, buf) != 0) {
                    FTL_DEBUG("[REMOVE_NODE] ERROR: Failed to read LPN %d\n", lpn);
                    return 0;
                }
                
                int16_t last_idx = count - 1;
                if (j != last_idx) {
                    // Overwrite current node with last node
                    uint16_t src_offset = META_SIZE + FREE_NODE_HEADER_SIZE + last_idx * sizeof(free_node_t);
                    uint16_t dst_offset = META_SIZE + FREE_NODE_HEADER_SIZE + j * sizeof(free_node_t);
                    memcpy(buf + dst_offset, buf + src_offset, sizeof(free_node_t));
                }
                
                // Clear last node (fill with 0xFF)
                uint16_t clear_offset = META_SIZE + FREE_NODE_HEADER_SIZE + last_idx * sizeof(free_node_t);
                memset(buf + clear_offset, 0xFF, sizeof(free_node_t));
                
                // Update count
                uint16_t count_offset = META_SIZE;
                buf[count_offset] = (count - 1) & 0xFF;
                buf[count_offset + 1] = ((count - 1) >> 8) & 0xFF;
                
                // Write entire page back via FTL
                if (write_free_node_page(lpn, buf) != 0) {
                    FTL_DEBUG("[REMOVE_NODE] ERROR: Failed to write LPN %d\n", lpn);
                    return 0;
                }
                
                // Update total free node count in memory
                MGR->total_free_nodes = get_total_node_count();
                
                // Debug: Print page counts when reaching multiples of 57
                if (MGR->total_free_nodes % FREE_NODES_PER_PAGE == 0) {
                    print_free_page_counts("REMOVE");
                    MGR_PRINTF("  [DEBUG REMOVE] Total nodes=%u\n", MGR->total_free_nodes);
                }
                
                FTL_DEBUG("[REMOVE_NODE] Removed addr=0x%08X from base LPN %d, new total=%u\n",
                         target_logical_addr, lpn, MGR->total_free_nodes);
                
                return node.size;
            }
        }
    }
    
    // Search in extended level pages
    for (int level = 0; level < MAX_FREE_NODE_EXT_LEVELS; level++) {
        if (MGR->ext_free_node_addrs[level] == 0xFFFFFFFF) {
            break;  // No more extensions
        }
        
        uint16_t start_lpn = (uint16_t)(MGR->ext_free_node_addrs[level] / EFLASH_PAGE_SIZE);
        for (int i = 0; i < FREE_NODE_EXT_PAGES; i++) {
            uint16_t lpn = start_lpn + i;
            int16_t count = read_node_count(lpn);
            if (count < 0) continue;
            
            for (int16_t j = 0; j < count; j++) {
                free_node_t node = read_free_node(lpn, j);
                
                if (node.addr == target_logical_addr) {
                    // Read entire page via FTL
                    uint8_t buf[USER_DATA_SIZE];
                    if (eflash_ftl_read(lpn, buf) != 0) {
                        FTL_DEBUG("[REMOVE_NODE] ERROR: Failed to read ext LPN %d\n", lpn);
                        return 0;
                    }
                    
                    int16_t last_idx = count - 1;
                    if (j != last_idx) {
                        // Overwrite current node with last node
                        uint16_t src_offset = META_SIZE + FREE_NODE_HEADER_SIZE + last_idx * sizeof(free_node_t);
                        uint16_t dst_offset = META_SIZE + FREE_NODE_HEADER_SIZE + j * sizeof(free_node_t);
                        memcpy(buf + dst_offset, buf + src_offset, sizeof(free_node_t));
                    }
                    
                    // Clear last node (fill with 0xFF)
                    uint16_t clear_offset = META_SIZE + FREE_NODE_HEADER_SIZE + last_idx * sizeof(free_node_t);
                    memset(buf + clear_offset, 0xFF, sizeof(free_node_t));
                    
                    // Update count
                    uint16_t count_offset = META_SIZE;
                    buf[count_offset] = (count - 1) & 0xFF;
                    buf[count_offset + 1] = ((count - 1) >> 8) & 0xFF;
                    
                    // Write entire page back via FTL
                    if (write_free_node_page(lpn, buf) != 0) {
                        FTL_DEBUG("[REMOVE_NODE] ERROR: Failed to write ext LPN %d\n", lpn);
                        return 0;
                    }
                    
                    // Update total free node count in memory
                    MGR->total_free_nodes = get_total_node_count();
                    
                    FTL_DEBUG("[REMOVE_NODE] Removed addr=0x%08X from ext LPN %d (level=%d, page=%d), new total=%u\n",
                             target_logical_addr, lpn, level, i, MGR->total_free_nodes);
                    
                    return node.size;
                }
            }
        }
    }
    
    FTL_DEBUG("[REMOVE_NODE] ERROR: Logical address 0x%08X not found in any free_node table!\n", target_logical_addr);
    return 0;  // Not found
}

// Insert new free_node into table (maintain sorting by size)
static void insert_node_to_table(uint32_t logical_addr, uint32_t size) {
    // Find a page with available space
    uint16_t target_lpn;
    int is_extended, ext_level, page_in_block;
    int page_idx = find_page_with_space(&target_lpn, &is_extended, &ext_level, &page_in_block);
    
    FTL_DEBUG("[INSERT_NODE] find_page_with_space returned: page_idx=%d, is_extended=%d, ext_level=%d, page_in_block=%d\n",
             page_idx, is_extended, ext_level, page_in_block);
    
    // If no space found, try to extend the table
    if (page_idx == -1) {
        FTL_DEBUG("[INSERT_NODE] No space found, attempting extension...\n");
        
        // Check if we can extend
        uint32_t ext_levels_used = 0;
        for (int i = 0; i < MAX_FREE_NODE_EXT_LEVELS; i++) {
            if (MGR->ext_free_node_addrs[i] != 0xFFFFFFFF) {
                ext_levels_used++;
            } else {
                break;
            }
        }
        
        if (ext_levels_used >= MAX_FREE_NODE_EXT_LEVELS) {
            FTL_DEBUG("[INSERT_NODE] ERROR: Maximum extension levels reached, cannot insert\n");
            return;
        }
        
        // Trigger extension
        if (extend_free_node_table() != 0) {
            FTL_DEBUG("[INSERT_NODE] ERROR: Extension failed\n");
            return;
        }
        
        // Try to find space again after extension
        page_idx = find_page_with_space(&target_lpn, &is_extended, &ext_level, &page_in_block);
        if (page_idx == -1) {
            FTL_DEBUG("[INSERT_NODE] ERROR: Still no space after extension\n");
            return;
        }
    }
    
    // Check if this is a base level page that hasn't been allocated yet
    if (!is_extended && MGR->free_node_pages[page_in_block] == PAGE_NONE) {
        FTL_DEBUG("[INSERT_NODE] Base page %d not allocated yet (PAGE_NONE), allocating via FTL...\n", page_in_block);
        
        // Prepare a blank page with count=0
        uint8_t blank_page[USER_DATA_SIZE];
        memset(blank_page, 0xFF, USER_DATA_SIZE);
        
        // Set count to 0
        uint16_t count_offset = META_SIZE;
        blank_page[count_offset] = 0;
        blank_page[count_offset + 1] = 0;
        
        // Write through FTL layer to allocate physical page
        FTL_DEBUG("[INSERT_NODE] Writing blank page to LPN %d...\n", target_lpn);
        if (write_free_node_page(target_lpn, blank_page) != 0) {
            FTL_DEBUG("[INSERT_NODE] ERROR: Failed to allocate base page %d\n", page_in_block);
            return;
        }
        FTL_DEBUG("[INSERT_NODE] Successfully wrote blank page to LPN %d\n", target_lpn);
        
        // Query Radix Tree for the physical page
        uint16_t phys_page = find_phys_page_by_sector(target_lpn);
        if (phys_page == PAGE_NONE) {
            FTL_DEBUG("[INSERT_NODE] ERROR: Failed to get physical page for LPN %d\n", target_lpn);
            return;
        }
        
        MGR->free_node_pages[page_in_block] = phys_page;
        FTL_DEBUG("[INSERT_NODE] Allocated LPN %d -> PPN %d (free_node[%d])\n", 
                 target_lpn, phys_page, page_in_block);
    } else if (!is_extended) {
        FTL_DEBUG("[INSERT_NODE] Base page %d already allocated (PPN=%d)\n", 
                 page_in_block, MGR->free_node_pages[page_in_block]);
    }
    
    // Read the target page's count via LPN
    int16_t count = read_node_count(target_lpn);
    if (count < 0) {
        FTL_DEBUG("[INSERT_NODE] ERROR: Failed to read node count from LPN %d\n", target_lpn);
        return;
    }
    
    // Find insertion position (maintain ascending order by size)
    uint16_t insert_pos = (uint16_t)count;
    for (int16_t j = 0; j < count; j++) {
        free_node_t node = read_free_node(target_lpn, j);
        if (node.size >= size) {
            insert_pos = j;
            break;
        }
    }
    
    // Read entire page via FTL
    uint8_t buf[USER_DATA_SIZE];
    if (eflash_ftl_read(target_lpn, buf) != 0) {
        FTL_DEBUG("[INSERT_NODE] ERROR: Failed to read page LPN %d\n", target_lpn);
        return;
    }
    
    // Shift elements backward
    for (uint16_t j = count; j > insert_pos; j--) {
        uint16_t src_offset = META_SIZE + FREE_NODE_HEADER_SIZE + (j - 1) * sizeof(free_node_t);
        uint16_t dst_offset = META_SIZE + FREE_NODE_HEADER_SIZE + j * sizeof(free_node_t);
        memcpy(buf + dst_offset, buf + src_offset, sizeof(free_node_t));
    }
    
    // Insert new node
    uint16_t node_offset = META_SIZE + FREE_NODE_HEADER_SIZE + insert_pos * sizeof(free_node_t);
    free_node_t new_node;
    new_node.addr = logical_addr;
    new_node.size = size;
    memcpy(buf + node_offset, &new_node, sizeof(free_node_t));
    
    // Update count
    uint16_t count_offset = META_SIZE;
    buf[count_offset] = (count + 1) & 0xFF;
    buf[count_offset + 1] = ((count + 1) >> 8) & 0xFF;
    
    // Write entire page back via FTL layer
    if (write_free_node_page(target_lpn, buf) != 0) {
        FTL_DEBUG("[INSERT_NODE] ERROR: Failed to write page LPN %d\n", target_lpn);
        return;
    }
    
    // Update total free node count in memory
    MGR->total_free_nodes = get_total_node_count();
    
    // Debug: Print page counts when reaching multiples of 57
    if (MGR->total_free_nodes % FREE_NODES_PER_PAGE == 0) {
        print_free_page_counts("INSERT");
        printf("  [DEBUG INSERT] Total nodes=%u, about to write page\n", MGR->total_free_nodes);
    }
    
    FTL_DEBUG("[INSERT_NODE] Inserted logical_addr=0x%08X, size=%u at LPN %d (ext=%d, level=%d), new total=%u\n",
             logical_addr, size, target_lpn, is_extended, ext_level, MGR->total_free_nodes);
}

// Get total free node count by summing all pages (base + extended)
static uint32_t get_total_node_count(void) {
    uint32_t total = 0;
    
    // Count base level nodes
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
        int16_t count = read_node_count(lpn);
        if (count > 0) {
            total += (uint32_t)count;
        }
    }
    
    // Count extended level nodes
    for (int level = 0; level < MAX_FREE_NODE_EXT_LEVELS; level++) {
        if (MGR->ext_free_node_addrs[level] == 0xFFFFFFFF) {
            break;  // No more extensions
        }
        
        uint16_t start_lpn = (uint16_t)(MGR->ext_free_node_addrs[level] / EFLASH_PAGE_SIZE);
        for (int i = 0; i < FREE_NODE_EXT_PAGES; i++) {
            int16_t count = read_node_count(start_lpn + i);
            if (count > 0) {
                total += (uint32_t)count;
            }
        }
    }
    
    return total;
}

// Debug: Print count of each free node page
static void print_free_page_counts(const char *tag) {
    printf("  [DEBUG %s] Base pages: ", tag);
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
        int16_t count = read_node_count(lpn);
        printf("[%d]=%d ", i, count);
    }
    
    for (int level = 0; level < MAX_FREE_NODE_EXT_LEVELS; level++) {
        if (MGR->ext_free_node_addrs[level] == 0xFFFFFFFF) {
            break;
        }
        
        printf("Ext%d: ", level);
        uint16_t start_lpn = (uint16_t)(MGR->ext_free_node_addrs[level] / EFLASH_PAGE_SIZE);
        for (int i = 0; i < FREE_NODE_EXT_PAGES; i++) {
            int16_t count = read_node_count(start_lpn + i);
            printf("[%d]=%d ", i, count);
        }
    }
    printf("\n");
}

// Find a page with available space across all levels (base + extended)
// Returns: page index (0-3 for base, or encodes level+page for extended), or -1 if full
static int find_page_with_space(uint16_t *out_lpn, int *out_is_extended, int *out_ext_level, int *out_page_in_block) {
    // First check base level pages
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
        int16_t count = read_node_count(lpn);
        if (count >= 0 && count < FREE_NODES_PER_PAGE) {
            *out_lpn = lpn;  // Return LPN, not physical page
            *out_is_extended = 0;
            *out_ext_level = 0;
            *out_page_in_block = i;
            return i;  // Return index in base array
        }
    }
    
    // Check extended level pages
    for (int level = 0; level < MAX_FREE_NODE_EXT_LEVELS; level++) {
        if (MGR->ext_free_node_addrs[level] == 0xFFFFFFFF) {
            break;  // No more extensions
        }
        
        uint16_t start_lpn = (uint16_t)(MGR->ext_free_node_addrs[level] / EFLASH_PAGE_SIZE);
        for (int i = 0; i < FREE_NODE_EXT_PAGES; i++) {
            int16_t count = read_node_count(start_lpn + i);
            if (count >= 0 && count < FREE_NODES_PER_PAGE) {
                *out_lpn = start_lpn + i;  // Return LPN
                *out_is_extended = 1;
                *out_ext_level = level;
                *out_page_in_block = i;
                return level * 100 + i;  // Encode level and page index
            }
        }
    }
    
    return -1;  // All pages are full
}

// Extend free node table by allocating a new 4-page block
static int extend_free_node_table(void) {
    FTL_DEBUG("[EXTEND_FREE_NODE] Starting extension...\n");
    
    // Find the first available extension level
    int level = 0;
    while (level < MAX_FREE_NODE_EXT_LEVELS && MGR->ext_free_node_addrs[level] != 0xFFFFFFFF) {
        level++;
    }
    
    if (level >= MAX_FREE_NODE_EXT_LEVELS) {
        FTL_DEBUG("[EXTEND_FREE_NODE] ERROR: Maximum extension levels reached\n");
        return -1;
    }
    
    // Allocate 4 pages for the extension block
    uint32_t ext_logical_addr;
    uint32_t alloc_size = FREE_NODE_EXT_PAGES * USER_DATA_SIZE;  // 4 * 464 bytes
    if (eflash_mgr_alloc(alloc_size, &ext_logical_addr) != 0) {
        FTL_DEBUG("[EXTEND_FREE_NODE] ERROR: Failed to allocate %u bytes for extension\n", alloc_size);
        return -1;
    }
    
    FTL_DEBUG("[EXTEND_FREE_NODE] Allocated extension block at logical_addr=0x%08X (level %d)\n",
             ext_logical_addr, level);
    
    // Initialize the 4 pages with count=0 via FTL layer
    uint16_t start_lpn = (uint16_t)(ext_logical_addr / EFLASH_PAGE_SIZE);
    uint8_t buf[USER_DATA_SIZE];
    memset(buf, 0xFF, USER_DATA_SIZE);
    
    for (int i = 0; i < FREE_NODE_EXT_PAGES; i++) {
        // Set count to 0 for each page
        uint16_t count_offset = META_SIZE;
        buf[count_offset] = 0;
        buf[count_offset + 1] = 0;
        
        // Write through FTL layer (handles physical page allocation)
        uint16_t lpn = start_lpn + i;
        if (write_free_node_page(lpn, buf) != 0) {
            FTL_DEBUG("[EXTEND_FREE_NODE] ERROR: Failed to initialize ext page LPN %d\n", lpn);
            return -1;
        }
    }
    
    // Record the extension address
    MGR->ext_free_node_addrs[level] = ext_logical_addr;
    
    // If this is not the first extension, update the link in the previous extension block
    if (level > 0) {
        uint32_t prev_ext_addr = MGR->ext_free_node_addrs[level - 1];
        free_node_link_t link;
        link.magic = LINK_FREE_NODE_MAGIC;
        link.next_ext_addr = ext_logical_addr;
        
        if (write_ext_link(prev_ext_addr, &link) != 0) {
            FTL_DEBUG("[EXTEND_FREE_NODE] WARNING: Failed to write link in previous block\n");
            // Continue anyway - the extension is still usable
        }
    } else {
        // This is the first extension, update the link in the base block's last page
        // The base block's last page is LPN 11 (SYS_FREE_LIST_BASE_LPN + 3)
        uint16_t base_last_lpn = SYS_FREE_LIST_BASE_LPN + FREE_NODE_PAGE_COUNT - 1;
        
        // Read the page via FTL
        uint8_t page_buf[USER_DATA_SIZE];
        if (eflash_ftl_read(base_last_lpn, page_buf) == 0) {
            // Write link at the end of user data area
            uint16_t link_offset = USER_DATA_SIZE - FREE_NODE_LINK_SIZE;
            free_node_link_t link;
            link.magic = LINK_FREE_NODE_MAGIC;
            link.next_ext_addr = ext_logical_addr;
            memcpy(page_buf + META_SIZE + link_offset, &link, FREE_NODE_LINK_SIZE);
            
            // Write back via FTL
            if (write_free_node_page(base_last_lpn, page_buf) != 0) {
                FTL_DEBUG("[EXTEND_FREE_NODE] WARNING: Failed to write link in base last page LPN %d\n", base_last_lpn);
            } else {
                FTL_DEBUG("[EXTEND_FREE_NODE] Updated link in base block's last page LPN %d\n", base_last_lpn);
            }
        }
    }
    
    FTL_DEBUG("[EXTEND_FREE_NODE] Extension level %d completed successfully\n", level);
    return 0;
}

// --- Interface Implementation ---

void eflash_mgr_init(uint16_t total_pages) {
    MGR->total_pages = total_pages;
    
    FTL_DEBUG("[SPACE_INIT] === Memory-only initialization (no Flash writes) ===\n");
    
    // Initialize free_node page array to invalid values
    // These will be set by FTL when system pages are allocated through Radix Tree
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        MGR->free_node_pages[i] = PAGE_NONE;
    }
    
    // Initialize extended free node table addresses to invalid values
    for (int i = 0; i < MAX_FREE_NODE_EXT_LEVELS; i++) {
        MGR->ext_free_node_addrs[i] = 0xFFFFFFFF;
    }
    
    // Initialize total free node count to 0
    MGR->total_free_nodes = 0;
    
    // Initialize header page array to invalid values
    // These will be set by FTL when system pages are allocated through Radix Tree
    for (int i = 0; i < BASE_HEADER_PAGES; i++) {
        MGR->header_pages[i] = PAGE_NONE;
    }
    
    // Calculate system reserved logical pages (LPN 0-11)
    // These logical pages are used for system areas:
    //   - LPN 0-7:   Base object header table
    //   - LPN 8-11:  Free list pages
    // Note: Physical pages (PPN) are dynamically allocated by FTL, not reserved here.
    uint32_t reserved_logic_page_count = FREE_NODE_PAGE_COUNT + BASE_HEADER_PAGES;  // 12 logical pages
    
    // NOTE: Do NOT write to Flash here!
    // Free node initialization will be done by FTL through write_system_page()
    // after Radix Tree is functional. This ensures wear leveling and proper mapping.
    
    FTL_DEBUG("[SPACE_INIT] Total physical pages: %d, Reserved logical pages: %d (LPN 0-%d)\n",
             total_pages, reserved_logic_page_count, reserved_logic_page_count - 1);
    FTL_DEBUG("[SPACE_INIT] Memory initialization complete (waiting for FTL to write system pages)\n");
    
    MGR->next_alloc_page = reserved_logic_page_count;  // Next logical page to allocate (LPN 12)
}

bool eflash_mgr_check_initialized(void) {
    // Check if at least one free_node page contains valid data (not all 0xFF)
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
        
        uint8_t buf[USER_DATA_SIZE];
        if (eflash_ftl_read(lpn, buf) != 0) {
            continue;  // Read failed
        }
        
        // Check if page is all 0xFF (blank/erased)
        bool is_blank = true;
        for (int j = 0; j < USER_DATA_SIZE; j++) {
            if (buf[j] != 0xFF) {
                is_blank = false;
                break;
            }
        }
        
        if (!is_blank) {
            // Page has data, check if it contains at least one valid node
            int16_t count = read_node_count(lpn);
            if (count > 0) {
                FTL_DEBUG("[SPACE_CHECK] Found initialized free_node LPN %d with %d nodes\n", lpn, count);
                return true;
            }
        }
    }
    
    FTL_DEBUG("[SPACE_CHECK] No initialized free_node pages found\n");
    return false;
}

int eflash_mgr_init_free_list(uint16_t total_pages, uint16_t reserved_logic_pages) {
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Initializing free list (via FTL layer)...\n");
    
    // Calculate available logical space
    // Total logical space = total_pages * USER_DATA_SIZE
    // Reserved space = reserved_logic_pages * USER_DATA_SIZE
    // Available space = (total_pages - reserved_logic_pages) * USER_DATA_SIZE
    uint32_t total_logical_size = (uint32_t)total_pages * USER_DATA_SIZE;
    uint32_t reserved_logical_size = (uint32_t)reserved_logic_pages * USER_DATA_SIZE;
    uint32_t available_logical_size = total_logical_size - reserved_logical_size;
    uint32_t start_logical_addr = reserved_logical_size;
    
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Total logical size: %u bytes\n", total_logical_size);
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Reserved logical size: %u bytes (%d pages)\n", 
             reserved_logical_size, reserved_logic_pages);
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Available logical size: %u bytes\n", available_logical_size);
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Start logical address: 0x%08X\n", start_logical_addr);
    
    if (available_logical_size == 0) {
        FTL_DEBUG("[SPACE_INIT_FREE_LIST] ERROR: No available space!\n");
        return -1;
    }
    
    // Create initial free node in memory
    free_node_t initial_node;
    initial_node.addr = start_logical_addr;
    initial_node.size = available_logical_size;
    
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Initial free node: addr=0x%08X, size=%u\n",
             initial_node.addr, initial_node.size);
    
    // Prepare the first free_node page data (count + node structure)
    // This will be written through FTL layer to trigger physical page allocation
    uint8_t page_data[USER_DATA_SIZE];
    memset(page_data, 0xFF, USER_DATA_SIZE);
    
    // Set count to 1
    uint16_t count_offset = META_SIZE;
    page_data[count_offset] = 1 & 0xFF;
    page_data[count_offset + 1] = (1 >> 8) & 0xFF;
    
    // Write the initial node at index 0
    uint16_t node_offset = META_SIZE + FREE_NODE_HEADER_SIZE;
    memcpy(page_data + node_offset, &initial_node, sizeof(free_node_t));
    
    // Write through FTL layer (this will allocate PPN and update Radix Tree)
    // LPN = SYS_FREE_LIST_BASE_LPN (which is 8)
    // Note: FTL will calculate ECC internally, no need to do it here
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Writing initial free node to LPN %d via FTL...\n", 
             SYS_FREE_LIST_BASE_LPN);
    
    if (write_system_page(SYS_FREE_LIST_BASE_LPN, page_data) != 0) {
        FTL_DEBUG("[SPACE_INIT_FREE_LIST] ERROR: Failed to write initial free node!\n");
        return -1;
    }
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Initial free node written successfully\n");
    
    // Query Radix Tree for the physical page of LPN 8
    uint16_t phys_page = find_phys_page_by_sector(SYS_FREE_LIST_BASE_LPN);
    if (phys_page == PAGE_NONE) {
        FTL_DEBUG("[SPACE_INIT_FREE_LIST] ERROR: Failed to find physical page for LPN %d\n", 
                 SYS_FREE_LIST_BASE_LPN);
        return -1;
    }
    
    MGR->free_node_pages[0] = phys_page;
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Updated LPN %d -> PPN %d (free_node[0])\n", 
             SYS_FREE_LIST_BASE_LPN, phys_page);
    
    // Initialize other free_node_pages to PAGE_NONE (will be allocated on-demand when written)
    for (int i = 1; i < FREE_NODE_PAGE_COUNT; i++) {
        MGR->free_node_pages[i] = PAGE_NONE;
    }
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Other free_node_pages set to PAGE_NONE (on-demand allocation)\n");
    
    // Set total_free_nodes to 1 (we have one initial free node)
    MGR->total_free_nodes = 1;
    
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Free list initialized with 1 node, total_free_nodes=%u\n",
             MGR->total_free_nodes);
    
    return 0;
}

int eflash_mgr_alloc(uint32_t size, uint32_t *out_logical_addr) {
    // Traverse all free_node pages, find first node that satisfies size requirement
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
        int16_t count = read_node_count(lpn);
        if (count < 0) continue;  // Skip invalid pages
        
        for (int16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(lpn, j);
            
            if (node.size >= size) {
                // Found suitable node
                uint32_t alloc_addr = node.addr;
                uint32_t remaining = node.size - size;
                
                FTL_DEBUG("[SPACE_ALLOC] Allocating logical_addr=0x%08X, size=%u from free_node LPN %d[%d]\n", 
                         alloc_addr, size, lpn, j);
                
                // Remove original node from table
                remove_node_from_table(alloc_addr);
                
                FTL_DEBUG("[SPACE_ALLOC] After removal, remaining=%u, will insert at addr=0x%08X\n", 
                         remaining, alloc_addr + size);
                
                // If there's remaining space, insert remaining node
                if (remaining > 0) {
                    insert_node_to_table(alloc_addr + size, remaining);
                    
#ifdef FTL_DEBUG_ENABLE
                    // Verify insertion
                    uint16_t first_lpn = SYS_FREE_LIST_BASE_LPN;
                    int16_t new_count = read_node_count(first_lpn);
                    if (new_count > 0) {
                        free_node_t verify_after = read_free_node(first_lpn, 0);
                        FTL_DEBUG("[SPACE_ALLOC] After insert: count=%d, node[0][0] addr=0x%08X, size=%u\n",
                                 new_count, verify_after.addr, verify_after.size);
                    }
#endif
                } else {
                    FTL_DEBUG("[SPACE_ALLOC] No remaining space to insert\n");
                }
                
                *out_logical_addr = alloc_addr;
                
#ifdef FTL_DEBUG_ENABLE
                // Verify: read the just-removed node position to confirm it's cleared
                free_node_t verify_node = read_free_node(lpn, j);
                FTL_DEBUG("[SPACE_ALLOC] After removal: node LPN %d[%d] addr=0x%08X, size=%u\n",
                         lpn, j, verify_node.addr, verify_node.size);
#endif
                
                return 0;
            }
        }
    }
    
    FTL_DEBUG("[SPACE_ALLOC] ERROR: No suitable free node found for size=%u\n", size);
    return -1;  // Insufficient space
}

// Find and remove the node that ends exactly at target_addr (i.e., node.addr + node.size == target_addr)
// Returns the size of the removed node, or 0 if not found
static uint32_t remove_node_ending_at(uint32_t target_addr) {
    // Search in base level pages
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
        int16_t count = read_node_count(lpn);
        if (count <= 0) continue;
        
        for (int16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(lpn, j);
            if (node.addr + node.size == target_addr) {
                return remove_node_from_table(node.addr);
            }
        }
    }
    
    // Search in extended level pages
    for (int level = 0; level < MAX_FREE_NODE_EXT_LEVELS; level++) {
        if (MGR->ext_free_node_addrs[level] == 0xFFFFFFFF) break;
        
        uint16_t start_lpn = (uint16_t)(MGR->ext_free_node_addrs[level] / EFLASH_PAGE_SIZE);
        for (int i = 0; i < FREE_NODE_EXT_PAGES; i++) {
            uint16_t lpn = start_lpn + i;
            int16_t count = read_node_count(lpn);
            if (count <= 0) continue;
            
            for (int16_t j = 0; j < count; j++) {
                free_node_t node = read_free_node(lpn, j);
                if (node.addr + node.size == target_addr) {
                    return remove_node_from_table(node.addr);
                }
            }
        }
    }
    
    return 0;  // Not found
}

void eflash_mgr_free(uint32_t logical_addr, uint32_t size) {

    MGR_PRINTF("[SPACE_FREE] Freeing logical_addr=0x%06X, size=%u\n", logical_addr, size);
    
    // Check if can merge with previous free block
    MGR_PRINTF("[SPACE_FREE] Checking merge with previous block ending at 0x%06X\n", logical_addr);
    uint32_t prev_size = remove_node_ending_at(logical_addr);
    MGR_PRINTF("[SPACE_FREE] Previous merge result: prev_size=%u\n", prev_size);
    if (prev_size > 0) {
        uint32_t prev_addr = logical_addr - prev_size;
        assert(prev_addr + prev_size == logical_addr);
        logical_addr = prev_addr;
        size += prev_size;
        MGR_PRINTF("[SPACE_FREE] Merged with previous block: new_addr=0x%06X, new_size=%u\n",
                 logical_addr, size);
    }
    
    // Check if can merge with next free block
    MGR_PRINTF("[SPACE_FREE] Checking merge with next block at addr=0x%06X\n", logical_addr + size);
    uint32_t next_size = remove_node_from_table(logical_addr + size);
    MGR_PRINTF("[SPACE_FREE] Next merge result: next_size=%u\n", next_size);
    if (next_size > 0) {
        size += next_size;
        MGR_PRINTF("[SPACE_FREE] Merged with next block: new_size=%u\n", size);
    }
    
    // Insert merged node
    MGR_PRINTF("[SPACE_FREE] Inserting merged node: addr=0x%06X, size=%u\n", logical_addr, size);
    insert_node_to_table(logical_addr, size);
}

void eflash_mgr_sync(void) {
    // free_node table already synced to Flash on each operation, this function kept for batch sync optimization
    (void)MGR;
}

uint32_t eflash_mgr_get_free_bytes(void) {
    uint32_t total_free_bytes = 0;
    
    // Sum up bytes from base level pages
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
        int16_t count = read_node_count(lpn);
        if (count < 0) continue;  // Skip invalid pages
        
        for (int16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(lpn, j);
            total_free_bytes += node.size;
        }
    }
    
    // Sum up bytes from extended level pages
    for (int level = 0; level < MAX_FREE_NODE_EXT_LEVELS; level++) {
        if (MGR->ext_free_node_addrs[level] == 0xFFFFFFFF) {
            break;  // No more extensions
        }
        
        uint16_t start_lpn = (uint16_t)(MGR->ext_free_node_addrs[level] / EFLASH_PAGE_SIZE);
        for (int i = 0; i < FREE_NODE_EXT_PAGES; i++) {
            int16_t count = read_node_count(start_lpn + i);
            if (count < 0) continue;
            
            for (int16_t j = 0; j < count; j++) {
                free_node_t node = read_free_node(start_lpn + i, j);
                total_free_bytes += node.size;
            }
        }
    }
    
    return total_free_bytes;
}


