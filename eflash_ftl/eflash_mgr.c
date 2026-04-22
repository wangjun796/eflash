#include "eflash_mgr.h"
#include "eflash_ftl.h"
#include "eflash_sim.h"
#include <string.h>
#include <stdio.h>

#ifndef FTL_DEBUG
#define FTL_DEBUG(...) do {} while(0)  // Disable debug output
#endif

// --- Internal Helper Functions ---

// Read count from free_node page
static int16_t read_node_count(uint16_t phys_page) {
    if (phys_page == PAGE_NONE) {
        return -1;
    }
    
    uint8_t buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(phys_page, buf) != 0) {
        return -1;
    }
    
    // count stored at beginning of user data area after meta region
    uint16_t offset = META_SIZE;  // Skip 48-byte meta
    return (int16_t)(buf[offset] | (buf[offset + 1] << 8));
}

// Write count to free_node page
static void write_node_count(uint16_t phys_page, uint16_t count) {
    uint8_t buf[EFLASH_PAGE_SIZE];
    
    // Initialize to all 0xFF (erased state)
    memset(buf, 0xFF, EFLASH_PAGE_SIZE);
    
    // count stored at beginning of user data area after meta region
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

// Read free_node at specified index
static free_node_t read_free_node(uint16_t phys_page, uint16_t index) {
    free_node_t node = {0xFFFFFFFF, 0xFFFF};  // Default return invalid node
    
    if (index >= FREE_NODES_PER_PAGE) {
        return node;  // Out of bounds protection
    }
    
    uint8_t buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(phys_page, buf) != 0) {
        return node;  // Read failed
    }
    
    // Node data starts after meta + 4-byte count
    uint16_t offset = META_SIZE + FREE_NODE_HEADER_SIZE + index * sizeof(free_node_t);
    
    // Boundary check
    if (offset + sizeof(free_node_t) > EFLASH_PAGE_SIZE) {
        return node;  // Beyond page range
    }
    
    memcpy(&node, buf + offset, sizeof(free_node_t));
    return node;
}

// Write free_node at specified index
static void write_free_node(uint16_t phys_page, uint16_t index, const free_node_t *node) {
    if (index >= FREE_NODES_PER_PAGE) {
        return;  // Out of bounds protection
    }
    
    uint8_t buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(phys_page, buf) != 0) {
        return;  // Read failed
    }
    
    // Node data starts after meta + 4-byte count
    uint16_t offset = META_SIZE + FREE_NODE_HEADER_SIZE + index * sizeof(free_node_t);
    
    // Boundary check
    if (offset + sizeof(free_node_t) > EFLASH_PAGE_SIZE) {
        return;  // Beyond page range
    }
    
    memcpy(buf + offset, node, sizeof(free_node_t));
    
    // Flash must be erased before writing
    eflash_hw_erase(phys_page);
    eflash_hw_prog(phys_page, buf);  // Write back entire page
}

// Find and remove node with specified logical address from free_node table, return its size
static uint32_t remove_node_from_table(eflash_mgr_t *mgr, uint32_t target_logical_addr) {
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        int16_t count = read_node_count(mgr->free_node_pages[i]);
        if (count < 0) continue;  // Skip invalid pages
        
        FTL_DEBUG("[REMOVE_NODE] Searching logical_addr=0x%08X in free_node[%d], count=%d\n", 
                 target_logical_addr, i, count);
        
        for (int16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            
            FTL_DEBUG("[REMOVE_NODE] Checking node[%d][%d]: addr=0x%08X, size=%u\n",
                     i, j, node.addr, node.size);
            
            if (node.addr == target_logical_addr) {
                FTL_DEBUG("[REMOVE_NODE] Found match at [%d][%d], removing...\n", i, j);
                
                // Read entire page
                uint8_t buf[EFLASH_PAGE_SIZE];
                eflash_hw_read(mgr->free_node_pages[i], buf);
                
                int16_t last_idx = count - 1;
                if (j != last_idx) {
                    // Overwrite current node with last node
                    uint16_t src_offset = META_SIZE + FREE_NODE_HEADER_SIZE + last_idx * sizeof(free_node_t);
                    uint16_t dst_offset = META_SIZE + FREE_NODE_HEADER_SIZE + j * sizeof(free_node_t);
                    memcpy(buf + dst_offset, buf + src_offset, sizeof(free_node_t));
                    
                    FTL_DEBUG("[REMOVE_NODE] Moved last node to position %d\n", j);
                }
                
                // Clear last node (fill with 0xFF)
                uint16_t clear_offset = META_SIZE + FREE_NODE_HEADER_SIZE + last_idx * sizeof(free_node_t);
                memset(buf + clear_offset, 0xFF, sizeof(free_node_t));
                
                // Update count
                uint16_t count_offset = META_SIZE;
                buf[count_offset] = (count - 1) & 0xFF;
                buf[count_offset + 1] = ((count - 1) >> 8) & 0xFF;
                
                // Calculate ECC
                size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
                uint8_t *ecc_ptr = buf + USER_DATA_SIZE + META_SIZE - 5;
                bch_generate(&bch_3bit, buf, protected_len, ecc_ptr);
                
                // Erase and write in one operation
                eflash_hw_erase(mgr->free_node_pages[i]);
                eflash_hw_prog(mgr->free_node_pages[i], buf);
                
                FTL_DEBUG("[REMOVE_NODE] Updated count: %d -> %d\n", count, count - 1);
                
                return node.size;
            }
        }
    }
    
    FTL_DEBUG("[REMOVE_NODE] ERROR: Logical address 0x%08X not found in any free_node table!\n", target_logical_addr);
    return 0;  // Not found
}

