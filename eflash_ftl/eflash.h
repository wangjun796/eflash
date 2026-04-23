#ifndef EFLASH_H
#define EFLASH_H

/**
 * @file eflash.h
 * @brief eFlash FTL - Embedded Flash Translation Layer
 * 
 * A lightweight flash translation layer for NAND flash memory management,
 * featuring wear leveling, garbage collection, and transaction support.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Configuration Constants
// ============================================================================

// Physical Layout
#define EFLASH_PAGE_SIZE        512     ///< Page size in bytes
#define EFLASH_TOTAL_PAGES      2048    ///< Total pages (1MB)
#define META_SIZE               48      ///< Metadata size per page
#define USER_DATA_SIZE          (EFLASH_PAGE_SIZE - META_SIZE)  ///< User data per page (464 bytes)
#define RADIX_DEPTH             16      ///< Radix tree depth

// Object Header Management
#define BASE_HEADER_PAGES       8       ///< Base object header pages
#define BASE_HEADER_CAPACITY    232     ///< Base capacity (232 objects)
#define EXT_HEADER_PAGES_UNIT   4       ///< Pages per extension unit
#define EXT_HEADER_CAPACITY     116     ///< Capacity per extension (116 objects)
#define FREE_LIST_PAGES         4       ///< Free list pages
#define MAX_EXT_LEVELS          16      ///< Maximum extension levels

// System Area Layout (Logical Page Numbers)
#define SYS_OBJ_HEADER_BASE_LPN     0   ///< Base object header table LPN
#define SYS_OBJ_HEADER_PAGES        8   ///< Base object header table size
#define SYS_FREE_LIST_BASE_LPN      8   ///< Free list base LPN
#define SYS_FREE_LIST_PAGES         4   ///< Free list size
#define SYS_RESERVED_LPN_COUNT      12  ///< Total reserved system LPNs

// Object Types
#define OBJ_TYPE_NORMAL         0x00    ///< Normal object header
#define OBJ_TYPE_LINK           0xFF    ///< Extension link object

// LINK Object Magic Numbers (for recovery detection)
#define LINK_OBJ_MAGIC_PKG_ID       0x5F54  ///< "FT" - Flash Translation
#define LINK_OBJ_MAGIC_CLASS_ID     0x4C4E  ///< "LN" - LiNk
#define LINK_OBJ_MAGIC_RESERVED0    0xAD    ///< Magic byte 0
#define LINK_OBJ_MAGIC_RESERVED1    0xDE    ///< Magic byte 1

// Transaction Status
#define TXN_STATUS_BLANK        0xFF    ///< Never-written blank page
#define TXN_STATUS_PENDING      0xEF    ///< Reserved (unused)
#define TXN_STATUS_READY        0xAD    ///< Data written, waiting for commit
#define TXN_STATUS_COMMITTED    0x21    ///< Successfully committed
#define TXN_STATUS_INVALID      0x00    ///< Invalidated page

// Special Values
#define PAGE_NONE               0xFFFF  ///< Invalid page number
#define TXN_ID_NONE             0xFFFF  ///< Invalid transaction ID

// ============================================================================
// Data Structures
// ============================================================================

/**
 * @brief Object Header Structure (16 bytes, packed)
 * 
 * Stores metadata for each logical object in the flash.
 */
#ifndef EFLASH_FTL_H
// Only define if eflash_ftl.h is not already included
#ifdef _MSC_VER
#pragma pack(push, 1)
typedef struct {
    uint16_t    pkg_id;         ///< Package ID
    uint16_t    class_id;       ///< Class ID
    uint8_t     type;           ///< Type (OBJ_TYPE_NORMAL or OBJ_TYPE_LINK)
    uint8_t     reserved[3];    ///< Reserved bytes
    uint32_t    body_addr;      ///< Data body logical address
    uint32_t    body_size;      ///< Data body size in bytes
} obj_header_t;
#pragma pack(pop)
#else
typedef struct __attribute__((packed)) {
    uint16_t    pkg_id;         ///< Package ID
    uint16_t    class_id;       ///< Class ID
    uint8_t     type;           ///< Type (OBJ_TYPE_NORMAL or OBJ_TYPE_LINK)
    uint8_t     reserved[3];    ///< Reserved bytes
    uint32_t    body_addr;      ///< Data body logical address
    uint32_t    body_size;      ///< Data body size in bytes
} obj_header_t;
#endif
#endif // EFLASH_FTL_H

