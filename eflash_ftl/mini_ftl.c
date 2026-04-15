#include "mini_ftl.h"
#include "eflash_sim.h"
#include "space_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

// --- 调试开关配置 ---
// 设置为 1 开启详细调试输出，设置为 0 关闭（测试时建议关闭）
#define FTL_DEBUG_ENABLE 1  // 开启调试以诊断压力测试问题

#if FTL_DEBUG_ENABLE
#define FTL_DEBUG(fmt, ...) printf("[FTL_DEBUG] " fmt, ##__VA_ARGS__)
#else
#define FTL_DEBUG(fmt, ...) do {} while(0)
#endif

// --- BCH ECC 包装函数 ---

// bch_encode: 生成ECC校验码
static void bch_encode(const struct bch_def *bch, const uint8_t *data, size_t len, uint8_t *ecc) {
    bch_generate(bch, data, len, ecc);
}

// bch_decode: 验证并纠正错误
// 返回: 0 无错误, >0 已纠正的错误数, -1 不可纠正
static int bch_decode(const struct bch_def *bch, uint8_t *data, size_t len, const uint8_t *ecc) {
    // 先验证
    if (bch_verify(bch, data, len, ecc) == 0) {
        return 0; // 无错误
    }

    // 需要纠错，创建副本进行修复
    // 注意：这里需要确保 data_copy 和 ecc_copy 足够大
    // 假设最大数据长度为 EFLASH_PAGE_SIZE 或 META_SIZE，具体取决于调用上下文
    // 为安全起见，使用较大的静态缓冲区或动态分配，这里参考方案使用了固定大小
    // 如果 META_SIZE 较小，可以使用 META_SIZE
    uint8_t data_copy[META_SIZE];
    uint8_t ecc_copy[5]; // 参考代码中 ecc 大小为 5 字节 (META_SIZE - (META_SIZE-5))

    if (len > sizeof(data_copy)) return -1; // 安全检查

    memcpy(data_copy, data, len);
    memcpy(ecc_copy, ecc, 5); // 假设 ecc 长度为 5

    bch_repair(bch, data_copy, len, ecc_copy);

    // 再次验证
    if (bch_verify(bch, data_copy, len, ecc_copy) != 0) {
        return -1; // 仍然有错误，不可纠正
    }

    // 复制纠正后的数据回去
    memcpy(data, data_copy, len);
    return 1; // 假设纠正了错误
}

#define TOTAL_PAGES EFLASH_TOTAL_PAGES
#define META_OFFSET USER_DATA_SIZE
static const struct bch_def *bch_cfg = &bch_3bit;

// --- ECC 辅助函数 ---

/**
 * calc_page_ecc: 计算完整页的ECC校验码（用户数据 + 元数据除ECC部分）
 * @page_buf: 指向完整页缓冲区的指针（包含USER_DATA_SIZE字节用户数据和META_SIZE字节元数据）
 * 
 * 布局说明：
 * - page_buf[0 .. USER_DATA_SIZE-1]: 用户数据
 * - page_buf[USER_DATA_SIZE .. USER_DATA_SIZE+META_SIZE-6]: 元数据（不含ECC）
 * - page_buf[USER_DATA_SIZE+META_SIZE-5 .. USER_DATA_SIZE+META_SIZE-1]: ECC校验码（5字节）
 */
static void calc_page_ecc(uint8_t *page_buf) {
    // ECC保护范围：用户数据 + 元数据（不含ECC字段）
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
    
    // ECC存储在元数据的最后5字节
    uint8_t *ecc_ptr = page_buf + USER_DATA_SIZE + META_SIZE - 5;
    
    bch_encode(bch_cfg, page_buf, protected_len, ecc_ptr);
}

/**
 * verify_and_correct_page: 校验并尝试纠正完整页数据
 * @page_buf: 指向完整页缓冲区的指针
 * 返回: 0 成功, -1 失败 (不可纠正的错误)
 */
