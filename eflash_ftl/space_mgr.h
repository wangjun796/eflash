#ifndef SPACE_MGR_H
#define SPACE_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "eflash_sim.h"

// 如果 META_SIZE 未在 eflash_sim.h 中定义，这里提供一个默认值（通常为0或特定开销）
// 请根据实际 Flash 模拟器的元数据开销调整此值
#ifndef META_SIZE
#define META_SIZE 0
#endif

// --- Free Node 结构 (5字节) ---
// 3字节逻辑地址 + 2字节大小
#ifdef _MSC_VER
#pragma pack(push, 1)
typedef struct {
    uint8_t     addr[3];        // 逻辑页地址（3字节，支持最多16M页）
    uint16_t    size;           // 连续空闲页数
} free_node_t;
#pragma pack(pop)
#else
typedef struct __attribute__((packed)) {
    uint8_t     addr[3];        // 逻辑页地址（3字节，支持最多16M页）
    uint16_t    size;           // 连续空闲页数
} free_node_t;
#endif

// --- Free Node Page 布局 ---
#define FREE_NODE_PAGE_COUNT    4       // 预留4页存储free_node表
#define FREE_NODES_PER_PAGE     92      // 每页node数: (464-4)/5 = 92
#define TOTAL_FREE_NODES        (FREE_NODE_PAGE_COUNT * FREE_NODES_PER_PAGE)  // 总计368个node
#define FREE_NODE_HEADER_SIZE   4       // 每页开头4字节存储count

// --- 对象头配置 ---
#define BASE_HEADER_PAGES       8       // 基础对象头页数
#define OBJ_HEADER_SIZE         16      // 每个对象头16字节
#define BASE_OBJ_CAPACITY       ((EFLASH_PAGE_SIZE * BASE_HEADER_PAGES - META_SIZE * BASE_HEADER_PAGES) / OBJ_HEADER_SIZE)  // 232个
#define EXT_HEADER_PAGES_UNIT   4       // 每次扩展4页
#define EXT_OBJ_CAPACITY        ((EFLASH_PAGE_SIZE * EXT_HEADER_PAGES_UNIT - META_SIZE * EXT_HEADER_PAGES_UNIT - 4) / OBJ_HEADER_SIZE)  // 116个（减去4字节链接指针）

// --- 空间管理器上下文 ---
typedef struct {
    uint16_t    total_pages;            // Flash总页数
    uint16_t    free_node_pages[FREE_NODE_PAGE_COUNT];  // free_node表的物理页号
    uint16_t    header_pages[BASE_HEADER_PAGES];        // 基础对象头页的物理页号
    uint16_t    next_alloc_page;        // 下一个待分配的物理页（用于顺序分配优化）
} space_mgr_t;

/**
 * space_mgr_init: 初始化空间管理器
 * @mgr: 空间管理器实例
 * @total_pages: Flash总页数
 */
void space_mgr_init(space_mgr_t *mgr, uint16_t total_pages);

/**
 * space_mgr_alloc: 分配指定大小的连续空间
 * @mgr: 空间管理器实例
 * @size: 请求的字节数
 * @out_page: 输出分配的起始物理页号
 * @out_offset: 输出页内偏移（始终为0，因为按页分配）
 * @return: 0成功，-1失败
 */
int space_mgr_alloc(space_mgr_t *mgr, uint32_t size, uint16_t *out_page, uint16_t *out_offset);

/**
 * space_mgr_free: 释放指定空间并合并相邻空闲块
 * @mgr: 空间管理器实例
 * @page: 释放的起始物理页号
 * @offset: 页内偏移（未使用）
 * @size: 释放的字节数
 */
void space_mgr_free(space_mgr_t *mgr, uint16_t page, uint16_t offset, uint32_t size);

/**
 * space_mgr_sync: 同步free_node表到Flash
 * @mgr: 空间管理器实例
 */
void space_mgr_sync(space_mgr_t *mgr);

/**
 * space_mgr_get_free_bytes: 获取剩余空闲字节数
 * @mgr: 空间管理器实例
 * @return: 空闲字节数
 */
uint32_t space_mgr_get_free_bytes(space_mgr_t *mgr);

#endif // SPACE_MGR_H
