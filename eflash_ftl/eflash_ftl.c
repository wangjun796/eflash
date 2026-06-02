#include "eflash_ftl.h"
#include "eflash_sim.h"
#include "eflash_mgr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// --- Forward Declarations ---
static int extend_headers(void);
static void scan_and_rebuild_ext_headers();
static uint16_t adjust_ppn_for_code_region(uint16_t ppn);
static void adjust_gc_pointers_for_code_region(void);
static uint16_t skip_code_region(uint16_t ppn);

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

// --- Public API: Instance Management ---

// Global static FTL instance (no dynamic allocation - suitable for embedded systems)
eflash_ftl_t g_ftl_instance;
static bool g_ftl_initialized = false;
ftl_page_t *g_ftl_page_ptr = (ftl_page_t*)FLASH_FILE_REMAP_ADDR;

// --- Read/Write Cache (Global) ---
static map_cache_entry_t g_map_cache[MAP_CACHE_SIZE];
static uint8_t          g_map_cache_idx = 0;

static page_cache_slot_t g_page_cache[PAGE_CACHE_SLOTS];
static uint32_t          g_cache_seq_counter = 0;
static uint8_t           g_dirty_count = 0;

/**
 * eflash_get_ftl: Get the global FTL instance
 */
eflash_ftl_t* eflash_get_ftl(void) {
    // Initialize on first call if not already initialized
    if (!g_ftl_initialized) {
        memset(&g_ftl_instance, 0, sizeof(eflash_ftl_t));
        g_ftl_instance.max_obj_id = 0xFFFF;  // Uninitialized marker
        g_ftl_instance.root_page = PAGE_NONE;
        g_ftl_instance.shadow_root = PAGE_NONE;
        g_ftl_initialized = true;
    }
    return &g_ftl_instance;
}

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
#define FTL_DEBUG_ENABLE 1  // Enabled for debugging delete operations
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

    // FTL_DEBUG("[ECC] Verifying page: protected_len=%zu, ecc at offset %zu\n",
    //           protected_len, (size_t)(ecc_ptr - page_buf));

    // Create copy for correction
    uint8_t data_copy[EFLASH_PAGE_SIZE];
    memcpy(data_copy, page_buf, EFLASH_PAGE_SIZE);

    // First verify
    int verify_result = bch_verify(bch_cfg, data_copy, protected_len, ecc_ptr);
    // FTL_DEBUG("[ECC] Verify result: %d (0=ok, >0=errors, <1=uncorrectable)\n", verify_result);

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

// --- Forward Declarations ---
static bool is_page_still_valid(uint16_t ppn);

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

// --- Forward Declarations for Internal Functions ---
static int get_header_page_info(uint16_t obj_id, uint16_t *out_log_page, uint16_t *out_offset);
int eflash_ftl_write_through(uint16_t sector_id, const uint8_t *data);
static void content_cache_flush(void);

// --- System Page Read/Write Functions ---

/**
 * write_system_page: Write a system page through FTL layer
 * @FTL: FTL instance
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
int write_system_page(uint16_t lpn, const uint8_t *data) {
    FTL_DEBUG("[SYS_WRITE] Writing system page LPN=%d (sector_id=%d)\n", lpn, lpn);
    return eflash_ftl_write_through(lpn, data);
}

/**
 * read_system_page: Read a system page through FTL layer
 * @FTL: FTL instance
 * @lpn: Logical Page Number
 * @data: Output buffer for user data (USER_DATA_SIZE bytes)
 * @return: 0 on success, -1 on failure
 */
static int read_system_page(uint16_t lpn, uint8_t *data) {
    FTL_DEBUG("[SYS_READ] Reading system page LPN=%d (sector_id=%d)\n", lpn, lpn);
    return eflash_ftl_read(lpn, data);
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

// ========================================================================
// Cache Management Functions
// ========================================================================

static void map_cache_upsert(uint16_t lpn, uint16_t ppn) {
    for (int i = 0; i < MAP_CACHE_SIZE; i++) {
        if (g_map_cache[i].lpn == lpn) {
            g_map_cache[i].ppn = ppn;
            return;
        }
    }
    g_map_cache[g_map_cache_idx].lpn = lpn;
    g_map_cache[g_map_cache_idx].ppn = ppn;
    g_map_cache_idx = (g_map_cache_idx + 1) % MAP_CACHE_SIZE;
}

static uint16_t map_cache_lookup(uint16_t lpn) {
    for (int i = 0; i < MAP_CACHE_SIZE; i++) {
        if (g_map_cache[i].lpn == lpn && g_map_cache[i].ppn != PAGE_NONE) {
            return g_map_cache[i].ppn;
        }
    }
    return PAGE_NONE;
}

static void map_cache_invalidate(uint16_t lpn) {
    for (int i = 0; i < MAP_CACHE_SIZE; i++) {
        if (g_map_cache[i].lpn == lpn) {
            g_map_cache[i].lpn = PAGE_NONE;
            g_map_cache[i].ppn = PAGE_NONE;
        }
    }
}

#if EFLASH_CACHE_ENABLE
static int content_cache_find(uint16_t lpn) {
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (g_page_cache[i].valid && g_page_cache[i].lpn == lpn) {
            return i;
        }
    }
    return -1;
}

static int content_cache_find_free(void) {
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (!g_page_cache[i].valid) return i;
    }
    return -1;
}

static int content_cache_find_min_seq(void) {
    int min_slot = -1;
    uint32_t min_seq = 0xFFFFFFFF;
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (g_page_cache[i].valid && g_page_cache[i].cache_seq < min_seq) {
            min_seq = g_page_cache[i].cache_seq;
            min_slot = i;
        }
    }
    return min_slot;
}

static void content_cache_evict_one(int slot) {
    if (!g_page_cache[slot].valid) return;
    if (g_page_cache[slot].dirty) {
        eflash_ftl_write_through(g_page_cache[slot].lpn,
                                 g_page_cache[slot].data);
        g_dirty_count--;
    }
    g_page_cache[slot].valid = 0;
    g_page_cache[slot].lpn = PAGE_NONE;
}

static void content_cache_fill(uint16_t lpn, const uint8_t *data) {
    int slot = content_cache_find(lpn);
    if (slot < 0) {
        slot = content_cache_find_free();
        if (slot < 0) {
            slot = content_cache_find_min_seq();
            content_cache_evict_one(slot);
        }
        g_page_cache[slot].lpn  = lpn;
        g_page_cache[slot].valid = 1;
    }
    g_page_cache[slot].dirty     = 0;
    g_page_cache[slot].cache_seq = ++g_cache_seq_counter;
    memcpy(g_page_cache[slot].data, data, USER_DATA_SIZE);
}

static void content_cache_invalidate(uint16_t lpn) {
    int slot = content_cache_find(lpn);
    if (slot >= 0) {
        if (g_page_cache[slot].dirty) {
            g_dirty_count--;
        }
        g_page_cache[slot].valid = 0;
        g_page_cache[slot].lpn = PAGE_NONE;
    }
}

static void content_cache_flush(void) {
    int indices[PAGE_CACHE_SLOTS];
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) indices[i] = i;

    for (int i = 0; i < PAGE_CACHE_SLOTS - 1; i++) {
        for (int j = i + 1; j < PAGE_CACHE_SLOTS; j++) {
            if (g_page_cache[indices[i]].cache_seq >
                g_page_cache[indices[j]].cache_seq) {
                int tmp = indices[i];
                indices[i] = indices[j];
                indices[j] = tmp;
            }
        }
    }

    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        int slot = indices[i];
        if (g_page_cache[slot].valid && g_page_cache[slot].dirty) {
            eflash_ftl_write_through(g_page_cache[slot].lpn,
                                     g_page_cache[slot].data);
        }
    }
}
#else
static int content_cache_find(uint16_t lpn) { (void)lpn; return -1; }
static int content_cache_find_free(void) { return -1; }
static int content_cache_find_min_seq(void) { return -1; }
static void content_cache_evict_one(int slot) { (void)slot; }
static void content_cache_fill(uint16_t lpn, const uint8_t *data) { (void)lpn; (void)data; }
static void content_cache_invalidate(uint16_t lpn) { (void)lpn; }
static void content_cache_flush(void) { }
#endif

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
 * @FTL: FTL instance
 * @return: Physical page index, -1 on failure
 *
 * Description: Allocate physical pages sequentially starting from Head
 * Note: GC should be triggered by the caller BEFORE calling this function.
 *       This function only handles head wraparound without triggering GC.
 */
static int allocate_physical_page(void) {
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;

    if (FTL->gc_head_page > last_user_page) {
        FTL->gc_head_page = 0;
        FTL_DEBUG("[ALLOC_PHYS] WARNING: Head wrapped to 0 (GC should have been triggered by caller)\n");
    }

    if (FTL->gc_head_page == FTL->gc_tail_page && FTL->valid_page_count > 0) {
        if (is_page_still_valid(FTL->gc_head_page)) {
            FTL_DEBUG("[ALLOC_PHYS] === ERROR: Flash is full! ===\n");
            FTL_DEBUG("[ALLOC_PHYS]   head_page    = %d (VALID)\n", FTL->gc_head_page);
            FTL_DEBUG("[ALLOC_PHYS]   tail_page    = %d\n", FTL->gc_tail_page);
            FTL_DEBUG("[ALLOC_PHYS]   next_count   = %u\n", FTL->next_count);
            FTL_DEBUG("[ALLOC_PHYS]   valid_pages  = %u\n", FTL->valid_page_count);
            FTL_DEBUG("[ALLOC_PHYS]   total_pages  = %u\n", FTL->total_user_pages);
            FTL_DEBUG("[ALLOC_PHYS]   free(est)    = %u\n", eflash_ftl_get_free_pages());
            FTL_DEBUG("[ALLOC_PHYS]   free(real)   = %u\n", eflash_ftl_get_real_free_pages());
            FTL_DEBUG("[ALLOC_PHYS]   gc_in_progress = %d\n", FTL->gc_in_progress);
            FTL_DEBUG("[ALLOC_PHYS]   Returning -1 to caller\n");
            return -1;
        }
    }

    uint16_t ppn = FTL->gc_head_page;
    FTL->gc_head_page++;

    FTL_DEBUG("[ALLOC_PHYS] Allocated ppn=%d, next_head=%d, tail=%d, free(est)=%u, free(real)=%u\n",
             ppn, FTL->gc_head_page, FTL->gc_tail_page,
             eflash_ftl_get_free_pages(), eflash_ftl_get_real_free_pages());

    return ppn;
}

// --- Radix Tree Core Logic ---

static inline int get_bit(uint16_t sector, int depth) {
    // Safety check: ensure depth is within valid range
    if (depth < 0 || depth >= RADIX_DEPTH) {
        return -1; // Return error code for invalid depth
    }
    return (sector >> (RADIX_DEPTH - 1 - depth)) & 1;
}

// Corrected trace_tree: remove new_phys parameter
// Responsibility: Only build metadata template for new node, do not insert any physical page
// Returns: 
//   >0 = Update write: returns the physical page number of the found node
//   0  = New write: sector_id not found in tree
//  <0 = Error
static int trace_tree(uint16_t base_root, uint16_t sector, ftl_meta_t *out_meta) {
    uint8_t meta_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t cur_meta;
    int depth = 0;
    uint16_t current = base_root;

    FTL_DEBUG("[TRACE] sector=%d, base_root=%d\n", sector, base_root);

    // Initialize new node's metadata (adr array initialized to all 0xFF/PAGE_NONE)
    memset(out_meta, 0xFF, sizeof(ftl_meta_t));
    out_meta->sector_id = sector;
    out_meta->global_count = FTL->next_count++;
    out_meta->epoch = FTL->current_epoch;
    out_meta->txn_id = FTL->active_txn_id;

    // Set status: READY in transaction mode, COMMITTED directly in non-transaction mode
    if (FTL->active_txn_id != TXN_ID_NONE) {
        out_meta->status = TXN_STATUS_READY;
    } else {
        out_meta->status = TXN_STATUS_COMMITTED;  // Non-transaction mode commits directly
    }

    // If tree is empty, return directly (adr array already initialized to all PAGE_NONE)
    if (current == PAGE_NONE) {
        FTL_DEBUG("[TRACE] Empty tree\n");
        return 0;  // New write
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

        FTL_DEBUG("[TRACE] depth=%d, target_bit=%d, current_bit=%d, adr[%d]=%d\n",
                  depth, target_bit, current_bit, depth, cur_meta.adr[depth]);

        if (target_bit != current_bit) {
            // Bits differ: divergence occurs
            // Save current physical page as new node's adr pointer (record divergence point)
            out_meta->adr[depth] = current;
            FTL_DEBUG("[TRACE] Diverge: out_meta->adr[%d]=%d\n", depth, current);

            // Get current node's adr pointer, continue searching downward
            uint16_t next_ppn = cur_meta.adr[depth];  // Next Physical Page Number
            if (next_ppn == PAGE_NONE) {
                // Path interrupted, jump to not_found to handle remaining depth
                FTL_DEBUG("[TRACE] Path interrupted at depth=%d\n", depth);
                depth++;
                goto not_found;
            }

            FTL_DEBUG("[TRACE] Follow adr[%d]=%d\n", depth, next_ppn);

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
            // Bits same: inherit adr pointer from current node
            out_meta->adr[depth] = cur_meta.adr[depth];
            FTL_DEBUG("[TRACE] Same bit, inherit adr[%d]=%d\n", depth, out_meta->adr[depth]);
        }

        depth++;
    }

    // Loop ends normally, meaning matching sector found (or traversed all depths)
    // new_meta's adr array already fully set during traversal through Diverge and Same bit branches
    FTL_DEBUG("[TRACE] Found match after full traversal at page %d (UPDATE WRITE)\n", current);
    if (current != 0){
        return current;  // Update write: return the physical page number of found node
    } else{
        return EFLASH_TOTAL_PAGES;  // Update write: return the physical page number of found node
    }
    

not_found:
    // Set all adr pointers from current depth to NONE
    // Note: depth++ already executed before goto not_found, so start setting from current depth
    FTL_DEBUG("[TRACE] Not found (NEW WRITE), setting remaining adr pointers to NONE from depth=%d\n", depth);
    while (depth < RADIX_DEPTH) {
        out_meta->adr[depth] = PAGE_NONE;
        depth++;
    }

    return 0;  // New write
}

// --- Object Header Management Core Logic ---

/**
 * eflash_ftl_obj_alloc_header: Allocate next object header ID (sequential allocation)
 * @FTL: FTL instance
 * @return: Next available obj_id, or 0xFFFF on failure
 *
 * Description:
 *   - Automatically handles extension when reaching capacity boundaries
 *   - Ensures sequential allocation (no gaps in obj_id sequence)
 *   - Returns max_obj_id + 1 after incrementing
 */
uint16_t eflash_ftl_obj_alloc_header(void) {
    if (!FTL) return PAGE_NONE;

    // Calculate next obj_id
    // If max_obj_id is 0xFFFF (uninitialized), start from 0
    uint16_t next_id = (FTL->max_obj_id == 0xFFFF) ? 0 : (FTL->max_obj_id + 1);

    // Skip LINK object positions
    // LINK objects are placed at:
    //   - Base area: obj_id = BASE_HEADER_CAPACITY - 1 (231)
    //   - Extension level N: obj_id = BASE_HEADER_CAPACITY + N * EXT_HEADER_CAPACITY - 1
    bool is_link_position = false;
    
    // Check if next_id is at a LINK position
    if (next_id == BASE_HEADER_CAPACITY - 1) {
        // This is the LINK position at end of base area
        is_link_position = true;
        FTL_DEBUG("[ALLOC_HDR] Skipping LINK position at obj_id=%d\n", next_id);
    } else if (next_id > BASE_HEADER_CAPACITY) {
        // Check if this is at the end of an extension level
        uint16_t ext_idx = next_id - BASE_HEADER_CAPACITY;
        // LINK positions are at: EXT_HEADER_CAPACITY - 1, 2*EXT_HEADER_CAPACITY - 1, etc.
        if ((ext_idx + 1) % EXT_HEADER_CAPACITY == 0) {
            is_link_position = true;
            FTL_DEBUG("[ALLOC_HDR] Skipping LINK position at obj_id=%d (ext_idx=%d)\n", next_id, ext_idx);
        }
    }
    
    // Skip to next available position if this is a LINK slot
    if (is_link_position) {
        next_id++;
    }

    // Check if we need to extend
    if (next_id == BASE_HEADER_CAPACITY) {
        // First extension: obj_id = 232
        FTL_DEBUG("[ALLOC_HDR] Triggering first extension at obj_id=%d\n", next_id);
        if (extend_headers() != 0) {
            FTL_DEBUG("[ALLOC_HDR] ERROR: First extension failed\n");
            return PAGE_NONE;
        }
    } else if (next_id > BASE_HEADER_CAPACITY) {
        // Check if we need next level extension
        uint16_t ext_idx = next_id - BASE_HEADER_CAPACITY;
        uint16_t level = (ext_idx / EXT_HEADER_CAPACITY) + 1;
        
        // If this is the first obj_id of a new extension level
        if (ext_idx % EXT_HEADER_CAPACITY == 0) {
            FTL_DEBUG("[ALLOC_HDR] Triggering extension level %d at obj_id=%d\n", level, next_id);
            if (extend_headers() != 0) {
                FTL_DEBUG("[ALLOC_HDR] ERROR: Extension level %d failed\n", level);
                return PAGE_NONE;
            }
        }
    }

    // Update and return
    FTL->max_obj_id = next_id;
    FTL_DEBUG("[ALLOC_HDR] Allocated obj_id=%d (max_obj_id=%d)\n", next_id, FTL->max_obj_id);
    return next_id;
}

/**
 * eflash_ftl_obj_get_header: Read object header by obj_id
 * @FTL: FTL instance
 * @obj_id: Object ID (0-2087)
 * @hdr: Output buffer for object header
 * @return: 0 on success, -1 on failure
 */
int eflash_ftl_obj_get_header(uint16_t obj_id, obj_header_t *hdr) {
    // Parameter validation
    if (!FTL || !FTL->is_initialized) return -1;
    if (hdr == NULL) return -1;
    if (obj_id == PAGE_NONE) return -1;  // Invalid object ID

    // Boundary check: obj_id must be <= max_obj_id (allocated)
    if (obj_id > FTL->max_obj_id) {
        FTL_DEBUG("[OBJ_GET] ERROR: obj_id=%d exceeds max_obj_id=%d (not allocated)\n", 
                  obj_id, FTL->max_obj_id);
        return -1;
    }

    uint16_t lpn, offset;  // LPN: Logical Page Number
    if (get_header_page_info(obj_id, &lpn, &offset) != 0) {
        FTL_DEBUG("[OBJ_GET] ERROR: Invalid obj_id=%d\n", obj_id);
        return -1;
    }

    FTL_DEBUG("[OBJ_GET] ===== START DEBUG =====\n");
    FTL_DEBUG("[OBJ_GET] obj_id=%d, target LPN=%d, offset=%d\n", obj_id, lpn, offset);

    // Read the entire system page through FTL layer (wear leveling enabled)
    uint8_t page_buf[USER_DATA_SIZE];  // System pages only contain user data area
    memset(page_buf, 0, USER_DATA_SIZE);  // Ensure buffer is zeroed
    int ret = read_system_page(lpn, page_buf);
    FTL_DEBUG("[OBJ_GET] read_system_page returned: %d\n", ret);
    
    if (ret != 0) {
        FTL_DEBUG("[OBJ_GET] ERROR: Failed to read system LPN %d\n", lpn);
        return -1;
    }

    // Print raw data read (at least print header portion)
    FTL_DEBUG("[OBJ_GET] Raw data at offset %d (first 32 bytes):\n", offset);
    for (int i = 0; i < 32 && (offset + i) < USER_DATA_SIZE; i++) {
        if (i % 8 == 0) FTL_DEBUG("[OBJ_GET]   ");
        FTL_DEBUG("%02X ", page_buf[offset + i]);
        if (i % 8 == 7) FTL_DEBUG("\n");
    }
    FTL_DEBUG("\n");

    // Extract the specific object header from the page
    memcpy(hdr, page_buf + offset, sizeof(obj_header_t));

    FTL_DEBUG("[OBJ_GET] Extracted header:\n");
    FTL_DEBUG("[OBJ_GET]   pkg_id    = 0x%04X\n", hdr->pkg_id);
    FTL_DEBUG("[OBJ_GET]   class_id  = 0x%04X\n", hdr->class_id);
    FTL_DEBUG("[OBJ_GET]   type      = 0x%02X\n", hdr->type);
    FTL_DEBUG("[OBJ_GET]   body_size = %d\n", hdr->body_size);
    FTL_DEBUG("[OBJ_GET]   body_addr = %d\n", hdr->body_addr);
    FTL_DEBUG("[OBJ_GET] ===== END DEBUG =====\n");

    return 0;
}

/**
 * find_phys_page_by_sector: Find physical page number by sector_id (LPN) through Radix Tree traversal
 * @FTL: FTL instance
 * @sector: Sector ID (Logical Page Number)
 * @return: Physical page number, PAGE_NONE if not found
 */
/**
 * find_phys_page_by_sector: Find physical page number for a given sector_id
 * @FTL: FTL instance
 * @sector: Target sector ID to search for
 * @return: Physical page number, or PAGE_NONE if not found
 *
 * Note: This function follows the same traversal logic as eflash_ftl_read,
 * using bit-by-bit comparison to navigate the Radix Tree.
 */
uint16_t find_phys_page_by_sector(uint16_t sector) {
    if (!FTL || FTL->root_page == PAGE_NONE) {
        FTL_DEBUG("[FIND_PHYS] ERROR: No root page or invalid FTL\n");
        return PAGE_NONE;
    }

    uint8_t meta_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t cur_meta;
    int depth = 0;
    uint16_t current = FTL->root_page;

    // Traverse radix tree using bit-by-bit comparison (same as eflash_ftl_read)
    while (depth < RADIX_DEPTH) {
        // Read current node metadata
        if (eflash_hw_read(current, meta_buf) != 0) {
            FTL_DEBUG("[FIND_PHYS] ERROR: Failed to read PPN %d at depth %d\n", current, depth);
            return PAGE_NONE;
        }
        if (verify_and_correct_page(meta_buf) != 0) {
            FTL_DEBUG("[FIND_PHYS] ERROR: Page verification failed at PPN %d\n", current);
            return PAGE_NONE;
        }
        memcpy(&cur_meta, meta_buf + META_OFFSET, META_SIZE);

        FTL_DEBUG("[FIND_PHYS] depth=%d, cur_sector=%d, target=%d, adr[%d]=%d\n",
                 depth, cur_meta.sector_id, sector, depth, cur_meta.adr[depth]);

        // Check if current node matches the target sector
        if (cur_meta.sector_id == sector) {
            FTL_DEBUG("[FIND_PHYS] Found sector %d at physical page %d (depth=%d)\n",
                     sector, current, depth);
            return current;
        }

        // Calculate bit value at current depth (MSB first)
        uint16_t bit_mask = 1 << (RADIX_DEPTH - 1 - depth);
        int target_bit = (sector & bit_mask) ? 1 : 0;
        int current_bit = (cur_meta.sector_id & bit_mask) ? 1 : 0;

        if (target_bit != current_bit) {
            // Bits differ: need to jump to adr[depth]
            FTL_DEBUG("[FIND_PHYS] Bit mismatch at depth=%d (target=%d, current=%d), jumping to adr[%d]\n",
                     depth, target_bit, current_bit, depth);
            
            current = cur_meta.adr[depth];
            if (current == PAGE_NONE) {
                FTL_DEBUG("[FIND_PHYS] Sector %d not found (adr[%d]=NONE)\n", sector, depth);
                return PAGE_NONE;
            }
            // After jump, continue comparing same depth bit with new node
            // Note: Don't increment depth here, continue in next loop iteration
        } else {
            // Bits same: continue to next depth level
            FTL_DEBUG("[FIND_PHYS] Bits match at depth=%d, moving to next level\n", depth);
            depth++;
        }
    }

    // Reached maximum depth without finding exact match
    FTL_DEBUG("[FIND_PHYS] Sector %d not found after full traversal\n", sector);
    return PAGE_NONE;
}