static int verify_and_correct_page(uint8_t *page_buf) {
    // ECC保护范围：用户数据 + 元数据（不含ECC字段）
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
    
    // ECC存储在元数据的最后5字节
    uint8_t *ecc_ptr = page_buf + USER_DATA_SIZE + META_SIZE - 5;
    
    FTL_DEBUG("[ECC] Verifying page: protected_len=%zu, ecc at offset %zu\n", 
              protected_len, (size_t)(ecc_ptr - page_buf));
    
    // 创建副本进行纠错
    uint8_t data_copy[EFLASH_PAGE_SIZE];
    memcpy(data_copy, page_buf, EFLASH_PAGE_SIZE);
    
    // 先验证
    int verify_result = bch_verify(bch_cfg, data_copy, protected_len, ecc_ptr);
    FTL_DEBUG("[ECC] Verify result: %d (0=ok, >0=errors, <1=uncorrectable)\n", verify_result);
    
    if (verify_result == 0) {
        return 0; // 无错误
    }
    
    // 尝试纠错
    int result = bch_decode(bch_cfg, data_copy, protected_len, ecc_ptr);
    FTL_DEBUG("[ECC] Decode result: %d\n", result);
    
    if (result < 0) {
        FTL_DEBUG("[ECC] ERROR: Uncorrectable errors detected!\n");
        return -1; // 不可纠正的错误
    }
    
    if (result > 0) {
        // 已纠正错误，复制回原缓冲区
        memcpy(page_buf, data_copy, EFLASH_PAGE_SIZE);
        FTL_DEBUG("[ECC] Corrected %d errors\n", result);
    }
    
    return 0;
}

// --- 底层 Flash 操作封装 ---

/**
 * is_page_valid: 判断物理页是否包含有效元数据
 */
static bool is_page_valid(uint16_t page, ftl_meta_t *meta) {
    uint8_t buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(page, buf) != 0) return false;

    // 使用新的完整页ECC验证
    if (verify_and_correct_page(buf) != 0) return false;

    // 从纠正后的缓冲区复制元数据
    memcpy(meta, buf + META_OFFSET, META_SIZE);

    // 1. 从未写过的页 (BLANK)
    if (meta->status == TXN_STATUS_BLANK) return false;

    // 2. 状态检查：READY, COMMITTED 或 非事务有效页 (INVALID/0x00)
    return (meta->status == TXN_STATUS_COMMITTED ||
            meta->status == TXN_STATUS_READY ||
            meta->status == TXN_STATUS_INVALID);
}

static int write_full_page(uint16_t page, const uint8_t *data, const ftl_meta_t *meta_in) {
    uint8_t buf[EFLASH_PAGE_SIZE];
    memset(buf, 0xFF, EFLASH_PAGE_SIZE);

    // 复制用户数据
    if (data) memcpy(buf, data, USER_DATA_SIZE);

    // 复制元数据（不含ECC字段）
    ftl_meta_t *meta_out = (ftl_meta_t *)(buf + META_OFFSET);
    memcpy(meta_out, meta_in, META_SIZE - 5);
    
    // 计算完整页的ECC（覆盖用户数据+元数据除ECC部分）
    calc_page_ecc(buf);

    if (eflash_hw_erase(page) != 0) return -1;
    return eflash_hw_prog(page, buf);
}

static int find_free_page(mini_ftl_t *ftl) {
    // 使用 space_mgr 快速查找空闲页
    uint16_t page, offset;
    if (space_mgr_alloc(&ftl->spc_mgr, EFLASH_PAGE_SIZE, &page, &offset) == 0) {
        return page;
    }
    return -1;
}

// --- Radix Tree 核心逻辑 (移植自 Dhara map.c) ---

static inline int get_bit(uint16_t sector, int depth) {
    return (sector >> (RADIX_DEPTH - 1 - depth)) & 1;
}

