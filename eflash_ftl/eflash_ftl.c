#include "eflash_ftl.h"
#include "eflash_sim.h"
#include "eflash_mgr.h"
#include <string.h>
#include <stdio.h>

// --- Abbreviation Reference ---
// This file uses the following abbreviations for clarity:
//   PPN  - Physical Page Number (physical flash page address)
//   LPN  - Logical Page Number (logical address in FTL mapping)
//   OBJ  - Object (object header or data object)
//   HDR  - Header (metadata header)
//   GC   - Garbage Collection
//   TXN  - Transaction
//   EXT  - Extension (extended header area)
//   MGR  - Manager (space manager)
//   FTL  - Flash Translation Layer
//   ECC  - Error Correction Code
//   META - Metadata (page metadata including epoch, count, status, etc.)

#ifndef PAGE_NONE
#define PAGE_NONE 0xFFFF
#endif

// --- Debug Configuration ---
// FTL_DEBUG macro: Enable verbose debug output for troubleshooting
//
// Usage:
//   - Default: Disabled (for performance during testing)
//   - To enable: Add -DFTL_DEBUG_ENABLE=1 to compiler flags
//   - Or define before including this file: #define FTL_DEBUG_ENABLE 1
//
// When disabled, all FTL_DEBUG() calls compile to no-op (zero overhead)
#ifndef FTL_DEBUG_ENABLE
#define FTL_DEBUG_ENABLE 1  // Temporarily enable for debugging word update issue
#endif

#if FTL_DEBUG_ENABLE
#define FTL_DEBUG(fmt, ...) printf("[FTL_DEBUG] " fmt, ##__VA_ARGS__)
#else
#define FTL_DEBUG(fmt, ...) do {} while(0)  // Compile to nothing when disabled
#endif

// --- System Area Logical Page Layout ---
// All pages (including system areas) are managed through FTL's Radix Tree for wear leveling:
//   FIXED System Pages (LPN 0-11):
//     LPN 0 ~ 7           : Base object header table (8 pages, 232 objects)
//     LPN 8 ~ 11          : Free list (4 pages)
//   DYNAMIC Pages (LPN 12+):
//     User data pages: allocated by FTL write operations
//     Extended object headers: allocated by eflash_mgr_alloc (LPN not fixed, e.g., 100-103, 200-203, etc.)
//
// All pages use LPN directly as sector_id and go through Radix Tree mapping.

#ifndef PAGE_NONE
#define PAGE_NONE 0xFFFF
#endif

// System area logical page numbers (LPN) - FIXED allocation
#define SYS_OBJ_HEADER_BASE_LPN   0       // Base object header table starts at LPN 0
#define SYS_OBJ_HEADER_PAGES      8       // Base object header table size (8 pages)
#define SYS_FREE_LIST_BASE_LPN    8       // Free list starts at LPN 8
#define SYS_FREE_LIST_PAGES       4       // Free list size (4 pages)
#define SYS_RESERVED_LPN_COUNT    12      // Total reserved LPNs for system areas

// --- BCH ECC Wrapper Functions ---

// bch_encode: Generate ECC checksum
static void bch_encode(const struct bch_def *bch, const uint8_t *data, size_t len, uint8_t *ecc) {
    bch_generate(bch, data, len, ecc);
}

// bch_decode: Verify and correct errors
// Returns: 0 no error, >0 number of corrected errors, -1 uncorrectable
static int bch_decode(const struct bch_def *bch, uint8_t *data, size_t len, const uint8_t *ecc) {
    // First verify
    if (bch_verify(bch, data, len, ecc) == 0) {
        return 0; // No error
    }

    // Need correction, create copy for repair
    // Note: Ensure data_copy and ecc_copy are large enough
    // Use EFLASH_PAGE_SIZE as it's the maximum possible data length
    uint8_t data_copy[EFLASH_PAGE_SIZE];
    uint8_t ecc_copy[5]; // Reference code uses 5 bytes for ecc

    if (len > sizeof(data_copy)) return -1; // Safety check

    memcpy(data_copy, data, len);
    memcpy(ecc_copy, ecc, 5); // Assume ecc length is 5

    bch_repair(bch, data_copy, len, ecc_copy);

    // Verify again
    if (bch_verify(bch, data_copy, len, ecc_copy) != 0) {
        return -1; // Still has errors, uncorrectable
    }

    // Copy corrected data back
    memcpy(data, data_copy, len);
    return 1; // Assume errors were corrected
}

#define TOTAL_PAGES EFLASH_TOTAL_PAGES
#define META_OFFSET USER_DATA_SIZE
static const struct bch_def *bch_cfg = &bch_3bit;

// --- ECC Helper Functions ---

/**
 * calc_page_ecc: Calculate ECC checksum for full page (user data + metadata excluding ECC part)
 * @page_buf: Pointer to full page buffer (contains USER_DATA_SIZE bytes user data and META_SIZE bytes metadata)
 *
 * Layout description:
 * - page_buf[0 .. USER_DATA_SIZE-1]: User data
 * - page_buf[USER_DATA_SIZE .. USER_DATA_SIZE+META_SIZE-6]: Metadata (excluding ECC)
 * - page_buf[USER_DATA_SIZE+META_SIZE-5 .. USER_DATA_SIZE+META_SIZE-1]: ECC checksum (5 bytes)
 */
static void calc_page_ecc(uint8_t *page_buf) {
    // ECC protection range: user data + metadata (excluding ECC field)
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;

    // ECC stored in last 5 bytes of metadata
    uint8_t *ecc_ptr = page_buf + USER_DATA_SIZE + META_SIZE - 5;

    bch_encode(bch_cfg, page_buf, protected_len, ecc_ptr);
}

/**
 * verify_and_correct_page: Verify and attempt to correct full page data
 * @page_buf: Pointer to full page buffer
 * Returns: 0 success, -1 failure (uncorrectable errors)
 */
static int verify_and_correct_page(uint8_t *page_buf) {
    // ECC protection range: user data + metadata (excluding ECC field)
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;

    // ECC stored in last 5 bytes of metadata
    uint8_t *ecc_ptr = page_buf + USER_DATA_SIZE + META_SIZE - 5;

    FTL_DEBUG("[ECC] Verifying page: protected_len=%zu, ecc at offset %zu\n",
              protected_len, (size_t)(ecc_ptr - page_buf));

    // Create copy for correction
    uint8_t data_copy[EFLASH_PAGE_SIZE];
    memcpy(data_copy, page_buf, EFLASH_PAGE_SIZE);

    // First verify
    int verify_result = bch_verify(bch_cfg, data_copy, protected_len, ecc_ptr);
    FTL_DEBUG("[ECC] Verify result: %d (0=ok, >0=errors, <1=uncorrectable)\n", verify_result);

    if (verify_result == 0) {
        return 0; // No error
    }

    // Attempt correction
    int result = bch_decode(bch_cfg, data_copy, protected_len, ecc_ptr);
    FTL_DEBUG("[ECC] Decode result: %d\n", result);

    if (result < 0) {
        FTL_DEBUG("[ECC] ERROR: Uncorrectable errors detected!\n");
        return -1; // Uncorrectable errors
    }

    if (result > 0) {
        // Errors corrected, copy back to original buffer
        memcpy(page_buf, data_copy, EFLASH_PAGE_SIZE);
        FTL_DEBUG("[ECC] Corrected %d errors\n", result);
    }

    return 0;
}

// --- Low-level Flash Operation Wrappers ---

/**
 * is_blank_page: Check if page is all 0xFF (freshly erased blank page)
 * @page: Physical page number
 * @return: true=all 0xFF blank page, false=contains valid data
 */
static bool is_blank_page(uint16_t page) {
    uint8_t buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(page, buf) != 0) return false;

    // Quick check: verify each byte is 0xFF
    for (int i = 0; i < EFLASH_PAGE_SIZE; i++) {
        if (buf[i] != 0xFF) {
            return false; // Not a blank page
        }
    }

    return true; // Is a blank page
}

// --- System Page Read/Write Functions ---