/**
 * find_sector_by_phys_page: Get logical sector ID from physical page number
 * @ppn: Physical Page Number
 * @return: Logical sector ID if valid, PAGE_NONE if invalid or error
 *
 * Description:
 *   Directly reads the physical page and extracts sector_id from metadata.
 *   Returns PAGE_NONE if:
 *   - Page is blank (all 0xFF)
 *   - ECC verification fails
 *   - Read operation fails
 *
 * This is the inverse operation of find_phys_page_by_sector().
 */
uint16_t find_sector_by_phys_page(uint16_t ppn) {
    if (!FTL || !FTL->is_initialized) {
        FTL_DEBUG("[FIND_SECTOR] ERROR: FTL not initialized\n");
        return PAGE_NONE;
    }

    // Validate physical page number range
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;
    if (ppn > last_user_page) {
        FTL_DEBUG("[FIND_SECTOR] ERROR: Invalid PPN %d (max=%d)\n", ppn, last_user_page);
        return PAGE_NONE;
    }

    uint8_t page_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t meta;

    // Step 1: Read physical page
    if (eflash_hw_read(ppn, page_buf) != 0) {
        FTL_DEBUG("[FIND_SECTOR] ERROR: Failed to read PPN %d\n", ppn);
        return PAGE_NONE;
    }

    // Step 2: Check if page is blank (all 0xFF)
    bool is_blank = true;
    for (int i = 0; i < EFLASH_PAGE_SIZE; i++) {
        if (page_buf[i] != 0xFF) {
            is_blank = false;
            break;
        }
    }
    if (is_blank) {
        FTL_DEBUG("[FIND_SECTOR] PPN %d is blank (no valid data)\n", ppn);
        return PAGE_NONE;
    }

    // Step 3: Verify and correct ECC
    if (verify_and_correct_page(page_buf) != 0) {
        FTL_DEBUG("[FIND_SECTOR] ERROR: ECC verification failed for PPN %d\n", ppn);
        return PAGE_NONE;
    }

    // Step 4: Extract metadata
    memcpy(&meta, page_buf + META_OFFSET, META_SIZE);

    // Step 5: Validate sector_id (PAGE_NONE = 0xFFFF is invalid)
    if (meta.sector_id == PAGE_NONE) {
        FTL_DEBUG("[FIND_SECTOR] WARNING: Invalid sector_id PAGE_NONE in PPN %d\n", ppn);
        return PAGE_NONE;
    }

    FTL_DEBUG("[FIND_SECTOR] PPN %d -> Sector %d (gc_count=%d)\n", 
             ppn, meta.sector_id, meta.global_count);

    return meta.sector_id;
}

/**
 * eflash_ftl_obj_set_header: Write object header by obj_id
 * @FTL: FTL instance
 * @obj_id: Object ID (0-2087)
 * @hdr: Object header to write
 * @return: 0 on success, -1 on failure
 *
 * Note: This function performs read-modify-write on the entire page
 * to maintain data integrity and ECC consistency.
 */
int eflash_ftl_obj_set_header(uint16_t obj_id, const obj_header_t *hdr) {
    // Parameter validation
    if (!FTL || !FTL->is_initialized) return -1;
    if (hdr == NULL) return -1;
    if (obj_id == PAGE_NONE) return -1;  // Invalid object ID

    uint16_t lpn, offset;  // LPN: Logical Page Number
    if (get_header_page_info(obj_id, &lpn, &offset) != 0) {
        FTL_DEBUG("[OBJ_SET] ERROR: Invalid obj_id=%d\n", obj_id);
        return -1;
    }

    FTL_DEBUG("[OBJ_SET] ===== START DEBUG =====\n");
    FTL_DEBUG("[OBJ_SET] obj_id=%d, target LPN=%d, offset=%d\n", obj_id, lpn, offset);
    FTL_DEBUG("[OBJ_SET] Input header:\n");
    FTL_DEBUG("[OBJ_SET]   pkg_id    = 0x%04X\n", hdr->pkg_id);
    FTL_DEBUG("[OBJ_SET]   class_id  = 0x%04X\n", hdr->class_id);
    FTL_DEBUG("[OBJ_SET]   type      = 0x%02X\n", hdr->type);
    FTL_DEBUG("[OBJ_SET]   body_size = %d\n", hdr->body_size);
    FTL_DEBUG("[OBJ_SET]   body_addr = %d\n", hdr->body_addr);

    // Step 1: Read the entire system page through FTL layer
    uint8_t page_buf[USER_DATA_SIZE];  // System pages only contain user data area
    memset(page_buf, 0, USER_DATA_SIZE);  // 确保缓冲区清零
    int ret = read_system_page(lpn, page_buf);
    FTL_DEBUG("[OBJ_SET] read_system_page returned: %d\n", ret);
    
    if (ret != 0) {
        // LPN not yet mapped in Radix Tree, this is the first write to this LPN
        // Fill buffer with blank data (all zeros)
        FTL_DEBUG("[OBJ_SET] WARNING: LPN %d not found in tree, using blank page\n", lpn);
        memset(page_buf, 0x00, USER_DATA_SIZE);
    }

    // Print original data before modification
    FTL_DEBUG("[OBJ_SET] Original data at offset %d (first 32 bytes):\n", offset);
    for (int i = 0; i < 32 && (offset + i) < USER_DATA_SIZE; i++) {
        if (i % 8 == 0) FTL_DEBUG("[OBJ_SET]   ");
        FTL_DEBUG("%02X ", page_buf[offset + i]);
        if (i % 8 == 7) FTL_DEBUG("\n");
    }
    FTL_DEBUG("\n");

    // Step 2: Modify the specific object header in buffer
    memcpy(page_buf + offset, hdr, sizeof(obj_header_t));

    // Print modified data after modification
    FTL_DEBUG("[OBJ_SET] Modified data at offset %d (first 32 bytes):\n", offset);
    for (int i = 0; i < 32 && (offset + i) < USER_DATA_SIZE; i++) {
        if (i % 8 == 0) FTL_DEBUG("[OBJ_SET]   ");
        FTL_DEBUG("%02X ", page_buf[offset + i]);
        if (i % 8 == 7) FTL_DEBUG("\n");
    }
    FTL_DEBUG("\n");

    // Step 3: Write the entire page back through FTL layer (wear leveling enabled)
    if (write_system_page(lpn, page_buf) != 0) {
        FTL_DEBUG("[OBJ_SET] ERROR: Failed to write system LPN %d\n", lpn);
        return -1;
    }

    FTL_DEBUG("[OBJ_SET] obj_id=%d -> LPN %d, offset %d (SUCCESS)\n",
              obj_id, lpn, offset);
    FTL_DEBUG("[OBJ_SET] ===== END DEBUG =====\n");

    return 0;
}

/**
 * get_header_page_info: Calculate logical page and page offset based on object ID
 */
static int get_header_page_info(uint16_t obj_id, uint16_t *out_log_page, uint16_t *out_offset) {
    if (obj_id < BASE_HEADER_CAPACITY) {
        // Base area
        *out_log_page = FTL->base_hdr_addr + (obj_id / OBJ_HEADERS_PER_PAGE);
        *out_offset = (obj_id % OBJ_HEADERS_PER_PAGE) * sizeof(obj_header_t);
        return 0;
    }

    // Extended area
    uint16_t ext_idx = obj_id - BASE_HEADER_CAPACITY;
    uint16_t level = (ext_idx / EXT_HEADER_CAPACITY) + 1;

    if (level > MAX_EXT_LEVELS || FTL->ext_hdr_addrs[level - 1] == PAGE_NONE) {
        return -1; // Not yet extended or out of range
    }

    uint16_t page_in_unit = (ext_idx % EXT_HEADER_CAPACITY) / OBJ_HEADERS_PER_PAGE;
    uint16_t idx_in_page = (ext_idx % EXT_HEADER_CAPACITY) % OBJ_HEADERS_PER_PAGE;

    *out_log_page = FTL->ext_hdr_addrs[level - 1] + page_in_unit;
    *out_offset = idx_in_page * sizeof(obj_header_t);
    return 0;
}

/**
 * extend_headers: Dynamically extend one level of object header space (4 pages)
 */