// 修正后的trace_path：严格遵循Dhara设计，移除new_phys参数
// 职责：仅构建新节点的元数据模板，不插入任何物理页
static int trace_path(mini_ftl_t *ftl, uint16_t base_root, uint16_t sector, ftl_meta_t *out_meta) {
    uint8_t meta_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t cur_meta;
    int depth = 0;
    uint16_t current = base_root;

    FTL_DEBUG("[TRACE] sector=%d, base_root=%d\n", sector, base_root);

    // 初始化新节点的元数据（alt数组初始化为全0xFF/PAGE_NONE）
    memset(out_meta, 0xFF, sizeof(ftl_meta_t));
    out_meta->sector_id = sector;
    out_meta->global_count = ftl->next_count++;
    out_meta->epoch = ftl->current_epoch;
    out_meta->txn_id = ftl->active_txn_id;
    out_meta->status = (ftl->active_txn_id != TXN_ID_NONE) ? TXN_STATUS_READY : TXN_STATUS_INVALID;

    // 如果树为空，直接返回（alt数组已初始化为全PAGE_NONE）
    if (current == PAGE_NONE) {
        FTL_DEBUG("[TRACE] Empty tree\n");
        return 0;
    }

    // 读取根节点元数据
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

    // 遍历基数树，构建路径
    while (depth < RADIX_DEPTH) {
        // 计算当前深度的位值（MSB first）
        uint16_t bit_mask = 1 << (RADIX_DEPTH - 1 - depth);
        int target_bit = (sector & bit_mask) ? 1 : 0;
        int current_bit = (cur_meta.sector_id & bit_mask) ? 1 : 0;

        FTL_DEBUG("[TRACE] depth=%d, target_bit=%d, current_bit=%d, alt[%d]=%d\n",
                  depth, target_bit, current_bit, depth, cur_meta.alt[depth]);

        if (target_bit != current_bit) {
            // 位不同：发生分叉
            // 将当前物理页保存为新节点的alt指针（记录分叉点）
            out_meta->alt[depth] = current;
            FTL_DEBUG("[TRACE] Diverge: out_meta->alt[%d]=%d\n", depth, current);

            // 获取当前节点的alt指针，继续向下搜索
            uint16_t next_page = cur_meta.alt[depth];
            if (next_page == PAGE_NONE) {
                // 路径中断，跳转到not_found处理剩余深度
                FTL_DEBUG("[TRACE] Path interrupted at depth=%d\n", depth);
                depth++;
                goto not_found;
            }

            FTL_DEBUG("[TRACE] Follow alt[%d]=%d\n", depth, next_page);

            // 读取下一个节点的元数据
            if (eflash_hw_read(next_page, meta_buf) != 0) {
                FTL_DEBUG("[TRACE] ERROR: Failed to read next page %d at depth %d\n", next_page, depth);
                return -1;
            }
            if (verify_and_correct_page(meta_buf) != 0) {
                FTL_DEBUG("[TRACE] ERROR: Page verification failed for page %d at depth %d\n", next_page, depth);
                return -1;
            }
            memcpy(&cur_meta, meta_buf + META_OFFSET, META_SIZE);

            current = next_page;
        } else {
            // 位相同：从当前节点继承alt指针
            out_meta->alt[depth] = cur_meta.alt[depth];
            FTL_DEBUG("[TRACE] Same bit, inherit alt[%d]=%d\n", depth, out_meta->alt[depth]);
        }

        depth++;
    }

    // 循环正常结束，说明找到了匹配的sector（或者遍历完所有深度）
    // new_meta的alt数组已经在遍历过程中通过Diverge和Same bit分支完整设置
    FTL_DEBUG("[TRACE] Found match after full traversal\n");
    return 0;

not_found:
    // Dhara的逻辑：将从当前depth开始的所有alt指针设置为NONE
    // 注意：在goto not_found之前已经执行了depth++，所以这里从当前depth开始设置
    FTL_DEBUG("[TRACE] Not found, setting remaining alt pointers to NONE from depth=%d\n", depth);
    while (depth < RADIX_DEPTH) {
        out_meta->alt[depth] = PAGE_NONE;
        depth++;
    }

    return 0;
}