/**
 * write_system_page: Write a system page through FTL layer
 * @ftl: FTL instance
 * @lpn: Logical Page Number (used directly as sector_id)
 * @data: User data to write (USER_DATA_SIZE bytes)
 * @return: 0 on success, -1 on failure
 * 
 * Description:
 *   System pages (object headers, free list) use their LPN directly as sector_id.
 *   They go through Radix Tree mapping just like user data pages,
 *   ensuring wear leveling across all Flash pages.
 *   
 *   Note: For extended object headers, the LPN is dynamically allocated by
 *   eflash_mgr_alloc, so it can be any value (e.g., 100-103, 200-203, etc.)
 */
static int write_system_page(eflash_ftl_t *ftl, uint16_t lpn, const uint8_t *data) {
    FTL_DEBUG("[SYS_WRITE] Writing system page LPN=%d (sector_id=%d)\n", lpn, lpn);
    return eflash_ftl_write(ftl, lpn, data);
}

/**
 * read_system_page: Read a system page through FTL layer
 * @ftl: FTL instance
 * @lpn: Logical Page Number
 * @data: Output buffer for user data (USER_DATA_SIZE bytes)
 * @return: 0 on success, -1 on failure
 */
static int read_system_page(eflash_ftl_t *ftl, uint16_t lpn, uint8_t *data) {
    FTL_DEBUG("[SYS_READ] Reading system page LPN=%d (sector_id=%d)\n", lpn, lpn);
    return eflash_ftl_read(ftl, lpn, data);
}

/**
 * is_valid_page: Determine if physical page contains valid metadata
 */
static bool is_valid_page(uint16_t page, ftl_meta_t *meta) {
    // Optimization 1: First check if it's all 0xFF blank page, return invalid directly if so
    if (is_blank_page(page)) {
        FTL_DEBUG("[PAGE_VALID] Page %d is BLANK (all 0xFF)\n", page);
        memset(meta, 0xFF, sizeof(ftl_meta_t));
        return false;
    }

    uint8_t buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(page, buf) != 0) return false;

    // Use new full-page ECC verification
    if (verify_and_correct_page(buf) != 0) return false;

    // Copy metadata from corrected buffer
    memcpy(meta, buf + META_OFFSET, META_SIZE);

    // 2. Unwritten page (BLANK status)
    if (meta->status == TXN_STATUS_BLANK) return false;

    // 3. Status check: READY, COMMITTED or non-transaction valid page (INVALID/0x00)
    return (meta->status == TXN_STATUS_COMMITTED ||
            meta->status == TXN_STATUS_READY ||
            meta->status == TXN_STATUS_INVALID);
}

/**
 * write_full_page: Write a complete page with data and metadata
 * @ppn: Physical Page Number to write
 * @data: User data buffer (USER_DATA_SIZE bytes)
 * @meta_in: Metadata to write
 * @return: 0 on success, -1 on failure
 */
static int write_full_page(uint16_t ppn, const uint8_t *data, const ftl_meta_t *meta_in) {
    uint8_t buf[EFLASH_PAGE_SIZE];
    memset(buf, 0xFF, EFLASH_PAGE_SIZE);

    // Copy user data
    if (data) memcpy(buf, data, USER_DATA_SIZE);

    // Copy metadata (excluding ECC field)
    ftl_meta_t *meta_out = (ftl_meta_t *)(buf + META_OFFSET);
    memcpy(meta_out, meta_in, (size_t)(META_SIZE - 5));

    // Calculate ECC for full page (covers user data + metadata excluding ECC part)
    calc_page_ecc(buf);

    if (eflash_hw_erase(ppn) != 0) return -1;
    return eflash_hw_prog(ppn, buf);
}

/**
 * allocate_physical_page: Allocate an available physical page (using Head/Tail mechanism)
 * @ftl: FTL instance
 * @return: Physical page index, -1 on failure
 *
 * Description: Following Dhara's design, allocate physical pages sequentially starting from Head
 */
static int allocate_physical_page(eflash_ftl_t *ftl) {
    uint16_t ppn = ftl->gc_head_page;  // Physical Page Number
    uint16_t first_user_page = FREE_NODE_PAGE_COUNT + BASE_HEADER_PAGES;
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;

    // Move to next physical page
    ftl->gc_head_page++;

    // Handle wraparound
    if (ftl->gc_head_page > last_user_page) {
        ftl->gc_head_page = first_user_page;
    }

    FTL_DEBUG("[ALLOC_PHYS] Allocated physical page %d (next head=%d)\n",
             ppn, ftl->gc_head_page);

    return ppn;
}

// --- Radix Tree Core Logic (ported from Dhara map.c) ---

static inline int get_bit(uint16_t sector, int depth) {
    // Safety check: ensure depth is within valid range
    if (depth < 0 || depth >= RADIX_DEPTH) {
        return -1; // Return error code for invalid depth
    }
    return (sector >> (RADIX_DEPTH - 1 - depth)) & 1;
}

// Corrected trace_path: Strictly follow Dhara design, remove new_phys parameter
// Responsibility: Only build metadata template for new node, do not insert any physical page
static int trace_path(eflash_ftl_t *ftl, uint16_t base_root, uint16_t sector, ftl_meta_t *out_meta) {
    uint8_t meta_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t cur_meta;
    int depth = 0;
    uint16_t current = base_root;

    FTL_DEBUG("[TRACE] sector=%d, base_root=%d\n", sector, base_root);

    // Initialize new node's metadata (alt array initialized to all 0xFF/PAGE_NONE)
    memset(out_meta, 0xFF, sizeof(ftl_meta_t));
    out_meta->sector_id = sector;
    out_meta->global_count = ftl->next_count++;
    out_meta->epoch = ftl->current_epoch;
    out_meta->txn_id = ftl->active_txn_id;

    // Set status: READY in transaction mode, COMMITTED directly in non-transaction mode
    if (ftl->active_txn_id != TXN_ID_NONE) {
        out_meta->status = TXN_STATUS_READY;
    } else {
        out_meta->status = TXN_STATUS_COMMITTED;  // Non-transaction mode commits directly
    }

    // If tree is empty, return directly (alt array already initialized to all PAGE_NONE)
    if (current == PAGE_NONE) {
        FTL_DEBUG("[TRACE] Empty tree\n");
        return 0;
    }

    // Read root node metadata
    if (eflash_hw_read(current, meta_buf) != 0) {
        FTL_DEBUG("[TRACE] ERROR: Failed to read base_root page %d\n", current);
        return -1;
    }
    if (verify_and_correct_page(meta_buf) != 0) {
        FTL_DEBUG("[TRACE] ERROR: Page verification failed for base_root page %d\n", current);
        return -1;
    }
    memcpy(&cur_meta, meta_buf + META_OFFSET, META_SIZE);

    FTL_DEBUG("[TRACE] Root sector=%d\n", cur_meta.sector_id);

    // Traverse radix tree to build path
    while (depth < RADIX_DEPTH) {
        // Calculate bit value at current depth (MSB first)
        uint16_t bit_mask = 1 << (RADIX_DEPTH - 1 - depth);
        int target_bit = (sector & bit_mask) ? 1 : 0;
        int current_bit = (cur_meta.sector_id & bit_mask) ? 1 : 0;

        FTL_DEBUG("[TRACE] depth=%d, target_bit=%d, current_bit=%d, alt[%d]=%d\n",
                  depth, target_bit, current_bit, depth, cur_meta.alt[depth]);

        if (target_bit != current_bit) {
            // Bits differ: divergence occurs
            // Save current physical page as new node's alt pointer (record divergence point)
            out_meta->alt[depth] = current;
            FTL_DEBUG("[TRACE] Diverge: out_meta->alt[%d]=%d\n", depth, current);

            // Get current node's alt pointer, continue searching downward
            uint16_t next_ppn = cur_meta.alt[depth];  // Next Physical Page Number
            if (next_ppn == PAGE_NONE) {
                // Path interrupted, jump to not_found to handle remaining depth
                FTL_DEBUG("[TRACE] Path interrupted at depth=%d\n", depth);
                depth++;
                goto not_found;
            }

            FTL_DEBUG("[TRACE] Follow alt[%d]=%d\n", depth, next_ppn);
            
            // Read next node's metadata
            if (eflash_hw_read(next_ppn, meta_buf) != 0) {
                FTL_DEBUG("[TRACE] ERROR: Failed to read next PPN %d\n", next_ppn);
                return -1;
            }
            if (verify_and_correct_page(meta_buf) != 0) {
                FTL_DEBUG("[TRACE] ERROR: Page verification failed at PPN %d\n", next_ppn);
                return -1;
            }
            memcpy(&cur_meta, meta_buf + META_OFFSET, META_SIZE);
            
            current = next_ppn;
        } else {
            // Bits same: inherit alt pointer from current node
            out_meta->alt[depth] = cur_meta.alt[depth];
            FTL_DEBUG("[TRACE] Same bit, inherit alt[%d]=%d\n", depth, out_meta->alt[depth]);
        }

        depth++;
    }

    // Loop ends normally, meaning matching sector found (or traversed all depths)
    // new_meta's alt array already fully set during traversal through Diverge and Same bit branches
    FTL_DEBUG("[TRACE] Found match after full traversal\n");
    return 0;

not_found:
    // Dhara's logic: Set all alt pointers from current depth to NONE
    // Note: depth++ already executed before goto not_found, so start setting from current depth
    FTL_DEBUG("[TRACE] Not found, setting remaining alt pointers to NONE from depth=%d\n", depth);
    while (depth < RADIX_DEPTH) {
        out_meta->alt[depth] = PAGE_NONE;
        depth++;
    }

    return 0;
}