/**
 * @brief FTL Context Structure (Opaque)
 * 
 * Main context for the Flash Translation Layer.
 * Internal structure is hidden - use eflash_ftl_t* as opaque pointer.
 * 
 * Usage:
 *   - Get instance with eflash_get_ftl()
 *   - Initialize with eflash_ftl_init()
 *   - No need to free - uses static allocation
 */
#ifndef EFLASH_FTL_H
// If eflash_ftl.h is not included, define as opaque pointer
typedef struct eflash_ftl eflash_ftl_t;
#endif

/**
 * @brief Get the global FTL instance
 * 
 * Returns a pointer to the statically allocated FTL context.
 * This function is safe for embedded systems (no dynamic memory allocation).
 * 
 * @return Pointer to the global FTL instance (never NULL)
 */
eflash_ftl_t* eflash_get_ftl(void);

// ============================================================================
// Flash Simulation Interface (for testing/development)
// ============================================================================

/**
 * @brief Initialize simulated Flash file
 * @param filename Path to the flash simulation file
 * @return 0 on success, -1 on failure
 */
int eflash_init(const char *filename);

/**
 * @brief Deinitialize and close Flash simulation
 */
void eflash_deinit(void);

/**
 * @brief Erase a flash page (set to 0xFF)
 * @param page_addr Page address (0 to EFLASH_TOTAL_PAGES-1)
 * @return 0 on success, -1 on failure
 */
int eflash_hw_erase(uint16_t page_addr);

/**
 * @brief Program a flash page
 * @param page_addr Page address
 * @param data Data to program (EFLASH_PAGE_SIZE bytes)
 * @return 0 on success, -1 on failure
 */
int eflash_hw_prog(uint16_t page_addr, const uint8_t *data);

/**
 * @brief Read a flash page
 * @param page_addr Page address
 * @param data Buffer to read into (EFLASH_PAGE_SIZE bytes)
 * @return 0 on success, -1 on failure
 */
int eflash_hw_read(uint16_t page_addr, uint8_t *data);

/**
 * @brief Update words in a flash page (1->0 transitions only)
 * @param page_addr Page address
 * @param offset Byte offset within page
 * @param data 16-bit data to write
 * @return 0 on success, -1 on failure
 */
int eflash_hw_word_update(uint16_t page_addr, uint16_t offset, uint16_t data);

/**
 * @brief Check if a page is blank (all 0xFF)
 * @param page_addr Page address
 * @return 1 if blank, 0 if not, -1 on error
 */
int eflash_hw_is_blank(uint16_t page_addr);

// ============================================================================
// FTL Core Interface
// ============================================================================

/**
 * @brief Initialize the FTL instance
 * 
 * Performs initialization or recovery based on flash state.
 * Must be called before any other FTL operations.
 * 
 * @return 0 on success, negative error code on failure
 */
int eflash_ftl_init(void);

/**
 * @brief Allocate a new object header ID
 * 
 * Allocates the next available object ID sequentially.
 * Automatically handles extension when base capacity is exceeded.
 * 
 * @return Object ID (0 to max), or PAGE_NONE on failure
 */
uint16_t eflash_ftl_obj_alloc_header(void);

/**
 * @brief Read an object header by ID
 * 
 * @param obj_id Object ID to read
 * @param hdr Output buffer for object header (must be at least sizeof(obj_header_t))
 * @return 0 on success, -1 if object not found or invalid
 */
int eflash_ftl_obj_get_header(uint16_t obj_id, obj_header_t *hdr);

/**
 * @brief Write an object header by ID
 * 
 * @param obj_id Object ID to write
 * @param hdr Object header data to write
 * @return 0 on success, -1 on failure
 */
int eflash_ftl_obj_set_header(uint16_t obj_id, const obj_header_t *hdr);

// ============================================================================
// Data I/O Interface (Sector-based)
// ============================================================================