// Insert new free_node into table (maintain sorting by size)
static void insert_node_to_table(eflash_mgr_t *mgr, uint32_t logical_addr, uint32_t size) {
    // Find suitable insertion location (first page with size >= new node's size)
    int best_page_idx = -1;
    uint32_t best_size = 0xFFFFFFFF;
    
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t count = read_node_count(mgr->free_node_pages[i]);
        
        // Skip if page is full
        if (count >= FREE_NODES_PER_PAGE) continue;
        
        // Find first position in this page where size >= new size
        for (int16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            if (node.size >= size && node.size < best_size) {
                best_size = node.size;
                best_page_idx = i;
                break;
            }
        }
        
        // If no larger size found but page has space, can insert at end
        if (best_page_idx == -1 && count < FREE_NODES_PER_PAGE) {
            best_page_idx = i;
        }
    }
    
    if (best_page_idx == -1) {
        FTL_DEBUG("[INSERT_NODE] ERROR: No space in free_node tables\n");
        return;  // Table is full
    }
    
    int16_t count = read_node_count(mgr->free_node_pages[best_page_idx]);
    if (count < 0) {
        FTL_DEBUG("[INSERT_NODE] ERROR: Failed to read node count\n");
        return;
    }
    
    // Find insertion position (maintain ascending order by size)
    uint16_t insert_pos = (uint16_t)count;
    for (int16_t j = 0; j < count; j++) {
        free_node_t node = read_free_node(mgr->free_node_pages[best_page_idx], j);
        if (node.size >= size) {
            insert_pos = j;
            break;
        }
    }
    
    // Read entire page
    uint8_t buf[EFLASH_PAGE_SIZE];
    eflash_hw_read(mgr->free_node_pages[best_page_idx], buf);
    
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
    
    // Calculate ECC
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
    uint8_t *ecc_ptr = buf + USER_DATA_SIZE + META_SIZE - 5;
    bch_generate(&bch_3bit, buf, protected_len, ecc_ptr);
    
    // Erase and write in one operation
    eflash_hw_erase(mgr->free_node_pages[best_page_idx]);
    eflash_hw_prog(mgr->free_node_pages[best_page_idx], buf);
    
    FTL_DEBUG("[INSERT_NODE] Inserted logical_addr=0x%08X, size=%u at [%d][%d]\n",
             logical_addr, size, best_page_idx, insert_pos);
}

// --- Interface Implementation ---

void eflash_mgr_init(eflash_mgr_t *mgr, uint16_t total_pages) {
    mgr->total_pages = total_pages;
    
    FTL_DEBUG("[SPACE_INIT] === Memory-only initialization (no Flash writes) ===\n");
    
    // Initialize free_node page array to invalid values
    // These will be set by FTL when system pages are allocated through Radix Tree
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        mgr->free_node_pages[i] = PAGE_NONE;
    }
    
    // Initialize header page array to invalid values
    // These will be set by FTL when system pages are allocated through Radix Tree
    for (int i = 0; i < BASE_HEADER_PAGES; i++) {
        mgr->header_pages[i] = PAGE_NONE;
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
    
    mgr->next_alloc_page = reserved_logic_page_count;  // Next logical page to allocate (LPN 12)
}

bool eflash_mgr_check_initialized(eflash_mgr_t *mgr) {
    // Check if at least one free_node page contains valid data (not all 0xFF)
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        if (mgr->free_node_pages[i] == PAGE_NONE) {
            continue;  // Physical page not assigned yet
        }
        
        uint8_t buf[EFLASH_PAGE_SIZE];
        if (eflash_hw_read(mgr->free_node_pages[i], buf) != 0) {
            continue;  // Read failed
        }
        
        // Check if page is all 0xFF (blank/erased)
        bool is_blank = true;
        for (int j = 0; j < EFLASH_PAGE_SIZE; j++) {
            if (buf[j] != 0xFF) {
                is_blank = false;
                break;
            }
        }
        
        if (!is_blank) {
            // Page has data, check if it contains at least one valid node
            int16_t count = read_node_count(mgr->free_node_pages[i]);
            if (count > 0) {
                FTL_DEBUG("[SPACE_CHECK] Found initialized free_node page %d with %d nodes\n", i, count);
                return true;
            }
        }
    }
    
    FTL_DEBUG("[SPACE_CHECK] No initialized free_node pages found\n");
    return false;
}