// --- Object Header Management Core Logic ---

/**
 * eflash_ftl_obj_get_header: Read object header by obj_id
 * @ftl: FTL instance
 * @obj_id: Object ID (0-2087)
 * @hdr: Output buffer for object header
 * @return: 0 on success, -1 on failure
 */
int eflash_ftl_obj_get_header(eflash_ftl_t *ftl, uint16_t obj_id, obj_header_t *hdr) {
    if (!ftl || !hdr) return -1;
    
    uint16_t lpn, offset;  // LPN: Logical Page Number
    if (get_header_page_info(ftl, obj_id, &lpn, &offset) != 0) {
        FTL_DEBUG("[OBJ_GET] ERROR: Invalid obj_id=%d\n", obj_id);
        return -1;
    }
    
    // Read the entire system page through FTL layer (wear leveling enabled)
    uint8_t page_buf[USER_DATA_SIZE];  // System pages only contain user data area
    if (read_system_page(ftl, lpn, page_buf) != 0) {
        FTL_DEBUG("[OBJ_GET] ERROR: Failed to read system LPN %d\n", lpn);
        return -1;
    }
    
    // Extract the specific object header from the page
    memcpy(hdr, page_buf + offset, sizeof(obj_header_t));
    
    FTL_DEBUG("[OBJ_GET] obj_id=%d, type=0x%02X, body_addr=%d from LPN %d\n", 
              obj_id, hdr->type, hdr->body_addr, lpn);
    
    return 0;
}

/**
 * eflash_ftl_obj_set_header: Write object header by obj_id
 * @ftl: FTL instance
 * @obj_id: Object ID (0-2087)
 * @hdr: Object header to write
 * @return: 0 on success, -1 on failure
 * 
 * Note: This function performs read-modify-write on the entire page
 * to maintain data integrity and ECC consistency.
 */
int eflash_ftl_obj_set_header(eflash_ftl_t *ftl, uint16_t obj_id, const obj_header_t *hdr) {
    if (!ftl || !hdr) return -1;
    
    uint16_t lpn, offset;  // LPN: Logical Page Number
    if (get_header_page_info(ftl, obj_id, &lpn, &offset) != 0) {
        FTL_DEBUG("[OBJ_SET] ERROR: Invalid obj_id=%d\n", obj_id);
        return -1;
    }
    
    // Step 1: Read the entire system page through FTL layer
    uint8_t page_buf[USER_DATA_SIZE];  // System pages only contain user data area
    if (read_system_page(ftl, lpn, page_buf) != 0) {
        FTL_DEBUG("[OBJ_SET] ERROR: Failed to read system LPN %d\n", lpn);
        return -1;
    }
    
    // Step 2: Modify the specific object header in buffer
    memcpy(page_buf + offset, hdr, sizeof(obj_header_t));
    
    // Step 3: Write the entire page back through FTL layer (wear leveling enabled)
    if (write_system_page(ftl, lpn, page_buf) != 0) {
        FTL_DEBUG("[OBJ_SET] ERROR: Failed to write system LPN %d\n", lpn);
        return -1;
    }
    
    FTL_DEBUG("[OBJ_SET] obj_id=%d, type=0x%02X, body_addr=%d -> LPN %d\n", 
              obj_id, hdr->type, hdr->body_addr, lpn);
    
    return 0;
}

/**
 * get_header_page_info: Calculate logical page and page offset based on object ID
 */
static int get_header_page_info(eflash_ftl_t *ftl, uint16_t obj_id, uint16_t *out_log_page, uint16_t *out_offset) {
    if (obj_id < BASE_HEADER_CAPACITY) {
        // Base area
        *out_log_page = ftl->base_hdr_addr + (obj_id / OBJ_HEADERS_PER_PAGE);
        *out_offset = (obj_id % OBJ_HEADERS_PER_PAGE) * sizeof(obj_header_t);
        return 0;
    }

    // Extended area
    uint16_t ext_idx = obj_id - BASE_HEADER_CAPACITY;
    uint16_t level = (ext_idx / EXT_HEADER_CAPACITY) + 1;

    if (level > MAX_EXT_LEVELS || ftl->ext_hdr_addrs[level - 1] == PAGE_NONE) {
        return -1; // Not yet extended or out of range
    }

    uint16_t page_in_unit = (ext_idx % EXT_HEADER_CAPACITY) / OBJ_HEADERS_PER_PAGE;
    uint16_t idx_in_page = (ext_idx % EXT_HEADER_CAPACITY) % OBJ_HEADERS_PER_PAGE;

    *out_log_page = ftl->ext_hdr_addrs[level - 1] + page_in_unit;
    *out_offset = idx_in_page * sizeof(obj_header_t);
    return 0;
}

/**
 * extend_headers: Dynamically extend one level of object header space (4 pages)
 */
static int extend_headers(eflash_ftl_t *ftl) {
    // 1. Find the highest level extension page, get its last object header (pointer field)
    uint16_t prev_ext_addr = ftl->base_hdr_addr + BASE_HEADER_PAGES - 1; // Default points to last page of base area
    int level = 0;
    while (level < MAX_EXT_LEVELS && ftl->ext_hdr_addrs[level] != PAGE_NONE) {
        prev_ext_addr = ftl->ext_hdr_addrs[level] + EXT_HEADER_PAGES_UNIT - 1;
        level++;
    }

    if (level >= MAX_EXT_LEVELS) return -1; // Reached maximum extension levels

    // 2. Allocate new 4-page logical space
    uint32_t new_ext_logical_addr;
    if (eflash_mgr_alloc(&ftl->spc_mgr, 4 * EFLASH_PAGE_SIZE, &new_ext_logical_addr) != 0) {
        return -1;
    }
    uint16_t new_ext_lpn = (uint16_t)(new_ext_logical_addr / EFLASH_PAGE_SIZE);  // Logical Page Number

    // 3. Update pointer at end of previous level
    obj_header_t link_hdr;
    memset(&link_hdr, 0, sizeof(link_hdr));
    link_hdr.type = OBJ_TYPE_LINK; // Mark as extension link object
    link_hdr.body_addr = new_ext_lpn;
    link_hdr.body_size = EXT_HEADER_PAGES_UNIT * USER_DATA_SIZE; // Total size of extension area

    // Write link info to end of previous level
    eflash_ftl_obj_set_header(ftl, (level == 0 ? BASE_HEADER_CAPACITY - 1 : (level * EXT_HEADER_CAPACITY + BASE_HEADER_CAPACITY - 1)), &link_hdr);

    // 4. Record new extension address (dynamically allocated LPN)
    ftl->ext_hdr_addrs[level] = new_ext_lpn;

    // 5. Initialize new pages through FTL layer (wear leveling enabled)
    //    Note: Extended object headers use dynamically allocated LPNs (not fixed)
    uint8_t empty_page[USER_DATA_SIZE];
    memset(empty_page, 0xFF, USER_DATA_SIZE);
    for (int i = 0; i < EXT_HEADER_PAGES_UNIT; i++) {
        if (write_system_page(ftl, new_ext_lpn + i, empty_page) != 0) {
            FTL_DEBUG("[EXTEND] ERROR: Failed to initialize system LPN %d\n", new_ext_lpn + i);
            return -1;
        }
    }

    FTL_DEBUG("[EXTEND] Extension level %d allocated at LPN %d-%d\n", 
              level, new_ext_lpn, new_ext_lpn + EXT_HEADER_PAGES_UNIT - 1);
    
    return 0;
}

