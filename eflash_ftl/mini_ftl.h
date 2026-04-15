#ifndef MINI_FTL_H
#define MINI_FTL_H

#include <stdint.h>
#include <stdbool.h>
#include "ecc/bch.h"
#include "space_mgr.h"

// --- 跨平台 packed 结构支持 ---
#ifdef _MSC_VER
    #define PACKED_STRUCT __pragma(pack(push, 1))
    #define PACKED_STRUCT_END __pragma(pack(pop))
    #define ATTRIBUTE_PACKED
#else
    #define PACKED_STRUCT
    #define PACKED_STRUCT_END
    #define ATTRIBUTE_PACKED __attribute__((packed))
#endif

// --- 物理布局配置 ---
#define EFLASH_PAGE_SIZE    512
#define META_SIZE           48
#define USER_DATA_SIZE      (EFLASH_PAGE_SIZE - META_SIZE)
#define RADIX_DEPTH         16

// --- 对象头管理配置 ---
#define BASE_HEADER_PAGES     8   // 基础对象头页数
#define BASE_HEADER_CAPACITY  232 // 基础容量 (8 * 464 / 16)
#define EXT_HEADER_PAGES_UNIT 4   // 每次扩展的页数
#define EXT_HEADER_CAPACITY   116 // 每扩展单元的容量 (29 * 4 - 4个指针位)
#define FREE_LIST_PAGES       4   // 空闲链表页数

// --- 对象头结构 (16 字节) ---
PACKED_STRUCT
typedef struct {
    uint16_t    pkg_id;         // 包 ID
    uint16_t    class_id;       // 类 ID
    uint8_t     type;           // 类型
    uint8_t     reserved[3];
    uint32_t    body_addr;      // 数据体逻辑地址
    uint32_t    body_size;      // 数据体大小
} ATTRIBUTE_PACKED obj_header_t;
PACKED_STRUCT_END

// --- 事务状态机 (Status Field) ---
// 0xAD -> 0x21 翻转符合 Flash 编程特性 (1->0)
#define TXN_STATUS_BLANK        0xFF  // 从未写过的空白页
#define TXN_STATUS_PENDING      0xEF  // 保留（未使用）
#define TXN_STATUS_READY        0xAD  // 事务数据页已写入，等待提交
#define TXN_STATUS_COMMITTED    0x21  // 事务已成功提交（通常仅标记 Root 页）
#define TXN_STATUS_INVALID      0x00  // 非事务页或已失效的旧页

// --- 特殊值 ---
#define PAGE_NONE             0xFFFF
#define TXN_ID_NONE           0xFFFF

// --- 元数据结构 (48 字节) ---
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

// --- FTL 上下文 ---
typedef struct {
    space_mgr_t   spc_mgr;
    uint16_t      root_page;
    uint16_t      shadow_root;
    uint32_t      next_count;
    uint16_t      current_epoch;
    uint16_t      active_txn_id;
    bool          is_initialized;

    // 预分配的系统区逻辑地址
    uint16_t      base_hdr_addr;    // 基础对象头起始逻辑页
    uint16_t      free_list_addr;   // 空闲链表起始逻辑页
    uint16_t      ext_hdr_addrs[16];// 扩展对象头页的逻辑地址数组
} mini_ftl_t;

// --- 接口函数 ---
int  mini_ftl_init(mini_ftl_t *ftl);
int  mini_ftl_obj_get_header(mini_ftl_t *ftl, uint16_t obj_id, obj_header_t *hdr);
int  mini_ftl_obj_set_header(mini_ftl_t *ftl, uint16_t obj_id, const obj_header_t *hdr);
int  mini_ftl_write(mini_ftl_t *ftl, uint16_t sector_id, const uint8_t *data);
int  mini_ftl_read(mini_ftl_t *ftl, uint16_t sector_id, uint8_t *data);
void mini_ftl_txn_begin(mini_ftl_t *ftl);
int  mini_ftl_txn_commit(mini_ftl_t *ftl);
void mini_ftl_txn_abort(mini_ftl_t *ftl);

#endif