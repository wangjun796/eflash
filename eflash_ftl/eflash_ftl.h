#ifndef EFLASH_FTL_H
#define EFLASH_FTL_H

#include <stdint.h>
#include <stdbool.h>
#include "bch.h"  /* Changed from "ecc/bch.h" for better include path compatibility */
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
//     LPN 12              : Code Region management data (reserved)
//   DYNAMIC Pages (LPN 13+):
//     User data pages: allocated by FTL write operations
//     Extended object headers: allocated by eflash_mgr_alloc (LPN not fixed)
#define SYS_OBJ_HEADER_BASE_LPN   0       // Base object header table starts at LPN 0
#define SYS_OBJ_HEADER_PAGES      8       // Base object header table size
#define SYS_FREE_LIST_BASE_LPN    8       // Free list starts at LPN 8
#define SYS_FREE_LIST_PAGES       4       // Free list size
#define SYS_CODE_REGION_LPN       12      // Code Region management data (reserved)
#define SYS_RESERVED_LPN_COUNT    13      // Total reserved LPNs for system areas (0-12)

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

// --- SuperBlock and Journal Configuration (for fast O(log N) recovery) ---
#define SUPERBLOCK_MAGIC        0xEFLASH  // Magic number for validation
#define SUPERBLOCK_VERSION      0x0100    // Version 1.0
#define SUPERBLOCK_LPN_START    0         // SuperBlock starts at LPN 0
#define SUPERBLOCK_PAGES        4         // Total SuperBlock pages (Header + Journal + Backup)
#define JOURNAL_MAX_ENTRIES     256       // Maximum journal entries per cycle
#define JOURNAL_ENTRY_SIZE      32        // Size of each journal entry in bytes

// --- Code Region Management Configuration ---
#define CODE_REGION_MAGIC       0xC0DE    // Magic number for code region
#define CODE_REGION_START_PPN   0         // Code region always starts at PPN 0
#define CODE_MIGRATE_IDLE       0x00      // Migration idle
#define CODE_MIGRATE_IN_PROGRESS 0x01     // Migration in progress
#define CODE_MIGRATE_COMPLETE   0x02      // Migration complete, pending reclaim
#define CODE_MIGRATE_FAILED     0xFF      // Migration failed

// --- Journal Entry Types ---
#define JOURNAL_OP_WRITE        0x01  // New write operation
#define JOURNAL_OP_UPDATE       0x02  // Update existing mapping
#define JOURNAL_OP_TRIM         0x03  // Trim/discard operation
#define JOURNAL_OP_GC_MIGRATE   0x04  // GC migration operation

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

// --- Journal Entry Structure (32 bytes) ---
// Used for fast O(log N) recovery
PACKED_STRUCT
typedef struct {
    uint32_t    global_count;     // Monotonically increasing counter
    uint16_t    sector_id;        // Logical sector ID
    uint16_t    phys_ppn;         // Physical page number
    uint8_t     op_type;          // Operation type (WRITE/UPDATE/TRIM/GC)
    uint8_t     reserved[3];      // Padding for alignment
    uint16_t    checksum;         // Simple checksum for validation
} ATTRIBUTE_PACKED journal_entry_t;
PACKED_STRUCT_END

// --- SuperBlock Header Structure (fits in one page) ---
PACKED_STRUCT
typedef struct {
    uint32_t    magic;            // SUPERBLOCK_MAGIC (0xEFLASH)
    uint16_t    version;          // SUPERBLOCK_VERSION
    uint16_t    root_page;        // Current root page PPN
    uint32_t    next_count;       // Next global count value
    uint16_t    active_txn_id;    // Last active transaction ID
    uint16_t    gc_head_page;     // GC head pointer
    uint16_t    gc_tail_page;     // GC tail pointer
    uint32_t    valid_page_count; // Number of valid pages
    uint16_t    journal_start_idx; // Start index of current journal cycle
    uint16_t    journal_end_idx;   // End index (latest entry)
    uint16_t    current_epoch;    // Current epoch counter
    uint8_t     reserved[18];     // Reserved for future use
    uint16_t    checksum;         // CRC or simple checksum
} ATTRIBUTE_PACKED superblock_header_t;
PACKED_STRUCT_END

// --- Code Region Information Structure ---
// Stored in a dedicated system page for power-failure recovery
// Total size: 464 bytes (fits in one logical page)