/**
 * scan_and_rebuild_ext_headers: Scan object header chain and rebuild ext_hdr_addrs array
 * @ftl: FTL instance
 * 
 * Description:
 *   Scans the base area and extension areas to find all LINK headers,
 *   then rebuilds the ext_hdr_addrs array based on the link chain.
 *   This ensures correct recovery after power failure.
 */
static void scan_and_rebuild_ext_headers(eflash_ftl_t *ftl) {
    FTL_DEBUG("[INIT] Scanning object header extension chain...\n");
    
    // Initialize all entries to PAGE_NONE
    for (int i = 0; i < MAX_EXT_LEVELS; i++) {
        ftl->ext_hdr_addrs[i] = PAGE_NONE;
    }
    
    int level = 0;
    uint16_t current_scan_lpn;  // Current scan Logical Page Number
    
    // Start from base area's last page (LPN 7 for fixed system pages)
    current_scan_lpn = ftl->base_hdr_addr + BASE_HEADER_PAGES - 1;
    
    FTL_DEBUG("[INIT] Starting scan from base area LPN %d\n", current_scan_lpn);
    
    // Traverse the extension chain
    while (level < MAX_EXT_LEVELS) {
        // Read the last object header of current level through FTL
        obj_header_t link_hdr;
        uint16_t offset = (OBJ_HEADERS_PER_PAGE - 1) * sizeof(obj_header_t);
        
        uint8_t page_buf[USER_DATA_SIZE];  // System pages only contain user data
        if (read_system_page(ftl, current_scan_lpn, page_buf) != 0) {
            FTL_DEBUG("[INIT] ERROR: Failed to read system LPN %d for link scan\n", current_scan_lpn);
            break;
        }
        
        // Extract the last object header from the page
        memcpy(&link_hdr, page_buf + offset, sizeof(obj_header_t));
        
        // Check if this is a valid LINK header
        if (link_hdr.type == OBJ_TYPE_LINK && link_hdr.body_addr != PAGE_NONE) {
            // Found a valid extension link
            ftl->ext_hdr_addrs[level] = (uint16_t)link_hdr.body_addr;
            FTL_DEBUG("[INIT] Found extension level %d at LPN %d\n", level, ftl->ext_hdr_addrs[level]);
            
            // Move to next level's last page (extension LPNs are dynamically allocated)
            level++;
            if (level < MAX_EXT_LEVELS) {
                current_scan_lpn = ftl->ext_hdr_addrs[level - 1] + EXT_HEADER_PAGES_UNIT - 1;
            }
        } else {
            // No more extensions
            FTL_DEBUG("[INIT] Extension chain ends at level %d\n", level);
            break;
        }
    }
    
    FTL_DEBUG("[INIT] Extension scan complete. Total levels: %d\n", level);
}

// --- FTL Initialization and Pre-allocation ---

int eflash_ftl_init(eflash_ftl_t *ftl) {
    FTL_DEBUG("[INIT] Starting eflash_ftl_init\n");

    eflash_mgr_init(&ftl->spc_mgr, EFLASH_TOTAL_PAGES);

    ftl_meta_t meta;
    uint32_t max_count = 0;
    uint16_t max_epoch = 0;

    ftl->root_page = PAGE_NONE;
    ftl->shadow_root = PAGE_NONE;
    ftl->next_count = 1;
    ftl->current_epoch = 0;
    ftl->active_txn_id = TXN_ID_NONE;
    ftl->is_initialized = true;

    // Initialize system area: Fixed system pages use LPN 0-11
    // Extended object headers will be dynamically allocated later
    ftl->base_hdr_addr = SYS_OBJ_HEADER_BASE_LPN;  // LPN 0
    ftl->free_list_addr = SYS_FREE_LIST_BASE_LPN;  // LPN 8
    
    // Initialize ext_hdr_addrs to PAGE_NONE (will be rebuilt by scanning)
    for (int i = 0; i < MAX_EXT_LEVELS; i++) {
        ftl->ext_hdr_addrs[i] = PAGE_NONE;
    }
    
    // Scan and rebuild extension header addresses from existing data
    scan_and_rebuild_ext_headers(ftl);

    // Initialize GC related fields (following Dhara Head/Tail model)
    // User data starts from LPN 12 (after system reserved areas)
    ftl->total_user_pages = EFLASH_TOTAL_PAGES - SYS_RESERVED_LPN_COUNT;
    ftl->gc_threshold = ftl->total_user_pages / 5; // 20% threshold

    // Head and Tail start from first user page (LPN 12)
    ftl->gc_head_page = SYS_RESERVED_LPN_COUNT;  // = 12
    ftl->gc_tail_page = SYS_RESERVED_LPN_COUNT;  // = 12
    ftl->gc_in_progress = false;  // Initialize GC flag

    FTL_DEBUG("[INIT] GC initialized: total_user_pages=%d, threshold=%d, head_page=%d, tail_page=%d\n",
              ftl->total_user_pages, ftl->gc_threshold, ftl->gc_head_page, ftl->gc_tail_page);

    // Pre-allocate fixed system pages through FTL (ensures they are in Radix Tree)
    FTL_DEBUG("[INIT] Pre-allocating fixed system pages LPN 0-11...\n");
    uint8_t empty_page[USER_DATA_SIZE];
    memset(empty_page, 0xFF, USER_DATA_SIZE);
    
    // Allocate base object header pages (LPN 0-7)
    for (uint16_t lpn = SYS_OBJ_HEADER_BASE_LPN; lpn < SYS_OBJ_HEADER_BASE_LPN + SYS_OBJ_HEADER_PAGES; lpn++) {
        if (write_system_page(ftl, lpn, empty_page) != 0) {
            FTL_DEBUG("[INIT] ERROR: Failed to pre-allocate system LPN %d\n", lpn);
            return -1;
        }
    }
    
    // Allocate free list pages (LPN 8-11)
    for (uint16_t lpn = SYS_FREE_LIST_BASE_LPN; lpn < SYS_FREE_LIST_BASE_LPN + SYS_FREE_LIST_PAGES; lpn++) {
        if (write_system_page(ftl, lpn, empty_page) != 0) {
            FTL_DEBUG("[INIT] ERROR: Failed to pre-allocate system LPN %d\n", lpn);
            return -1;
        }
    }
    
    FTL_DEBUG("[INIT] Fixed system pages pre-allocated successfully\n");

    // Scan entire chip to find latest COMMITTED page as Root
    FTL_DEBUG("[INIT] Scanning %d pages for valid root...\n", EFLASH_TOTAL_PAGES);
    for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
        if (is_valid_page(i, &meta) && meta.status == TXN_STATUS_COMMITTED) {
            if (meta.epoch > max_epoch ||
                (meta.epoch == max_epoch && meta.global_count > max_count)) {
                max_epoch = meta.epoch;
                max_count = meta.global_count;
                ftl->root_page = i;
                ftl->next_count = max_count + 1;
                ftl->current_epoch = max_epoch;
                FTL_DEBUG("[INIT] Found valid root at page %d, count=%d\n", i, max_count);
            }
        }

        // Output progress every 1000 pages scanned
        if (i % 1000 == 0 && i > 0) {
            FTL_DEBUG("[INIT] Scanned %d/%d pages...\n", i, EFLASH_TOTAL_PAGES);
        }
    }

    FTL_DEBUG("[INIT] Scan complete. Root page: %d, next_count: %d\n", ftl->root_page, ftl->next_count);

    // If valid Root found, set gc_tail_page to position after Root
    // This avoids immediately recycling recently written data
    if (ftl->root_page != PAGE_NONE) {
        ftl->gc_tail_page = ftl->root_page + 1;
        if (ftl->gc_tail_page >= EFLASH_TOTAL_PAGES) {
            ftl->gc_tail_page = SYS_RESERVED_LPN_COUNT;
        }
        FTL_DEBUG("[GC] GC tail_page adjusted to %d (after root)\n", ftl->gc_tail_page);
    }

    return 0;
}

