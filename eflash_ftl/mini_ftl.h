#ifndef MINI_FTL_H
#define MINI_FTL_H

#include <stdint.h>
#include <stdbool.h>
#include "ecc/bch.h"
#include "space_mgr.h"

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
#define EXT_HEADER_CAPACITY   116 // Capacity per extension unit (29 * 4 - 4 pointer fields)
#define FREE_LIST_PAGES       4   // Free list pages

// --- Object Header Structure (16 bytes) ---
PACKED_STRUCT
typedef struct {
    uint16_t    pkg_id;         // Package ID
    uint16_t    class_id;       // Class ID
    uint8_t     type;           // Type
    uint8_t     reserved[3];
    uint32_t    body_addr;      // Data body logical address
    uint32_t    body_size;      // Data body size
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
    uint16_t        sector_id;
    uint32_t        global_count;
    uint16_t        epoch;
    uint16_t        alt[RADIX_DEPTH];
    uint16_t        txn_id;
    uint8_t         status;
    uint8_t         ecc[5];
} ATTRIBUTE_PACKED ftl_meta_t;
PACKED_STRUCT_END

// --- FTL Context ---
typedef struct {
    space_mgr_t   spc_mgr;
    uint16_t      root_page;
    uint16_t      shadow_root;
    uint32_t      next_count;
    uint16_t      current_epoch;
    uint16_t      active_txn_id;
    bool          is_initialized;

    // Pre-allocated system area logical addresses
    uint16_t      base_hdr_addr;    // Base object header starting logical page
    uint16_t      free_list_addr;   // Free list starting logical page
    uint16_t      ext_hdr_addrs[16];// Extended object header page logical address array
    
    // GC related fields (following Dhara Head/Tail model)
    uint16_t      gc_head_page;     // GC allocation pointer: points to next writable physical page
    uint16_t      gc_tail_page;     // GC reclamation pointer: points to next physical page to reclaim
    uint16_t      gc_threshold;     // GC trigger threshold (remaining free pages)
    uint32_t      total_user_pages; // Total user-available pages (excluding system reserved area)
    bool          gc_in_progress;   // GC in progress flag, prevents recursive GC triggering
} mini_ftl_t;

// --- Interface Functions ---
int  mini_ftl_init(mini_ftl_t *ftl);
int  mini_ftl_obj_get_header(mini_ftl_t *ftl, uint16_t obj_id, obj_header_t *hdr);
int  mini_ftl_obj_set_header(mini_ftl_t *ftl, uint16_t obj_id, const obj_header_t *hdr);

// Read/write interface based on sector_id (recommended)
int  mini_ftl_write(mini_ftl_t *ftl, uint16_t sector_id, const uint8_t *data);
int  mini_ftl_read(mini_ftl_t *ftl, uint16_t sector_id, uint8_t *data);

// Read/write interface based on logical address (optional)
int  mini_ftl_write_logical(mini_ftl_t *ftl, uint32_t logical_addr, const uint8_t *data);
int  mini_ftl_read_logical(mini_ftl_t *ftl, uint32_t logical_addr, uint8_t *data);

void mini_ftl_txn_begin(mini_ftl_t *ftl);
int  mini_ftl_txn_commit(mini_ftl_t *ftl);
void mini_ftl_txn_abort(mini_ftl_t *ftl);

// --- GC Interface Functions ---
int  mini_ftl_gc_trigger(mini_ftl_t *ftl);  // Manually trigger GC
int  mini_ftl_gc_collect(mini_ftl_t *ftl, uint16_t pages_to_free); // Reclaim specified number of pages
uint32_t mini_ftl_get_free_pages(mini_ftl_t *ftl); // Get current number of free pages

#endif