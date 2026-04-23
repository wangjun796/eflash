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
#define FREE_NODE_PAGE_COUNT    4       // Reserve 4 pages for free_node table
#define FREE_NODES_PER_PAGE     58      // Nodes per page: 464/8 = 58
#define TOTAL_FREE_NODES        (FREE_NODE_PAGE_COUNT * FREE_NODES_PER_PAGE)  // Total 232 nodes
#define FREE_NODE_HEADER_SIZE   4       // First 4 bytes of each page store count

// --- Object Header Configuration ---
#define BASE_HEADER_PAGES       8       // Base object header pages
#define OBJ_HEADER_SIZE         16      // Each object header is 16 bytes
#define BASE_OBJ_CAPACITY       ((EFLASH_PAGE_SIZE * BASE_HEADER_PAGES - META_SIZE * BASE_HEADER_PAGES) / OBJ_HEADER_SIZE)  // 232 objects
#define EXT_HEADER_PAGES_UNIT   4       // 4 pages per extension
#define EXT_OBJ_CAPACITY        ((EFLASH_PAGE_SIZE * EXT_HEADER_PAGES_UNIT - META_SIZE * EXT_HEADER_PAGES_UNIT - 4) / OBJ_HEADER_SIZE)  // 116 objects (minus 4-byte link pointer)

// --- Space Manager Context ---
typedef struct {
    uint16_t    total_pages;            // Total Flash pages
    uint16_t    free_node_pages[FREE_NODE_PAGE_COUNT];  // Physical page numbers for free_node table
    uint16_t    header_pages[BASE_HEADER_PAGES];        // Physical page numbers for base object headers
    uint16_t    next_alloc_page;        // Next physical page to allocate (for sequential allocation optimization)
} eflash_mgr_t;

/**
 * eflash_mgr_init: Initialize space manager (memory-only, no Flash writes)
 * @mgr: Space manager instance
 * @total_pages: Total Flash pages
 */
void eflash_mgr_init(uint16_t total_pages);

/**
 * eflash_mgr_alloc: Allocate specified size of logical address space
 * @size: Requested size in bytes
 * @out_logical_addr: Output allocated starting logical address (24-bit)
 * @return: 0 success, -1 failure (insufficient space)
 */
int eflash_mgr_alloc(uint32_t size, uint32_t *out_logical_addr);

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