// --- Object Management Implementation (temporarily disabled, to be improved later) ---

int eflash_ftl_obj_create(eflash_ftl_t *ftl, uint16_t pkg_id, uint16_t class_id, uint8_t type) {
    // TODO: Re-implement object management logic
    (void)ftl; (void)pkg_id; (void)class_id; (void)type;
    return -1;
}

int eflash_ftl_obj_write_header(eflash_ftl_t *ftl, uint16_t obj_id, const obj_header_t *hdr) {
    // TODO: Re-implement object header write logic
    (void)ftl; (void)obj_id; (void)hdr;
    return -1;
}

int eflash_ftl_obj_read_header(eflash_ftl_t *ftl, uint16_t obj_id, obj_header_t *hdr) {
    // TODO: Re-implement object header read logic
    (void)ftl; (void)obj_id; (void)hdr;
    return -1;
}

int eflash_ftl_obj_write_body(eflash_ftl_t *ftl, uint16_t obj_id, const uint8_t *data, uint32_t size) {
    // TODO: Re-implement object data write logic
    (void)ftl; (void)obj_id; (void)data; (void)size;
    return -1;
}

int eflash_ftl_write(eflash_ftl_t *ftl, uint16_t sector_id, const uint8_t *data) {
    if (!ftl->is_initialized) return -1;

    // Before allocating space, check if GC needs to be triggered
    if (eflash_ftl_gc_trigger(ftl) != 0) {
        FTL_DEBUG("[WRITE] ERROR: GC trigger failed, no space available\n");
        return -1;
    }

    FTL_DEBUG("[WRITE] sector_id=%d\n", sector_id);

    uint16_t base_root = (ftl->active_txn_id != TXN_ID_NONE) ? ftl->shadow_root : ftl->root_page;

    // Step 1: Call trace_path to build metadata template for new node (excluding data page info)
    ftl_meta_t new_node_meta;
    if (trace_path(ftl, base_root, sector_id, &new_node_meta) != 0) {
        FTL_DEBUG("[WRITE] ERROR: trace_path failed!\n");
        return -1;
    }

    // Step 2: Allocate physical page using Head/Tail mechanism
    int new_phys = allocate_physical_page(ftl);
    if (new_phys < 0) {
        FTL_DEBUG("[WRITE] ERROR: Failed to allocate physical page!\n");
        return -1;
    }

    FTL_DEBUG("[WRITE] Allocated physical page=%d\n", new_phys);

    // Step 3: Write user data + metadata to same physical page
    // Note: In Dhara design, each physical page is both data page and metadata node page
    if (write_full_page(new_phys, data, &new_node_meta) != 0) {
        FTL_DEBUG("[WRITE] ERROR: Failed to write page!\n");
        return -1;
    }

    // Step 4: Update FTL root pointer
    if (ftl->active_txn_id != TXN_ID_NONE) {
        // Transaction mode: update shadow root
        ftl->shadow_root = new_phys;
    } else {
        // Non-transaction mode: update root directly
        ftl->root_page = new_phys;
    }

    FTL_DEBUG("[WRITE] Success: root_page=%d, next_count=%d\n",
              (ftl->active_txn_id != TXN_ID_NONE) ? ftl->shadow_root : ftl->root_page,
              ftl->next_count);

    return 0;
}

int eflash_ftl_read(eflash_ftl_t *ftl, uint16_t sector_id, uint8_t *data) {
    if (!ftl->is_initialized || ftl->root_page == PAGE_NONE) return -1;

    uint16_t lpn = sector_id;  // Logical Page Number

    FTL_DEBUG("[READ] LPN=%d, root_page=%d\n", lpn, ftl->root_page);

    uint8_t meta_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t cur_meta;
    int depth = 0;
    uint16_t current = ftl->root_page;

    while (depth < RADIX_DEPTH) {
        if (eflash_hw_read(current, meta_buf) != 0) {
            FTL_DEBUG("[READ] ERROR: Failed to read PPN %d at depth %d\n", current, depth);
            return -1;
        }

        if (verify_and_correct_page(meta_buf) != 0) {
            FTL_DEBUG("[READ] ERROR: Page verification failed at PPN %d\n", current);
            return -1;
        }
        memcpy(&cur_meta, meta_buf + META_OFFSET, META_SIZE);

        FTL_DEBUG("[READ] depth=%d, cur_sector=%d, alt[depth]=%d\n", depth, cur_meta.sector_id, cur_meta.alt[depth]);

        if (cur_meta.sector_id == lpn) {
            // Found matching node, data is in first USER_DATA_SIZE bytes of current page
            FTL_DEBUG("[READ] Found match at depth=%d, reading data from PPN %d\n", depth, current);
            // Data already in meta_buf and verified/corrected by verify_and_correct_page
            memcpy(data, meta_buf, USER_DATA_SIZE);
            FTL_DEBUG("[READ] Success, first byte=0x%02X\n", data[0]);
            return 0;
        }

        uint16_t bit_mask = 1 << (RADIX_DEPTH - 1 - depth);
        int target_bit = (lpn & bit_mask) ? 1 : 0;
        int current_bit = (cur_meta.sector_id & bit_mask) ? 1 : 0;

        if (target_bit != current_bit) {
            // Bits differ, need to jump
            current = cur_meta.alt[depth];
            if (current == PAGE_NONE) {
                FTL_DEBUG("[READ] ERROR: Path not found at depth=%d\n", depth);
                return -1;
            }
            // After jump, continue comparing same depth bit with new node
            // Note: Don't increment depth, continue comparing same depth bit in next loop
        } else {
            // Bits same, continue to next bit
            depth++;
        }
    }

    // Loop ended without finding, try reading last jumped-to node (if any)
    // This happens when jump occurred in last iteration
    if (current != PAGE_NONE && current != ftl->root_page) {
        if (eflash_hw_read(current, meta_buf) != 0) {
            FTL_DEBUG("[READ] ERROR: Failed to read final PPN %d\n", current);
            return -1;
        }

        if (verify_and_correct_page(meta_buf) != 0) {
            FTL_DEBUG("[READ] ERROR: Page verification failed at final PPN %d\n", current);
            return -1;
        }
        memcpy(&cur_meta, meta_buf + META_OFFSET, META_SIZE);

        if (cur_meta.sector_id == lpn) {
            FTL_DEBUG("[READ] Found match at final PPN %d, reading data\n", current);
            memcpy(data, meta_buf, USER_DATA_SIZE);
            FTL_DEBUG("[READ] Success, first byte=0x%02X\n", data[0]);
            return 0;
        }
    }

    // Really not found
    FTL_DEBUG("[READ] ERROR: Sector not found in tree (exceeded max depth)\n");
    return -1;
}

/**
 * eflash_ftl_write_logical: Write interface based on logical address
 * @ftl: FTL instance
 * @logical_addr: 24-bit logical address
 * @data: Data pointer
 * @return: 0 success, -1 failure
 *
 * Description: Extract sector_id from logical address, then call eflash_ftl_write
 */
int eflash_ftl_write_logical(eflash_ftl_t *ftl, uint32_t logical_addr, const uint8_t *data) {
    if (!ftl->is_initialized) return -1;

    // Extract sector_id from logical address
    uint16_t sector_id = (uint16_t)(logical_addr / EFLASH_PAGE_SIZE);

    FTL_DEBUG("[WRITE_LOGICAL] logical_addr=0x%06X -> sector_id=%d\n", logical_addr, sector_id);

    return eflash_ftl_write(ftl, sector_id, data);
}

/**
 * eflash_ftl_read_logical: Read interface based on logical address
 * @ftl: FTL instance
 * @logical_addr: 24-bit logical address
 * @data: Data output buffer
 * @return: 0 success, -1 failure
 *
 * Description: Extract sector_id from logical address, then call eflash_ftl_read
 */
