#ifndef EFLASH_FTL_H
#define EFLASH_FTL_H

#include <stdint.h>
#include <stdbool.h>
#include "ecc/bch.h"
#include "eflash_mgr.h"

// --- Cross-platform packed structure support ---
#ifdef _MSC_VER
    #define PACKED_STRUCT __pragma(pack(push, 1))
    #define PACKED_STRUCT_END __pragma(pack(pop))
    #define ATTRIBUTE_PACKED
#else
    #define PACKED_STRUCT
    #define PACKED_STRUCT_END
    #define ATTRIBUTE_PACKED __attribute__((packed))
#endif

// --- Physical Layout Configuration ---
#define EFLASH_PAGE_SIZE    512
#define META_SIZE           48
#define USER_DATA_SIZE      (EFLASH_PAGE_SIZE - META_SIZE)
#define RADIX_DEPTH         16

// --- Object Header Management Configuration ---
#define BASE_HEADER_PAGES     8   // Base object header pages
#define BASE_HEADER_CAPACITY  232 // Base capacity (8 * 464 / 16)
#define EXT_HEADER_PAGES_UNIT 4   // Pages per extension
#define EXT_HEADER_CAPACITY   116 // Capacity per extension unit (OBJ_HEADERS_PER_PAGE * 4 - 4 pointer fields)
#define FREE_LIST_PAGES       4   // Free list pages
#define MAX_EXT_LEVELS        16  // Maximum extension levels for object headers

// --- Object Header Storage Configuration ---
#define OBJ_HEADERS_PER_PAGE  (USER_DATA_SIZE / sizeof(obj_header_t))  // Number of object headers per page (464/16 = 29)

// --- System Area Logical Page Layout ---
// All pages (including system areas) are managed through FTL's Radix Tree for wear leveling:
//   FIXED System Pages (LPN 0-11):
//     LPN 0 ~ 7           : Base object header table (8 pages, 232 objects)
//     LPN 8 ~ 11          : Free list (4 pages)
//   DYNAMIC Pages (LPN 12+):
//     User data pages: allocated by FTL write operations
//     Extended object headers: allocated by eflash_mgr_alloc (LPN not fixed)
#define SYS_OBJ_HEADER_BASE_LPN   0       // Base object header table starts at LPN 0
#define SYS_OBJ_HEADER_PAGES      8       // Base object header table size
#define SYS_FREE_LIST_BASE_LPN    8       // Free list starts at LPN 8
#define SYS_FREE_LIST_PAGES       4       // Free list size
#define SYS_RESERVED_LPN_COUNT    12      // Total reserved LPNs for system areas

// --- Object Header Type Definitions ---
#define OBJ_TYPE_NORMAL     0x00    // Normal object header
#define OBJ_TYPE_LINK       0xFF    // Extension link object (points to next extension level)

// --- LINK Object Magic Numbers (for reliable detection during recovery) ---
#define LINK_OBJ_MAGIC_PKG_ID     0x5F54  // "FT" - Flash Translation
#define LINK_OBJ_MAGIC_CLASS_ID   0x4C4E  // "LN" - LiNk
#define LINK_OBJ_MAGIC_RESERVED0  0xAD    // Additional magic byte 0
#define LINK_OBJ_MAGIC_RESERVED1  0xDE    // Additional magic byte 1
// Usage: When writing LINK object, set these fields to enable fast region-skip during max_obj_id recovery

// --- Object Header Structure (16 bytes) ---
PACKED_STRUCT
typedef struct {
    uint16_t    pkg_id;         // Package ID
    uint16_t    class_id;       // Class ID
    uint8_t     type;           // Type (OBJ_TYPE_NORMAL or OBJ_TYPE_LINK)
    uint8_t     reserved[3];
    uint32_t    body_addr;      // Data body logical address (for LINK: points to next extension start page)
    uint32_t    body_size;      // Data body size (for LINK: should be EXT_HEADER_PAGES_UNIT * USER_DATA_SIZE)
} ATTRIBUTE_PACKED obj_header_t;
PACKED_STRUCT_END

// --- Transaction State Machine (Status Field) ---
// 0xAD -> 0x21 transition conforms to Flash programming characteristics (1->0)
#define TXN_STATUS_BLANK        0xFF  // Never-written blank page
#define TXN_STATUS_PENDING      0xEF  // Reserved (unused)
#define TXN_STATUS_READY        0xAD  // Transaction data page written, waiting for commit
#define TXN_STATUS_COMMITTED    0x21  // Transaction successfully committed (usually only marks Root page)
#define TXN_STATUS_INVALID      0x00  // Non-transaction page or invalidated old page

// --- Special Values ---
#define PAGE_NONE             0xFFFF
#define TXN_ID_NONE           0xFFFF

// --- Metadata Structure (48 bytes) ---
PACKED_STRUCT
typedef struct {
    uint32_t        global_count;
    uint16_t        sector_id;
    uint16_t        epoch;
    uint16_t        txn_id;
    uint16_t        adr[RADIX_DEPTH];
    uint8_t         status;
    uint8_t         ecc[5];
} ATTRIBUTE_PACKED ftl_meta_t;
PACKED_STRUCT_END

// --- Complete Page Structure (512 bytes = 464 user data + 48 metadata) ---
PACKED_STRUCT
typedef struct {
    uint8_t         user_data[USER_DATA_SIZE];  // 464 bytes user data
    ftl_meta_t      meta;                        // 48 bytes metadata
} ATTRIBUTE_PACKED ftl_page_t;
PACKED_STRUCT_END