static int extend_headers() {
    // 1. Find the highest level extension page, get its last object header (pointer field)
    uint16_t prev_ext_addr = FTL->base_hdr_addr + BASE_HEADER_PAGES - 1; // Default points to last page of base area
    int level = 0;
    while (level < MAX_EXT_LEVELS && FTL->ext_hdr_addrs[level] != PAGE_NONE) {
        prev_ext_addr = FTL->ext_hdr_addrs[level] + EXT_HEADER_PAGES_UNIT - 1;
        level++;
    }

    if (level >= MAX_EXT_LEVELS) return -1; // Reached maximum extension levels

    // 2. Allocate new 4-page logical space (page-aligned)
    uint32_t new_ext_logical_addr;
    if (eflash_mgr_alloc_pages(4, &new_ext_logical_addr) != 0) {
        return -1;
    }
    uint16_t new_ext_lpn = (uint16_t)(new_ext_logical_addr / USER_DATA_SIZE);  // Logical Page Number

    // 3. Update pointer at end of previous level with Magic Numbers
    obj_header_t link_hdr;
    memset(&link_hdr, 0, sizeof(link_hdr));
    
    // Set Magic Numbers for reliable LINK object detection
    link_hdr.pkg_id = LINK_OBJ_MAGIC_PKG_ID;        // "FT" - Flash Translation
    link_hdr.class_id = LINK_OBJ_MAGIC_CLASS_ID;      // "LN" - LiNk
    link_hdr.type = OBJ_TYPE_LINK;   // Link type
    link_hdr.reserved[0] = LINK_OBJ_MAGIC_RESERVED0;     // Additional magic
    link_hdr.reserved[1] = LINK_OBJ_MAGIC_RESERVED1;     // Additional magic
    link_hdr.body_addr = new_ext_lpn;
    link_hdr.body_size = EXT_HEADER_PAGES_UNIT * USER_DATA_SIZE;

    // Write link info to end of previous level
    uint16_t link_obj_id = (level == 0) ? (BASE_HEADER_CAPACITY - 1) : 
                           (level * EXT_HEADER_CAPACITY + BASE_HEADER_CAPACITY - 1);
    FTL_DEBUG("[EXTEND] Writing LINK object at obj_id=%d (magic: 0x%04X/0x%04X)\n",
             link_obj_id, link_hdr.pkg_id, link_hdr.class_id);
    eflash_ftl_obj_set_header(link_obj_id, &link_hdr);

    // 4. Record new extension address (dynamically allocated LPN)
    FTL->ext_hdr_addrs[level] = new_ext_lpn;

    // 5. Initialize new pages through FTL layer (wear leveling enabled)
    //    Note: Extended object headers use dynamically allocated LPNs (not fixed)
    uint8_t empty_page[USER_DATA_SIZE];
    memset(empty_page, 0xFF, USER_DATA_SIZE);
    for (int i = 0; i < EXT_HEADER_PAGES_UNIT; i++) {
        if (write_system_page(new_ext_lpn + i, empty_page) != 0) {
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
 * @FTL: FTL instance
 *
 * Description:
 *   Scans the base area and extension areas to find all LINK headers,
 *   then rebuilds the ext_hdr_addrs array based on the link chain.
 *   This ensures correct recovery after power failure.
 */
static void scan_and_rebuild_ext_headers(void) {
    FTL_DEBUG("[INIT] Scanning object header extension chain...\n");

    // Initialize all entries to PAGE_NONE
    for (int i = 0; i < MAX_EXT_LEVELS; i++) {
        FTL->ext_hdr_addrs[i] = PAGE_NONE;
    }

    int level = 0;
    uint16_t current_scan_lpn;  // Current scan Logical Page Number

    // Start from base area's last page (LPN 7 for fixed system pages)
    current_scan_lpn = FTL->base_hdr_addr + BASE_HEADER_PAGES - 1;
    FTL_DEBUG("[INIT] Starting scan from base area LPN %d\n", current_scan_lpn);

    // Traverse the extension chain
    while (level < MAX_EXT_LEVELS) {
        // Read the last object header of current level through FTL
        obj_header_t link_hdr;
        uint16_t offset = (OBJ_HEADERS_PER_PAGE - 1) * sizeof(obj_header_t);

        uint8_t page_buf[USER_DATA_SIZE];  // System pages only contain user data
        if (read_system_page(current_scan_lpn, page_buf) != 0) {
            FTL_DEBUG("[INIT] ERROR: Failed to read system LPN %d for link scan\n", current_scan_lpn);
            break;
        }

        // Extract the last object header from the page
        memcpy(&link_hdr, page_buf + offset, sizeof(obj_header_t));

        // Check if this is a valid LINK header
        if (link_hdr.type == OBJ_TYPE_LINK && link_hdr.body_addr != PAGE_NONE) {
            // Found a valid extension link
            FTL->ext_hdr_addrs[level] = (uint16_t)link_hdr.body_addr;
            FTL_DEBUG("[INIT] Found extension level %d at LPN %d\n", level, FTL->ext_hdr_addrs[level]);

            // Move to next level's last page (extension LPNs are dynamically allocated)
            level++;
            if (level < MAX_EXT_LEVELS) {
                current_scan_lpn = FTL->ext_hdr_addrs[level - 1] + EXT_HEADER_PAGES_UNIT - 1;
            }
        } else {
            // No more extensions
            FTL_DEBUG("[INIT] Extension chain ends at level %d\n", level);
            break;
        }
    }

    FTL_DEBUG("[INIT] Extension scan complete. Total levels: %d\n", level);
}

/**
 * is_link_object: Check if an object header is a LINK object with magic numbers
 * @hdr: Object header to check
 * @return: true if it's a valid LINK object, false otherwise
 *
 * Magic numbers:
 *   - pkg_id = 0x5F54 ("FT" - Flash Translation)
 *   - class_id = 0x4C4E ("LN" - LiNk)
 *   - type = OBJ_TYPE_LINK
 *   - reserved[0] = 0xAD, reserved[1] = 0xDE
 */
static bool is_link_object(const obj_header_t *hdr) {
    return (hdr->type == OBJ_TYPE_LINK &&
            hdr->pkg_id == LINK_OBJ_MAGIC_PKG_ID &&
            hdr->class_id == LINK_OBJ_MAGIC_CLASS_ID &&
            hdr->reserved[0] == LINK_OBJ_MAGIC_RESERVED0 &&
            hdr->reserved[1] == LINK_OBJ_MAGIC_RESERVED1);
}

/**
 * scan_and_rebuild_max_obj_id: Optimized scan to find max allocated obj_id
 * @FTL: FTL instance
 *
 * Description:
 *   Optimization strategy:
 *   1. Check last obj of each region (base/ext levels) for LINK object
 *   2. If LINK found, skip entire region (it's full)
 *   3. Start detailed scan from first non-full region
 *   4. This reduces scan time from O(N) to O(R + M) where R=regions, M=objs in last region
 */
static void scan_and_rebuild_max_obj_id(void) {
    FTL_DEBUG("[INIT] Scanning object headers to rebuild max_obj_id (optimized)...\n");

    uint16_t max_obj_id = 0;
    bool found_first_non_full = false;

    // Step 1: Check base zone and extension zones for LINK objects
    // This helps us skip full regions quickly
    
    // Check base zone last obj (obj_id = BASE_HEADER_CAPACITY - 1 = 231)
    uint16_t check_obj_id = BASE_HEADER_CAPACITY - 1;
    uint16_t lpn, offset;
    
    if (get_header_page_info(check_obj_id, &lpn, &offset) == 0) {
        uint8_t page_buf[USER_DATA_SIZE];
        if (read_system_page(lpn, page_buf) == 0) {
            obj_header_t hdr;
            memcpy(&hdr, page_buf + offset, sizeof(obj_header_t));
            
            if (is_link_object(&hdr)) {
                // Base zone is full, max_obj_id >= 231
                max_obj_id = check_obj_id;
                FTL_DEBUG("[INIT] Base zone full (LINK at obj_id=%d), checking extensions...\n", check_obj_id);
                
                // Check extension levels
                for (int level = 0; level < MAX_EXT_LEVELS; level++) {
                    if (FTL->ext_hdr_addrs[level] == PAGE_NONE) {
                        // No more extensions
                        FTL_DEBUG("[INIT] Extension level %d not allocated, stopping region check\n", level);
                        break;
                    }
                    
                    // Calculate last obj_id of this extension level
                    check_obj_id = BASE_HEADER_CAPACITY + (level + 1) * EXT_HEADER_CAPACITY - 1;
                    
                    if (get_header_page_info(check_obj_id, &lpn, &offset) != 0) {
                        FTL_DEBUG("[INIT] Cannot map obj_id=%d, stopping region check\n", check_obj_id);
                        break;
                    }
                    
                    if (read_system_page(lpn, page_buf) != 0) {
                        FTL_DEBUG("[INIT] Failed to read LPN %d, stopping region check\n", lpn);
                        break;
                    }
                    
                    memcpy(&hdr, page_buf + offset, sizeof(obj_header_t));
                    
                    if (is_link_object(&hdr)) {
                        // This extension level is also full
                        max_obj_id = check_obj_id;
                        FTL_DEBUG("[INIT] Extension level %d full (LINK at obj_id=%d)\n", level + 1, check_obj_id);
                    } else {
                        // Found first non-full region
                        FTL_DEBUG("[INIT] Extension level %d not full, will scan from obj_id=%d\n", 
                                 level + 1, BASE_HEADER_CAPACITY + level * EXT_HEADER_CAPACITY);
                        found_first_non_full = true;
                        // Start scanning from the first obj of this level
                        max_obj_id = BASE_HEADER_CAPACITY + level * EXT_HEADER_CAPACITY - 1;
                        break;
                    }
                }
            } else {
                // Base zone not full, start scanning from beginning
                FTL_DEBUG("[INIT] Base zone not full, scanning from obj_id=0\n");
                max_obj_id = 0;
            }
        }
    }

    // Step 2: Detailed scan from max_obj_id + 1
    uint16_t scan_start = max_obj_id + 1;
    FTL_DEBUG("[INIT] Starting detailed scan from obj_id=%d\n", scan_start);
    
    uint16_t obj_id = scan_start;
    bool scanning = true;

    while (scanning) {
        // Get LPN and offset for current obj_id
        if (get_header_page_info(obj_id, &lpn, &offset) != 0) {
            // Cannot map this obj_id (extension not allocated)
            FTL_DEBUG("[INIT] Cannot map obj_id=%d, stopping scan\n", obj_id);
            break;
        }

        // Read the page
        uint8_t page_buf[USER_DATA_SIZE];
        if (read_system_page(lpn, page_buf) != 0) {
            FTL_DEBUG("[INIT] Failed to read LPN %d for obj_id=%d, stopping scan\n", lpn, obj_id);
            break;
        }

        // Extract the object header
        obj_header_t hdr;
        memcpy(&hdr, page_buf + offset, sizeof(obj_header_t));

        // Check if this is a valid (non-blank, non-LINK) header
        if (hdr.pkg_id == 0 || is_link_object(&hdr)) {
            FTL_DEBUG("[INIT] Found invalid/LINK header at obj_id=%d, stopping scan\n", obj_id);
            scanning = false;
        } else {
            // Valid header, continue to next
            max_obj_id = obj_id;
            obj_id++;
        }
    }

    FTL->max_obj_id = max_obj_id;
    FTL_DEBUG("[INIT] max_obj_id rebuilt: %d (scan started from %d)\n", FTL->max_obj_id, scan_start);
}


// ========================================================================
// Root Page Binary Search (O(log N) instead of O(N))
// ========================================================================

static bool probe_is_committed(uint16_t ppn) {
    ftl_meta_t meta;
    if (!is_valid_page(ppn, &meta)) return false;
    return (meta.status == TXN_STATUS_COMMITTED);
}

static uint16_t find_root_full_scan(ftl_meta_t *out_meta) {
    uint32_t max_count = 0;
    uint16_t max_epoch = 0;
    uint16_t root = PAGE_NONE;
    ftl_meta_t meta;

    for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
        if (is_valid_page(i, &meta) && meta.status == TXN_STATUS_COMMITTED) {
            if (meta.epoch > max_epoch ||
                (meta.epoch == max_epoch && meta.global_count > max_count)) {
                max_epoch = meta.epoch;
                max_count = meta.global_count;
                root = i;
                if (out_meta) memcpy(out_meta, &meta, sizeof(ftl_meta_t));
            }
        }
    }
    return root;
}

static uint16_t find_root_binary(void) {
    uint16_t last_ppn = EFLASH_TOTAL_PAGES - 1;
    bool c0  = probe_is_committed(0);
    bool cl  = probe_is_committed(last_ppn);
    uint16_t root = PAGE_NONE;

    if (c0 && !cl) {
        uint16_t lo = 0, hi = last_ppn;
        while (lo < hi) {
            uint16_t mid = (lo + hi + 1) / 2;
            if (probe_is_committed(mid)) lo = mid;
            else hi = mid - 1;
        }
        root = lo;
    } else if (!c0 && cl) {
        uint16_t lo = 0, hi = last_ppn;
        while (lo < hi) {
            uint16_t mid = (lo + hi) / 2;
            if (probe_is_committed(mid)) hi = mid;
            else lo = mid + 1;
        }
        root = last_ppn;
        if (!probe_is_committed(root)) root = lo;
    } else if (c0 && cl) {
        uint16_t lo = 0, hi = last_ppn;
        while (lo < hi) {
            uint16_t mid = (lo + hi) / 2;
            if (!probe_is_committed(mid)) hi = mid;
            else lo = mid + 1;
        }
        uint16_t first_b = lo;
        lo = first_b; hi = last_ppn;
        while (lo < hi) {
            uint16_t mid = (lo + hi + 1) / 2;
            if (probe_is_committed(mid)) lo = mid;
            else hi = mid - 1;
        }
        root = lo;
        if (!probe_is_committed(root)) root = first_b > 0 ? first_b - 1 : PAGE_NONE;
    }

    if (root == PAGE_NONE || root >= EFLASH_TOTAL_PAGES) {
        return PAGE_NONE;
    }

    ftl_meta_t root_meta;
    if (!is_valid_page(root, &root_meta) || root_meta.status != TXN_STATUS_COMMITTED) {
        return PAGE_NONE;
    }

    return root;
}

// --- FTL Initialization and Pre-allocation ---

int eflash_ftl_init(void) {
    FTL_DEBUG("[INIT] Starting eflash_ftl_init\n");

    memset(g_map_cache, 0, sizeof(g_map_cache));
    g_map_cache_idx = 0;
    memset(g_page_cache, 0, sizeof(g_page_cache));
    g_cache_seq_counter = 0;
    g_dirty_count = 0;

    // Step 1: Initialize space manager (memory-only, no Flash writes)
    eflash_mgr_init(EFLASH_TOTAL_PAGES);

    FTL->root_page = PAGE_NONE;
    FTL->shadow_root = PAGE_NONE;
    FTL->next_count = 1;
    FTL->current_epoch = 0;
    FTL->active_txn_id = TXN_ID_NONE;
    FTL->is_initialized = true;
    FTL->max_obj_id = 0xFFFF;  // Will be set during init or recovery
    
    // Initialize GC head/tail pointers to physical page 0
    // Physical pages are NOT pre-reserved for system areas!
    // System logical pages (LPN 0-11) will be allocated physical pages on-demand when first written.
    // First write to LPN 8 (free list) will allocate PPN 0.
    FTL->gc_head_page = 0;
    FTL->gc_tail_page = 0;
    FTL_DEBUG("[INIT] GC pointers initialized: head=%d, tail=%d\n", 
             FTL->gc_head_page, FTL->gc_tail_page);
    
    // Calculate total user pages and GC threshold
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;
    FTL->total_user_pages = EFLASH_TOTAL_PAGES;  // All physical pages available
    FTL->gc_threshold = FTL->total_user_pages / 20;  //5% threshold
    
    FTL_DEBUG("[INIT] Total physical pages: %d (PPN 0-%d), GC threshold: %d\n",
             FTL->total_user_pages, last_user_page, FTL->gc_threshold);

    // Initialize system area logical page numbers
    FTL->base_hdr_addr = SYS_OBJ_HEADER_BASE_LPN;  // LPN 0
    FTL->free_list_addr = SYS_FREE_LIST_BASE_LPN;  // LPN 8

    // Initialize ext_hdr_addrs to PAGE_NONE (will be rebuilt by scanning)
    for (int i = 0; i < MAX_EXT_LEVELS; i++) {
        FTL->ext_hdr_addrs[i] = PAGE_NONE;
    }

    // Step 2: Binary search (O(log N)) to find root, fallback to full scan
    FTL_DEBUG("[INIT] Root binary search...\n");
    FTL->root_page = find_root_binary();
    if (FTL->root_page != PAGE_NONE) {
        ftl_meta_t root_meta;
        is_valid_page(FTL->root_page, &root_meta);
        FTL->next_count = root_meta.global_count + 1;
        FTL->current_epoch = root_meta.epoch;
        FTL_DEBUG("[INIT] Binary search OK. Root: ppn=%d, epoch=%d, count=%d\n",
                 FTL->root_page, (int)FTL->current_epoch, (int)root_meta.global_count);
    } else {
        FTL_DEBUG("[INIT] Binary search failed, fallback full scan...\n");
        ftl_meta_t root_meta;
        FTL->root_page = find_root_full_scan(&root_meta);
        if (FTL->root_page == PAGE_NONE) {
            FTL_DEBUG("[INIT] No valid root found (first boot)\n");
            FTL->next_count = 1;
            FTL->current_epoch = 0;
        } else {
            FTL->next_count = root_meta.global_count + 1;
            FTL->current_epoch = root_meta.epoch;
            FTL_DEBUG("[INIT] Full scan OK. Root: ppn=%d, epoch=%d, count=%d\n",
                     FTL->root_page, (int)FTL->current_epoch, (int)root_meta.global_count);
        }
    }

    // Step 2.3: Recover extended free node table from Flash
    // This must be done AFTER root_page is found (Radix Tree is functional)
    int recovered_levels = eflash_mgr_recover_ext_free_nodes();
    if (recovered_levels > 0) {
        FTL_DEBUG("[INIT] Recovered %d extension levels for free node table\n", recovered_levels);
    }

    // Step 2.5: Recover Head/Tail pointers and initialize valid page counter
    // This must be done AFTER root_page is found
    if (FTL->root_page != PAGE_NONE) {
        // Recovery mode: reconstruct Head/Tail pointers from physical state
        
        // Strategy: 
        // 1. Set Head = Tail = root_page + 1 (next page after root)
        // 2. Scan forward from Tail to find first non-blank page
        // 3. That position is the recovered Tail (boundary between used and free space)
        
        uint16_t initial_tail = FTL->root_page + 1;
        if (initial_tail > last_user_page) {
            initial_tail = 0;  // Wrap around
        }
        //ASSERT(!is_page_still_valid(initial_tail),"head should be invalid");
        FTL_DEBUG("[INIT] Starting Head/Tail recovery from page %d\n", initial_tail);
        
        // Scan forward to find the boundary between used and free space
        uint16_t recovered_tail = initial_tail;
        uint32_t scan_count = 0;
        
        while (scan_count < FTL->total_user_pages) {
            if (is_blank_page(recovered_tail)) {
                // This page is blank (all 0xFF), continue scanning
                recovered_tail++;
                if (recovered_tail > last_user_page) {
                    recovered_tail = 0;  // Wrap around
                }
                scan_count++;
            } else {
                // Found first non-blank page, this is the boundary
                FTL_DEBUG("[INIT] Found non-blank page at %d after scanning %u pages\n", 
                         recovered_tail, scan_count);
                break;
            }
        }
        
        // Set Head and Tail to the recovered position
        FTL->gc_head_page = initial_tail;
        FTL->gc_tail_page = recovered_tail;
        
        // CRITICAL: Ensure recovered pointers don't land in code region
        adjust_gc_pointers_for_code_region();
        
        FTL_DEBUG("[INIT] Recovered Head/Tail pointers: head=%d, tail=%d\n",
                 FTL->gc_head_page, FTL->gc_tail_page);
        
        // Now count valid pages using the recovered state
        uint32_t valid_count = 0;
        for (uint16_t ppn = 0; ppn <= last_user_page; ppn++) {
            if (is_page_still_valid(ppn)) {
                valid_count++;
            }
        }
        FTL->valid_page_count = valid_count;
        FTL_DEBUG("[INIT] Valid page count initialized: %u (by scanning)\n", valid_count);
        FTL_DEBUG("[INIT] Free pages calculated: %u\n", eflash_ftl_get_free_pages());
    } else {
        // First power-on: no valid pages yet
        FTL->valid_page_count = 0;
        FTL_DEBUG("[INIT] Valid page count initialized: 0 (first power-on)\n");
    }

    // Step 3: Determine if initialization is needed
    bool need_init = false;

    if (FTL->root_page == PAGE_NONE) {
        // No root found - first power-on or corrupted
        FTL_DEBUG("[INIT] No root page found - performing full initialization\n");
        need_init = true;
    } else {
        // Root found - this is a recovery scenario
        FTL_DEBUG("[INIT] Root found at page %d, entering recovery mode\n", FTL->root_page);

        // First, query Radix Tree to restore system page physical mappings
        FTL_DEBUG("[INIT] Restoring system page mappings from Radix Tree...\n");

        // Find physical page for free list (LPN 8)
        uint16_t phys_page = find_phys_page_by_sector(SYS_FREE_LIST_BASE_LPN);
        if (phys_page != PAGE_NONE) {
            FTL->spc_mgr.free_node_pages[0] = phys_page;
            FTL_DEBUG("[INIT] Restored LPN %d -> PPN %d (free_node[0])\n", SYS_FREE_LIST_BASE_LPN, phys_page);
        } else {
            FTL_DEBUG("[INIT] WARNING: Free list LPN %d not found in tree\n", SYS_FREE_LIST_BASE_LPN);
        }

        // Set remaining system pages to PAGE_NONE (will be restored on-demand)
        for (int i = 1; i < FREE_NODE_PAGE_COUNT; i++) {
            FTL->spc_mgr.free_node_pages[i] = PAGE_NONE;
        }
        for (int i = 0; i < BASE_HEADER_PAGES; i++) {
            FTL->spc_mgr.header_pages[i] = PAGE_NONE;
        }
        FTL_DEBUG("[INIT] Remaining system pages set to PAGE_NONE (on-demand restoration)\n");

        // Scan and rebuild extension header addresses from existing data
        scan_and_rebuild_ext_headers();

        // Scan and rebuild max_obj_id by traversing valid object headers
        scan_and_rebuild_max_obj_id();

        // Check if free list contains at least one valid node
        bool free_list_initialized = eflash_mgr_check_initialized();

        if (!free_list_initialized) {
            FTL_DEBUG("[INIT] WARNING: Free list appears uninitialized, but skipping re-init in recovery mode\n");
            FTL_DEBUG("[INIT] This is expected if free list was never written after last GC\n");
            // In recovery mode, don't re-initialize as it would corrupt the radix tree
            // The free list will be rebuilt when needed
        } else {
            FTL_DEBUG("[INIT] Free list verified as initialized\n");
        }

        FTL_DEBUG("[INIT] Recovery complete - preserving existing radix tree structure\n");
    }

    // Step 4: Perform initialization if needed (Strategy B: Minimal Initialization)
    if (need_init) {
        FTL_DEBUG("[INIT] === Starting minimal system initialization (Strategy B) ===\n");

        // Strategy B: Only initialize LPN 8 (first free list page)
        // Object header pages (LPN 0-7) will be written on-demand before access
        // Free list pages (LPN 9-11) will be allocated on-demand when list grows
        FTL_DEBUG("[INIT] Initializing only LPN 8 (free list head)...\n");

        // Note: Don't write all 0xFF, as it will be detected as blank page
        // Write a placeholder pattern instead
        uint8_t init_page[USER_DATA_SIZE];
        memset(init_page, 0x00, USER_DATA_SIZE);  // Use 0x00 instead of 0xFF

        // Write LPN 8 through FTL layer (ensures Radix Tree mapping)
        if (write_system_page(SYS_FREE_LIST_BASE_LPN, init_page) != 0) {
            FTL_DEBUG("[INIT] ERROR: Failed to initialize system LPN %d\n", SYS_FREE_LIST_BASE_LPN);
            return -1;
        }
        FTL_DEBUG("[INIT] System LPN %d initialized successfully\n", SYS_FREE_LIST_BASE_LPN);

        // Query Radix Tree for physical page of LPN 8
        FTL_DEBUG("[INIT] Querying Radix Tree for LPN 8 physical mapping...\n");
        uint16_t phys_page = find_phys_page_by_sector(SYS_FREE_LIST_BASE_LPN);
        if (phys_page == PAGE_NONE) {
            FTL_DEBUG("[INIT] ERROR: Failed to find physical page for LPN %d\n", SYS_FREE_LIST_BASE_LPN);
            return -1;
        }
        FTL->spc_mgr.free_node_pages[0] = phys_page;
        FTL_DEBUG("[INIT] LPN %d -> PPN %d (free_node[0])\n", SYS_FREE_LIST_BASE_LPN, phys_page);

        // Set remaining system pages to PAGE_NONE (will be allocated on-demand)
        for (int i = 1; i < FREE_NODE_PAGE_COUNT; i++) {
            FTL->spc_mgr.free_node_pages[i] = PAGE_NONE;
        }
        for (int i = 0; i < BASE_HEADER_PAGES; i++) {
            FTL->spc_mgr.header_pages[i] = PAGE_NONE;
        }
        FTL_DEBUG("[INIT] Remaining system pages set to PAGE_NONE (on-demand allocation)\n");

        // Initialize free list with one large node
        // CRITICAL: Reserve LPN 12 for Code Region management data
        // Node addr = LPN 13 * USER_DATA_SIZE (first available user data page after code region reservation)
        // Node size = (EFLASH_TOTAL_PAGES - 13) * USER_DATA_SIZE
        FTL_DEBUG("[INIT] Initializing free list with single node (addr=LPN13, size=%d pages)...\n",
                 EFLASH_TOTAL_PAGES - SYS_RESERVED_LPN_COUNT - 1);
        
        // Prepare the free list page data (count + node structure)
        uint8_t free_list_page[USER_DATA_SIZE];
        memset(free_list_page, 0xFF, USER_DATA_SIZE);

        // Set count = 1 at offset 0 (beginning of user data region)
        free_list_page[0] = 1 & 0xFF;
        free_list_page[1] = (1 >> 8) & 0xFF;

        // Create initial free node (skip LPN 12 reserved for code region)
        free_node_t initial_node;
        initial_node.addr = (uint32_t)(SYS_RESERVED_LPN_COUNT + 1) * USER_DATA_SIZE;  // Start from LPN 13
        initial_node.size = (uint32_t)(EFLASH_TOTAL_PAGES - SYS_RESERVED_LPN_COUNT - 1) * USER_DATA_SIZE;

        FTL_DEBUG("[INIT] Initial free node: addr=0x%08X, size=%u bytes\n",
                 initial_node.addr, initial_node.size);

        // Write node data after 2-byte count header (at offset 2)
        uint16_t node_offset = FREE_NODE_HEADER_SIZE;
        memcpy(free_list_page + node_offset, &initial_node, sizeof(free_node_t));

        // Write through FTL layer (this will allocate PPN and update Radix Tree)
        // Note: eflash_ftl_write will calculate ECC internally, no need to do it here
        FTL_DEBUG("[INIT] Writing initial free node to LPN %d via FTL...\n", SYS_FREE_LIST_BASE_LPN);
        if (write_system_page(SYS_FREE_LIST_BASE_LPN, free_list_page) != 0) {
            FTL_DEBUG("[INIT] ERROR: Failed to write initial free node!\n");
            return -1;
        }
        FTL_DEBUG("[INIT] Initial free node written successfully\n");
        
        // Update free_node_pages[0] with the new physical page
        phys_page = find_phys_page_by_sector(SYS_FREE_LIST_BASE_LPN);
        if (phys_page == PAGE_NONE) {
            FTL_DEBUG("[INIT] ERROR: Failed to find physical page for LPN %d after writing free node\n", SYS_FREE_LIST_BASE_LPN);
            return -1;
        }
        FTL->spc_mgr.free_node_pages[0] = phys_page;
        FTL_DEBUG("[INIT] Updated LPN %d -> PPN %d (free_node[0])\n", SYS_FREE_LIST_BASE_LPN, phys_page);
        
        // Initialize remaining base free_node pages (LPN 9-11) with count=0
        // This ensures find_page_with_space can find empty pages instead of triggering extension prematurely
        uint8_t blank_page[USER_DATA_SIZE];
        memset(blank_page, 0xFF, USER_DATA_SIZE);
        blank_page[0] = 0;  // count = 0
        blank_page[1] = 0;
        
        for (int i = 1; i < FREE_NODE_PAGE_COUNT; i++) {
            uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
            if (write_system_page(lpn, blank_page) != 0) {
                FTL_DEBUG("[INIT] ERROR: Failed to initialize LPN %d\n", lpn);
                return -1;
            }
            
            // Get the physical page for this LPN
            phys_page = find_phys_page_by_sector(lpn);
            if (phys_page == PAGE_NONE) {
                FTL_DEBUG("[INIT] ERROR: Failed to get PPN for LPN %d\n", lpn);
                return -1;
            }
            FTL->spc_mgr.free_node_pages[i] = phys_page;
            FTL_DEBUG("[INIT] Initialized LPN %d -> PPN %d (count=0)\n", lpn, phys_page);
        }
        
        // Set total_free_nodes to 1 (we have one initial free node)
        FTL->spc_mgr.total_free_nodes = 1;
        FTL_DEBUG("[INIT] total_free_nodes initialized to %u\n", FTL->spc_mgr.total_free_nodes);

        // Initialize max_obj_id to 0xFFFF (no objects allocated yet)
        // The first call to alloc_header will return 0
        FTL->max_obj_id = 0xFFFF;
        FTL_DEBUG("[INIT] max_obj_id initialized to 0xFFFF (fresh initialization)\n");
    } else {
        // System already initialized - still need to query Radix Tree for physical page mappings
        FTL_DEBUG("[INIT] System already initialized, querying Radix Tree for physical page mappings...\n");

        // Find physical page for free list (ONLY LPN 8)
        uint16_t phys_page = find_phys_page_by_sector(SYS_FREE_LIST_BASE_LPN);
        if (phys_page == PAGE_NONE) {
            FTL_DEBUG("[INIT] ERROR: Failed to find physical page for LPN %d\n", SYS_FREE_LIST_BASE_LPN);
            return -1;
        }

        FTL->spc_mgr.free_node_pages[0] = phys_page;
        FTL_DEBUG("[INIT] LPN %d -> PPN %d (free_node[0])\n", SYS_FREE_LIST_BASE_LPN, phys_page);

        // Query physical pages for remaining base free_node pages (LPN 9-11)
        for (int i = 1; i < FREE_NODE_PAGE_COUNT; i++) {
            uint16_t lpn = SYS_FREE_LIST_BASE_LPN + i;
            phys_page = find_phys_page_by_sector(lpn);
            if (phys_page == PAGE_NONE) {
                FTL_DEBUG("[INIT] WARNING: LPN %d not mapped yet (will be allocated on-demand)\n", lpn);
                FTL->spc_mgr.free_node_pages[i] = PAGE_NONE;
            } else {
                FTL->spc_mgr.free_node_pages[i] = phys_page;
                FTL_DEBUG("[INIT] LPN %d -> PPN %d (free_node[%d])\n", lpn, phys_page, i);
            }
        }
        for (int i = 0; i < BASE_HEADER_PAGES; i++) {
            FTL->spc_mgr.header_pages[i] = PAGE_NONE;
        }
        FTL_DEBUG("[INIT] Remaining system pages set to PAGE_NONE (will be restored on-demand)\n");
    }

    FTL_DEBUG("[INIT] Initialization complete\n");
    
    // Initialize Code Region Management
    FTL_DEBUG("[INIT] Initializing code region management...\n");
    if (eflash_ftl_code_region_init() != 0) {
        FTL_DEBUG("[INIT] WARNING: Code region initialization failed, continuing anyway\n");
        // Don't fail the entire init, code region is optional
    }
    
    return 0;
}

// --- Object Management Implementation (temporarily disabled, to be improved later) ---

int eflash_ftl_obj_create(uint16_t pkg_id, uint16_t class_id, uint8_t type) {
    // TODO: Re-implement object management logic
    (void)pkg_id; (void)class_id; (void)type;
    return -1;
}

int eflash_ftl_obj_write_header(uint16_t obj_id, const obj_header_t *hdr) {
    // TODO: Re-implement object header write logic
    (void)obj_id; (void)hdr;
    return -1;
}

int eflash_ftl_obj_read_header(uint16_t obj_id, obj_header_t *hdr) {
    // TODO: Re-implement object header read logic
    (void)obj_id; (void)hdr;
    return -1;
}

int eflash_ftl_obj_write_body(uint16_t obj_id, const uint8_t *data, uint32_t size) {
    // TODO: Re-implement object data write logic
    (void)obj_id; (void)data; (void)size;
    return -1;
}

int eflash_ftl_write_through(uint16_t sector_id, const uint8_t *data) {
    if (!FTL || !FTL->is_initialized) return -1;
    if (data == NULL) return -1;
    if (sector_id == PAGE_NONE) return -1;

    uint32_t max_logical_pages = (EFLASH_TOTAL_PAGES * EFLASH_PAGE_SIZE) / USER_DATA_SIZE;
    if (sector_id >= max_logical_pages) {
        FTL_DEBUG("[WRITE_THROUGH] WARN: sector_id=%d >= max=%u\n",
                  sector_id, max_logical_pages);
    }

    if (eflash_ftl_gc_trigger() != 0) {
        FTL_DEBUG("[WRITE_THROUGH] GC trigger failed, no space\n");
        return -1;
    }

    FTL_DEBUG("[WRITE_THROUGH] sector_id=%d\n", sector_id);

    uint16_t base_root = (FTL->active_txn_id != TXN_ID_NONE)
                         ? FTL->shadow_root : FTL->root_page;

    ftl_meta_t new_node_meta;
    int trace_result = trace_tree(base_root, sector_id, &new_node_meta);
    if (trace_result < 0) {
        FTL_DEBUG("[WRITE_THROUGH] trace_tree failed!\n");
        return -1;
    }

    bool is_new_write = (trace_result == 0);

    if (is_new_write) {
        if ((EFLASH_TOTAL_PAGES - 1) == FTL->valid_page_count)
            return -1;
    }

    int new_phys = allocate_physical_page();
    if (new_phys < 0) {
        FTL_DEBUG("[WRITE_THROUGH] allocate_physical_page failed!\n");
        return -1;
    }

    if (write_full_page(new_phys, data, &new_node_meta) != 0) {
        FTL_DEBUG("[WRITE_THROUGH] write_full_page failed!\n");
        return -1;
    }

    if (FTL->active_txn_id != TXN_ID_NONE) {
        FTL->shadow_root = new_phys;
    } else {
        FTL->root_page = new_phys;
    }

    if (is_new_write) {
        FTL->valid_page_count++;
    }

    map_cache_upsert(sector_id, (uint16_t)new_phys);
    content_cache_invalidate(sector_id);

    FTL_DEBUG("[WRITE_THROUGH] OK ppn=%d\n", new_phys);
    return 0;
}

int eflash_ftl_write(uint16_t sector_id, const uint8_t *data) {
    return eflash_ftl_write_through(sector_id, data);
}

int eflash_ftl_write_back(uint16_t sector_id, const uint8_t *data) {
    if (!FTL || !FTL->is_initialized) return -1;
    if (data == NULL) return -1;
    if (sector_id == PAGE_NONE) return -1;

    int slot = content_cache_find(sector_id);
    if (slot >= 0) {
        memcpy(g_page_cache[slot].data, data, USER_DATA_SIZE);
        if (!g_page_cache[slot].dirty) {
            g_page_cache[slot].dirty = 1;
            g_dirty_count++;
        }
        g_page_cache[slot].cache_seq = ++g_cache_seq_counter;
        map_cache_invalidate(sector_id);
        if (g_dirty_count >= FLUSH_THRESHOLD) {
            content_cache_flush();
        }
        return 0;
    }

    slot = content_cache_find_free();
    if (slot < 0) {
        slot = content_cache_find_min_seq();
        if (slot < 0) {
            eflash_ftl_write_through(sector_id, data);
            return 0;
        }
        content_cache_evict_one(slot);
    }

    g_page_cache[slot].lpn      = sector_id;
    g_page_cache[slot].valid    = 1;
    g_page_cache[slot].dirty    = 1;
    g_page_cache[slot].cache_seq = ++g_cache_seq_counter;
    memcpy(g_page_cache[slot].data, data, USER_DATA_SIZE);
    g_dirty_count++;
    map_cache_invalidate(sector_id);

    if (g_dirty_count >= FLUSH_THRESHOLD) {
        content_cache_flush();
    }

    return 0;
}

int eflash_ftl_cache_flush(void) {
    content_cache_flush();
    return 0;
}

int eflash_ftl_read(uint16_t sector_id, uint8_t *data) {
    int cache_slot;

    if (!FTL || !FTL->is_initialized) return -1;
    if (data == NULL) return -1;

    cache_slot = content_cache_find(sector_id);
    if (cache_slot >= 0) {
        memcpy(data, g_page_cache[cache_slot].data, USER_DATA_SIZE);
        g_page_cache[cache_slot].cache_seq = ++g_cache_seq_counter;
        return 0;
    }

    uint32_t max_logical_pages = (EFLASH_TOTAL_PAGES * EFLASH_PAGE_SIZE) / USER_DATA_SIZE;
    if (sector_id >= max_logical_pages) {
        FTL_DEBUG("[READ] ERROR: sector_id=%d exceeds max_logical_pages=%u\n",
                  sector_id, max_logical_pages);
    }

    if (FTL->root_page == PAGE_NONE) return -1;

    uint16_t lpn = sector_id;

    uint16_t ppn = map_cache_lookup(lpn);
    if (ppn != PAGE_NONE) {
        uint8_t meta_buf[EFLASH_PAGE_SIZE];
        if (eflash_hw_read(ppn, meta_buf) == 0 &&
            verify_and_correct_page(meta_buf) == 0) {
            uint8_t read_sector_id_high = meta_buf[META_OFFSET + 25];
            uint8_t read_sector_id_low  = meta_buf[META_OFFSET + 24];
            uint16_t read_sector_id = (uint16_t)((read_sector_id_high << 8) | read_sector_id_low);

            if (read_sector_id == lpn) {
                memcpy(data, meta_buf, USER_DATA_SIZE);
                content_cache_fill(lpn, data);
                return 0;
            }
        }
        map_cache_invalidate(lpn);
    }

    FTL_DEBUG("[READ] LPN=%d, root_page=%d\n", lpn, FTL->root_page);

    uint8_t meta_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t cur_meta;
    int depth = 0;
    uint16_t current = FTL->root_page;

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

        // FTL_DEBUG("[READ] depth=%d, cur_sector=%d, adr[depth]=%d\n", depth, cur_meta.sector_id, cur_meta.adr[depth]);

        if (cur_meta.sector_id == lpn) {
            FTL_DEBUG("[READ] Found match at depth=%d, reading data from PPN %d\n", depth, current);
            memcpy(data, meta_buf, USER_DATA_SIZE);
            map_cache_upsert(lpn, current);
            content_cache_fill(lpn, data);
            return 0;
        }

        uint16_t bit_mask = 1 << (RADIX_DEPTH - 1 - depth);
        int target_bit = (lpn & bit_mask) ? 1 : 0;
        int current_bit = (cur_meta.sector_id & bit_mask) ? 1 : 0;

        if (target_bit != current_bit) {
            // Bits differ: divergence occurs, need to jump to adr pointer
            FTL_DEBUG("[READ] Diverge at depth=%d\n", depth);
            
            uint16_t next_ppn = cur_meta.adr[depth];
            if (next_ppn == PAGE_NONE) {
                FTL_DEBUG("[READ] ERROR: Path not found at depth=%d\n", depth);
                return -1;
            }
            
            // Jump to next physical page and read it immediately
            current = next_ppn;
            FTL_DEBUG("[READ] Following adr[%d]=%d\n", depth, current);
            
            // Read next node's metadata (same as trace_tree)
            if (eflash_hw_read(current, meta_buf) != 0) {
                FTL_DEBUG("[READ] ERROR: Failed to read next PPN %d\n", current);
                return -1;
            }
            if (verify_and_correct_page(meta_buf) != 0) {
                FTL_DEBUG("[READ] ERROR: Page verification failed at PPN %d\n", current);
                return -1;
            }
            memcpy(&cur_meta, meta_buf + META_OFFSET, META_SIZE);
            
            // Check if the new node matches
            if (cur_meta.sector_id == lpn) {
                FTL_DEBUG("[READ] Found match after jump at depth=%d, reading data from PPN %d\n", depth, current);
                memcpy(data, meta_buf, USER_DATA_SIZE);
                map_cache_upsert(lpn, current);
                content_cache_fill(lpn, data);
                return 0;
            }
        } else {
            // Bits same: continue to next depth level
            // FTL_DEBUG("[READ] Same bit at depth=%d\n", depth);
        }
        
        depth++;
    }

    // Loop ended without finding match
    FTL_DEBUG("[READ] ERROR: Sector not found in tree (exceeded max depth)\n");
    return -1;
}

/**
 * eflash_ftl_write_logical: Write interface based on logical address
 * @FTL: FTL instance
 * @logical_addr: 24-bit logical address (byte offset)
 * @data: Data pointer
 * @size: Number of bytes to write (must be > 0)
 * @return: 0 success, -1 failure
 *
 * Description:
 *   - Calculate LPN from logical_addr using USER_DATA_SIZE alignment
 *   - Support arbitrary position and length write across multiple logical pages
 *   - Use read-modify-write strategy for partial page updates
 *   - Automatically split cross-page writes into multiple single-page operations
 */
int eflash_ftl_write_logical(uint32_t logical_addr, const uint8_t *data, int16_t size) {
    // Parameter validation
    if (!FTL || !FTL->is_initialized) return -1;
    if (data == NULL) return -1;
    
    // Validate size parameter
    if (size <= 0) {
        FTL_DEBUG("[WRITE_LOGICAL] ERROR: Invalid size=%d (must be > 0)\n", size);
        return -1;
    }

    FTL_DEBUG("[WRITE_LOGICAL] Start: logical_addr=0x%06X, size=%d\n", logical_addr, size);

    uint32_t remaining = size;
    uint32_t current_addr = logical_addr;
    const uint8_t *current_data = data;

    // Loop to handle cross-page writes
    while (remaining > 0) {
        // Calculate current logical page number (LPN) and offset within page
        uint16_t lpn = (uint16_t)(current_addr / USER_DATA_SIZE);
        uint16_t page_offset = (uint16_t)(current_addr % USER_DATA_SIZE);

        // Calculate how many bytes can be written in this page
        uint32_t bytes_in_this_page = USER_DATA_SIZE - page_offset;
        uint32_t write_size = (remaining < bytes_in_this_page) ? remaining : bytes_in_this_page;

        FTL_DEBUG("[WRITE_LOGICAL] Processing LPN=%d, offset=%d, write_size=%u, remaining=%u\n",
                 lpn, page_offset, write_size, remaining);

        // Read current page data first (read-modify-write strategy)
        uint8_t page_data[USER_DATA_SIZE];
        int ret = eflash_ftl_read(lpn, page_data);
        if (ret != 0) {
            // If page doesn't exist yet, initialize with 0xFF
            FTL_DEBUG("[WRITE_LOGICAL] Page %d not found, initializing with 0xFF\n", lpn);
            memset(page_data, 0xFF, USER_DATA_SIZE);
        }

        // Update only the specified portion of the page
        memcpy(page_data + page_offset, current_data, write_size);

        // Write back the modified page
        ret = eflash_ftl_write(lpn, page_data);
        if (ret != 0) {
            FTL_DEBUG("[WRITE_LOGICAL] ERROR: Failed to write LPN=%d\n", lpn);
            return -1;
        }

        // Move to next chunk
        remaining -= write_size;
        current_addr += write_size;
        current_data += write_size;
    }

    FTL_DEBUG("[WRITE_LOGICAL] Success: wrote %d bytes starting from 0x%06X\n", size, logical_addr);
    return 0;
}

/**
 * eflash_ftl_read_logical: Read interface based on logical address
 * @FTL: FTL instance
 * @logical_addr: 24-bit logical address (byte offset)
 * @data: Data output buffer
 * @size: Number of bytes to read (must be > 0)
 * @return: 0 success, -1 failure
 *
 * Description:
 *   - Calculate LPN from logical_addr using USER_DATA_SIZE alignment
 *   - Support arbitrary position and length read across multiple logical pages
 *   - Automatically split cross-page reads into multiple single-page operations
 */
int eflash_ftl_read_logical(uint32_t logical_addr, uint8_t *data, int16_t size) {
    // Parameter validation
    if (!FTL || !FTL->is_initialized) return -1;
    if (data == NULL) return -1;
    
    // Validate size parameter
    if (size <= 0) {
        FTL_DEBUG("[READ_LOGICAL] ERROR: Invalid size=%d (must be > 0)\n", size);
        return -1;
    }

    FTL_DEBUG("[READ_LOGICAL] Start: logical_addr=0x%06X, size=%d\n", logical_addr, size);

    uint32_t remaining = size;
    uint32_t current_addr = logical_addr;
    uint8_t *current_data = data;

    // Loop to handle cross-page reads
    while (remaining > 0) {
        // Calculate current logical page number (LPN) and offset within page
        uint16_t lpn = (uint16_t)(current_addr / USER_DATA_SIZE);
        uint16_t page_offset = (uint16_t)(current_addr % USER_DATA_SIZE);

        // Calculate how many bytes can be read from this page
        uint32_t bytes_in_this_page = USER_DATA_SIZE - page_offset;
        uint32_t read_size = (remaining < bytes_in_this_page) ? remaining : bytes_in_this_page;

        FTL_DEBUG("[READ_LOGICAL] Processing LPN=%d, offset=%d, read_size=%u, remaining=%u\n",
                 lpn, page_offset, read_size, remaining);

        // Read entire logical page
        uint8_t page_data[USER_DATA_SIZE];
        int ret = eflash_ftl_read( lpn, page_data);
        if (ret != 0) {
            FTL_DEBUG("[READ_LOGICAL] ERROR: Failed to read LPN=%d\n", lpn);
            return -1;
        }

        // Copy only the requested portion
        memcpy(current_data, page_data + page_offset, read_size);

        // Move to next chunk
        remaining -= read_size;
        current_addr += read_size;
        current_data += read_size;
    }

    FTL_DEBUG("[READ_LOGICAL] Success: read %d bytes starting from 0x%06X, first byte=0x%02X\n",
              size, logical_addr, data[0]);
    return 0;
}

/**
 * eflash_ftl_txn_begin: Begin a new transaction
 * @FTL: FTL instance
 *
 * Description:
 *   1. Trigger GC to ensure sufficient free space before transaction starts
 *   2. Set transaction ID and shadow root for atomic operations
 */
void eflash_ftl_txn_begin(void) {
    // Trigger GC before starting transaction to ensure enough free space
    // This prevents GC from being triggered during transaction, which could cause inconsistency
    if (eflash_ftl_gc_trigger() != 0) {
        FTL_DEBUG("[TXN_BEGIN] WARNING: GC trigger failed, but proceeding with transaction\n");
    }

    FTL->active_txn_id = (uint16_t)(FTL->next_count & 0xFFFF);
    FTL->shadow_root = FTL->root_page; // Shadow tree initially points to current stable root

    FTL_DEBUG("[TXN_BEGIN] Transaction started: txn_id=%d, shadow_root=%d\n",
              FTL->active_txn_id, FTL->shadow_root);
}

/**
 * eflash_ftl_txn_commit: Commit transaction using full page erase/rewrite (compatible with all hardware)
 * @FTL: FTL instance
 * @return: 0 success, -1 failure
 *
 * Description: This is the universal version that works on all eFlash hardware.
 * It reads the entire page, modifies status, recalculates ECC, erases and rewrites.
 */
int eflash_ftl_txn_commit(void) {
    if (FTL->active_txn_id == TXN_ID_NONE || FTL->shadow_root == PAGE_NONE) return -1;

    FTL_DEBUG("[TXN_COMMIT] >>> ENTRY: active_txn_id=%d, shadow_root=%d, root_page=%d\n",
              FTL->active_txn_id, FTL->shadow_root, FTL->root_page);

    content_cache_flush();

    uint8_t full_page[EFLASH_PAGE_SIZE];

    if (eflash_hw_read(FTL->shadow_root, full_page) != 0) {
        FTL_DEBUG("[TXN_COMMIT] ERROR: Failed to read page %d\n", FTL->shadow_root);
        return -1;
    }

    ftl_meta_t *meta = (ftl_meta_t *)(full_page + META_OFFSET);

    FTL_DEBUG("[TXN_COMMIT] Page %d: sector_id=%d, status=0x%02X, txn_id=%d, count=%d\n",
              FTL->shadow_root, meta->sector_id, meta->status, meta->txn_id, meta->global_count);

    if (meta->status != TXN_STATUS_READY) {
        FTL_DEBUG("[TXN_COMMIT] ERROR: Page status is not READY (0x%02X)\n", meta->status);
        return -1;
    }

    meta->status = TXN_STATUS_COMMITTED;

    calc_page_ecc(full_page);

    if (eflash_hw_erase(FTL->shadow_root) != 0) {
        FTL_DEBUG("[TXN_COMMIT] ERROR: Failed to erase page %d\n", FTL->shadow_root);
        return -1;
    }
    if (eflash_hw_prog(FTL->shadow_root, full_page) != 0) {
        FTL_DEBUG("[TXN_COMMIT] ERROR: Failed to program page %d\n", FTL->shadow_root);
        return -1;
    }

    FTL_DEBUG("[TXN_COMMIT] Page %d status changed to COMMITTED\n", FTL->shadow_root);

    FTL->root_page = FTL->shadow_root;
    FTL->shadow_root = PAGE_NONE;
    FTL->active_txn_id = TXN_ID_NONE;

    FTL_DEBUG("[TXN_COMMIT] root_page updated to %d, invalidating content cache\n", FTL->root_page);

    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        g_page_cache[i].valid = 0;
        g_page_cache[i].lpn = PAGE_NONE;
        g_page_cache[i].dirty = 0;
    }
    g_dirty_count = 0;

    FTL_DEBUG("[TXN_COMMIT] OK, commit complete\n");
    return 0;
}