int eflash_ftl_read_logical(eflash_ftl_t *ftl, uint32_t logical_addr, uint8_t *data) {
    if (!ftl->is_initialized) return -1;

    // Extract sector_id from logical address
    uint16_t sector_id = (uint16_t)(logical_addr / EFLASH_PAGE_SIZE);

    FTL_DEBUG("[READ_LOGICAL] logical_addr=0x%06X -> sector_id=%d\n", logical_addr, sector_id);

    return eflash_ftl_read(ftl, sector_id, data);
}

/**
 * eflash_ftl_txn_begin: Begin a new transaction
 * @ftl: FTL instance
 *
 * Description: 
 *   1. Trigger GC to ensure sufficient free space before transaction starts
 *   2. Set transaction ID and shadow root for atomic operations
 */
void eflash_ftl_txn_begin(eflash_ftl_t *ftl) {
    // Trigger GC before starting transaction to ensure enough free space
    // This prevents GC from being triggered during transaction, which could cause inconsistency
    if (eflash_ftl_gc_trigger(ftl) != 0) {
        FTL_DEBUG("[TXN_BEGIN] WARNING: GC trigger failed, but proceeding with transaction\n");
    }

    ftl->active_txn_id = (uint16_t)(ftl->next_count & 0xFFFF);
    ftl->shadow_root = ftl->root_page; // Shadow tree initially points to current stable root
    
    FTL_DEBUG("[TXN_BEGIN] Transaction started: txn_id=%d, shadow_root=%d\n", 
              ftl->active_txn_id, ftl->shadow_root);
}

/**
 * eflash_ftl_txn_commit: Commit transaction using full page erase/rewrite (compatible with all hardware)
 * @ftl: FTL instance
 * @return: 0 success, -1 failure
 *
 * Description: This is the universal version that works on all eFlash hardware.
 * It reads the entire page, modifies status, recalculates ECC, erases and rewrites.
 */
int eflash_ftl_txn_commit(eflash_ftl_t *ftl) {
    if (ftl->active_txn_id == TXN_ID_NONE || ftl->shadow_root == PAGE_NONE) return -1;

    FTL_DEBUG("[TXN_COMMIT] Committing transaction on page %d (full rewrite mode)\n", ftl->shadow_root);

    // Atomic commit: need to read entire page, modify status, recalculate ECC, then write back
    uint8_t full_page[EFLASH_PAGE_SIZE];

    // 1. Read current page
    if (eflash_hw_read(ftl->shadow_root, full_page) != 0) {
        FTL_DEBUG("[TXN_COMMIT] ERROR: Failed to read page\n");
        return -1;
    }

    // 2. Modify status to COMMITTED
    ftl_meta_t *meta = (ftl_meta_t *)(full_page + META_OFFSET);

    if (meta->status != TXN_STATUS_READY) {
        FTL_DEBUG("[TXN_COMMIT] ERROR: Page status is not READY (0x%02X)\n", meta->status);
        return -1;
    }

    meta->status = TXN_STATUS_COMMITTED;

    // 3. Recalculate entire page ECC (because metadata changed, and ECC covers user data + metadata)
    calc_page_ecc(full_page);

    // 4. Erase and rewrite entire page
    if (eflash_hw_erase(ftl->shadow_root) != 0) {
        FTL_DEBUG("[TXN_COMMIT] ERROR: Failed to erase page\n");
        return -1;
    }
    if (eflash_hw_prog(ftl->shadow_root, full_page) != 0) {
        FTL_DEBUG("[TXN_COMMIT] ERROR: Failed to program page\n");
        return -1;
    }

    FTL_DEBUG("[TXN_COMMIT] Transaction committed successfully\n");

    // Commit successful, shadow tree becomes main tree
    ftl->root_page = ftl->shadow_root;
    ftl->shadow_root = PAGE_NONE;
    ftl->active_txn_id = TXN_ID_NONE;
    return 0;
}

/**
 * eflash_ftl_txn_commit_with_update: Commit transaction using word update (optimized for hardware that supports it)
 * @ftl: FTL instance
 * @return: 0 success, -1 failure
 *
 * Description: This optimized version uses eflash_hw_word_update to modify only the status and ECC fields.
 * Status transition: READY (0xAD) -> COMMITTED (0x21) complies with Flash 1->0 programming rule.
 * After updating status, ECC must be recalculated and updated to maintain data integrity.
 * This avoids erasing and rewriting the entire page, extending Flash lifespan.
 *
 * Update strategy:
 *   - Read full page to buffer
 *   - Modify status in buffer (0xAD -> 0x21)
 *   - Recalculate ECC for the modified page
 *   - Use 3 word updates to write status(1B) + ecc(5B) = 6 bytes total
 *     * Update 1: status[0] + ecc[0] (2 bytes at offset META_OFFSET+42)
 *     * Update 2: ecc[1] + ecc[2] (2 bytes at offset META_OFFSET+44)
 *     * Update 3: ecc[3] + ecc[4] (2 bytes at offset META_OFFSET+46)
 *
 * Note: Only use this function if your eFlash hardware supports partial page updates!
 */
int eflash_ftl_txn_commit_with_update(eflash_ftl_t *ftl) {
    printf("[TXN_COMMIT_UPDATE] >>> ENTRY: active_txn_id=%d, shadow_root=%d\n",
           ftl->active_txn_id, ftl->shadow_root);

    if (ftl->active_txn_id == TXN_ID_NONE || ftl->shadow_root == PAGE_NONE) {
        printf("[TXN_COMMIT_UPDATE] ERROR: Invalid state - returning -1\n");
        return -1;
    }

    FTL_DEBUG("[TXN_COMMIT_UPDATE] Committing transaction on page %d (word update mode)\n", ftl->shadow_root);

    // Step 1: Read the entire page to get current data
    uint8_t page_buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(ftl->shadow_root, page_buf) != 0) {
        FTL_DEBUG("[TXN_COMMIT_UPDATE] ERROR: Failed to read page\n");
        return -1;
    }

    // Step 2: Verify current status is READY
    uint8_t *meta_ptr = page_buf + META_OFFSET;
    ftl_meta_t *meta = (ftl_meta_t *)meta_ptr;

    FTL_DEBUG("[TXN_COMMIT_UPDATE] Current status: 0x%02X\n", meta->status);

    if (meta->status != TXN_STATUS_READY) {
        FTL_DEBUG("[TXN_COMMIT_UPDATE] ERROR: Page status is not READY (0x%02X)\n", meta->status);
        return -1;
    }

    // Step 3: Modify status in buffer from READY (0xAD) to COMMITTED (0x21)
    meta->status = TXN_STATUS_COMMITTED;
    FTL_DEBUG("[TXN_COMMIT_UPDATE] Status changed to COMMITTED (0x%02X) in buffer\n", meta->status);

    // Step 4: Recalculate ECC for the modified page
    // ECC protects: user data + metadata (excluding ECC field itself)
    calc_page_ecc(page_buf);
    FTL_DEBUG("[TXN_COMMIT_UPDATE] ECC recalculated successfully\n");

    // Step 5: Update status and ECC fields using word updates
    // Layout in page:
    //   Offset 506: status (1 byte at META_OFFSET+42)
    //   Offset 507-511: ecc (5 bytes at META_OFFSET+43 to META_OFFSET+47)
    // We need to update 6 bytes total, using 3 word (2-byte) updates
    // Note: Using big-endian byte order (high byte first, low byte second)

    uint16_t status_offset = META_OFFSET + offsetof(ftl_meta_t, status);  // 42
    uint16_t ecc_offset = META_OFFSET + offsetof(ftl_meta_t, ecc);        // 43

    // Update 1: status[0] + ecc[0] (bytes at offset 506-507)
    // Big-endian: high byte = page_buf[506], low byte = page_buf[507]
    uint16_t word1 = ((uint16_t)page_buf[status_offset] << 8) | page_buf[status_offset + 1];
    FTL_DEBUG("[TXN_COMMIT_UPDATE] Word1: offset=%d, buf[%d]=0x%02X(high), buf[%d]=0x%02X(low) -> 0x%04X\n",
              status_offset, status_offset, page_buf[status_offset], status_offset+1, page_buf[status_offset+1], word1);
    if (eflash_hw_word_update(ftl->shadow_root, status_offset, word1) != 0) {
        FTL_DEBUG("[TXN_COMMIT_UPDATE] ERROR: Word update 1 failed\n");
        return -1;
    }

    // Update 2: ecc[1] + ecc[2] (bytes at offset 508-509)
    uint16_t word2 = ((uint16_t)page_buf[ecc_offset + 1] << 8) | page_buf[ecc_offset + 2];
    FTL_DEBUG("[TXN_COMMIT_UPDATE] Word2: offset=%d, buf[%d]=0x%02X(high), buf[%d]=0x%02X(low) -> 0x%04X\n",
              ecc_offset+1, ecc_offset+1, page_buf[ecc_offset+1], ecc_offset+2, page_buf[ecc_offset+2], word2);
    if (eflash_hw_word_update(ftl->shadow_root, ecc_offset + 1, word2) != 0) {
        FTL_DEBUG("[TXN_COMMIT_UPDATE] ERROR: Word update 2 failed\n");
        return -1;
    }

    // Update 3: ecc[3] + ecc[4] (bytes at offset 510-511)
    uint16_t word3 = ((uint16_t)page_buf[ecc_offset + 3] << 8) | page_buf[ecc_offset + 4];
    FTL_DEBUG("[TXN_COMMIT_UPDATE] Word3: offset=%d, buf[%d]=0x%02X(high), buf[%d]=0x%02X(low) -> 0x%04X\n",
              ecc_offset+3, ecc_offset+3, page_buf[ecc_offset+3], ecc_offset+4, page_buf[ecc_offset+4], word3);
    if (eflash_hw_word_update(ftl->shadow_root, ecc_offset + 3, word3) != 0) {
        FTL_DEBUG("[TXN_COMMIT_UPDATE] ERROR: Word update 3 failed\n");
        return -1;
    }

    FTL_DEBUG("[TXN_COMMIT_UPDATE] Status and ECC updated successfully (3 word updates, no erase needed)\n");

    // Step 6: Commit successful, shadow tree becomes main tree
    ftl->root_page = ftl->shadow_root;
    ftl->shadow_root = PAGE_NONE;
    ftl->active_txn_id = TXN_ID_NONE;

    FTL_DEBUG("[TXN_COMMIT_UPDATE] Transaction committed, root_page=%d\n", ftl->root_page);
    return 0;
}