/**
 * @brief Write data to a logical sector
 * 
 * Writes USER_DATA_SIZE bytes to the specified sector.
 * The write is buffered until transaction commit.
 * 
 * @param sector_id Logical sector ID
 * @param data Data to write (USER_DATA_SIZE bytes)
 * @return 0 on success, negative error code on failure
 */
int eflash_ftl_write(uint16_t sector_id, const uint8_t *data);

/**
 * @brief Read data from a logical sector
 * 
 * Reads USER_DATA_SIZE bytes from the specified sector.
 * 
 * @param sector_id Logical sector ID
 * @param data Buffer to read into (USER_DATA_SIZE bytes)
 * @return 0 on success, negative error code on failure
 */
int eflash_ftl_read(uint16_t sector_id, uint8_t *data);

// ============================================================================
// Data I/O Interface (Byte-addressable)
// ============================================================================

/**
 * @brief Write data using byte-level logical address
 * 
 * Allocates space and writes data of arbitrary size.
 * 
 * @param logical_addr Logical byte address (output from eflash_mgr_alloc)
 * @param data Data to write
 * @param size Number of bytes to write
 * @return 0 on success, negative error code on failure
 */
int eflash_ftl_write_logical(uint32_t logical_addr, const uint8_t *data, int16_t size);

/**
 * @brief Read data using byte-level logical address
 * 
 * @param logical_addr Logical byte address
 * @param data Buffer to read into
 * @param size Number of bytes to read
 * @return 0 on success, negative error code on failure
 */
int eflash_ftl_read_logical(uint32_t logical_addr, uint8_t *data, int16_t size);

// ============================================================================
// Transaction Management
// ============================================================================

/**
 * @brief Begin a new transaction
 * 
 * All subsequent writes will be part of this transaction
 * until commit or abort is called.
 */
void eflash_ftl_txn_begin(void);

/**
 * @brief Commit the current transaction (universal method)
 * 
 * Commits all pending writes by rewriting full pages.
 * Works on all hardware but slower than word update.
 * 
 * @return 0 on success, negative error code on failure
 */
int eflash_ftl_txn_commit(void);

/**
 * @brief Commit the current transaction (optimized method)
 * 
 * Commits using word-level updates (1->0 bit transitions only).
 * Faster and extends flash lifespan, but requires hardware support.
 * 
 * @return 0 on success, negative error code on failure
 */
int eflash_ftl_txn_commit_with_update(void);

/**
 * @brief Abort the current transaction
 * 
 * Discards all pending writes and rolls back to previous state.
 */
void eflash_ftl_txn_abort(void);

// ============================================================================
// Garbage Collection Interface
// ============================================================================

/**
 * @brief Manually trigger garbage collection
 * 
 * Triggers GC if free space is below threshold.
 * 
 * @return 0 if GC was triggered or not needed, negative on error
 */
int eflash_ftl_gc_trigger(void);

/**
 * @brief Force garbage collection to free specific pages
 * 
 * @param pages_to_free Minimum number of pages to reclaim
 * @return Number of pages actually freed, or negative on error
 */
int eflash_ftl_gc_collect(uint16_t pages_to_free);

/**
 * @brief Get current number of free pages
 * 
 * @return Number of free pages available
 */
uint32_t eflash_ftl_get_free_pages(void);

// ============================================================================
// Space Manager Interface (Advanced)
// ============================================================================

/**
 * @brief Allocate logical address space
 * 
 * Allocates a contiguous block of logical address space.
 * Used for byte-addressable I/O operations.
 * 
 * @param size Size in bytes to allocate
 * @param out_logical_addr Output: allocated logical address
 * @return 0 on success, -1 if insufficient space
 */
int eflash_mgr_alloc(uint32_t size, uint32_t *out_logical_addr);

/**
 * @brief Free logical address space
 * 
 * @param logical_addr Starting logical address to free
 * @param size Size in bytes to free
 */
void eflash_mgr_free(uint32_t logical_addr, uint32_t size);

/**
 * @brief Get total free bytes available
 * 
 * @return Total free bytes
 */
uint32_t eflash_mgr_get_free_bytes(void);

#ifdef __cplusplus
}
#endif

#endif // EFLASH_H