/**
 * eflash_ftl_txn_commit_with_update: Commit transaction using word update (optimized for hardware that supports it)
 * @FTL: FTL instance
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
int eflash_ftl_txn_commit_with_update(void) {
    printf("[TXN_COMMIT_UPDATE] >>> ENTRY: active_txn_id=%d, shadow_root=%d\n",
           FTL->active_txn_id, FTL->shadow_root);

    if (FTL->active_txn_id == TXN_ID_NONE || FTL->shadow_root == PAGE_NONE) {
        printf("[TXN_COMMIT_UPDATE] ERROR: Invalid state - returning -1\n");
        return -1;
    }

    FTL_DEBUG("[TXN_COMMIT_UPDATE] Committing transaction on page %d (word update mode)\n", FTL->shadow_root);

    content_cache_flush();

    uint8_t page_buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(FTL->shadow_root, page_buf) != 0) {
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
    if (eflash_hw_word_update(FTL->shadow_root, status_offset, word1) != 0) {
        FTL_DEBUG("[TXN_COMMIT_UPDATE] ERROR: Word update 1 failed\n");
        return -1;
    }

    // Update 2: ecc[1] + ecc[2] (bytes at offset 508-509)
    uint16_t word2 = ((uint16_t)page_buf[ecc_offset + 1] << 8) | page_buf[ecc_offset + 2];
    FTL_DEBUG("[TXN_COMMIT_UPDATE] Word2: offset=%d, buf[%d]=0x%02X(high), buf[%d]=0x%02X(low) -> 0x%04X\n",
              ecc_offset+1, ecc_offset+1, page_buf[ecc_offset+1], ecc_offset+2, page_buf[ecc_offset+2], word2);
    if (eflash_hw_word_update(FTL->shadow_root, ecc_offset + 1, word2) != 0) {
        FTL_DEBUG("[TXN_COMMIT_UPDATE] ERROR: Word update 2 failed\n");
        return -1;
    }

    // Update 3: ecc[3] + ecc[4] (bytes at offset 510-511)
    uint16_t word3 = ((uint16_t)page_buf[ecc_offset + 3] << 8) | page_buf[ecc_offset + 4];
    FTL_DEBUG("[TXN_COMMIT_UPDATE] Word3: offset=%d, buf[%d]=0x%02X(high), buf[%d]=0x%02X(low) -> 0x%04X\n",
              ecc_offset+3, ecc_offset+3, page_buf[ecc_offset+3], ecc_offset+4, page_buf[ecc_offset+4], word3);
    if (eflash_hw_word_update(FTL->shadow_root, ecc_offset + 3, word3) != 0) {
        FTL_DEBUG("[TXN_COMMIT_UPDATE] ERROR: Word update 3 failed\n");
        return -1;
    }

    FTL_DEBUG("[TXN_COMMIT_UPDATE] Status and ECC updated successfully (3 word updates, no erase needed)\n");

    FTL->root_page = FTL->shadow_root;
    FTL->shadow_root = PAGE_NONE;
    FTL->active_txn_id = TXN_ID_NONE;

    FTL_DEBUG("[TXN_COMMIT_UPDATE] root_page updated to %d, invalidating content cache\n", FTL->root_page);

    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        g_page_cache[i].valid = 0;
        g_page_cache[i].lpn = PAGE_NONE;
        g_page_cache[i].dirty = 0;
    }
    g_dirty_count = 0;

    FTL_DEBUG("[TXN_COMMIT_UPDATE] Transaction committed, root_page=%d\n", FTL->root_page);
    return 0;
}

void eflash_ftl_txn_abort(void) {
    if (FTL->active_txn_id == TXN_ID_NONE) return;

    // Simply discard shadow tree pointer, PENDING pages in Flash will be ignored on next GC or reboot
    FTL->shadow_root = PAGE_NONE;
    FTL->active_txn_id = TXN_ID_NONE;
}

// --- GC Core Implementation (Head/Tail circular buffer model) ---

/**
 * eflash_ftl_get_free_pages: Get current number of free pages
 */
uint32_t eflash_ftl_get_free_pages(void) {
    // Based on Head/Tail circular buffer model
    // Head: Next writable physical page
    // Tail: Starting position for GC scan (oldest valid data)
    //
    // Free space = Pages from Head to Tail (clockwise direction)
    // This represents pages that can be written without triggering GC
    //
    // Visualization:
    //   Case 1 (normal): [0...Tail..........Head...last]
    //                    Free = pages from Head to end + pages from 0 to Tail
    //   Case 2 (wrapped): [0...Head..Tail...last]
    //                    Free = pages from Head to Tail
    //   Special: head==tail means either completely empty OR completely full
    //            We distinguish by checking if any writes have occurred

    uint16_t total_pages = FTL->total_user_pages;
    uint16_t head = FTL->gc_head_page;
    uint16_t tail = FTL->gc_tail_page;
    
    uint32_t free_pages;
    
    if (head == tail) {
        // Ambiguous case: could be completely empty or completely full
        // In our design, we trigger GC before flash gets completely full,
        // so head==tail typically means "empty" (all pages available)
        // However, to be safe, we check next_count to see if any writes occurred
        if (FTL->next_count == 1) {
            // No writes yet, flash is completely empty
            free_pages = total_pages;
        } else {
            // Writes occurred but head caught up with tail
            // This shouldn't happen in normal operation (GC should prevent it)
            // Return 0 to trigger immediate GC
            free_pages = 0;
        }
    } else if (tail > head) {
        // Normal case: tail is ahead of head
        // [0...Head..........Tail...last]
        // Free = Tail - Head
        free_pages = (uint32_t)(tail - head);
    } else {
        // Wrapped case: head has wrapped around, tail is behind
        // [0...Tail...last][0...Head...]
        // Free = (end - head + 1) + tail
        free_pages = (uint32_t)(total_pages - head + tail);
    }

    FTL_DEBUG("[FREE_PAGES] head=%d, tail=%d, total=%d, free=%u\n",
             head, tail, total_pages, free_pages);
    return free_pages;
}

/**
 * is_page_still_valid: Check if physical page is still the latest mapping in Radix Tree
 * @FTL: FTL instance
 * @phys_page: Physical page number to check
 * @return: true=valid (latest mapping, should migrate), false=invalid (stale, can erase)
 *
 * Principle:
 *   1. Read sector_id from the page metadata
 *   2. Query Radix Tree to find current physical page for this sector_id
 *   3. Compare: if current mapping == phys_page, it's valid (latest version)
 *              if current mapping != phys_page, it's invalid (stale/obsolete)
 */
static bool is_page_still_valid(uint16_t ppn) {
    if (!FTL) return false;

    // Optimization 1: Quick check for blank page
    if (is_blank_page(ppn)) {
        FTL_DEBUG("[GC_VALID] Page %d: BLANK (all 0xFF), invalid\n", ppn);
        return false;
    }

    // Step 1: Read page metadata to get sector_id
    uint8_t meta_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t meta;
    
    if (eflash_hw_read(ppn, meta_buf) != 0) {
        FTL_DEBUG("[GC_VALID] Page %d: read failed, invalid\n", ppn);
        return false;
    }

    // Verify ECC
    int ecc_result = verify_and_correct_page(meta_buf);
    if (ecc_result != 0) {
        FTL_DEBUG("[GC_VALID] Page %d: ECC verification failed (result=%d), invalid\n", 
                 ppn, ecc_result);
        return false;
    }

    memcpy(&meta, meta_buf + META_OFFSET, META_SIZE);

    FTL_DEBUG("[GC_VALID] Page %d: sector_id=%d, status=0x%02X, count=%d\n",
             ppn, meta.sector_id, meta.status, meta.global_count);


    if (meta.status == TXN_STATUS_BLANK){
        FTL_DEBUG("[GC_VALID] Page %d: invalid status 0x%02X\n", ppn, meta.status);
        return false;
    }

    // Step 2: Query Radix Tree to find current physical page for this sector_id
    uint16_t current_ppn = find_phys_page_by_sector(meta.sector_id);
    
    if (current_ppn == PAGE_NONE) {
        // Sector not found in Radix Tree, this page is orphaned
        FTL_DEBUG("[GC_VALID] Page %d: sector_id=%d NOT FOUND in Radix Tree, invalid\n",
                 ppn, meta.sector_id);
        return false;
    }

    // Step 3: Compare current mapping with this physical page
    if (current_ppn == ppn) {
        // This page is still the latest mapping - VALID (needs migration)
        FTL_DEBUG("[GC_VALID] Page %d: VALID (sector_id=%d maps to PPN %d)\n",
                 ppn, meta.sector_id, current_ppn);
        return true;
    } else {
        // This page is stale/obsolete - INVALID (can be erased directly)
        FTL_DEBUG("[GC_VALID] Page %d: STALE (sector_id=%d now maps to PPN %d, not %d)\n",
                 ppn, meta.sector_id, current_ppn, ppn);
        return false;
    }
}