// --- 对象头管理核心逻辑 ---

/**
 * get_header_page_info: 根据对象 ID 计算其所在的逻辑页和页内偏移
 */
static int get_header_page_info(mini_ftl_t *ftl, uint16_t obj_id, uint16_t *out_log_page, uint16_t *out_offset) {
    if (obj_id < BASE_HEADER_CAPACITY) {
        // 基础区
        *out_log_page = ftl->base_hdr_addr + (obj_id / 29);
        *out_offset = (obj_id % 29) * sizeof(obj_header_t);
        return 0;
    }

    // 扩展区
    uint16_t ext_idx = obj_id - BASE_HEADER_CAPACITY;
    uint16_t level = (ext_idx / EXT_HEADER_CAPACITY) + 1;

    if (level > 16 || ftl->ext_hdr_addrs[level - 1] == PAGE_NONE) {
        return -1; // 尚未扩展或超出范围
    }

    uint16_t page_in_unit = (ext_idx % EXT_HEADER_CAPACITY) / 29;
    uint16_t idx_in_page = (ext_idx % EXT_HEADER_CAPACITY) % 29;

    *out_log_page = ftl->ext_hdr_addrs[level - 1] + page_in_unit;
    *out_offset = idx_in_page * sizeof(obj_header_t);
    return 0;
}

/**
 * extend_headers: 动态扩展一级对象头空间（4页）
 */
static int extend_headers(mini_ftl_t *ftl) {
    // 1. 找到当前最高级的扩展页，获取其最后一个对象头（指针位）
    uint16_t prev_ext_addr = ftl->base_hdr_addr + BASE_HEADER_PAGES - 1; // 默认指向基础区最后一页
    int level = 0;
    while (level < 16 && ftl->ext_hdr_addrs[level] != PAGE_NONE) {
        prev_ext_addr = ftl->ext_hdr_addrs[level] + EXT_HEADER_PAGES_UNIT - 1;
        level++;
    }

    if (level >= 16) return -1; // 达到最大扩展级数

    // 2. 分配新的 4 页逻辑空间
    uint16_t new_ext_page, new_ext_offset;
    if (space_mgr_alloc(&ftl->spc_mgr, 4 * EFLASH_PAGE_SIZE, &new_ext_page, &new_ext_offset) != 0) {
        return -1;
    }
    uint16_t new_ext_addr = new_ext_page;

    // 3. 更新上一级末尾的指针
    obj_header_t link_hdr;
    memset(&link_hdr, 0, sizeof(link_hdr));
    link_hdr.type = 0xFF; // 标记为链接对象
    link_hdr.body_addr = new_ext_addr;

    // 写入链接信息到上一级末尾
    mini_ftl_obj_set_header(ftl, (level == 0 ? BASE_HEADER_CAPACITY - 1 : (level * EXT_HEADER_CAPACITY + BASE_HEADER_CAPACITY - 1)), &link_hdr);

    // 4. 记录新扩展地址
    ftl->ext_hdr_addrs[level] = new_ext_addr;

    // 5. 初始化新页（写全 0xFF 或空对象头）
    uint8_t empty_page[EFLASH_PAGE_SIZE];
    memset(empty_page, 0xFF, EFLASH_PAGE_SIZE);
    for (int i = 0; i < EXT_HEADER_PAGES_UNIT; i++) {
        mini_ftl_write(ftl, new_ext_addr + i, empty_page);
    }

    return 0;
}