// --- FTL Context ---
typedef struct {
    eflash_mgr_t   spc_mgr;
    uint16_t      root_page;
    uint16_t      shadow_root;
    uint32_t      next_count;
    uint16_t      current_epoch;
    uint16_t      active_txn_id;
    bool          is_initialized;

    // Pre-allocated system area logical addresses
    uint16_t      base_hdr_addr;    // Base object header starting logical page
    uint16_t      free_list_addr;   // Free list starting logical page
    uint16_t      ext_hdr_addrs[MAX_EXT_LEVELS]; // Extended object header page logical address array

    // Object header management
    uint16_t      max_obj_id;       // Current maximum allocated object ID (for sequential allocation)

    // GC related fields (following Head/Tail circular buffer model)
    uint16_t      gc_head_page;     // GC allocation pointer: points to next writable physical page
    uint16_t      gc_tail_page;     // GC reclamation pointer: points to next physical page to reclaim
    uint16_t      gc_threshold;     // GC trigger threshold (remaining free pages)
    uint32_t      total_user_pages; // Total user-available pages (excluding system reserved area)
    bool          gc_in_progress;   // GC in progress flag, prevents recursive GC triggering
    
    // Real-time tracking of valid pages in Radix Tree
    uint32_t      valid_page_count; // Number of unique logical sectors currently mapped
} eflash_ftl_t;

// --- Interface Functions ---
int  eflash_ftl_init(void);
uint16_t eflash_ftl_obj_alloc_header(void);  // Allocate next object header ID
int  eflash_ftl_obj_get_header(uint16_t obj_id, obj_header_t *hdr);
int  eflash_ftl_obj_set_header(uint16_t obj_id, const obj_header_t *hdr);

// System page access functions (used by space manager)
int  write_system_page(uint16_t lpn, const uint8_t *data);
uint16_t find_phys_page_by_sector(uint16_t sector);

// Read/write interface based on sector_id (recommended)
int  eflash_ftl_write(uint16_t sector_id, const uint8_t *data);
int  eflash_ftl_read(uint16_t sector_id, uint8_t *data);

// Read/write interface based on logical address (optional)
int  eflash_ftl_write_logical(uint32_t logical_addr, const uint8_t *data, int16_t size);
int  eflash_ftl_read_logical(uint32_t logical_addr, uint8_t *data, int16_t size);

void eflash_ftl_txn_begin(void);
int  eflash_ftl_txn_commit(void);  // Universal version (full page rewrite)
int  eflash_ftl_txn_commit_with_update(void);  // Optimized version (word update, requires hardware support)
void eflash_ftl_txn_abort(void);

// --- GC Interface Functions ---
int  eflash_ftl_gc_trigger(void);  // Manually trigger GC
int  eflash_ftl_gc_collect(uint16_t pages_to_free); // Reclaim specified number of pages
uint32_t eflash_ftl_get_free_pages(void); // Get current number of free pages (based on Head/Tail)
uint32_t eflash_ftl_get_real_free_pages(void); // Get REAL free pages by scanning all physical pages

// --- Visualization Functions (for debugging, only available when FTL_DEBUG_ENABLE is defined) ---
#ifdef FTL_DEBUG_ENABLE
void eflash_ftl_print_radix_tree_mermaid(eflash_ftl_t *ftl, uint16_t root_page); // Print radix tree in Mermaid format to stdout
void eflash_ftl_print_radix_tree_mermaid_to_file(eflash_ftl_t *ftl, uint16_t root_page); // Save radix tree to file
#endif

// --- Helper Functions (internal use, exposed for testing) ---
uint16_t find_phys_page_by_sector(uint16_t sector);  // Find physical page by logical sector ID
uint16_t find_sector_by_phys_page(uint16_t ppn);     // Find logical sector ID by physical page number

// --- Global FTL Instance ---
extern eflash_ftl_t g_ftl_instance;
#define FTL (&g_ftl_instance)

#ifdef FTL_DEBUG_ENABLE
void eflash_ftl_print_radix_tree_mermaid(eflash_ftl_t *ftl, uint16_t root_page); // Print radix tree in Mermaid format to stdout
void eflash_ftl_print_radix_tree_mermaid_to_file(eflash_ftl_t *ftl, uint16_t root_page); // Save radix tree to file
#endif

// --- Global FTL Instance ---
extern eflash_ftl_t g_ftl_instance;
#define FTL (&g_ftl_instance)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] Assertion failed: %s\n", msg); \
        while(1); \
    } \
} while(0)

#define ASSERT_FMT(cond, fmt, ...) do { \
    if (!(cond)) { \
        printf("  [FAIL] Assertion failed: " fmt "\n", ##__VA_ARGS__); \
        while(1); \
    } \
} while(0)

// --- Force Assert Macro (Unaffected by NDEBUG) ---
// In Release mode, standard assert() is disabled, causing tests to hang on failure
// Use this macro to ensure proper termination in all build modes
#define FORCE_ASSERT(expr, msg) do { \
    if (!(expr)) { \
        fprintf(stderr, "\n[ASSERTION FAILED] %s\n", msg); \
        fprintf(stderr, "  File: %s, Line: %d\n", __FILE__, __LINE__); \
        fprintf(stderr, "  Expression: %s\n\n", #expr); \
        fflush(stderr); \
        abort(); \
    } \
} while(0)

#endif