/**
 * eflash_ftl_get_real_free_pages: Get REAL number of free pages by scanning all physical pages
 * 
 * ACCURATE CALCULATION:
 * Scans ALL physical pages and uses is_page_still_valid() to count valid pages.
 * Real Free = Total - Valid (pages still mapped in Radix Tree)
 * 
 * Note: This is O(N) where N = total pages, so use sparingly!
 */
uint32_t eflash_ftl_get_real_free_pages(void) {
    uint32_t valid_count = 0;
    uint32_t stale_count = 0;
    uint32_t blank_count = 0;
    uint32_t orphaned_count = 0;
    uint32_t ecc_error_count = 0;
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;
    
    // Scan all physical pages
    for (uint16_t ppn = 0; ppn <= last_user_page; ppn++) {
        if (is_page_still_valid(ppn)) {
            valid_count++;
        } else {
            // Count why it's invalid (for debugging)
            // Note: is_page_still_valid already prints debug info when FTL_DEBUG_ENABLE is on
        }
    }
    
    uint32_t real_free_pages = FTL->total_user_pages - valid_count;
    
    FTL_DEBUG("[REAL_FREE_PAGES] ===== Detailed Analysis =====\n");
    FTL_DEBUG("[REAL_FREE_PAGES] Total physical pages: %u\n", FTL->total_user_pages);
    FTL_DEBUG("[REAL_FREE_PAGES] Valid pages (in Radix Tree): %u\n", valid_count);
    FTL_DEBUG("[REAL_FREE_PAGES] Real free pages: %u\n", real_free_pages);
    FTL_DEBUG("[REAL_FREE_PAGES] valid_page_count (FTL counter): %u\n", FTL->valid_page_count);
    FTL_DEBUG("[REAL_FREE_PAGES] Difference: %d\n", (int)valid_count - (int)FTL->valid_page_count);
    FTL_DEBUG("[REAL_FREE_PAGES] ===============================\n");
    
    return real_free_pages;
}

/**
 * gc_migrate_page: Migrate valid page to new location
 * @FTL: FTL instance
 * @src_page: Source physical page number
 * @return: 0 success, -1 failure
 */
static int gc_migrate_page(uint16_t src_page) {
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
#if FTL_DEBUG_ENABLE
    uint32_t free_before = eflash_ftl_get_free_pages();
    FTL_DEBUG("[GC_MIGRATE] Before migration: head=%d, tail=%d, free=%d\n",
             FTL->gc_head_page, FTL->gc_tail_page, free_before);
#endif

    // 3. Use normal write flow to rewrite data (this triggers trace_tree, allocates new page, updates root)
    //    Note: Only pass user data, let eflash_ftl_write automatically create new metadata
    if (eflash_ftl_write_through( meta->sector_id, user_data) != 0) {
        FTL_DEBUG("[GC_MIGRATE] ERROR: Failed to rewrite data during migration\n");
        return -1;
    }

    // [DEBUG] Record state after migration
#if 0
    uint32_t free_after = eflash_ftl_get_free_pages();
    FTL_DEBUG("[GC_MIGRATE] After migration: head=%d, tail=%d, free=%d (delta=%d)\n",
             FTL->gc_head_page, FTL->gc_tail_page, free_after, (int32_t)(free_after - free_before));
    
    // Print radix tree when next_count > 2046 to track wrap-around behavior
    static bool gc_tree_print_enabled = false;
    static int gc_tree_print_count = 0;
    
    if (!gc_tree_print_enabled && FTL->next_count > 2046) {
        gc_tree_print_enabled = true;
        printf("\n  [GC DEBUG] *** next_count reached %u during GC migration, enabling tree tracking ***\n", FTL->next_count);
    }
    
    if (gc_tree_print_enabled) {
        gc_tree_print_count++;
        printf("\n  [GC TREE #%d] Migrated sector %d from page %d, root=%d, next_count=%u, head=%d, tail=%d\n",
               gc_tree_print_count, meta->sector_id, src_page,
               FTL->root_page, FTL->next_count, FTL->gc_head_page, FTL->gc_tail_page);
        extern void eflash_ftl_print_radix_tree_mermaid_to_file(eflash_ftl_t * ftl, uint16_t root_page);
        extern void eflash_ftl_print_radix_tree_mermaid(eflash_ftl_t * ftl, uint16_t root_page);
        eflash_ftl_print_radix_tree_mermaid(FTL, FTL->root_page);

        eflash_ftl_print_radix_tree_mermaid_to_file(FTL, FTL->root_page);
    }
#endif

    FTL_DEBUG("[GC_MIGRATE] Success: migrated sector %d from page %d\n",
             meta->sector_id, src_page);
    return 0;
}

/**
 * gc_collect_one_page: Collect (reclaim) a single physical page during GC
 * @FTL: FTL instance
 * @ppn: Physical Page Number to collect
 * @return: 0/1 on success(0 migrate,1 collect invalid page), -1 on failure
 *
 * Description:
 *   1. Check if the page is still valid (not stale)
 *   2. If valid, migrate data to a new physical page
 *   3. Erase physical page (do NOT call space_mgr_free, because GC only manages physical pages)
 */