int mini_ftl_obj_get_header(mini_ftl_t *ftl, uint16_t obj_id, obj_header_t *hdr) {
    uint16_t log_page, offset;
    if (get_header_page_info(ftl, obj_id, &log_page, &offset) != 0) {
        return -1;
    }

    uint8_t page_data[USER_DATA_SIZE];
    if (mini_ftl_read(ftl, log_page, page_data) != 0) return -1;

    memcpy(hdr, page_data + offset, sizeof(obj_header_t));
    return 0;
}

int mini_ftl_obj_set_header(mini_ftl_t *ftl, uint16_t obj_id, const obj_header_t *hdr) {
    uint16_t log_page, offset;
    if (get_header_page_info(ftl, obj_id, &log_page, &offset) != 0) {
        // 如果是因为没扩展导致的失败，尝试自动扩展
        if (obj_id >= BASE_HEADER_CAPACITY) {
            if (extend_headers(ftl) != 0) return -1;
            if (get_header_page_info(ftl, obj_id, &log_page, &offset) != 0) return -1;
        } else {
            return -1;
        }
    }

    uint8_t page_data[USER_DATA_SIZE];
    // 先读后写，防止覆盖同页其他对象头
    if (mini_ftl_read(ftl, log_page, page_data) != 0) {
        memset(page_data, 0xFF, USER_DATA_SIZE); // 如果是新页则初始化
    }

    memcpy(page_data + offset, hdr, sizeof(obj_header_t));
    return mini_ftl_write(ftl, log_page, page_data);
}

// --- FTL 初始化与预分配 ---

int mini_ftl_init(mini_ftl_t *ftl) {
    FTL_DEBUG("[INIT] Starting mini_ftl_init\n");

    space_mgr_init(&ftl->spc_mgr, EFLASH_TOTAL_PAGES);

    ftl_meta_t meta;
    uint32_t max_count = 0;
    uint16_t max_epoch = 0;

    ftl->root_page = PAGE_NONE;
    ftl->shadow_root = PAGE_NONE;
    ftl->next_count = 1;
    ftl->current_epoch = 0;
    ftl->active_txn_id = TXN_ID_NONE;
    ftl->is_initialized = true;

    // 扫描全片寻找最新的 COMMITTED 页作为 Root
    FTL_DEBUG("[INIT] Scanning %d pages for valid root...\n", EFLASH_TOTAL_PAGES);
    for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
        if (is_page_valid(i, &meta) && meta.status == TXN_STATUS_COMMITTED) {
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

        // 每扫描1000页输出一次进度
        if (i % 1000 == 0 && i > 0) {
            FTL_DEBUG("[INIT] Scanned %d/%d pages...\n", i, EFLASH_TOTAL_PAGES);
        }
    }

    FTL_DEBUG("[INIT] Scan complete. Root page: %d, next_count: %d\n", ftl->root_page, ftl->next_count);
    return 0;
}

// --- 对象管理实现 (暂时禁用，待后续完善) ---

int mini_ftl_obj_create(mini_ftl_t *ftl, uint16_t pkg_id, uint16_t class_id, uint8_t type) {
    // TODO: 重新实现对象管理逻辑
    (void)ftl; (void)pkg_id; (void)class_id; (void)type;
    return -1;
}

int mini_ftl_obj_write_header(mini_ftl_t *ftl, uint16_t obj_id, const obj_header_t *hdr) {
    // TODO: 重新实现对象头写入逻辑
    (void)ftl; (void)obj_id; (void)hdr;
    return -1;
}

int mini_ftl_obj_read_header(mini_ftl_t *ftl, uint16_t obj_id, obj_header_t *hdr) {
    // TODO: 重新实现对象头读取逻辑
    (void)ftl; (void)obj_id; (void)hdr;
    return -1;
}

int mini_ftl_obj_write_body(mini_ftl_t *ftl, uint16_t obj_id, const uint8_t *data, uint32_t size) {
    // TODO: 重新实现对象数据写入逻辑
    (void)ftl; (void)obj_id; (void)data; (void)size;
    return -1;
}