void eflash_ftl_txn_abort(eflash_ftl_t *ftl) {
    if (ftl->active_txn_id == TXN_ID_NONE) return;

    // Simply discard shadow tree pointer, PENDING pages in Flash will be ignored on next GC or reboot
    ftl->shadow_root = PAGE_NONE;
    ftl->active_txn_id = TXN_ID_NONE;
}

// --- GC Core Implementation (following Dhara Head/Tail model) ---

/**
 * eflash_ftl_get_free_pages: Get current number of free pages
 */
uint32_t eflash_ftl_get_free_pages(eflash_ftl_t *ftl) {
    // Based on Dhara's Head/Tail circular buffer model
    // Head: Next writable physical page
    // Tail: Starting position for GC scan
    //
    // Free space = Distance from Head to Tail (clockwise direction)

    uint16_t first_user_page = FREE_NODE_PAGE_COUNT + BASE_HEADER_PAGES;
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;
    (void)(last_user_page - first_user_page + 1);  // Suppress unused variable warning

    if (ftl->gc_head_page >= ftl->gc_tail_page) {
        // Case 1: Head is after or equal to Tail
        // [first...Tail...Head...last]
        // Free space = (last - Head + 1) + (Tail - first)
        uint32_t free = (uint32_t)(last_user_page - ftl->gc_head_page + 1) +
                        (uint32_t)(ftl->gc_tail_page - first_user_page);

        FTL_DEBUG("[FREE_PAGES] Case 1: head=%d, tail=%d, free=%u\n",
                 ftl->gc_head_page, ftl->gc_tail_page, free);
        return free;
    } else {
        // Case 2: Head has wrapped around to start, before Tail
        // [first...Head...Tail...last]
        // Free space = Tail - Head
        uint32_t free = (uint32_t)(ftl->gc_tail_page - ftl->gc_head_page);

        FTL_DEBUG("[FREE_PAGES] Case 2: head=%d, tail=%d, free=%u\n",
                 ftl->gc_head_page, ftl->gc_tail_page, free);
        return free;
    }
}

/**
 * is_page_still_valid: Check if physical page is still referenced by Radix Tree
 * @ftl: FTL instance
 * @phys_page: Physical page number to check
 * @return: true=valid (still in tree), false=invalid (can be reclaimed)
 *
 * Principle: Read sector_id of the page, then search current mapping via eflash_ftl_read.
 * If current mapping points to same physical page as phys_page, it's valid data.
 */
static bool is_page_still_valid(eflash_ftl_t *ftl, uint16_t phys_page) {
    (void)ftl;  // Suppress unused parameter warning
    uint8_t meta_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t meta;

    // Read page metadata
    if (eflash_hw_read(phys_page, meta_buf) != 0) {
        FTL_DEBUG("[GC_VALID] Page %d: read failed\n", phys_page);
        return false; // Read failed, treat as invalid
    }

    // Verify ECC
    int ecc_result = verify_and_correct_page(meta_buf);
    if (ecc_result != 0) {
        FTL_DEBUG("[GC_VALID] Page %d: ECC verification failed (result=%d)\n", phys_page, ecc_result);
        return false; // ECC verification failed, treat as invalid
    }

    memcpy(&meta, meta_buf + META_OFFSET, META_SIZE);

    FTL_DEBUG("[GC_VALID] Page %d: sector_id=%d, status=0x%02X, count=%d\n",
             phys_page, meta.sector_id, meta.status, meta.global_count);

    // Check status: must be COMMITTED or READY to be potentially valid data
    if (meta.status != TXN_STATUS_COMMITTED && meta.status != TXN_STATUS_READY) {
        FTL_DEBUG("[GC_VALID] Page %d: invalid status 0x%02X\n", phys_page, meta.status);
        return false;
    }

    // Conservative strategy: COMMITTED/READY pages with valid ECC are treated as valid
    FTL_DEBUG("[GC_VALID] Page %d: VALID\n", phys_page);
    return true;
}

/**
 * gc_migrate_page: Migrate valid page to new location
 * @ftl: FTL instance
 * @src_page: Source physical page number
 * @return: 0 success, -1 failure
 */
static int gc_migrate_page(eflash_ftl_t *ftl, uint16_t src_page) {
    uint8_t full_page[EFLASH_PAGE_SIZE];
    ftl_meta_t *meta;

    // 1. Read complete data from source page
    if (eflash_hw_read(src_page, full_page) != 0) {
        FTL_DEBUG("[GC_MIGRATE] ERROR: Failed to read source page %d\n", src_page);
        return -1;
    }

    meta = (ftl_meta_t *)(full_page + META_OFFSET);

    FTL_DEBUG("[GC_MIGRATE] Migrating sector %d from physical page %d (count=%d)\n",
              meta->sector_id, src_page, meta->global_count);

    // 2. Extract user data portion (first 464 bytes)
    uint8_t user_data[USER_DATA_SIZE];
    memcpy(user_data, full_page, USER_DATA_SIZE);

    // [DEBUG] Record state before migration
    uint32_t free_before = eflash_ftl_get_free_pages(ftl);
    FTL_DEBUG("[GC_MIGRATE] Before migration: head=%d, tail=%d, free=%d\n",
             ftl->gc_head_page, ftl->gc_tail_page, free_before);

    // 3. Use normal write flow to rewrite data (this triggers trace_path, allocates new page, updates root)
    //    Note: Only pass user data, let eflash_ftl_write automatically create new metadata
    if (eflash_ftl_write(ftl, meta->sector_id, user_data) != 0) {
        FTL_DEBUG("[GC_MIGRATE] ERROR: Failed to rewrite data during migration\n");
        return -1;
    }

    // [DEBUG] Record state after migration
    uint32_t free_after = eflash_ftl_get_free_pages(ftl);
    FTL_DEBUG("[GC_MIGRATE] After migration: head=%d, tail=%d, free=%d (delta=%d)\n",
             ftl->gc_head_page, ftl->gc_tail_page, free_after, (int32_t)(free_after - free_before));

    FTL_DEBUG("[GC_MIGRATE] Success: migrated sector %d from page %d\n",
             meta->sector_id, src_page);
    return 0;
}