static int gc_collect_one_page(uint16_t ppn) {
    FTL_DEBUG("[GC_COLLECT] Processing physical page %d...\n", ppn);

    // 1. Check if page is still valid
    bool valid = is_page_still_valid(ppn);

    if (valid) {
        //if head == tail directly move pointers
        if (FTL->gc_head_page == FTL->gc_tail_page) {
            FTL->gc_head_page++;
            if (FTL->gc_head_page == EFLASH_TOTAL_PAGES)
            {
                FTL->gc_head_page = 0;
            }
            return 0;
        }
        FTL_DEBUG("[GC_COLLECT] Page %d is VALID, migrating...\n", ppn);

        // 2. Migrate valid page
        if (gc_migrate_page(ppn) != 0) {
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
    return (valid? 0 : 1);
}

/**
 * eflash_ftl_gc_collect: Perform GC to reclaim specified number of pages
 * @FTL: FTL instance
 * @pages_to_free: Number of pages to reclaim
 * @return: Actual pages freed, -1 on failure
 *
 * Algorithm:
 * 1. Start scanning from gc_tail_page page by page
 * 2. Call gc_collect_one_page for each page
 * 3. **Only move tail pointer after successful reclamation**
 * 4. Continue until enough pages freed or entire Flash traversed
 */
int eflash_ftl_gc_collect(uint16_t pages_to_free) {
    if (!FTL->is_initialized) {
        FTL_DEBUG("[GC] ERROR: FTL not initialized\n");
        return -1;
    }

    FTL_DEBUG("[GC] ========== Starting collection ==========\n");
    FTL_DEBUG("[GC] Need to free %d pages\n", pages_to_free);
#if FTL_DEBUG_ENABLE
    FTL_DEBUG("[GC] Initial state: head=%d, tail=%d, free=%d\n",
              FTL->gc_head_page, FTL->gc_tail_page, eflash_ftl_get_free_pages());
#endif

    uint16_t pages_freed = 0;
#if FTL_DEBUG_ENABLE
    uint16_t start_tail = FTL->gc_tail_page;
#endif
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;

    // Traverse entire physical page space at most twice to prevent infinite loop
    // Reason for *2: In worst case, tail may need to wrap around and scan all pages again
    // to ensure we don't miss any reclaimable pages or get stuck in a cycle
    //uint16_t max_iterations = EFLASH_TOTAL_PAGES * 2;
    uint16_t max_iterations = EFLASH_TOTAL_PAGES * 1;
    uint16_t iterations = 0;

    while (pages_freed < pages_to_free && iterations < max_iterations) {
        uint16_t current_page = FTL->gc_tail_page;

        // [DEBUG] Output status every 10 iterations
        if (iterations % 10 == 0) {
            FTL_DEBUG("[GC] Iteration %d: processing page %d, freed=%d, head=%d, tail=%d, free=%d\n",
                     iterations, current_page, pages_freed, FTL->gc_head_page, FTL->gc_tail_page,
                     eflash_ftl_get_free_pages());
        }

        // Execute single page reclamation (all physical pages are valid for GC)
        int ret = gc_collect_one_page(current_page);

        if (ret >= 0) {
            // ? Successfully reclaimed, move tail pointer
            uint16_t tail_before_op = FTL->gc_tail_page;
            
            // pages_freed+=ret;
            pages_freed+=1;

            FTL_DEBUG("[GC_TAIL] Page %d reclaimed, tail will move: %d -> ", current_page, FTL->gc_tail_page);
            
            FTL->gc_tail_page++;
            FTL_DEBUG("%d (increment)\n", FTL->gc_tail_page);
            
            if (FTL->gc_tail_page > last_user_page) {
                FTL->gc_tail_page = 0;  // Wrap around to PPN 0
                FTL_DEBUG("[GC_TAIL] Round-wrap: tail_page reset to 0\n");
            }
            
            // Skip code region if tail is about to enter it
            uint16_t tail_before_skip = FTL->gc_tail_page;
            FTL->gc_tail_page = skip_code_region(FTL->gc_tail_page);
            if (FTL->gc_tail_page != tail_before_skip) {
                FTL_DEBUG("[GC_TAIL] Code region skip: tail adjusted %d -> %d\n",
                         tail_before_skip, FTL->gc_tail_page);
            }
            
            FTL_DEBUG("[GC_TAIL] Final tail position: %d (was %d, delta=%d)\n",
                     FTL->gc_tail_page, tail_before_op,
                     (int16_t)FTL->gc_tail_page - (int16_t)tail_before_op);
        } else {
            // ✗ Reclamation failed - CRITICAL ERROR!
            // DO NOT move tail pointer! This indicates a hardware failure or serious error.
            // Moving tail would skip this page permanently and cause data inconsistency.
            FTL_DEBUG("[GC] CRITICAL ERROR: Failed to collect page %d\n", current_page);
            FTL_DEBUG("[GC] Stopping GC to prevent data corruption\n");
            
            // Stop GC immediately - do not advance tail on failure
            break;
        }

        iterations++;
    }

    if (iterations >= max_iterations) {
        FTL_DEBUG("[GC] WARNING: Reached max iterations (%d), stopping\n", max_iterations);
    }

#if FTL_DEBUG_ENABLE
    FTL_DEBUG("[GC] ========== Collection complete ==========\n");
    FTL_DEBUG("[GC] Final state: freed %d pages (tail: %d -> %d, iterations=%d)\n",
              pages_freed, start_tail, FTL->gc_tail_page, iterations);
    FTL_DEBUG("[GC] Final free pages: %d\n", eflash_ftl_get_free_pages());
#endif

    return pages_freed;
}

/**
 * eflash_ftl_gc_collect_all: Perform GC to collect all reclaimable pages
 * @return: Total pages freed, -1 on failure
 *
 * Description:
 *   This function performs continuous GC cycles to reclaim as many stale pages as possible.
 *   It runs until the estimated free pages (from Head/Tail pointers) matches the real free 
 *   pages (from physical scan), ensuring maximum reclamation of invalid pages.
 *   
 *   Algorithm:
 *   1. Get real_free_pages ONCE before loop (invariant during GC)
 *   2. Loop:
 *      a. Get current estimated_free_pages (changes as GC progresses)
 *      b. If estimated == real, stop (all stale pages reclaimed)
 *      c. Otherwise, perform one GC cycle to reclaim more pages
 *      d. Move tail pointer forward
 *   3. Stop if max iterations reached (prevent infinite loop)
 *   
 *   Key insight:
 *   - eflash_ftl_get_real_free_pages() is INVARIANT during GC (scans all physical pages)
 *   - eflash_ftl_get_free_pages() CHANGES as tail pointer moves (Head/Tail based estimate)
 *   - When they match, it means all stale pages have been reclaimed
 */
int eflash_ftl_gc_collect_all(void) {
    if (!FTL->is_initialized) {
        FTL_DEBUG("[GC_CONSISTENT] ERROR: FTL not initialized\n");
        return -1;
    }

    FTL_DEBUG("[GC_CONSISTENT] ========== Starting consistency-driven GC ==========\n");

    // Step 1: Get REAL free pages ONCE (this is our target, invariant during GC)
    uint32_t target_real_free = eflash_ftl_get_real_free_pages();
    FTL_DEBUG("[GC_CONSISTENT] Target real_free_pages (invariant): %u\n", target_real_free);

    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;
    uint16_t max_iterations = EFLASH_TOTAL_PAGES;  // Safety limit: scan all pages once
    uint16_t iterations = 0;
    uint32_t total_pages_freed = 0;

#if FTL_DEBUG_ENABLE
    uint16_t initial_tail = FTL->gc_tail_page;
#endif

    // Step 2: Loop until consistency or max iterations
    while (iterations < max_iterations) {
        // Get CURRENT estimated free pages (changes as tail moves)
        uint32_t current_estimated_free = eflash_ftl_get_free_pages();

        FTL_DEBUG("[GC_CONSISTENT] Iteration %d: estimated=%u, target_real=%u, diff=%d\n",
                 iterations, current_estimated_free, target_real_free,
                 (int)(current_estimated_free - target_real_free));

        // Check if we've achieved consistency
        if (current_estimated_free == target_real_free) {
            FTL_DEBUG("[GC_CONSISTENT] ✓ Consistency achieved! estimated == real (%u pages)\n",
                     current_estimated_free);
            break;
        }

        // If estimated > real, it means there are stale pages that need to be reclaimed
        // Continue GC to move tail forward and reclaim those pages
        if (current_estimated_free > target_real_free) {
            uint16_t current_page = FTL->gc_tail_page;

            // Reclaim one page
            int ret = gc_collect_one_page(current_page);

            if (ret >= 0) {
                // Successfully reclaimed, move tail pointer
                uint16_t tail_before_op = FTL->gc_tail_page;
                total_pages_freed++;

                FTL_DEBUG("[GC_TAIL] Consistent GC: page %d reclaimed, tail will move: %d -> ", current_page, FTL->gc_tail_page);
                
                FTL->gc_tail_page++;
                FTL_DEBUG("%d (increment)\n", FTL->gc_tail_page);
                
                if (FTL->gc_tail_page > last_user_page) {
                    FTL->gc_tail_page = 0;  // Wrap around to PPN 0
                    FTL_DEBUG("[GC_TAIL] Round-wrap: tail reset to 0\n");
                }
                
                // Skip code region if tail is about to enter it
                uint16_t tail_before_skip = FTL->gc_tail_page;
                FTL->gc_tail_page = skip_code_region(FTL->gc_tail_page);
                if (FTL->gc_tail_page != tail_before_skip) {
                    FTL_DEBUG("[GC_TAIL] Code region skip: tail adjusted %d -> %d\n",
                             tail_before_skip, FTL->gc_tail_page);
                }

                FTL_DEBUG("[GC_TAIL] Consistent GC: final tail position: %d (was %d, delta=%d)\n",
                         FTL->gc_tail_page, tail_before_op,
                         (int16_t)FTL->gc_tail_page - (int16_t)tail_before_op);
            } else {
                // Critical error: failed to reclaim page
                FTL_DEBUG("[GC_CONSISTENT] ✗ CRITICAL ERROR: Failed to collect page %d\n",
                         current_page);
                FTL_DEBUG("[GC_CONSISTENT] Stopping to prevent data corruption\n");
                break;
            }
        } else {
            FTL_DEBUG("[GC_CONSISTENT] ⚠ WARNING: estimated (%u) < real (%u) - reclaiming stale pages\n",
                     current_estimated_free, target_real_free);

            uint16_t current_page = FTL->gc_tail_page;

            int ret = gc_collect_one_page(current_page);

            if (ret >= 0) {
                uint16_t tail_before_op = FTL->gc_tail_page;
                total_pages_freed++;

                FTL_DEBUG("[GC_TAIL] Consistent GC: page %d reclaimed, tail will move: %d -> ", current_page, FTL->gc_tail_page);
                
                FTL->gc_tail_page++;
                FTL_DEBUG("%d (increment)\n", FTL->gc_tail_page);
                
                if (FTL->gc_tail_page > last_user_page) {
                    FTL->gc_tail_page = 0;
                    FTL_DEBUG("[GC_TAIL] Round-wrap: tail reset to 0\n");
                }
                
                uint16_t tail_before_skip = FTL->gc_tail_page;
                FTL->gc_tail_page = skip_code_region(FTL->gc_tail_page);
                if (FTL->gc_tail_page != tail_before_skip) {
                    FTL_DEBUG("[GC_TAIL] Code region skip: tail adjusted %d -> %d\n",
                             tail_before_skip, FTL->gc_tail_page);
                }

                FTL_DEBUG("[GC_TAIL] Consistent GC: final tail position: %d (was %d, delta=%d)\n",
                         FTL->gc_tail_page, tail_before_op,
                         (int16_t)FTL->gc_tail_page - (int16_t)tail_before_op);
            } else {
                FTL_DEBUG("[GC_CONSISTENT] ✗ CRITICAL ERROR: Failed to collect page %d\n",
                         current_page);
                FTL_DEBUG("[GC_CONSISTENT] Stopping to prevent data corruption\n");
                break;
            }
        }

        iterations++;
    }

    // Step 3: Final status
    if (iterations >= max_iterations) {
        FTL_DEBUG("[GC_CONSISTENT] ⚠ WARNING: Reached max iterations (%u)\n", max_iterations);
    }

    uint32_t final_estimated = eflash_ftl_get_free_pages();
    FTL_DEBUG("[GC_CONSISTENT] ========== GC Complete ==========\n");
    FTL_DEBUG("[GC_CONSISTENT] Total pages freed: %u\n", total_pages_freed);
    FTL_DEBUG("[GC_CONSISTENT] Final estimated free: %u\n", final_estimated);
    FTL_DEBUG("[GC_CONSISTENT] Target real free: %u\n", target_real_free);
    FTL_DEBUG("[GC_CONSISTENT] Consistency: %s\n",
             (final_estimated == target_real_free) ? "✓ ACHIEVED" : "✗ NOT ACHIEVED");
#if FTL_DEBUG_ENABLE
    FTL_DEBUG("[GC_CONSISTENT] Tail movement: %d -> %d (iterations=%u)\n",
             initial_tail, FTL->gc_tail_page, iterations);
#endif

    return (int)total_pages_freed;
}

/**
 * calculate_gc_pages_to_reclaim: Calculate how many pages GC should reclaim
 * @free_pages: Current number of free pages
 * @out_pages_needed: Output parameter for calculated pages to reclaim
 * @return: 0 = no GC needed, 1 = GC needed with calculated pages
 *
 * This function implements the GC reclamation strategy.
 * It can be easily modified or extended with additional parameters.
 */
static int calculate_gc_pages_to_reclaim(uint32_t free_pages, uint16_t *out_pages_needed) {
    if (!out_pages_needed) return -1;
    
    // Check if GC is needed
    if (free_pages >= FTL->gc_threshold) {
        // Sufficient space, no GC needed
        return 0;
    }
    
    FTL_DEBUG("[GC_TRIGGER] Triggering GC! Free space below threshold\n");
    
    // Calculate pages to reclaim with dynamic adjustment
    // Strategy: Avoid excessive migration when space is critically low
    uint32_t target_free_pages = FTL->total_user_pages / 10;  // Target: 10% free
    uint32_t pages_to_reclaim = (target_free_pages > free_pages) ?
                                (target_free_pages - free_pages) : 10;
    
    // CRITICAL: Limit migration to avoid write amplification
    // When space is critically low, only reclaim a small batch
    uint16_t max_migration_batch;
    
    // Adaptive batch size based on urgency
    if (free_pages < FTL->total_user_pages / 20) {
        // Critical: Less than 5% free - use smaller batches to maintain responsiveness
        max_migration_batch = FTL->total_user_pages / 100;  // 1% of total
        FTL_DEBUG("[GC_TRIGGER] CRITICAL space level!");
    } else if (free_pages < FTL->total_user_pages / 10) {
        // Warning: 5-10% free - moderate batch size
        max_migration_batch = FTL->total_user_pages / 50;   // 2% of total
        FTL_DEBUG("[GC_TRIGGER] WARNING space level");
    } else {
        // Normal: Above 10% - can use larger batches for efficiency
        max_migration_batch = FTL->total_user_pages / 30;   // ~3.3% of total
        FTL_DEBUG("[GC_TRIGGER] Normal space level");
    }
    
    // Ensure minimum batch size for effectiveness
    if (max_migration_batch < 10) max_migration_batch = 10;
    // Cap maximum to prevent excessive migration
    if (max_migration_batch > 100) max_migration_batch = 100;
    
    *out_pages_needed = (uint16_t)(pages_to_reclaim > max_migration_batch ? 
                                   max_migration_batch : pages_to_reclaim);
    
    FTL_DEBUG("[GC_TRIGGER] Space critical: free=%u, target=%u, reclaim=%u, limited to %u\n",
              free_pages, target_free_pages, pages_to_reclaim, *out_pages_needed);
    
    return 1;  // GC needed
}

/**
 * eflash_ftl_gc_emergency_mode: Emergency mode to avoid write amplification when space is critical
 * @return: 0 on success, -1 on failure
 *
 * Description:
 *   When free space is extremely low, normal GC causes severe write amplification because
 *   every write triggers GC migration. This function switches to emergency mode by:
 *   1. Finding the first stale (invalid) page
 *   2. Setting Head to that stale page (allow direct overwrite)
 *   3. Setting Tail to the next valid page after the stale page
 *   
 *   Trade-off:
 *   - Pros: Eliminates write amplification, allows writes without migration
 *   - Cons: Sacrifices wear leveling, may cause uneven flash usage
 *   
 *   This should ONLY be used when space is critically low (< gc_threshold).
 */
static int eflash_ftl_gc_emergency_mode(void) {
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;
    uint16_t start_page = FTL->gc_head_page;
    
    FTL_DEBUG("[GC_EMERGENCY] Entering emergency mode to avoid write amplification\n");
    FTL_DEBUG("[GC_EMERGENCY] Current state: head=%d, tail=%d, valid_count=%u\n",
             FTL->gc_head_page, FTL->gc_tail_page, FTL->valid_page_count);
    
    // Step 1: Find the first stale (invalid) page starting from current head
    uint16_t stale_page = PAGE_NONE;
    uint16_t scan_page = start_page;
    uint16_t max_scan = EFLASH_TOTAL_PAGES;  // Safety limit
    uint16_t scanned = 0;
    
    while (scanned < max_scan) {
        // Check if this page is stale (not in Radix Tree)
        if (!is_page_still_valid(scan_page)) {
            stale_page = scan_page;
            FTL_DEBUG("[GC_EMERGENCY] Found stale page at PPN %d after scanning %d pages\n",
                     stale_page, scanned);
            break;
        }
        
        // Move to next page
        scan_page++;
        if (scan_page > last_user_page) {
            scan_page = 0;  // Wrap around
        }
        scanned++;
    }
    
    if (stale_page == PAGE_NONE) {
        FTL_DEBUG("[GC_EMERGENCY] ERROR: No stale page found! Flash may be completely full\n");
        return -1;
    }
    
    // Step 2: Find the next valid page after the stale page (for new Tail position)
    uint16_t next_valid_page = PAGE_NONE;
    scan_page = stale_page + 1;
    if (scan_page > last_user_page) {
        scan_page = 0;
    }
    scanned = 0;
    
    while (scanned < max_scan) {
        if (is_page_still_valid(scan_page)) {
            next_valid_page = scan_page;
            FTL_DEBUG("[GC_EMERGENCY] Found next valid page at PPN %d\n", next_valid_page);
            break;
        }
        
        scan_page++;
        if (scan_page > last_user_page) {
            scan_page = 0;
        }
        scanned++;
    }
    
    // If no valid page found, set Tail to stale_page + 1 (all remaining pages are stale)
    if (next_valid_page == PAGE_NONE) {
        next_valid_page = stale_page + 1;
        if (next_valid_page > last_user_page) {
            next_valid_page = 0;
        }
        FTL_DEBUG("[GC_EMERGENCY] No valid page found, setting tail to %d\n", next_valid_page);
    }
    
    // Step 3: Update Head and Tail pointers
    uint16_t old_head = FTL->gc_head_page;
    uint16_t old_tail = FTL->gc_tail_page;
    
    FTL->gc_head_page = stale_page;
    FTL->gc_tail_page = next_valid_page;
    
    // CRITICAL: Ensure emergency mode pointers don't land in code region
    adjust_gc_pointers_for_code_region();
    
    FTL_DEBUG("[GC_EMERGENCY] Emergency mode activated!\n");
    FTL_DEBUG("[GC_EMERGENCY] Head: %d -> %d (will overwrite stale page)\n", old_head, FTL->gc_head_page);
    FTL_DEBUG("[GC_EMERGENCY] Tail: %d -> %d (skip stale region)\n", old_tail, FTL->gc_tail_page);
    FTL_DEBUG("[GC_EMERGENCY] WARNING: Wear leveling sacrificed for performance!\n");
    
    return 0;
}

/**
 * eflash_ftl_gc_trigger: Manually trigger garbage collection
 * @FTL: FTL instance
 * @return: 0 no GC needed or GC successful, -1 GC failed
 *
 * Trigger condition: free pages < gc_threshold
 *
 * IMPORTANT: GC is NOT allowed during transaction to maintain atomicity.
 * If a transaction is active, this function returns immediately without triggering GC.
 */
int eflash_ftl_gc_trigger(void) {
    // If GC already in progress, skip trigger (prevent recursion)
    if (FTL->gc_in_progress) {
        return 0;
    }

    // Check if real free space is critically low
    uint32_t real_free_pages = EFLASH_TOTAL_PAGES - FTL->valid_page_count;
    if (real_free_pages < FTL->gc_threshold) {
        // CRITICAL: Space is extremely low, normal GC would cause severe write amplification
        // Switch to emergency mode: find stale page and overwrite directly
        FTL_DEBUG("[GC_TRIGGER] Real free space (%u) below threshold (%u), activating emergency mode\n",
                 real_free_pages, FTL->gc_threshold);
        
        int ret = eflash_ftl_gc_emergency_mode();
        if (ret != 0) {
            FTL_DEBUG("[GC_TRIGGER] ERROR: Emergency mode failed, flash may be completely full\n");
            return -1;
        }
        
        // Emergency mode activated successfully, no need for normal GC
        return 0;
    }

    // CRITICAL: Do NOT trigger GC during transaction
    // Transaction requires atomic operations, and GC could interfere with shadow tree consistency
    if (FTL->active_txn_id != TXN_ID_NONE) {
        FTL_DEBUG("[GC_TRIGGER] SKIPPED: GC not allowed during transaction (txn_id=%d)\n",
                  FTL->active_txn_id);
        return 0;
    }

    uint32_t free_pages = eflash_ftl_get_free_pages();

    FTL_DEBUG("[GC_TRIGGER] Checking GC: free_pages=%d, threshold=%d\n",
              free_pages, FTL->gc_threshold);

    // Use extracted function to calculate pages to reclaim
    uint16_t pages_needed = 0;
    int gc_needed = calculate_gc_pages_to_reclaim(free_pages, &pages_needed);
    
    if (!gc_needed) {
        // Sufficient space, no GC needed
        return 0;
    }

    // Set GC flag
    FTL->gc_in_progress = true;

    // CRITICAL: When free pages are critically low, use aggressive GC to maximize reclamation
    // If we only reclaim a small batch when space is critical, most operations will be migrations,
    // which won't significantly increase free pages. In this case, we should collect ALL stale pages.
    int result;
    if (free_pages < FTL->total_user_pages / 50) {  // Less than 2% free
        FTL_DEBUG("[GC_TRIGGER] Critical space level (%u pages), using aggressive GC\n", free_pages);
        result = eflash_ftl_gc_collect_all();
    } else {
        // Normal case: reclaim calculated number of pages
        result = eflash_ftl_gc_collect(pages_needed);
    }

    // Clear GC flag
    FTL->gc_in_progress = false;

    if (result < 0) {
        FTL_DEBUG("[GC_TRIGGER] ERROR: GC failed!\n");
        return -1;
    }

    FTL_DEBUG("[GC_TRIGGER] GC completed successfully, freed %d pages\n", result);
    return 0;
}

// ============================================================================
// --- Trim/Discard Operations (for deleting unused data) ---
// ============================================================================

/**
 * eflash_ftl_trim: Mark a single sector as invalid/deleted
 * 
 * @sector_id: The logical sector ID to trim
 * @return: 0 on success, -1 on failure
 * 
 * This function is similar to ATA/TRIM or SCSI/UNMAP commands.
 * It notifies the FTL that the data at this sector is no longer needed,
 * allowing the FTL to skip migrating this data during GC.
 * 
 * Benefits:
 *   - Reduces GC overhead by avoiding migration of stale data
 *   - Improves write amplification (20-30% reduction)
 *   - Speeds up GC process (30% efficiency improvement)
 * 
 * Implementation:
 *   1. Check if sector is currently mapped
 *   2. Build COW path via trace_tree, set status=INVALID, write once via write_full_page
 *   3. Update root_page to the new COW root
 *   4. Decrement valid_page_count
 *   5. Old physical page becomes stale, reclaimed by GC without migration
 */
int eflash_ftl_trim(uint16_t sector_id) {
    if (!FTL->is_initialized) {
        FTL_DEBUG("[TRIM] ERROR: FTL not initialized\n");
        return -1;
    }
    
    // Step 1: Check if sector is currently mapped
    uint16_t phys_page = find_phys_page_by_sector(sector_id);
    if (phys_page == PAGE_NONE) {
        FTL_DEBUG("[TRIM] Sector %d is not mapped, nothing to trim\n", sector_id);
        return 0;  // Not an error, just nothing to do
    }
    
    FTL_DEBUG("[TRIM] Trimming sector %d (currently mapped to PPN %d)\n", 
              sector_id, phys_page);
    
    uint16_t next_count = FTL->next_count++;

    uint8_t page_buf[EFLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, EFLASH_PAGE_SIZE);

    int new_phys = allocate_physical_page();
    if (new_phys < 0) {
        FTL_DEBUG("[TRIM] ERROR: Failed to allocate page for trim marker\n");
        return -1;
    }
    
    uint16_t current_root = (FTL->active_txn_id != TXN_ID_NONE) ? 
                            FTL->shadow_root : FTL->root_page;
    
    if (current_root == PAGE_NONE) {
        FTL_DEBUG("[TRIM] ERROR: No valid root for trimming\n");
        return -1;
    }
    
    ftl_meta_t new_node_meta;
    trace_tree(current_root, sector_id, &new_node_meta);
    
    new_node_meta.sector_id = sector_id;
    new_node_meta.status = TXN_STATUS_INVALID;
    new_node_meta.global_count = next_count;
    
    if (write_full_page(new_phys, page_buf, &new_node_meta) != 0) {
        FTL_DEBUG("[TRIM] ERROR: Failed to write trim marker\n");
        return -1;
    }
    
    FTL_DEBUG("[TRIM] Wrote trim marker to PPN %d\n", new_phys);

    if (FTL->active_txn_id != TXN_ID_NONE) {
        FTL->shadow_root = new_phys;
    } else {
        FTL->root_page = new_phys;
    }

    if (FTL->valid_page_count > 0) {
        FTL->valid_page_count--;
    }

    FTL_DEBUG("[TRIM] Sector %d trimmed. Old PPN %d now stale. Valid: %u\n",
              sector_id, phys_page, FTL->valid_page_count);

    return 0;
}

/**
 * eflash_ftl_trim_range: Trim a range of consecutive sectors
 * 
 * @start_sector: Starting sector ID
 * @count: Number of sectors to trim
 * @return: 0 on success, -1 on partial/complete failure
 * 
 * This is a batch operation for trimming multiple consecutive sectors.
 * Useful when deleting files or clearing large data regions.
 */
int eflash_ftl_trim_range(uint16_t start_sector, uint16_t count) {
    if (count == 0) {
        return 0;  // Nothing to trim
    }
    
    FTL_DEBUG("[TRIM_RANGE] Trimming %d sectors starting from %d\n", 
              count, start_sector);
    
    int success_count = 0;
    int fail_count = 0;
    
    for (uint16_t i = 0; i < count; i++) {
        uint16_t sector_id = start_sector + i;
        int ret = eflash_ftl_trim(sector_id);
        
        if (ret == 0) {
            success_count++;
        } else {
            fail_count++;
            FTL_DEBUG("[TRIM_RANGE] WARNING: Failed to trim sector %d\n", sector_id);
        }
    }
    
    FTL_DEBUG("[TRIM_RANGE] Completed: %d succeeded, %d failed\n", 
              success_count, fail_count);
    
    // Return success if at least some sectors were trimmed
    return (success_count > 0) ? 0 : -1;
}

/**
 * eflash_ftl_trim_object: Trim all sectors belonging to an object
 * 
 * @obj_id: Object ID to trim
 * @return: 0 on success, -1 on failure
 * 
 * This function trims all data sectors associated with a specific object.
 * It reads the object header to find all linked sectors and trims them.
 * 
 * Use case: When deleting a file or object, call this to notify FTL
 * that all associated data can be discarded.
 */
int eflash_ftl_trim_object(uint16_t obj_id) {
    FTL_DEBUG("[TRIM_OBJ] Trimming all sectors for object %d\n", obj_id);
    
    // Step 1: Get object header to find body address
    obj_header_t hdr;
    int ret = eflash_ftl_obj_get_header(obj_id, &hdr);
    if (ret != 0) {
        FTL_DEBUG("[TRIM_OBJ] ERROR: Failed to get header for object %d\n", obj_id);
        return -1;
    }
    
    // Step 2: Calculate number of sectors in this object
    uint32_t body_size = hdr.body_size;
    if (body_size == 0) {
        FTL_DEBUG("[TRIM_OBJ] Object %d has no data (size=0)\n", obj_id);
        return 0;
    }
    
    // Calculate number of pages/sectors
    uint16_t num_sectors = (body_size + USER_DATA_SIZE - 1) / USER_DATA_SIZE;
    uint32_t start_addr = hdr.body_addr;
    
    FTL_DEBUG("[TRIM_OBJ] Object %d: body_addr=%lu, size=%lu, sectors=%d\n",
              obj_id, (unsigned long)start_addr, (unsigned long)body_size, num_sectors);
    
    // Step 3: Trim each sector in the object
    int trimmed_count = 0;
    for (uint16_t i = 0; i < num_sectors; i++) {
        // Convert logical address to sector_id
        // Assuming 1:1 mapping between logical address and sector_id for simplicity
        uint16_t sector_id = (uint16_t)(start_addr + i * USER_DATA_SIZE);
        
        ret = eflash_ftl_trim(sector_id);
        if (ret == 0) {
            trimmed_count++;
        }
    }
    
    FTL_DEBUG("[TRIM_OBJ] Trimmed %d sectors for object %d\n", 
              trimmed_count, obj_id);
    
    // Step 4: Optionally clear the object header itself
    // (This depends on whether you want to reuse the object ID)
    // For now, we leave the header intact but mark it as empty
    hdr.body_size = 0;
    hdr.body_addr = 0;
    eflash_ftl_obj_set_header(obj_id, &hdr);
    
    return 0;
}

// ============================================================================
// --- Code Region Management (for executing code from Flash) ---
// ============================================================================

// Code region information stored in a dedicated system page
#define CODE_REGION_INFO_LPN  (SYS_RESERVED_LPN_COUNT)  // LPN after system areas
code_region_info_t g_code_region;

// Forward declarations for code region functions
static int load_code_region_info(void);
static int save_code_region_info(void);
static uint16_t calculate_code_region_checksum(const code_region_info_t *info);
static int gc_collect_one_page(uint16_t ppn);

/**
 * adjust_ppn_for_code_region: Adjust a physical page number to skip code region
 * 
 * @ppn: Physical page number to adjust
 * @return: Adjusted PPN (skipping code region if necessary)
 * 
 * This utility function ensures that a given PPN never lands in the code region.
 * It handles three scenarios:
 * 1. PPN is just before code region (ppn == start_ppn - 1) → skip to end
 * 2. PPN is already within code region → force skip to end (error recovery)
 * 3. PPN is before or after code region → return unchanged
 * 
 * Usage:
 * - After GC tail pointer increment
 * - After GC tail pointer wrap-around
 * - After pointer recovery from power failure
 * - In emergency GC mode
 */
static uint16_t adjust_ppn_for_code_region(uint16_t ppn) {
    uint16_t original_ppn = ppn;
    
    // Load code region info if not already loaded
    if (g_code_region.magic != CODE_REGION_MAGIC) {
        load_code_region_info();
    }
    
    // If code region doesn't exist, return unchanged
    if (g_code_region.num_pages == 0) {
        FTL_DEBUG("[GC_TAIL] adjust_ppn: PPN %d unchanged (no code region)\n", ppn);
        return ppn;
    }
    
    uint16_t code_end_ppn = g_code_region.start_ppn + g_code_region.num_pages;
    
    // Only skip if ppn is exactly at the page BEFORE code region
    // This means the next increment would enter the code region
    if (ppn == g_code_region.start_ppn - 1) {
        FTL_DEBUG("[GC_TAIL] adjust_ppn: PPN %d -> %d (skip code region: %d-%d)\n",
                 ppn, code_end_ppn, g_code_region.start_ppn, code_end_ppn - 1);
        return code_end_ppn;
    }
    
    // If ppn is already within code region, this is an error state
    // Force skip to end to recover from the error
    if (ppn >= g_code_region.start_ppn && ppn < code_end_ppn) {
        FTL_DEBUG("[GC_TAIL] ERROR: adjust_ppn: PPN %d inside code region, forcing skip to %d\n",
                 ppn, code_end_ppn);
        return code_end_ppn;
    }
    
    // ppn is before or after code region, no adjustment needed
    FTL_DEBUG("[GC_TAIL] adjust_ppn: PPN %d unchanged (outside code region %d-%d)\n",
             ppn, g_code_region.start_ppn, code_end_ppn - 1);
    return ppn;
}

/**
 * adjust_gc_pointers_for_code_region: Adjust both GC head and tail pointers to skip code region
 * 
 * This utility function adjusts both GC head and tail pointers in one call.
 * It should be called after:
 * - Pointer recovery from power failure
 * - Entering emergency GC mode
 * - Any operation that may set pointers to arbitrary values
 * 
 * Note: For tail pointer increment scenarios, use adjust_ppn_for_code_region() instead.
 */
static void adjust_gc_pointers_for_code_region(void) {
    uint16_t head_before = FTL->gc_head_page;
    uint16_t tail_before = FTL->gc_tail_page;
    
    FTL_DEBUG("[GC_TAIL] adjust_gc_pointers: before adjustment: head=%d, tail=%d\n",
             head_before, tail_before);
    
    FTL->gc_head_page = adjust_ppn_for_code_region(FTL->gc_head_page);
    FTL->gc_tail_page = adjust_ppn_for_code_region(FTL->gc_tail_page);
    
    if (FTL->gc_head_page != head_before || FTL->gc_tail_page != tail_before) {
        FTL_DEBUG("[GC_TAIL] adjust_gc_pointers: after adjustment: head=%d (was %d), tail=%d (was %d)\n",
                 FTL->gc_head_page, head_before, FTL->gc_tail_page, tail_before);
    } else {
        FTL_DEBUG("[GC_TAIL] adjust_gc_pointers: no adjustment needed\n");
    }
}

/**
 * skip_code_region: Legacy wrapper for adjust_ppn_for_code_region
 * 
 * @ppn: Current physical page number
 * @return: Adjusted PPN
 * 
 * This function is kept for backward compatibility.
 * New code should use adjust_ppn_for_code_region() directly.
 */
static uint16_t skip_code_region(uint16_t ppn) {
    return adjust_ppn_for_code_region(ppn);
}

/**
 * eflash_ftl_gc_reclaim_code_region: Specialized GC for code region expansion
 * 
 * @pages_needed: Number of pages needed after code region
 * @return: 0 on success, -1 on failure
 * 
 * This function reclaims pages immediately after the code region to allow
 * code region expansion. It moves the GC tail pointer past the code region
 * and reclaims stale pages until enough space is available.
 * 
 * Usage scenario:
 * - Before expanding code region, call this to ensure contiguous free pages
 * - The reclaimed pages will be erased and ready for code programming
 */
int eflash_ftl_gc_reclaim_code_region(uint16_t pages_needed) {
    FTL_DEBUG("[GC_CODE] Reclaiming %d pages after code region...\n", pages_needed);
    
    if (pages_needed == 0) {
        return 0;
    }
    
    // Load code region info
    if (g_code_region.magic != CODE_REGION_MAGIC) {
        load_code_region_info();
    }
    
    if (g_code_region.num_pages == 0) {
        FTL_DEBUG("[GC_CODE] ERROR: Code region not initialized\n");
        return -1;
    }
    
    uint16_t code_end_ppn = g_code_region.start_ppn + g_code_region.num_pages;
    FTL_DEBUG("[GC_CODE] Code region ends at PPN %d\n", code_end_ppn);
    
    // Check if we need to move tail pointer past code region
    if (FTL->gc_tail_page < code_end_ppn) {
        FTL_DEBUG("[GC_TAIL] Code region reclaim: moving tail from %d to %d (past code region)\n",
                  FTL->gc_tail_page, code_end_ppn);
        FTL->gc_tail_page = code_end_ppn;
    } else {
        FTL_DEBUG("[GC_TAIL] Code region reclaim: tail already at %d (>= code_end %d)\n",
                  FTL->gc_tail_page, code_end_ppn);
    }
    
    // Now reclaim pages starting from code_end_ppn
    uint16_t pages_reclaimed = 0;
    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;  // Adjust based on your config
    
    while (pages_reclaimed < pages_needed && FTL->gc_tail_page <= last_user_page) {
        uint16_t current_page = FTL->gc_tail_page;
        uint16_t tail_before_op = FTL->gc_tail_page;
        
        FTL_DEBUG("[GC_CODE] Attempting to reclaim PPN %d\n", current_page);
        
        // Try to collect this page
        int ret = gc_collect_one_page(current_page);
        
        if (ret >= 0) {
            pages_reclaimed++;
            FTL_DEBUG("[GC_CODE] Successfully reclaimed PPN %d (%d/%d)\n",
                     current_page, pages_reclaimed, pages_needed);
        } else {
            FTL_DEBUG("[GC_CODE] WARNING: Failed to reclaim PPN %d\n", current_page);
        }
        
        // Move to next page
        FTL_DEBUG("[GC_TAIL] Code region GC: page %d processed, tail will move: %d -> ", current_page, FTL->gc_tail_page);
        FTL->gc_tail_page++;
        FTL_DEBUG("%d (increment)\n", FTL->gc_tail_page);
        
        // Skip code region if tail is about to enter it
        uint16_t tail_before_skip = FTL->gc_tail_page;
        FTL->gc_tail_page = skip_code_region(FTL->gc_tail_page);
        if (FTL->gc_tail_page != tail_before_skip) {
            FTL_DEBUG("[GC_TAIL] Code region skip: tail adjusted %d -> %d\n",
                     tail_before_skip, FTL->gc_tail_page);
        }
        
        // Wrap around if needed
        if (FTL->gc_tail_page > last_user_page) {
            FTL->gc_tail_page = 0;
            FTL_DEBUG("[GC_TAIL] Code region GC: wrapped around to PPN 0\n");
            
            // Skip code region again after wrap
            uint16_t tail_after_wrap = FTL->gc_tail_page;
            FTL->gc_tail_page = skip_code_region(FTL->gc_tail_page);
            if (FTL->gc_tail_page != tail_after_wrap) {
                FTL_DEBUG("[GC_TAIL] Code region skip after wrap: tail adjusted %d -> %d\n",
                         tail_after_wrap, FTL->gc_tail_page);
            }
        }
        
        FTL_DEBUG("[GC_TAIL] Code region GC: final tail position: %d (was %d, delta=%d)\n",
                 FTL->gc_tail_page, tail_before_op,
                 (int16_t)FTL->gc_tail_page - (int16_t)tail_before_op);
    }
    
    if (pages_reclaimed >= pages_needed) {
        FTL_DEBUG("[GC_CODE] Successfully reclaimed %d pages\n", pages_reclaimed);
        return 0;
    } else {
        FTL_DEBUG("[GC_CODE] WARNING: Only reclaimed %d/%d pages\n",
                 pages_reclaimed, pages_needed);
        return -1;
    }
}

/**
 * eflash_ftl_gc_reserve_physical_range: GC to reserve a contiguous physical page range
 *
 * @start_ppn: Starting physical page number of the range to reserve
 * @num_pages: Number of pages to reserve
 * @return: 0 on success, -1 on failure
 *
 * Description:
 *   Performs targeted GC to free and reserve a contiguous physical page range.
 *   This is used to prepare physical space for code region before migration.
 *   All valid data in the range is migrated out, all pages are erased,
 *   and both head and tail pointers are ensured to be outside the range.
 *
 * Key guarantees:
 *   1. All pages in [start_ppn, start_ppn + num_pages) are erased and free
 *   2. gc_head_page is NOT within the reserved range (migration destinations are safe)
 *   3. gc_tail_page is NOT within the reserved range (GC scanning skips it)
 *   4. GC migration destinations never land in the reserved range
 */
int eflash_ftl_gc_reserve_physical_range(uint16_t start_ppn, uint16_t num_pages) {
    if (!FTL->is_initialized) {
        FTL_DEBUG("[GC_RESERVE] ERROR: FTL not initialized\n");
        return -1;
    }

    if (num_pages == 0) {
        return 0;
    }

    uint16_t last_user_page = EFLASH_TOTAL_PAGES - 1;

    if (start_ppn + num_pages > EFLASH_TOTAL_PAGES) {
        FTL_DEBUG("[GC_RESERVE] ERROR: Range [%d, %d) exceeds total pages %d\n",
                 start_ppn, start_ppn + num_pages, EFLASH_TOTAL_PAGES);
        return -1;
    }

    uint16_t end_ppn = start_ppn + num_pages;

    FTL_DEBUG("[GC_RESERVE] ========== Reserving range [%d, %d) ==========\n",
             start_ppn, end_ppn);
    FTL_DEBUG("[GC_RESERVE] Initial state: head=%d, tail=%d, free=%d\n",
             FTL->gc_head_page, FTL->gc_tail_page, eflash_ftl_get_free_pages());

    if (FTL->gc_in_progress) {
        FTL_DEBUG("[GC_RESERVE] ERROR: GC already in progress\n");
        return -1;
    }
    FTL->gc_in_progress = true;

    /*
     * Step 1: Move head out of reserved range.
     * This guarantees that all subsequent page allocations during migration
     * will use pages outside the reserved range.
     */
    {
        uint16_t head_before = FTL->gc_head_page;
        if (FTL->gc_head_page >= start_ppn && FTL->gc_head_page < end_ppn) {
            FTL->gc_head_page = end_ppn;
            if (FTL->gc_head_page > last_user_page) {
                FTL->gc_head_page = 0;
                if (FTL->gc_head_page >= start_ppn && FTL->gc_head_page < end_ppn) {
                    FTL_DEBUG("[GC_RESERVE] ERROR: Cannot move head out of reserved range\n");
                    FTL->gc_in_progress = false;
                    return -1;
                }
            }
            FTL_DEBUG("[GC_RESERVE] Head moved: %d -> %d (out of reserved range)\n",
                     head_before, FTL->gc_head_page);
        } else {
            FTL_DEBUG("[GC_RESERVE] Head %d already outside reserved range\n", FTL->gc_head_page);
        }
    }

    /*
     * Step 2: Free all pages in the reserved range.
     * For each page: if it has valid data, migrate it out; then erase.
     */
    uint16_t pages_migrated = 0;
    uint16_t pages_erased = 0;

    for (uint16_t ppn = start_ppn; ppn < end_ppn; ppn++) {
        FTL_DEBUG("[GC_RESERVE] Processing PPN %d (%d/%d)...\n",
                 ppn, ppn - start_ppn + 1, num_pages);

        bool valid = is_page_still_valid(ppn);

        if (valid) {
            FTL_DEBUG("[GC_RESERVE] PPN %d has VALID data, migrating...\n", ppn);

            if (gc_migrate_page(ppn) != 0) {
                FTL_DEBUG("[GC_RESERVE] ERROR: Failed to migrate PPN %d\n", ppn);
                FTL->gc_in_progress = false;
                return -1;
            }
            pages_migrated++;

            /*
             * After migration, head may have advanced and potentially wrapped
             * back into the reserved range. Check and adjust if needed.
             */
            if (FTL->gc_head_page >= start_ppn && FTL->gc_head_page < end_ppn) {
                uint16_t head_before = FTL->gc_head_page;
                FTL->gc_head_page = end_ppn;
                if (FTL->gc_head_page > last_user_page) {
                    FTL->gc_head_page = 0;
                }
                FTL_DEBUG("[GC_RESERVE] Head re-entered range post-migration, adjusted: %d -> %d\n",
                         head_before, FTL->gc_head_page);
            }
        } else {
            FTL_DEBUG("[GC_RESERVE] PPN %d is STALE/BLANK, no migration needed\n", ppn);
        }

        if (eflash_hw_erase(ppn) != 0) {
            FTL_DEBUG("[GC_RESERVE] ERROR: Failed to erase PPN %d\n", ppn);
            FTL->gc_in_progress = false;
            return -1;
        }
        pages_erased++;
        FTL_DEBUG("[GC_RESERVE] PPN %d erased\n", ppn);
    }

    /*
     * Step 3: Move tail out of reserved range.
     * This ensures future GC scans skip the reserved area.
     */
    {
        uint16_t tail_before = FTL->gc_tail_page;
        if (FTL->gc_tail_page >= start_ppn && FTL->gc_tail_page < end_ppn) {
            FTL->gc_tail_page = end_ppn;
            if (FTL->gc_tail_page > last_user_page) {
                FTL->gc_tail_page = 0;
                if (FTL->gc_tail_page >= start_ppn && FTL->gc_tail_page < end_ppn) {
                    FTL->gc_tail_page = end_ppn;
                }
            }
            FTL_DEBUG("[GC_RESERVE] Tail moved: %d -> %d (out of reserved range)\n",
                     tail_before, FTL->gc_tail_page);
        } else {
            FTL_DEBUG("[GC_RESERVE] Tail %d already outside reserved range\n", FTL->gc_tail_page);
        }
    }

    FTL->gc_in_progress = false;

    FTL_DEBUG("[GC_RESERVE] ========== Range [%d, %d) reserved ==========\n",
             start_ppn, end_ppn);
    FTL_DEBUG("[GC_RESERVE] Migrated: %d, Erased: %d\n", pages_migrated, pages_erased);
    FTL_DEBUG("[GC_RESERVE] Final state: head=%d, tail=%d, free=%d\n",
             FTL->gc_head_page, FTL->gc_tail_page, eflash_ftl_get_free_pages());

    return 0;
}

/**
 * calculate_code_region_checksum: Calculate checksum for code region info
 */
static uint16_t calculate_code_region_checksum(const code_region_info_t *info) {
    uint16_t sum = 0;
    const uint8_t *data = (const uint8_t *)info;
    
    // Sum all bytes except the checksum field itself
    for (size_t i = 0; i < sizeof(code_region_info_t) - 2; i++) {
        sum += data[i];
    }
    
    return sum;
}

/**
 * save_code_region_info: Save code region info to Flash with atomic update
 */
static int save_code_region_info(void) {
    // Update checksum
    g_code_region.checksum = calculate_code_region_checksum(&g_code_region);
    
    // Write to Flash
    uint8_t page_buf[EFLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, EFLASH_PAGE_SIZE);
    memcpy(page_buf, &g_code_region, sizeof(code_region_info_t));
    
    return write_system_page(CODE_REGION_INFO_LPN, page_buf);
}

/**
 * load_code_region_info: Load code region info from Flash
 */
static int load_code_region_info(void) {
    uint8_t page_buf[EFLASH_PAGE_SIZE];
    
    int ret = read_system_page(CODE_REGION_INFO_LPN, page_buf);
    if (ret != 0) {
        FTL_DEBUG("[CODE] ERROR: Failed to read code region info\n");
        return -1;
    }
    
    memcpy(&g_code_region, page_buf, sizeof(code_region_info_t));
    
    // Verify magic and checksum
    if (g_code_region.magic != CODE_REGION_MAGIC) {
        FTL_DEBUG("[CODE] WARNING: Invalid magic number, initializing new\n");
        return -1;
    }
    
    uint16_t expected_checksum = calculate_code_region_checksum(&g_code_region);
    if (g_code_region.checksum != expected_checksum) {
        FTL_DEBUG("[CODE] WARNING: Checksum mismatch, may be corrupted\n");
        return -1;
    }
    
    return 0;
}

/**
 * eflash_ftl_code_region_init: Initialize code region management
 * 
 * @return: 0 on success, -1 on failure
 * 
 * This function initializes the code region management system.
 * It loads existing code region info from Flash or creates a new one.
 */
int eflash_ftl_code_region_init(void) {
    FTL_DEBUG("[CODE] Initializing code region management...\n");
    
    // Try to load existing code region info
    int ret = load_code_region_info();
    
    if (ret == 0) {
        FTL_DEBUG("[CODE] Loaded existing code region: start=%d, pages=%d, status=%d\n",
                  g_code_region.start_ppn, g_code_region.num_pages, g_code_region.status);
        
        // Check if there's an incomplete migration to recover
        if (g_code_region.status == CODE_MIGRATE_IN_PROGRESS) {
            FTL_DEBUG("[CODE] Found incomplete migration, attempting recovery...\n");
            return eflash_ftl_code_region_recover();
        }
        
        // CRITICAL: Adjust GC pointers to skip code region
        if (g_code_region.num_pages > 0) {
            uint16_t code_end_ppn = g_code_region.start_ppn + g_code_region.num_pages;
            
            // If GC head/tail are within code region, move them past it
            if (FTL->gc_head_page < code_end_ppn) {
                FTL->gc_head_page = code_end_ppn;
                FTL_DEBUG("[CODE] Adjusted GC head from %d to %d (skip code region)\n",
                         FTL->gc_head_page - g_code_region.num_pages, FTL->gc_head_page);
            }
            
            if (FTL->gc_tail_page < code_end_ppn) {
                FTL->gc_tail_page = code_end_ppn;
                FTL_DEBUG("[CODE] Adjusted GC tail from %d to %d (skip code region)\n",
                         FTL->gc_tail_page - g_code_region.num_pages, FTL->gc_tail_page);
            }
        }
    } else {
        // Initialize new code region
        memset(&g_code_region, 0, sizeof(code_region_info_t));
        g_code_region.magic = CODE_REGION_MAGIC;
        g_code_region.start_ppn = CODE_REGION_START_PPN;
        g_code_region.num_pages = 0;
        g_code_region.status = CODE_MIGRATE_IDLE;
        
        ret = save_code_region_info();
        if (ret != 0) {
            FTL_DEBUG("[CODE] ERROR: Failed to save initial code region info\n");
            return -1;
        }
        
        FTL_DEBUG("[CODE] Initialized new code region at PPN 0\n");
    }
    
    return 0;
}

/**
 * eflash_ftl_code_migrate_from_logical: Migrate code from logical pages to physical code region
 * 
 * @src_lpn: Starting logical page number containing code data
 * @num_pages: Number of pages to migrate
 * @return: 0 on success, -1 on failure
 * 
 * This function migrates code data from FTL-managed logical pages to the
 * physical code region starting at PPN 0. The code region is excluded from
 * FTL management (GC will skip it).
 * 
 * Process:
 * 1. Record migration state (for power-failure recovery)
 * 2. Read data from logical pages via FTL
 * 3. Write directly to physical pages in code region
 * 4. Update migration progress after each page
 * 5. Reclaim logical pages after successful migration
 * 6. Update code region size
 */
int eflash_ftl_code_migrate_from_logical(uint16_t src_lpn, uint16_t num_pages) {
    FTL_DEBUG("[CODE] Migrating %d pages from LPN %d to code region...\n", 
              num_pages, src_lpn);
    
    if (num_pages == 0) {
        FTL_DEBUG("[CODE] ERROR: num_pages is 0\n");
        return -1;
    }
    
    // Step 1: Initialize migration state
    g_code_region.status = CODE_MIGRATE_IN_PROGRESS;
    g_code_region.src_lpn = src_lpn;
    g_code_region.total_pages = num_pages;
    g_code_region.pages_migrated = 0;

    // Calculate byte-contiguous physical start (end of existing code data)
    uint32_t code_region_start_byte = (uint32_t)g_code_region.start_ppn * EFLASH_PAGE_SIZE;
    uint32_t current_end_byte = code_region_start_byte + g_code_region.code_size_bytes;

    g_code_region.code_size_bytes += num_pages * USER_DATA_SIZE;  // Accumulate, don't overwrite
    
    // Initialize start_ppn and num_pages if this is the first migration
    if (g_code_region.magic != CODE_REGION_MAGIC || g_code_region.num_pages == 0) {
        // First time migration: initialize code region from scratch
        g_code_region.start_ppn = CODE_REGION_START_PPN;  // Always starts at PPN 0
        g_code_region.num_pages = 0;  // Will be updated after successful migration
        g_code_region.magic = CODE_REGION_MAGIC;
        current_end_byte = code_region_start_byte;  // Reset for first migration
        FTL_DEBUG("[CODE] Initializing new code region at PPN 0\n");
    }
    
    // Save initial state
    if (save_code_region_info() != 0) {
        FTL_DEBUG("[CODE] ERROR: Failed to save migration state\n");
        return -1;
    }
    
    // Step 2: Read code data using eflash_ftl_read_logical (supports cross-page reads)
    // and write directly to physical pages (512 bytes each)
    uint16_t dst_ppn = current_end_byte / EFLASH_PAGE_SIZE;
    uint16_t dst_offset = current_end_byte % EFLASH_PAGE_SIZE;
    uint32_t total_code_bytes = num_pages * USER_DATA_SIZE;
    uint32_t bytes_read = 0;
    uint16_t pages_written = 0;
    
    // Calculate the starting addresses for this migration session
    uint32_t session_start_logical_addr = (uint32_t)src_lpn * USER_DATA_SIZE;
    uint32_t session_start_physical_addr = current_end_byte;

    FTL_DEBUG("[CODE] dst_ppn=%d, dst_offset=%d, phys_start=0x%06X\n",
              dst_ppn, dst_offset, session_start_physical_addr);
    FTL_DEBUG("[CODE] Reading code data and writing to physical pages...\n");

    while (bytes_read < total_code_bytes) {
        uint16_t current_ppn = dst_ppn + pages_written;
        uint16_t current_offset = (pages_written == 0) ? dst_offset : 0;
        uint32_t remaining = total_code_bytes - bytes_read;
        uint16_t chunk_size = EFLASH_PAGE_SIZE - current_offset;
        if (chunk_size > remaining) chunk_size = remaining;
        
        // Calculate logical byte address (src_lpn * USER_DATA_SIZE + bytes_read)
        uint32_t logical_addr = session_start_logical_addr + bytes_read;
        
        // Read chunk from logical address (eflash_ftl_read_logical handles cross-page)
        uint8_t page_buffer[EFLASH_PAGE_SIZE];
        int ret;
        
        // For partial page writes, read existing page content first to preserve other data
        if (current_offset > 0 || chunk_size < EFLASH_PAGE_SIZE) {
            ret = eflash_hw_read(current_ppn, page_buffer);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to read PPN %d for partial write\n", current_ppn);
                g_code_region.status = CODE_MIGRATE_FAILED;
                save_code_region_info();
                return -1;
            }
        } else {
            memset(page_buffer, 0xFF, EFLASH_PAGE_SIZE);
        }
        
        ret = eflash_ftl_read_logical(logical_addr, page_buffer + current_offset, chunk_size);
        if (ret != 0) {
            FTL_DEBUG("[CODE] ERROR: Failed to read logical address 0x%06X\n", logical_addr);
            g_code_region.status = CODE_MIGRATE_FAILED;
            save_code_region_info();
            return -1;
        }
        
        // Program the physical page directly
        // Code region pages are always erased before programming
        ret = eflash_hw_prog(current_ppn, page_buffer);
        if (ret != 0) {
            FTL_DEBUG("[CODE] ERROR: Failed to program PPN %d\n", current_ppn);
            g_code_region.status = CODE_MIGRATE_FAILED;
            save_code_region_info();
            return -1;
        }
        
        bytes_read += chunk_size;
        pages_written++;
        
        // Update migration progress
        g_code_region.pages_migrated = pages_written;
        g_code_region.dst_ppn = current_ppn;
        
        if (save_code_region_info() != 0) {
            FTL_DEBUG("[CODE] WARNING: Failed to save progress at page %d\n", pages_written);
        }
        
        FTL_DEBUG("[CODE] Written physical page %d (PPN %d, %d bytes at offset %d)\n",
                  pages_written, current_ppn, chunk_size, current_offset);
    }
    
    // Step 3: Add ONE segment record for this entire migration session
    if (g_code_region.migration_records_count < MAX_MIGRATION_RECORDS) {
        g_code_region.migration_map[g_code_region.migration_records_count].logical_addr = session_start_logical_addr;
        g_code_region.migration_map[g_code_region.migration_records_count].physical_addr = session_start_physical_addr;
        g_code_region.migration_map[g_code_region.migration_records_count].size = total_code_bytes;
        g_code_region.migration_records_count++;
        
        FTL_DEBUG("[CODE] Added segment record: logical=0x%06X, physical=0x%06X, size=%d bytes\n",
                  session_start_logical_addr, session_start_physical_addr, total_code_bytes);
    } else {
        FTL_DEBUG("[CODE] WARNING: Migration map full, cannot record segment\n");
    }
    
    // Step 4: Migration complete, mark as complete
    g_code_region.status = CODE_MIGRATE_COMPLETE;
    g_code_region.num_pages += num_pages;
    
    if (save_code_region_info() != 0) {
        FTL_DEBUG("[CODE] ERROR: Failed to save completion state\n");
        return -1;
    }
    
    // CRITICAL: Adjust GC pointers to skip the newly created/expanded code region
    uint16_t code_end_ppn = g_code_region.start_ppn + g_code_region.num_pages;
    
    if (FTL->gc_head_page < code_end_ppn) {
        FTL->gc_head_page = code_end_ppn;
        FTL_DEBUG("[CODE] Adjusted GC head to %d (skip code region PPN 0-%d)\n",
                 FTL->gc_head_page, code_end_ppn - 1);
    }
    
    if (FTL->gc_tail_page < code_end_ppn) {
        FTL->gc_tail_page = code_end_ppn;
        FTL_DEBUG("[CODE] Adjusted GC tail to %d (skip code region PPN 0-%d)\n",
                 FTL->gc_tail_page, code_end_ppn - 1);
    }
    
    // Step 5: Reclaim source logical pages (trim them from FTL)
    FTL_DEBUG("[CODE] Reclaiming %d logical pages...\n", num_pages);
    
    for (uint16_t i = 0; i < num_pages; i++) {
        uint16_t lpn = src_lpn + i;
        
        // Trim the logical page from FTL (mark as invalid)
        int ret = eflash_ftl_trim(lpn);
        if (ret != 0) {
            FTL_DEBUG("[CODE] WARNING: Failed to trim LPN %d\n", lpn);
            // Continue anyway, GC will eventually reclaim it
        }
    }
    
    // Step 5: Mark migration as idle
    g_code_region.status = CODE_MIGRATE_IDLE;
    g_code_region.src_lpn = 0;
    g_code_region.dst_ppn = 0;
    g_code_region.pages_migrated = 0;
    g_code_region.total_pages = 0;
    
    save_code_region_info();
    
    FTL_DEBUG("[CODE] Migration completed successfully. Code region size: %d pages\n",
              g_code_region.num_pages);
    
    return 0;
}

/**
 * eflash_ftl_code_region_expand: Expand code region by reclaiming pages after it
 * 
 * @additional_pages: Number of additional pages to add to code region
 * @return: 0 on success, -1 on failure
 * 
 * This function expands the code region by:
 * 1. Calling specialized GC to reclaim pages after code region
 * 2. Erasing the reclaimed pages
 * 3. Updating code region size
 */
int eflash_ftl_code_region_expand(uint16_t additional_pages) {
    FTL_DEBUG("[CODE] Expanding code region by %d pages...\n", additional_pages);
    
    if (additional_pages == 0) {
        return 0;
    }
    
    // Step 1: Use specialized GC to reclaim pages after code region
    int ret = eflash_ftl_gc_reclaim_code_region(additional_pages);
    if (ret != 0) {
        FTL_DEBUG("[CODE] ERROR: Failed to reclaim pages for expansion\n");
        return -1;
    }
    
    // Step 2: Update code region size
    uint16_t old_size = g_code_region.num_pages;
    g_code_region.num_pages += additional_pages;
    
    // Step 3: Save updated code region info
    if (save_code_region_info() != 0) {
        FTL_DEBUG("[CODE] ERROR: Failed to save expanded code region info\n");
        g_code_region.num_pages = old_size;  // Rollback
        return -1;
    }
    
    FTL_DEBUG("[CODE] Code region expanded: %d -> %d pages\n",
              old_size, g_code_region.num_pages);
    
    return 0;
}

/**
 * eflash_ftl_code_region_delete_segment: Delete a code segment by logical address
 * 
 * @logical_addr: Logical byte address of the code segment to delete
 * @return: 0 on success, -1 on failure
 * 
 * This function deletes a code segment identified by its logical address.
 * If the segment is in the middle, subsequent segments are moved forward
 * to fill the gap. The migration_map is updated accordingly.
 * 
 * Steps:
 * 1. Find the segment in migration_map by logical address
 * 2. Erase the physical pages occupied by this segment
 * 3. If not the last segment, move subsequent segments forward
 * 4. Update migration_map entries and total size
 * 5. Save updated code region info
 */
int eflash_ftl_code_region_delete_segment(uint32_t logical_addr) {
    FTL_DEBUG("[CODE] Deleting code segment at logical address 0x%06X...\n", logical_addr);
    
    // Load code region info if not already loaded
    if (g_code_region.magic != CODE_REGION_MAGIC) {
        load_code_region_info();
    }
    
    // Find the segment index by logical address
    int segment_idx = -1;
    for (int i = 0; i < g_code_region.migration_records_count; i++) {
        if (g_code_region.migration_map[i].logical_addr == logical_addr) {
            segment_idx = i;
            break;
        }
    }
    
    if (segment_idx == -1) {
        FTL_DEBUG("[CODE] ERROR: Code segment not found at logical address 0x%06X\n", logical_addr);
        return -1;
    }
    
    uint32_t segment_size = g_code_region.migration_map[segment_idx].size;
    uint32_t segment_phys_addr = g_code_region.migration_map[segment_idx].physical_addr;
    uint16_t segment_start_ppn = segment_phys_addr / EFLASH_PAGE_SIZE;
    uint16_t segment_end_byte = segment_phys_addr + segment_size;
    uint16_t segment_end_ppn = (segment_end_byte > 0) ? ((segment_end_byte - 1) / EFLASH_PAGE_SIZE) : 0;
    uint16_t segment_pages = segment_end_ppn - segment_start_ppn + 1;
    
    FTL_DEBUG("[CODE] Found segment at index %d: logical=0x%06X, physical=0x%06X, size=%d bytes, pages=%d\n",
              segment_idx, logical_addr, segment_phys_addr, segment_size, segment_pages);
    
    bool is_last_segment = (segment_idx == g_code_region.migration_records_count - 1);
    
    // Step 2: If not the last segment, move subsequent segments forward
    if (!is_last_segment) {
        FTL_DEBUG("[CODE] Segment is not last, moving subsequent segments forward...\n");
        
        // The source address is the physical address of the segment AFTER the deleted one
        uint32_t move_src_addr = g_code_region.migration_map[segment_idx + 1].physical_addr;
        uint32_t move_dst_addr = segment_phys_addr;  // Destination (fill the gap)
        
        FTL_DEBUG("[CODE] move_src_addr=0x%06X, move_dst_addr=0x%06X\n", move_src_addr, move_dst_addr);
        
        // Calculate total bytes to move (all segments after the deleted one)
        uint32_t total_move_bytes = 0;
        for (int i = segment_idx + 1; i < g_code_region.migration_records_count; i++) {
            total_move_bytes += g_code_region.migration_map[i].size;
        }
        
        FTL_DEBUG("[CODE] Moving %d bytes from physical 0x%06X to 0x%06X\n",
                  total_move_bytes, move_src_addr, move_dst_addr);
        
        // Move data using byte-granular addressing (source may not be page-aligned)
        // Read all data first, then erase source+destination pages, then write
        
        // Use a static buffer to avoid stack overflow for large moves
        // Max code region is typically small (< 64KB = 128 pages)
        #define MAX_MOVE_PAGES 128
        static uint8_t move_buffer[MAX_MOVE_PAGES * EFLASH_PAGE_SIZE];
        
        if (total_move_bytes > MAX_MOVE_PAGES * EFLASH_PAGE_SIZE) {
            FTL_DEBUG("[CODE] ERROR: Move too large (%d bytes > %d max)\n",
                      total_move_bytes, MAX_MOVE_PAGES * EFLASH_PAGE_SIZE);
            return -1;
        }
        
        // Step 2a: Read source data byte-granular into buffer
        uint32_t buf_offset = 0;
        uint32_t read_addr = move_src_addr;
        uint32_t remaining = total_move_bytes;
        while (remaining > 0) {
            uint16_t src_ppn = read_addr / EFLASH_PAGE_SIZE;
            uint16_t src_offset = read_addr % EFLASH_PAGE_SIZE;
            uint32_t chunk = EFLASH_PAGE_SIZE - src_offset;
            if (chunk > remaining) chunk = remaining;
            
            uint8_t temp_page[EFLASH_PAGE_SIZE];
            int ret = eflash_hw_read(src_ppn, temp_page);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to read PPN %d\n", src_ppn);
                return -1;
            }
            memcpy(move_buffer + buf_offset, temp_page + src_offset, chunk);
            FTL_DEBUG("[CODE] Read PPN %d offset %d, %d bytes: %02X %02X %02X %02X\n",
                      src_ppn, src_offset, chunk,
                      move_buffer[buf_offset], move_buffer[buf_offset+1],
                      move_buffer[buf_offset+2], move_buffer[buf_offset+3]);
            
            buf_offset += chunk;
            remaining -= chunk;
            read_addr += chunk;
        }
        
        // Step 2b-2d: Write moved data to destination, preserving existing data
        // on partial pages that overlap with segments before the deleted one
        buf_offset = 0;
        uint32_t write_addr = move_dst_addr;
        remaining = total_move_bytes;

        uint16_t dst_start_ppn = move_dst_addr / EFLASH_PAGE_SIZE;
        uint16_t dst_end_ppn = (move_dst_addr + total_move_bytes - 1) / EFLASH_PAGE_SIZE;

        for (uint16_t ppn = dst_start_ppn; ppn <= dst_end_ppn; ppn++) {
            uint16_t dst_offset = (ppn == dst_start_ppn) ? (move_dst_addr % EFLASH_PAGE_SIZE) : 0;
            uint32_t chunk = EFLASH_PAGE_SIZE - dst_offset;
            if (chunk > remaining) chunk = remaining;

            uint8_t temp_page[EFLASH_PAGE_SIZE];
            bool page_has_preserved_data = (ppn == dst_start_ppn && dst_offset > 0);

            if (page_has_preserved_data) {
                int ret = eflash_hw_read(ppn, temp_page);
                if (ret != 0) {
                    FTL_DEBUG("[CODE] ERROR: Failed to read PPN %d for partial merge\n", ppn);
                    return -1;
                }
                FTL_DEBUG("[CODE] Read PPN %d to preserve existing data before erase\n", ppn);
            }

            int ret = eflash_hw_erase(ppn);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to erase dest PPN %d\n", ppn);
                return -1;
            }
            FTL_DEBUG("[CODE] Erased dest PPN %d\n", ppn);

            if (page_has_preserved_data) {
                memcpy(temp_page + dst_offset, move_buffer + buf_offset, chunk);
            } else {
                memset(temp_page, 0xFF, EFLASH_PAGE_SIZE);
                memcpy(temp_page + dst_offset, move_buffer + buf_offset, chunk);
            }

            ret = eflash_hw_prog(ppn, temp_page);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to program PPN %d\n", ppn);
                return -1;
            }
            FTL_DEBUG("[CODE] Programmed PPN %d offset %d, %d bytes: %02X %02X %02X %02X\n",
                      ppn, dst_offset, chunk,
                      move_buffer[buf_offset], move_buffer[buf_offset+1],
                      move_buffer[buf_offset+2], move_buffer[buf_offset+3]);

            buf_offset += chunk;
            remaining -= chunk;
            write_addr += chunk;
        }

        // Step 2c: Erase source pages (skip pages already handled as destination)
        uint16_t src_start_ppn = move_src_addr / EFLASH_PAGE_SIZE;
        uint16_t src_end_ppn = (move_src_addr + total_move_bytes - 1) / EFLASH_PAGE_SIZE;
        for (uint16_t ppn = src_start_ppn; ppn <= src_end_ppn; ppn++) {
            if (ppn >= dst_start_ppn && ppn <= dst_end_ppn) {
                FTL_DEBUG("[CODE] Skipping src PPN %d (already handled as dest)\n", ppn);
                continue;
            }
            int ret = eflash_hw_erase(ppn);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to erase src PPN %d\n", ppn);
                return -1;
            }
            FTL_DEBUG("[CODE] Erased src PPN %d\n", ppn);
        }
    } else {
        // Last segment: erase only pages fully within the segment
        // Partial first page (shared with previous segment) must NOT be erased
        FTL_DEBUG("[CODE] Segment is last, erasing full pages only...\n");
        for (uint16_t i = 0; i < segment_pages; i++) {
            uint16_t ppn = segment_start_ppn + i;

            // Skip first page if segment doesn't start at page boundary
            // (it shares space with the preceding segment)
            if (i == 0 && (segment_phys_addr % EFLASH_PAGE_SIZE) != 0) {
                FTL_DEBUG("[CODE] Skipping partial first page PPN %d (shared with other segments)\n",
                          ppn);
                continue;
            }

            FTL_DEBUG("[CODE] Erasing PPN %d...\n", ppn);
            
            int ret = eflash_hw_erase(ppn);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to erase PPN %d\n", ppn);
                return -1;
            }
        }
    }
    
    // Step 3: Remove the deleted segment from migration_map by shifting subsequent entries forward
    for (int i = segment_idx; i < g_code_region.migration_records_count - 1; i++) {
        g_code_region.migration_map[i] = g_code_region.migration_map[i + 1];
    }
    
    // Clear the last entry
    memset(&g_code_region.migration_map[g_code_region.migration_records_count - 1], 0, sizeof(migration_record_t));
    g_code_region.migration_records_count--;
    
    // Step 4: Update migration_map for moved segments - adjust physical addresses
    // After shifting, the subsequent segments are now at indices [segment_idx, segment_idx+1, ...]
    // Their physical addresses need to be updated to reflect the data movement
    if (!is_last_segment) {
        uint32_t new_phys_addr = segment_phys_addr;
        for (int i = segment_idx; i < g_code_region.migration_records_count; i++) {
            uint32_t old_phys_addr = g_code_region.migration_map[i].physical_addr;
            g_code_region.migration_map[i].physical_addr = new_phys_addr;
            
            FTL_DEBUG("[CODE] Updated segment %d (logical=0x%06X): physical 0x%06X -> 0x%06X\n",
                      i, g_code_region.migration_map[i].logical_addr, old_phys_addr, new_phys_addr);
            
            new_phys_addr += g_code_region.migration_map[i].size;
        }
    }
    
    // Step 5: Update total code region size
    g_code_region.code_size_bytes -= segment_size;
    g_code_region.num_pages = (g_code_region.code_size_bytes + EFLASH_PAGE_SIZE - 1) / EFLASH_PAGE_SIZE;
    
    // Step 6: Save updated code region info
    if (save_code_region_info() != 0) {
        FTL_DEBUG("[CODE] ERROR: Failed to save updated code region info\n");
        return -1;
    }
    
    FTL_DEBUG("[CODE] Code segment deleted successfully, remaining segments: %d, total size: %d bytes\n",
              g_code_region.migration_records_count, g_code_region.code_size_bytes);
    
    return 0;
}