int mini_ftl_write(mini_ftl_t *ftl, uint16_t sector_id, const uint8_t *data) {
    if (!ftl->is_initialized) return -1;

    // sector_id 实际上是24位逻辑地址的低16位
    // 对于完整24位地址支持，需要扩展接口
    // 这里假设sector_id就是逻辑页号（已经右移了LOG2_PAGE_SIZE位）
    uint16_t logical_page = sector_id;

    FTL_DEBUG("[WRITE] logical_page=%d\n", logical_page);

    uint16_t base_root = (ftl->active_txn_id != TXN_ID_NONE) ? ftl->shadow_root : ftl->root_page;

    // 步骤1：调用trace_path构建新节点的元数据模板（不包含数据页信息）
    ftl_meta_t new_node_meta;
    if (trace_path(ftl, base_root, logical_page, &new_node_meta) != 0) {
        FTL_DEBUG("[WRITE] ERROR: trace_path failed!\n");
        return -1;
    }

    // 步骤2：从 space_mgr 分配一个物理页（同时存储数据和元数据）
    int new_phys = find_free_page(ftl);
    if (new_phys < 0) {
        FTL_DEBUG("[WRITE] ERROR: Failed to allocate physical page!\n");
        return -1;
    }

    FTL_DEBUG("[WRITE] Allocated physical page=%d\n", new_phys);

    // 步骤3：将用户数据 + 元数据写入同一个物理页
    // 注意：Dhara设计中，每个物理页既是数据页也是元数据节点页
    if (write_full_page(new_phys, data, &new_node_meta) != 0) {
        FTL_DEBUG("[WRITE] ERROR: Failed to write page!\n");
        return -1;
    }

    // 步骤4：更新FTL的根指针
    if (ftl->active_txn_id != TXN_ID_NONE) {
        // 事务模式下，更新影子根
        ftl->shadow_root = new_phys;
    } else {
        // 非事务模式，直接更新根
        ftl->root_page = new_phys;
    }

    FTL_DEBUG("[WRITE] Success: root_page=%d, next_count=%d\n",
              (ftl->active_txn_id != TXN_ID_NONE) ? ftl->shadow_root : ftl->root_page,
              ftl->next_count);

    return 0;
}

