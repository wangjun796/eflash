#ifndef EFLASH_MGR_H
#define EFLASH_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "eflash_sim.h"

// --- Free Node Structure (8 bytes) ---
// 4-byte logical address + 4-byte size (unit: bytes)
#ifdef _MSC_VER
#pragma pack(push, 1)
typedef struct {
    uint32_t    addr;           // 32-bit logical address
    uint32_t    size;           // Number of consecutive free bytes
} free_node_t;
#pragma pack(pop)
#else
typedef struct __attribute__((packed)) {
    uint32_t    addr;           // 32-bit logical address
    uint32_t    size;           // Number of consecutive free bytes
} free_node_t;
#endif

// --- Free Node Page Layout ---
#define FREE_NODE_PAGE_COUNT    4       // Reserve 4 pages for free_node table (base level)
#define FREE_NODE_HEADER_SIZE   2       // First 2 bytes of each page store count (uint16_t)
#define FREE_NODES_PER_PAGE     57      // Nodes per page: (464-2)/8 = 57
#define TOTAL_FREE_NODES_BASE   (FREE_NODE_PAGE_COUNT * FREE_NODES_PER_PAGE)  // Base level: 228 nodes
#define FREE_NODE_LINK_SIZE     6       // Link info size at end of last page (2-byte magic + 4-byte addr)
#define FREE_NODE_EXT_PAGES     4       // Each extension allocates 4 pages
#define MAX_FREE_NODE_EXT_LEVELS 4      // Maximum extension levels (total nodes: 228 + 4*57*4 = 1140)

// Link node structure for extended free node chain (6 bytes)
#ifdef _MSC_VER
#pragma pack(push, 1)
typedef struct {
    uint16_t    magic;          // LINK_FREE_NODE_MAGIC = 0x5F54
    uint32_t    next_ext_addr;  // Logical address of next 4-page extension block
} free_node_link_t;
#pragma pack(pop)
#else
typedef struct __attribute__((packed)) {
    uint16_t    magic;          // LINK_FREE_NODE_MAGIC = 0x5F54
    uint32_t    next_ext_addr;  // Logical address of next 4-page extension block
} free_node_link_t;
#endif

#define LINK_FREE_NODE_MAGIC    0x5F54  // Magic number for free node link

// --- Object Header Configuration ---
#define BASE_HEADER_PAGES       8       // Base object header pages
#define OBJ_HEADER_SIZE         16      // Each object header is 16 bytes
#define BASE_OBJ_CAPACITY       ((EFLASH_PAGE_SIZE * BASE_HEADER_PAGES - META_SIZE * BASE_HEADER_PAGES) / OBJ_HEADER_SIZE)  // 232 objects
#define EXT_HEADER_PAGES_UNIT   4       // 4 pages per extension
#define EXT_OBJ_CAPACITY        ((EFLASH_PAGE_SIZE * EXT_HEADER_PAGES_UNIT - META_SIZE * EXT_HEADER_PAGES_UNIT - 4) / OBJ_HEADER_SIZE)  // 116 objects (minus 4-byte link pointer)

// --- Space Manager Context ---
typedef struct {
    uint16_t    total_pages;            // Total Flash pages
    uint16_t    free_node_pages[FREE_NODE_PAGE_COUNT];  // Physical page numbers for base free_node table
    uint32_t    ext_free_node_addrs[MAX_FREE_NODE_EXT_LEVELS];  // Logical addresses of extended free node blocks (4 pages each)
    uint16_t    header_pages[BASE_HEADER_PAGES];        // Physical page numbers for base object headers
    uint16_t    next_alloc_page;        // Next physical page to allocate (for sequential allocation optimization)
    uint32_t    total_free_nodes;       // Total free node count across all levels (maintained in memory)
} eflash_mgr_t;

/**
 * eflash_mgr_init: Initialize space manager (memory-only, no Flash writes)
 * @mgr: Space manager instance
 * @total_pages: Total Flash pages
 */
void eflash_mgr_init(uint16_t total_pages);

/**
 * eflash_mgr_recover_ext_free_nodes: Recover extended free node table from Flash
 * @return: Number of extension levels recovered, -1 on failure
 * 
 * Note: This function scans the LINK chain in base and extended free node pages
 *       to rebuild the ext_free_node_addrs array after power failure.
 */
int eflash_mgr_recover_ext_free_nodes(void);

/**
 * eflash_mgr_alloc: Allocate specified size of logical address space
 * @size: Requested size in bytes
 * @out_logical_addr: Output allocated starting logical address (24-bit)
 * @return: 0 success, -1 failure (insufficient space)
 */
int eflash_mgr_alloc(uint32_t size, uint32_t *out_logical_addr);

/**
 * eflash_mgr_alloc_pages: Allocate page-aligned logical address space
 * @pages: Number of pages to allocate
 * @out_logical_addr: Output allocated starting logical address (guaranteed page-aligned)
 * @return: 0 success, -1 failure (insufficient space)
 * 
 * Note: This function ensures the returned address is aligned to USER_DATA_SIZE boundary.
 *       It may temporarily allocate extra space for alignment, then free it.
 */
int eflash_mgr_alloc_pages(uint16_t pages, uint32_t *out_logical_addr);

/**
 * eflash_mgr_free: Free specified logical address space and merge adjacent free blocks
 * @logical_addr: Starting logical address to free (24-bit)
 * @size: Size in bytes to free
 */
void eflash_mgr_free(uint32_t logical_addr, uint32_t size);

/**
 * eflash_mgr_sync: Sync free_node table to Flash (batch sync optimization)
 * @mgr: Space manager instance
 */
void eflash_mgr_sync(void);

/**
 * eflash_mgr_get_free_bytes: Get remaining available bytes
 * @return: Total free bytes
 */
uint32_t eflash_mgr_get_free_bytes(void);

/**
 * eflash_mgr_check_initialized: Check if space manager has been initialized
 * @mgr: Space manager instance
 * @return: true if initialized (free list contains at least one valid node), false otherwise
 */
bool eflash_mgr_check_initialized(void);

/**
 * eflash_mgr_init_free_list: Initialize free list with one large node (called by FTL after system pages allocated)
 * @mgr: Space manager instance
 * @total_pages: Total Flash pages
 * @reserved_logic_pages: Number of reserved logical pages (default 12)
 * @return: 0 success, -1 failure
 */
int eflash_mgr_init_free_list(uint16_t total_pages, uint16_t reserved_logic_pages);

// --- Global Space Manager Access ---
#define MGR (&g_ftl_instance.spc_mgr)

#endif