/**
 * eflash_ftl_code_region_shrink: Shrink code region by removing pages from the end
 * 
 * @pages_to_remove: Number of pages to remove from code region
 * @return: 0 on success, -1 on failure
 * 
 * This function shrinks the code region by:
 * 1. Validating the shrink request
 * 2. Erasing the pages to be removed
 * 3. Updating code region size
 * 4. Saving updated code region info
 * 
 * Note: Shrink only removes pages from the END of code region.
 *       It does NOT move remaining code to lower addresses.
 */
int eflash_ftl_code_region_shrink(uint16_t pages_to_remove) {
    FTL_DEBUG("[CODE] Shrinking code region by %d pages...\n", pages_to_remove);
    
    if (pages_to_remove == 0) {
        FTL_DEBUG("[CODE] Nothing to shrink (pages_to_remove=0)\n");
        return 0;
    }
    
    // Load code region info if not already loaded
    if (g_code_region.magic != CODE_REGION_MAGIC) {
        load_code_region_info();
    }
    
    // Validate: cannot remove more pages than exist
    if (pages_to_remove >= g_code_region.num_pages) {
        FTL_DEBUG("[CODE] ERROR: Cannot remove %d pages, only %d pages exist\n",
                 pages_to_remove, g_code_region.num_pages);
        return -1;
    }
    
    // Calculate the range of pages to remove
    uint16_t old_size = g_code_region.num_pages;
    uint16_t new_size = old_size - pages_to_remove;
    uint16_t start_remove_ppn = g_code_region.start_ppn + new_size;
    uint16_t end_remove_ppn = g_code_region.start_ppn + old_size;
    
    FTL_DEBUG("[CODE] Shrinking code region: %d -> %d pages\n", old_size, new_size);
    FTL_DEBUG("[CODE] Pages to erase: PPN %d ~ %d\n", start_remove_ppn, end_remove_ppn - 1);
    
    // Step 1: Erase the pages to be removed
    for (uint16_t ppn = start_remove_ppn; ppn < end_remove_ppn; ppn++) {
        FTL_DEBUG("[CODE] Erasing PPN %d...\n", ppn);
        
        int ret = eflash_hw_erase(ppn);
        if (ret != 0) {
            FTL_DEBUG("[CODE] ERROR: Failed to erase PPN %d\n", ppn);
            return -1;
        }
    }
    
    // Step 2: Update code region size
    g_code_region.num_pages = new_size;
    
    // Step 3: Save updated code region info
    if (save_code_region_info() != 0) {
        FTL_DEBUG("[CODE] ERROR: Failed to save shrunk code region info\n");
        g_code_region.num_pages = old_size;  // Rollback
        return -1;
    }
    
    FTL_DEBUG("[CODE] Code region shrunk successfully: %d -> %d pages\n",
              old_size, g_code_region.num_pages);
    
    return 0;
}