int mini_ftl_read(mini_ftl_t *ftl, uint16_t sector_id, uint8_t *data) {
    if (!ftl->is_initialized || ftl->root_page == PAGE_NONE) return -1;

    uint16_t logical_page = sector_id;

    FTL_DEBUG("[READ] logical_page=%d, root_page=%d\n", logical_page, ftl->root_page);

    uint8_t meta_buf[EFLASH_PAGE_SIZE];
    ftl_meta_t cur_meta;
    int depth = 0;
    uint16_t current = ftl->root_page;

    while (depth < RADIX_DEPTH) {
        if (eflash_hw_read(current, meta_buf) != 0) {
            FTL_DEBUG("[READ] ERROR: Failed to read page %d at depth %d\n", current, depth);
            return -1;
        }

        if (verify_and_correct_page(meta_buf) != 0) {
            FTL_DEBUG("[READ] ERROR: Page verification failed at page %d\n", current);
            return -1;
        }
        memcpy(&cur_meta, meta_buf + META_OFFSET, META_SIZE);

        FTL_DEBUG("[READ] depth=%d, cur_sector=%d, alt[depth]=%d\n", depth, cur_meta.sector_id, cur_meta.alt[depth]);

        if (cur_meta.sector_id == logical_page) {
            // 找到匹配的节点，数据就在当前页的前USER_DATA_SIZE字节
            FTL_DEBUG("[READ] Found match at depth=%d, reading data from page %d\n", depth, current);
            // 数据已经在 meta_buf 中，且已通过 verify_and_correct_page 验证/纠正
            memcpy(data, meta_buf, USER_DATA_SIZE);
            FTL_DEBUG("[READ] Success, first byte=0x%02X\n", data[0]);
            return 0;
        }

        uint16_t bit_mask = 1 << (RADIX_DEPTH - 1 - depth);
        int target_bit = (logical_page & bit_mask) ? 1 : 0;
        int current_bit = (cur_meta.sector_id & bit_mask) ? 1 : 0;

        if (target_bit != current_bit) {
            // 位不同，需要跳转
            current = cur_meta.alt[depth];
            if (current == PAGE_NONE) {
                FTL_DEBUG("[READ] ERROR: Path not found at depth=%d\n", depth);
                return -1;
            }
            // 跳转后继续在同一深度比较新节点
            // 注意：不增加depth，在下一次循环中继续比较同一深度的bit
        } else {
            // 位相同，继续比较下一个bit
            depth++;
        }
    }

    // 循环结束仍未找到，尝试读取最后跳转到的节点（如果有的话）
    // 这种情况发生在最后一次迭代中进行了跳转
    if (current != PAGE_NONE && current != ftl->root_page) {
        if (eflash_hw_read(current, meta_buf) != 0) {
            FTL_DEBUG("[READ] ERROR: Failed to read final page %d\n", current);
            return -1;
        }

        if (verify_and_correct_page(meta_buf) != 0) {
            FTL_DEBUG("[READ] ERROR: Page verification failed at final page %d\n", current);
            return -1;
        }
        memcpy(&cur_meta, meta_buf + META_OFFSET, META_SIZE);

        if (cur_meta.sector_id == logical_page) {
            FTL_DEBUG("[READ] Found match at final page %d, reading data\n", current);
            memcpy(data, meta_buf, USER_DATA_SIZE);
            FTL_DEBUG("[READ] Success, first byte=0x%02X\n", data[0]);
            return 0;
        }
    }

    // 确实没有找到
    FTL_DEBUG("[READ] ERROR: Sector not found in tree (exceeded max depth)\n");
    return -1;
}

void mini_ftl_txn_begin(mini_ftl_t *ftl) {
    ftl->active_txn_id = (uint16_t)(ftl->next_count & 0xFFFF);
    ftl->shadow_root = ftl->root_page; // 影子树初始指向当前的稳定根
}

int mini_ftl_txn_commit(mini_ftl_t *ftl) {
    if (ftl->active_txn_id == TXN_ID_NONE || ftl->shadow_root == PAGE_NONE) return -1;

    // 原子提交：需要读取整页，修改status，重新计算ECC，然后写回
    uint8_t full_page[EFLASH_PAGE_SIZE];

    // 1. 读取当前页
    if (eflash_hw_read(ftl->shadow_root, full_page) != 0) {
        return -1;
    }

    // 2. 修改status为COMMITTED
    ftl_meta_t *meta = (ftl_meta_t *)(full_page + META_OFFSET);
    meta->status = TXN_STATUS_COMMITTED;

    // 3. 重新计算整页ECC (因为元数据变了，且ECC覆盖用户数据+元数据)
    calc_page_ecc(full_page);

    // 4. 擦除并重写整页
    if (eflash_hw_erase(ftl->shadow_root) != 0) {
        return -1;
    }
    if (eflash_hw_prog(ftl->shadow_root, full_page) != 0) {
        return -1;
    }

    // 提交成功，影子树转正
    ftl->root_page = ftl->shadow_root;
    ftl->shadow_root = PAGE_NONE;
    ftl->active_txn_id = TXN_ID_NONE;
    return 0;
}

void mini_ftl_txn_abort(mini_ftl_t *ftl) {
    if (ftl->active_txn_id == TXN_ID_NONE) return;

    // 简单丢弃影子树指针，Flash 中的 PENDING 页将在下次 GC 或重启时被忽略
    ftl->shadow_root = PAGE_NONE;
    ftl->active_txn_id = TXN_ID_NONE;
}