/**
 * gc_collect_one_page: Collect (reclaim) a single physical page during GC
 * @ftl: FTL instance
 * @ppn: Physical Page Number to collect
 * @return: 0 on success, -1 on failure
 *
 * Description:
 *   1. Check if the page is still valid (not stale)
 *   2. If valid, migrate data to a new physical page
 *   3. Erase physical page (do NOT call space_mgr_free, because GC only manages physical pages)
 */
static int gc_collect_one_page(eflash_ftl_t *ftl, uint16_t ppn) {
    FTL_DEBUG("[GC_COLLECT] Processing physical page %d...\n", ppn);

    // 1. Check if page is still valid
    bool valid = is_page_still_valid(ftl, ppn);

    if (valid) {
        FTL_DEBUG("[GC_COLLECT] Page %d is VALID, migrating...\n", ppn);

        // 2. Migrate valid page
        if (gc_migrate_page(ftl, ppn) != 0) {
            FTL_DEBUG("[GC_COLLECT] ERROR: Migration failed for page %d\n", ppn);
            return -1;
        }

        FTL_DEBUG("[GC_COLLECT] Page %d migrated successfully\n", ppn);
    } else {
        FTL_DEBUG("[GC_COLLECT] Page %d is STALE, will be erased\n", ppn);
    }

    // 3. Erase physical page (Note: do NOT call space_mgr_free!)
    //    Space Manager manages logical address space, unrelated to physical pages
    //    Physical page reclamation is done by GC direct erasure
    if (eflash_hw_erase(ppn) != 0) {
        FTL_DEBUG("[GC_COLLECT] ERROR: Failed to erase physical page %d\n", ppn);
        return -1;
    }

    FTL_DEBUG("[GC_COLLECT] Erased physical page %d\n", ppn);
    return 0;
}

/**
 * eflash_ftl_gc_collect: Perform GC to reclaim specified number of pages
 * @ftl: FTL instance
 * @pages_to_free: Number of pages to reclaim
 * @return: Actual pages freed, -1 on failure
 *
 * Algorithm (strictly following Dhara):
 * 1. Start scanning from gc_tail_page page by page
 * 2. Call gc_collect_one_page for each page
 * 3. **Only move tail pointer after successful reclamation** (following Dhara's dequeue)
 * 4. Continue until enough pages freed or entire Flash traversed
 */
int eflash_ftl_gc_collect(eflash_ftl_t *ftl, uint16_t pages_to_free) {
    if (!ftl->is_initialized) {
        FTL_DEBUG("[GC] ERROR: FTL not initialized\n");
        return -1;
    }

    FTL_DEBUG("[GC] ========== Starting collection ==========\n");
    FTL_DEBUG("[GC] Need to free %d pages\n", pages_to_free);
    FTL_DEBUG("[GC] Initial state: head=%d, tail=%d, free=%d\n",
              ftl->gc_head_page, ftl->gc_tail_page, eflash_ftl_get_free_pages(ftl));

    uint16_t pages_freed = 0;
    uint16_t start_tail = ftl->gc_tail_page;
    uint16_t first_user_page = FREE_NODE_PAGE_COUNT + BASE_HEADER_PAGES;
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;

    // Traverse entire user area at most twice to prevent infinite loop
    uint16_t max_iterations = (last_user_page - first_user_page + 1) * 2;
    uint16_t iterations = 0;

    while (pages_freed < pages_to_free && iterations < max_iterations) {
        uint16_t current_page = ftl->gc_tail_page;

        // [DEBUG] Output status every 10 iterations
        if (iterations % 10 == 0) {
            FTL_DEBUG("[GC] Iteration %d: processing page %d, freed=%d, head=%d, tail=%d, free=%d\n",
                     iterations, current_page, pages_freed, ftl->gc_head_page, ftl->gc_tail_page,
                     eflash_ftl_get_free_pages(ftl));
        }

        // Skip system reserved area
        if (current_page < first_user_page) {
            ftl->gc_tail_page = first_user_page;
            FTL_DEBUG("[GC] Skipping system reserved pages, jumping to %d\n", first_user_page);
            continue;
        }

        // Execute single page reclamation
        int ret = gc_collect_one_page(ftl, current_page);

        if (ret == 0) {
            // ✅ Successfully reclaimed, move tail pointer (following Dhara's dequeue)
            pages_freed++;

            ftl->gc_tail_page++;
            if (ftl->gc_tail_page > last_user_page) {
                ftl->gc_tail_page = first_user_page;
                FTL_DEBUG("[GC] Round-wrap: tail_page reset to %d\n", first_user_page);
            }
        } else {
            // ❌ Reclamation failed, but still move tail to avoid infinite loop
            FTL_DEBUG("[GC] WARNING: Failed to collect page %d, skipping\n", current_page);

            ftl->gc_tail_page++;
            if (ftl->gc_tail_page > last_user_page) {
                ftl->gc_tail_page = first_user_page;
                FTL_DEBUG("[GC] Round-wrap on error: tail_page reset to %d\n", first_user_page);
            }
        }

        iterations++;
    }

    if (iterations >= max_iterations) {
        FTL_DEBUG("[GC] WARNING: Reached max iterations (%d), stopping\n", max_iterations);
    }

    FTL_DEBUG("[GC] ========== Collection complete ==========\n");
    FTL_DEBUG("[GC] Final state: freed %d pages (tail: %d -> %d, iterations=%d)\n",
              pages_freed, start_tail, ftl->gc_tail_page, iterations);
    FTL_DEBUG("[GC] Final free pages: %d\n", eflash_ftl_get_free_pages(ftl));

    return pages_freed;
}

/**
 * eflash_ftl_gc_trigger: Manually trigger garbage collection
 * @ftl: FTL instance
 * @return: 0 no GC needed or GC successful, -1 GC failed
 *
 * Trigger condition: free pages < gc_threshold
 * 
 * IMPORTANT: GC is NOT allowed during transaction to maintain atomicity.
 * If a transaction is active, this function returns immediately without triggering GC.
 */
int eflash_ftl_gc_trigger(eflash_ftl_t *ftl) {
    // If GC already in progress, skip trigger (prevent recursion)
    if (ftl->gc_in_progress) {
        return 0;
    }

    // CRITICAL: Do NOT trigger GC during transaction
    // Transaction requires atomic operations, and GC could interfere with shadow tree consistency
    if (ftl->active_txn_id != TXN_ID_NONE) {
        FTL_DEBUG("[GC_TRIGGER] SKIPPED: GC not allowed during transaction (txn_id=%d)\n", 
                  ftl->active_txn_id);
        return 0;
    }

    uint32_t free_pages = eflash_ftl_get_free_pages(ftl);

    FTL_DEBUG("[GC_TRIGGER] Checking GC: free_pages=%d, threshold=%d\n",
              free_pages, ftl->gc_threshold);

    if (free_pages >= ftl->gc_threshold) {
        // Sufficient space, no GC needed
        return 0;
    }

    FTL_DEBUG("[GC_TRIGGER] Triggering GC! Free space below threshold\n");

    // Calculate pages to reclaim (target: restore to 50% free space)
    uint32_t target_free_pages = ftl->total_user_pages / 2;
    uint16_t pages_needed = (target_free_pages > free_pages) ?
                            (uint16_t)(target_free_pages - free_pages) : 10;

    // Set GC flag
    ftl->gc_in_progress = true;

    // Execute GC
    int result = eflash_ftl_gc_collect(ftl, pages_needed);

    // Clear GC flag
    ftl->gc_in_progress = false;

    if (result < 0) {
        FTL_DEBUG("[GC_TRIGGER] ERROR: GC failed!\n");
        return -1;
    }

    FTL_DEBUG("[GC_TRIGGER] GC completed successfully, freed %d pages\n", result);
    return 0;
}