/**
 * eflash_ftl_get_code_region_size: Get current code region size
 * 
 * @return: Number of pages in code region
 */
uint16_t eflash_ftl_get_code_region_size(void) {
    return g_code_region.num_pages;
}

/**
 * eflash_ftl_code_region_recover: Recover from power failure during migration
 * 
 * @return: 0 on success, -1 on failure
 * 
 * This function recovers the migration process after a power failure.
 * It reads the saved state and continues from where it left off.
 */
int eflash_ftl_code_region_recover(void) {
    FTL_DEBUG("[CODE] Recovering from power failure...\n");
    FTL_DEBUG("[CODE] Status: %d, Progress: %d/%d pages\n",
              g_code_region.status,
              g_code_region.pages_migrated,
              g_code_region.total_pages);
    
    if (g_code_region.status != CODE_MIGRATE_IN_PROGRESS) {
        FTL_DEBUG("[CODE] No incomplete migration to recover (status=%d)\n", g_code_region.status);
        return 0;  // No recovery needed
    }
    
    uint16_t total_pages = g_code_region.total_pages;
    uint16_t src_lpn = g_code_region.src_lpn;
    uint16_t dst_ppn = g_code_region.start_ppn + g_code_region.num_pages;
    uint32_t total_code_bytes = total_pages * USER_DATA_SIZE;
    uint16_t pages_written = 0;  // Track total pages written for finalization
    
    // Check if we have migration records for recovery
    bool has_migration_map = (g_code_region.migration_records_count > 0);
    
    if (has_migration_map) {
        FTL_DEBUG("[CODE] Using migration map for recovery (%d records)...\n", 
                  g_code_region.migration_records_count);
        
        // Find the last successfully migrated record using the count
        uint32_t last_migrated_bytes = 0;
        uint16_t last_record_idx = g_code_region.migration_records_count - 1;
        
        if (last_record_idx < MAX_MIGRATION_RECORDS) {
            uint32_t logical_addr = g_code_region.migration_map[last_record_idx].logical_addr;
            uint32_t base_addr = (uint32_t)src_lpn * USER_DATA_SIZE;
            last_migrated_bytes = logical_addr - base_addr + EFLASH_PAGE_SIZE;
        }
        
        FTL_DEBUG("[CODE] Last migrated byte offset: %d\n", last_migrated_bytes);
        
        // Continue migration from last successful position
        uint32_t bytes_read = last_migrated_bytes;
        pages_written = (last_migrated_bytes + EFLASH_PAGE_SIZE - 1) / EFLASH_PAGE_SIZE;
        
        while (bytes_read < total_code_bytes) {
            uint16_t current_ppn = dst_ppn + pages_written;
            uint32_t remaining = total_code_bytes - bytes_read;
            uint16_t chunk_size = (remaining < EFLASH_PAGE_SIZE) ? remaining : EFLASH_PAGE_SIZE;
            
            uint32_t logical_addr = (uint32_t)src_lpn * USER_DATA_SIZE + bytes_read;
            
            uint8_t page_buffer[EFLASH_PAGE_SIZE];
            memset(page_buffer, 0xFF, EFLASH_PAGE_SIZE);
            
            int ret = eflash_ftl_read_logical(logical_addr, page_buffer, chunk_size);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to read logical address 0x%06X during recovery\n", logical_addr);
                g_code_region.status = CODE_MIGRATE_FAILED;
                save_code_region_info();
                return -1;
            }
            
            ret = eflash_hw_prog(current_ppn, page_buffer);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to program PPN %d during recovery\n", current_ppn);
                g_code_region.status = CODE_MIGRATE_FAILED;
                save_code_region_info();
                return -1;
            }
            
            bytes_read += chunk_size;
            pages_written++;
            
            g_code_region.pages_migrated = pages_written;
            save_code_region_info();
            
            FTL_DEBUG("[CODE] Recovery: written physical page %d (PPN %d)\n", pages_written, current_ppn);
        }
    } else {
        FTL_DEBUG("[CODE] No migration map, using legacy recovery method...\n");
        
        // Legacy recovery: read from logical pages and write to physical pages
        uint8_t page_buffer[EFLASH_PAGE_SIZE];
        uint16_t buffer_offset = 0;
        uint16_t pages_written = 0;
        
        for (uint16_t i = 0; i < total_pages; i++) {
            uint16_t current_lpn = src_lpn + i;
            uint8_t user_data[USER_DATA_SIZE];
            
            int ret = eflash_ftl_read(current_lpn, user_data);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to read LPN %d during recovery\n", current_lpn);
                g_code_region.status = CODE_MIGRATE_FAILED;
                save_code_region_info();
                return -1;
            }
            
            uint16_t data_offset = 0;
            while (data_offset < USER_DATA_SIZE) {
                uint16_t copy_size = USER_DATA_SIZE - data_offset;
                uint16_t space_in_buffer = EFLASH_PAGE_SIZE - buffer_offset;
                
                if (copy_size > space_in_buffer) {
                    copy_size = space_in_buffer;
                }
                
                memcpy(page_buffer + buffer_offset, user_data + data_offset, copy_size);
                buffer_offset += copy_size;
                data_offset += copy_size;
                
                if (buffer_offset == EFLASH_PAGE_SIZE) {
                    uint16_t current_ppn = dst_ppn + pages_written;
                    
                    ret = eflash_hw_prog(current_ppn, page_buffer);
                    if (ret != 0) {
                        FTL_DEBUG("[CODE] ERROR: Failed to program PPN %d during recovery\n", current_ppn);
                        g_code_region.status = CODE_MIGRATE_FAILED;
                        save_code_region_info();
                        return -1;
                    }
                    
                    pages_written++;
                    buffer_offset = 0;
                    
                    g_code_region.pages_migrated = pages_written;
                    save_code_region_info();
                }
            }
        }
        
        if (buffer_offset > 0) {
            uint16_t current_ppn = dst_ppn + pages_written;
            memset(page_buffer + buffer_offset, 0xFF, EFLASH_PAGE_SIZE - buffer_offset);
            
            int ret = eflash_hw_prog(current_ppn, page_buffer);
            if (ret != 0) {
                FTL_DEBUG("[CODE] ERROR: Failed to program PPN %d during recovery\n", current_ppn);
                g_code_region.status = CODE_MIGRATE_FAILED;
                save_code_region_info();
                return -1;
            }
            
            pages_written++;
        }
    }
    
    // Recovery complete, finalize migration
    g_code_region.status = CODE_MIGRATE_COMPLETE;
    g_code_region.num_pages += pages_written;
    save_code_region_info();
    
    // Reclaim source logical pages
    for (uint16_t i = 0; i < total_pages; i++) {
        eflash_ftl_trim(src_lpn + i);
    }
    
    // Reset to idle state
    g_code_region.status = CODE_MIGRATE_IDLE;
    g_code_region.src_lpn = 0;
    g_code_region.dst_ppn = 0;
    g_code_region.pages_migrated = 0;
    g_code_region.total_pages = 0;
    save_code_region_info();
    
    FTL_DEBUG("[CODE] Recovery completed successfully, wrote %d physical pages\n", pages_written);
    
    return 0;
}

/**
 * eflash_ftl_code_read: Read code data from code region
 * 
 * @page_offset: Page offset within code region (0-based, each page = USER_DATA_SIZE bytes)
 * @buffer: Output buffer for code data
 * @size: Number of bytes to read
 * @return: 0 on success, -1 on failure
 * 
 * This function reads from the code region where code is stored continuously
 * across physical pages (512 bytes each). It handles cross-page reads correctly.
 * 
 * Layout example (3 logical pages = 1392 bytes):
 *   Physical Page 0: [Logical Page 0: 464 bytes] + [Logical Page 1: 48 bytes]
 *   Physical Page 1: [Logical Page 1: 416 bytes] + [Logical Page 2: 96 bytes]
 *   Physical Page 2: [Logical Page 2: 368 bytes] + [48 bytes padding]
 */
int eflash_ftl_code_read(uint16_t page_offset, uint8_t *buffer, uint16_t size) {
    if (!FTL || !FTL->is_initialized) {
        FTL_DEBUG("[CODE] ERROR: FTL not initialized\n");
        return -1;
    }
    
    // Calculate byte offset from page offset
    uint32_t byte_offset = (uint32_t)page_offset * USER_DATA_SIZE;
    uint32_t total_code_size = (uint32_t)g_code_region.num_pages * USER_DATA_SIZE;
    
    // Validate read range
    if (byte_offset >= total_code_size) {
        FTL_DEBUG("[CODE] ERROR: Byte offset %u out of range (total %u bytes)\n",
                 byte_offset, total_code_size);
        return -1;
    }
    
    if (byte_offset + size > total_code_size) {
        FTL_DEBUG("[CODE] ERROR: Read range %u+%u exceeds code size %u\n",
                 byte_offset, size, total_code_size);
        return -1;
    }
    
    // Find which segment contains this byte offset
    uint32_t current_offset = byte_offset;
    uint32_t bytes_read = 0;
    
    while (bytes_read < size) {
        // Find the segment that contains current_offset
        int segment_idx = -1;
        uint32_t segment_start = 0;
        uint32_t segment_end = 0;
        
        for (int i = 0; i < g_code_region.migration_records_count; i++) {
            uint32_t seg_size = g_code_region.migration_map[i].size;
            
            // Calculate segment's position in code region (relative to first segment)
            // Segments are stored contiguously in migration_map order
            if (i == 0) {
                segment_start = 0;
                segment_end = seg_size;
            } else {
                segment_start = segment_end;
                segment_end += seg_size;
            }
            
            if (current_offset >= segment_start && current_offset < segment_end) {
                segment_idx = i;
                break;
            }
        }
        
        if (segment_idx == -1) {
            FTL_DEBUG("[CODE] ERROR: No segment found for byte offset %u\n", current_offset);
            return -1;
        }
        
        // Calculate offset within the segment
        uint32_t offset_in_segment = current_offset - segment_start;
        uint32_t seg_physical_start = g_code_region.migration_map[segment_idx].physical_addr;
        uint32_t seg_size = g_code_region.migration_map[segment_idx].size;
        
        // Calculate physical page and offset within it (byte-contiguous, physical_start may not be page-aligned)
        uint32_t byte_addr = seg_physical_start + offset_in_segment;
        uint16_t current_ppn = byte_addr / EFLASH_PAGE_SIZE;
        uint16_t offset_in_page = byte_addr % EFLASH_PAGE_SIZE;
        
        // Calculate how many bytes to read from this physical page
        uint32_t remaining_in_page = EFLASH_PAGE_SIZE - offset_in_page;
        uint32_t remaining_in_segment = seg_size - offset_in_segment;
        uint32_t remaining_to_read = size - bytes_read;
        uint32_t chunk_size = remaining_in_page;
        if (remaining_in_segment < chunk_size) chunk_size = remaining_in_segment;
        if (remaining_to_read < chunk_size) chunk_size = remaining_to_read;
        
        // Read physical page
        uint8_t physical_page[EFLASH_PAGE_SIZE];
        int ret = eflash_hw_read(current_ppn, physical_page);
        if (ret != 0) {
            FTL_DEBUG("[CODE] ERROR: Failed to read PPN %d\n", current_ppn);
            return -1;
        }
        
        FTL_DEBUG("[CODE] Read PPN %d, first 4 bytes: %02X %02X %02X %02X\n",
                  current_ppn, physical_page[offset_in_page], 
                  physical_page[(offset_in_page+1) % EFLASH_PAGE_SIZE],
                  physical_page[(offset_in_page+2) % EFLASH_PAGE_SIZE],
                  physical_page[(offset_in_page+3) % EFLASH_PAGE_SIZE]);
        
        // Copy data from physical page to output buffer
        memcpy(buffer + bytes_read, physical_page + offset_in_page, chunk_size);
        
        current_offset += chunk_size;
        bytes_read += chunk_size;
    }
    
    FTL_DEBUG("[CODE] Read %d bytes from code region (byte offset %u)\n",
             size, byte_offset);
    
    return 0;
}