int eflash_mgr_init_free_list(eflash_mgr_t *mgr, uint16_t total_pages, uint16_t reserved_logic_pages) {
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Initializing free list (memory only)...\n");
    
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
    
    // Check if physical page is assigned (should be set by FTL before calling this)
    if (mgr->free_node_pages[0] == PAGE_NONE) {
        FTL_DEBUG("[SPACE_INIT_FREE_LIST] WARNING: free_node_pages[0] not yet assigned.\n");
        FTL_DEBUG("[SPACE_INIT_FREE_LIST] This will be written via FTL layer when system page is allocated.\n");
        // Don't return error - the actual write will happen through FTL
        return 0;
    }
    
    // NOTE: Do NOT write directly to hardware here!
    // The actual write should be done by FTL through write_system_page()
    // which ensures Radix Tree mapping and wear leveling.
    // This function only prepares the data structure in memory.
    
    FTL_DEBUG("[SPACE_INIT_FREE_LIST] Free list initialization prepared (will be written via FTL)\n");
    
    return 0;
}

int eflash_mgr_alloc(eflash_mgr_t *mgr, uint32_t size, uint32_t *out_logical_addr) {
    // Traverse all free_node pages, find first node that satisfies size requirement
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        int16_t count = read_node_count(mgr->free_node_pages[i]);
        if (count < 0) continue;  // Skip invalid pages
        
        for (int16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            
            if (node.size >= size) {
                // Found suitable node
                uint32_t alloc_addr = node.addr;
                uint32_t remaining = node.size - size;
                
                FTL_DEBUG("[SPACE_ALLOC] Allocating logical_addr=0x%08X, size=%u from free_node[%d][%d]\n", 
                         alloc_addr, size, i, j);
                
                // Remove original node from table
                remove_node_from_table(mgr, alloc_addr);
                
                FTL_DEBUG("[SPACE_ALLOC] After removal, remaining=%u, will insert at addr=0x%08X\n", 
                         remaining, alloc_addr + size);
                
                // If there's remaining space, insert remaining node
                if (remaining > 0) {
                    insert_node_to_table(mgr, alloc_addr + size, remaining);
                    
#ifdef FTL_DEBUG_ENABLE
                    // Verify insertion
                    int16_t new_count = read_node_count(mgr->free_node_pages[0]);
                    if (new_count > 0) {
                        free_node_t verify_after = read_free_node(mgr->free_node_pages[0], 0);
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
                free_node_t verify_node = read_free_node(mgr->free_node_pages[i], j);
                FTL_DEBUG("[SPACE_ALLOC] After removal: node[%d][%d] addr=0x%08X, size=%u\n",
                         i, j, verify_node.addr, verify_node.size);
#endif
                
                return 0;
            }
        }
    }
    
    FTL_DEBUG("[SPACE_ALLOC] ERROR: No suitable free node found for size=%u\n", size);
    return -1;  // Insufficient space
}

void eflash_mgr_free(eflash_mgr_t *mgr, uint32_t logical_addr, uint32_t size) {
    FTL_DEBUG("[SPACE_FREE] Freeing logical_addr=0x%06X, size=%u\n", logical_addr, size);
    
    // Check if can merge with previous free block
    uint32_t prev_size = remove_node_from_table(mgr, logical_addr - 1);
    if (prev_size > 0) {
        logical_addr = logical_addr - 1;
        size += prev_size;
        FTL_DEBUG("[SPACE_FREE] Merged with previous block: new_addr=0x%06X, new_size=%u\n",
                 logical_addr, size);
    }
    
    // Check if can merge with next free block
    uint32_t next_size = remove_node_from_table(mgr, logical_addr + size);
    if (next_size > 0) {
        size += next_size;
        FTL_DEBUG("[SPACE_FREE] Merged with next block: new_size=%u\n", size);
    }
    
    // Insert merged node
    insert_node_to_table(mgr, logical_addr, size);
}

void eflash_mgr_sync(eflash_mgr_t *mgr) {
    // free_node table already synced to Flash on each operation, this function kept for batch sync optimization
    (void)mgr;
}

uint32_t eflash_mgr_get_free_bytes(eflash_mgr_t *mgr) {
    uint32_t total_free_pages = 0;
    
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        int16_t count = read_node_count(mgr->free_node_pages[i]);
        if (count < 0) continue;  // Skip invalid pages
        
        for (int16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            total_free_pages += node.size;
        }
    }
    
    return total_free_pages * EFLASH_PAGE_SIZE;
}