// Migration record: maps a code segment from logical to physical address
// Each record represents one continuous code segment
PACKED_STRUCT
typedef struct {
    uint32_t    logical_addr;     // 4 bytes: Logical byte address (start of segment)
    uint32_t    physical_addr;    // 4 bytes: Physical byte address (PPN * 512 + offset)
    uint16_t    size;             // 2 bytes: Segment size in bytes (max 64KB)
} ATTRIBUTE_PACKED migration_record_t;
PACKED_STRUCT_END

// Maximum number of migration records that fit in available space
// Available: 464 - 26 (header) - 2 (checksum) - 2 (count) = 434 bytes
// Each record: 10 bytes (4 + 4 + 2)
// Max records: 434 / 10 = 43 records (with 4 bytes padding)
#define MAX_MIGRATION_RECORDS  43

PACKED_STRUCT
typedef struct {
    // Header (26 bytes)
    uint32_t    magic;            // CODE_REGION_MAGIC (0xC0DE)
    uint16_t    start_ppn;        // Code region start physical page (always 0)
    uint16_t    num_pages;        // Number of pages in code region
    uint8_t     status;           // Migration status (IDLE/IN_PROGRESS/COMPLETE/FAILED)
    uint8_t     reserved1[3];     // Padding
    uint16_t    src_lpn;          // Source logical page number (during migration)
    uint16_t    dst_ppn;          // Destination physical page number (during migration)
    uint16_t    pages_migrated;   // Number of pages already migrated
    uint16_t    total_pages;      // Total pages to migrate
    uint32_t    code_size_bytes;  // Total code size in bytes
    
    // Checksum (2 bytes)
    uint16_t    checksum;         // Checksum for validation
    
    // Migration record count (2 bytes)
    // Tracks the number of valid entries in migration_map[]
    // Used for power-failure recovery and space reuse when code region is deleted
    uint16_t    migration_records_count;
    
    // Migration mapping array (43 records ¡Á 10 bytes = 430 bytes)
    // Records logical ¡ú physical address mappings for power-failure recovery
    migration_record_t migration_map[MAX_MIGRATION_RECORDS];
    
    // Remaining padding (4 bytes)
    uint8_t     reserved2[4];     // Padding to fill 464 bytes
} ATTRIBUTE_PACKED code_region_info_t;
PACKED_STRUCT_END

// Global code region info (extern declaration for test access)
extern code_region_info_t g_code_region;

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
int  eflash_ftl_gc_collect_all(void);  // Reclaim all stale pages (maximize reclamation)
uint32_t eflash_ftl_get_free_pages(void); // Get current number of free pages (based on Head/Tail)
uint32_t eflash_ftl_get_real_free_pages(void); // Get REAL free pages by scanning all physical pages

// --- Trim/Discard Interface Functions (for deleting unused data) ---
int  eflash_ftl_trim(uint16_t sector_id);  // Trim single sector (mark as invalid)
int  eflash_ftl_trim_range(uint16_t start_sector, uint16_t count);  // Trim sector range
int  eflash_ftl_trim_object(uint16_t obj_id);  // Trim entire object (all its sectors)

// --- SuperBlock and Journal Functions (for fast O(log N) recovery) ---
int  eflash_ftl_superblock_init(void);  // Initialize SuperBlock on first boot
int  eflash_ftl_superblock_update(void);  // Update SuperBlock after major operations
int  eflash_ftl_journal_append(const journal_entry_t *entry);  // Append entry to journal
int  eflash_ftl_fast_recovery(void);  // Fast recovery using SuperBlock + Journal (O(log N))

// --- Code Region Management Functions ---
int  eflash_ftl_code_region_init(void);  // Initialize code region management
int  eflash_ftl_code_migrate_from_logical(uint16_t src_lpn, uint16_t num_pages);  // Migrate code from logical to physical
int  eflash_ftl_code_region_expand(uint16_t additional_pages);  // Expand code region
int  eflash_ftl_code_region_shrink(uint16_t pages_to_remove);  // Shrink code region
int  eflash_ftl_code_region_delete_segment(uint32_t logical_addr);  // Delete a code segment by logical address
uint16_t eflash_ftl_get_code_region_size(void);  // Get current code region size in pages
int  eflash_ftl_code_region_recover(void);  // Recover from power failure during migration
int  eflash_ftl_gc_reclaim_code_region(uint16_t pages_needed);  // GC to reclaim pages after code region
int  eflash_ftl_code_read(uint16_t page_offset, uint8_t *buffer, uint16_t size);  // Read code from code region

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