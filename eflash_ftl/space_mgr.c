#include "space_mgr.h"
#include "eflash_sim.h"
#include "mini_ftl.h"
#include <string.h>
#include <stdio.h>

#ifndef FTL_DEBUG
#define FTL_DEBUG(...) do {} while(0)  // 关闭调试输出
#endif

// --- 内部辅助函数 ---

// 读取free_node页的count
static uint16_t read_node_count(uint16_t phys_page) {
    uint8_t buf[EFLASH_PAGE_SIZE];
    eflash_hw_read(phys_page, buf);
    // count存储在meta区域之后的用户数据区开头
    uint16_t offset = META_SIZE;  // 跳过48字节meta
    return (uint16_t)(buf[offset] | (buf[offset + 1] << 8));
}

// 写入free_node页的count
static void write_node_count(uint16_t phys_page, uint16_t count) {
    uint8_t buf[EFLASH_PAGE_SIZE];
    
    // 初始化为全0xFF（擦除状态）
    memset(buf, 0xFF, EFLASH_PAGE_SIZE);
    
    // count存储在meta区域之后的用户数据区开头
    uint16_t offset = META_SIZE;
    buf[offset] = count & 0xFF;
    buf[offset + 1] = (count >> 8) & 0xFF;
    
    // 计算整页的ECC（覆盖用户数据+元数据除ECC部分）
    // ECC保护范围：USER_DATA_SIZE + META_SIZE - 5
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
    uint8_t *ecc_ptr = buf + USER_DATA_SIZE + META_SIZE - 5;
    bch_generate(&bch_3bit, buf, protected_len, ecc_ptr);
    
    // Flash写入前必须先擦除
    eflash_hw_erase(phys_page);
    eflash_hw_prog(phys_page, buf);
}

// 读取指定索引的free_node
static free_node_t read_free_node(uint16_t phys_page, uint16_t index) {
    free_node_t node = {0xFFFFFFFF, 0xFFFF};  // 默认返回无效节点
    
    if (index >= FREE_NODES_PER_PAGE) {
        return node;  // 越界保护
    }
    
    uint8_t buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(phys_page, buf) != 0) {
        return node;  // 读取失败
    }
    
    // 节点数据从meta + 4字节count之后开始
    uint16_t offset = META_SIZE + FREE_NODE_HEADER_SIZE + index * sizeof(free_node_t);
    
    // 边界检查
    if (offset + sizeof(free_node_t) > EFLASH_PAGE_SIZE) {
        return node;  // 超出页范围
    }
    
    memcpy(&node, buf + offset, sizeof(free_node_t));
    return node;
}

// 写入指定索引的free_node
static void write_free_node(uint16_t phys_page, uint16_t index, const free_node_t *node) {
    if (index >= FREE_NODES_PER_PAGE) {
        return;  // 越界保护
    }
    
    uint8_t buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(phys_page, buf) != 0) {
        return;  // 读取失败
    }
    
    // 节点数据从meta + 4字节count之后开始
    uint16_t offset = META_SIZE + FREE_NODE_HEADER_SIZE + index * sizeof(free_node_t);
    
    // 边界检查
    if (offset + sizeof(free_node_t) > EFLASH_PAGE_SIZE) {
        return;  // 超出页范围
    }
    
    memcpy(buf + offset, node, sizeof(free_node_t));
    
    // Flash写入前必须先擦除
    eflash_hw_erase(phys_page);
    eflash_hw_prog(phys_page, buf);  // 写回整页
}

// 在free_node表中查找并删除指定逻辑地址的节点，返回其大小
static uint32_t remove_node_from_table(space_mgr_t *mgr, uint32_t target_logical_addr) {
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t count = read_node_count(mgr->free_node_pages[i]);
        
        FTL_DEBUG("[REMOVE_NODE] Searching logical_addr=0x%08X in free_node[%d], count=%d\n", 
                 target_logical_addr, i, count);
        
        for (uint16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            
            FTL_DEBUG("[REMOVE_NODE] Checking node[%d][%d]: addr=0x%08X, size=%u\n",
                     i, j, node.addr, node.size);
            
            if (node.addr == target_logical_addr) {
                FTL_DEBUG("[REMOVE_NODE] Found match at [%d][%d], removing...\n", i, j);
                
                // 读取整页
                uint8_t buf[EFLASH_PAGE_SIZE];
                eflash_hw_read(mgr->free_node_pages[i], buf);
                
                uint16_t last_idx = count - 1;
                if (j != last_idx) {
                    // 用最后一个节点覆盖当前节点
                    uint16_t src_offset = META_SIZE + FREE_NODE_HEADER_SIZE + last_idx * sizeof(free_node_t);
                    uint16_t dst_offset = META_SIZE + FREE_NODE_HEADER_SIZE + j * sizeof(free_node_t);
                    memcpy(buf + dst_offset, buf + src_offset, sizeof(free_node_t));
                    
                    FTL_DEBUG("[REMOVE_NODE] Moved last node to position %d\n", j);
                }
                
                // 清除最后一个节点（填充0xFF）
                uint16_t clear_offset = META_SIZE + FREE_NODE_HEADER_SIZE + last_idx * sizeof(free_node_t);
                memset(buf + clear_offset, 0xFF, sizeof(free_node_t));
                
                // 更新count
                uint16_t count_offset = META_SIZE;
                buf[count_offset] = (count - 1) & 0xFF;
                buf[count_offset + 1] = ((count - 1) >> 8) & 0xFF;
                
                // 计算ECC
                size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
                uint8_t *ecc_ptr = buf + USER_DATA_SIZE + META_SIZE - 5;
                bch_generate(&bch_3bit, buf, protected_len, ecc_ptr);
                
                // 一次性擦除并写入
                eflash_hw_erase(mgr->free_node_pages[i]);
                eflash_hw_prog(mgr->free_node_pages[i], buf);
                
                FTL_DEBUG("[REMOVE_NODE] Updated count: %d -> %d\n", count, count - 1);
                
                return node.size;
            }
        }
    }
    
    FTL_DEBUG("[REMOVE_NODE] ERROR: Logical address 0x%08X not found in any free_node table!\n", target_logical_addr);
    return 0;  // 未找到
}

// 插入新的free_node到表中（保持按size排序）
static void insert_node_to_table(space_mgr_t *mgr, uint32_t logical_addr, uint32_t size) {
    // 寻找合适的插入位置（第一个size大于等于新节点的页）
    int best_page_idx = -1;
    uint32_t best_size = 0xFFFFFFFF;
    
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t count = read_node_count(mgr->free_node_pages[i]);
        
        // 如果页已满，跳过
        if (count >= FREE_NODES_PER_PAGE) continue;
        
        // 找到该页中第一个size >= 新size的位置
        for (uint16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            if (node.size >= size && node.size < best_size) {
                best_size = node.size;
                best_page_idx = i;
                break;
            }
        }
        
        // 如果没找到更大的，但页有空位，也可以插入末尾
        if (best_page_idx == -1 && count < FREE_NODES_PER_PAGE) {
            best_page_idx = i;
        }
    }
    
    if (best_page_idx == -1) {
        FTL_DEBUG("[INSERT_NODE] ERROR: No space in free_node tables\n");
        return;  // 表已满
    }
    
    uint16_t count = read_node_count(mgr->free_node_pages[best_page_idx]);
    
    // 找到插入位置（保持按size升序）
    uint16_t insert_pos = count;
    for (uint16_t j = 0; j < count; j++) {
        free_node_t node = read_free_node(mgr->free_node_pages[best_page_idx], j);
        if (node.size >= size) {
            insert_pos = j;
            break;
        }
    }
    
    // 读取整页
    uint8_t buf[EFLASH_PAGE_SIZE];
    eflash_hw_read(mgr->free_node_pages[best_page_idx], buf);
    
    // 向后移动元素
    for (uint16_t j = count; j > insert_pos; j--) {
        uint16_t src_offset = META_SIZE + FREE_NODE_HEADER_SIZE + (j - 1) * sizeof(free_node_t);
        uint16_t dst_offset = META_SIZE + FREE_NODE_HEADER_SIZE + j * sizeof(free_node_t);
        memcpy(buf + dst_offset, buf + src_offset, sizeof(free_node_t));
    }
    
    // 插入新节点
    uint16_t node_offset = META_SIZE + FREE_NODE_HEADER_SIZE + insert_pos * sizeof(free_node_t);
    free_node_t new_node;
    new_node.addr = logical_addr;
    new_node.size = size;
    memcpy(buf + node_offset, &new_node, sizeof(free_node_t));
    
    // 更新count
    uint16_t count_offset = META_SIZE;
    buf[count_offset] = (count + 1) & 0xFF;
    buf[count_offset + 1] = ((count + 1) >> 8) & 0xFF;
    
    // 计算ECC
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
    uint8_t *ecc_ptr = buf + USER_DATA_SIZE + META_SIZE - 5;
    bch_generate(&bch_3bit, buf, protected_len, ecc_ptr);
    
    // 一次性擦除并写入
    eflash_hw_erase(mgr->free_node_pages[best_page_idx]);
    eflash_hw_prog(mgr->free_node_pages[best_page_idx], buf);
    
    FTL_DEBUG("[INSERT_NODE] Inserted logical_addr=0x%08X, size=%u at [%d][%d]\n",
             logical_addr, size, best_page_idx, insert_pos);
}

// --- 接口实现 ---

void space_mgr_init(space_mgr_t *mgr, uint16_t total_pages) {
    mgr->total_pages = total_pages;
    
    FTL_DEBUG("[SPACE_INIT] === Starting initialization ===\n");
    
    // 分配free_node表物理页（使用前4页）
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        mgr->free_node_pages[i] = i;
        // 擦除页面并初始化为全0
        eflash_hw_erase(i);
        uint8_t blank_buf[EFLASH_PAGE_SIZE];
        memset(blank_buf, 0x00, EFLASH_PAGE_SIZE);
        eflash_hw_prog(i, blank_buf);
        FTL_DEBUG("[SPACE_INIT] Initialized free_node page %d to all zeros\n", i);
    }
    
    // 分配对象头表物理页（接下来的8页）
    for (int i = 0; i < BASE_HEADER_PAGES; i++) {
        mgr->header_pages[i] = FREE_NODE_PAGE_COUNT + i;
        // 擦除对象头页并初始化为全0
        eflash_hw_erase(mgr->header_pages[i]);
        uint8_t blank_buf[EFLASH_PAGE_SIZE];
        memset(blank_buf, 0x00, EFLASH_PAGE_SIZE);
        eflash_hw_prog(mgr->header_pages[i], blank_buf);
    }
    
    // 计算系统保留的物理页数
    uint32_t reserved_pages = FREE_NODE_PAGE_COUNT + BASE_HEADER_PAGES;  // 12
    
    // 计算可用逻辑地址空间
    // 注意：系统预留的12页不参与逻辑地址分配
    uint32_t available_pages = total_pages - reserved_pages;  // 8192 - 12 = 8180
    uint32_t total_logical_size = available_pages * USER_DATA_SIZE;  // 8180 * 464 = 3,795,520
    uint32_t start_logical_addr = reserved_pages * USER_DATA_SIZE;   // 12 * 464 = 5,568
    
    FTL_DEBUG("[SPACE_INIT] Total pages: %d, Reserved pages: %d, Available pages: %d\n",
             total_pages, reserved_pages, available_pages);
    FTL_DEBUG("[SPACE_INIT] Logical address space: start=0x%08X, size=%u bytes\n",
             start_logical_addr, total_logical_size);
    
    // 在第一个free_node页初始化唯一的空闲节点
    // count = 1, addr = start_logical_addr, size = total_logical_size
    uint8_t buf[EFLASH_PAGE_SIZE];
    memset(buf, 0x00, EFLASH_PAGE_SIZE);
    
    // 设置count = 1
    uint16_t count_offset = META_SIZE;
    buf[count_offset] = 1 & 0xFF;
    buf[count_offset + 1] = (1 >> 8) & 0xFF;
    
    // 设置第一个节点
    uint16_t node_offset = META_SIZE + FREE_NODE_HEADER_SIZE;
    free_node_t initial_node;
    initial_node.addr = start_logical_addr;
    initial_node.size = total_logical_size;
    memcpy(buf + node_offset, &initial_node, sizeof(free_node_t));
    
    FTL_DEBUG("[SPACE_INIT] Initial free node: addr=0x%08X, size=%u\n",
             initial_node.addr, initial_node.size);
    
    // 计算ECC
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
    uint8_t *ecc_ptr = buf + USER_DATA_SIZE + META_SIZE - 5;
    bch_generate(&bch_3bit, buf, protected_len, ecc_ptr);
    
    // 写入第一个free_node页
    eflash_hw_erase(mgr->free_node_pages[0]);
    eflash_hw_prog(mgr->free_node_pages[0], buf);
    
    // 验证写入
    uint8_t verify_buf[EFLASH_PAGE_SIZE];
    eflash_hw_read(mgr->free_node_pages[0], verify_buf);
    uint16_t verify_count = (uint16_t)(verify_buf[META_SIZE] | (verify_buf[META_SIZE + 1] << 8));
    free_node_t verify_node;
    memcpy(&verify_node, verify_buf + META_SIZE + FREE_NODE_HEADER_SIZE, sizeof(free_node_t));
    FTL_DEBUG("[SPACE_INIT] VERIFY: count=%d, addr=0x%08X, size=%u\n",
             verify_count, verify_node.addr, verify_node.size);
    
    FTL_DEBUG("[SPACE_INIT] === Initialization complete ===\n");
    mgr->next_alloc_page = reserved_pages;
}

int space_mgr_alloc(space_mgr_t *mgr, uint32_t size, uint32_t *out_logical_addr) {
    // 遍历所有free_node页，寻找第一个满足大小的节点
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t count = read_node_count(mgr->free_node_pages[i]);
        
        for (uint16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            
            if (node.size >= size) {
                // 找到合适的节点
                uint32_t alloc_addr = node.addr;
                uint32_t remaining = node.size - size;
                
                FTL_DEBUG("[SPACE_ALLOC] Allocating logical_addr=0x%08X, size=%u from free_node[%d][%d]\n", 
                         alloc_addr, size, i, j);
                
                // 从表中移除原节点
                remove_node_from_table(mgr, alloc_addr);
                
                FTL_DEBUG("[SPACE_ALLOC] After removal, remaining=%u, will insert at addr=0x%08X\n", 
                         remaining, alloc_addr + size);
                
                // 如果有剩余空间，插入剩余节点
                if (remaining > 0) {
                    insert_node_to_table(mgr, alloc_addr + size, remaining);
                    
                    // 验证插入
                    uint16_t new_count = read_node_count(mgr->free_node_pages[0]);
                    free_node_t verify_after = read_free_node(mgr->free_node_pages[0], 0);
                    FTL_DEBUG("[SPACE_ALLOC] After insert: count=%d, node[0][0] addr=0x%08X, size=%u\n",
                             new_count, verify_after.addr, verify_after.size);
                } else {
                    FTL_DEBUG("[SPACE_ALLOC] No remaining space to insert\n");
                }
                
                *out_logical_addr = alloc_addr;
                
                // 验证：读取刚被移除的节点位置，确认已被清除
                free_node_t verify_node = read_free_node(mgr->free_node_pages[i], j);
                FTL_DEBUG("[SPACE_ALLOC] After removal: node[%d][%d] addr=0x%08X, size=%u\n",
                         i, j, verify_node.addr, verify_node.size);
                
                return 0;
            }
        }
    }
    
    FTL_DEBUG("[SPACE_ALLOC] ERROR: No suitable free node found for size=%u\n", size);
    return -1;  // 空间不足
}

void space_mgr_free(space_mgr_t *mgr, uint32_t logical_addr, uint32_t size) {
    FTL_DEBUG("[SPACE_FREE] Freeing logical_addr=0x%06X, size=%u\n", logical_addr, size);
    
    // 检查是否可以与前一个空闲块合并
    uint32_t prev_size = remove_node_from_table(mgr, logical_addr - 1);
    if (prev_size > 0) {
        logical_addr = logical_addr - 1;
        size += prev_size;
        FTL_DEBUG("[SPACE_FREE] Merged with previous block: new_addr=0x%06X, new_size=%u\n",
                 logical_addr, size);
    }
    
    // 检查是否可以与后一个空闲块合并
    uint32_t next_size = remove_node_from_table(mgr, logical_addr + size);
    if (next_size > 0) {
        size += next_size;
        FTL_DEBUG("[SPACE_FREE] Merged with next block: new_size=%u\n", size);
    }
    
    // 插入合并后的节点
    insert_node_to_table(mgr, logical_addr, size);
}

void space_mgr_sync(space_mgr_t *mgr) {
    // free_node表已经在每次操作时同步到Flash，此函数保留用于批量同步优化
    (void)mgr;
}

uint32_t space_mgr_get_free_bytes(space_mgr_t *mgr) {
    uint32_t total_free_pages = 0;
    
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t count = read_node_count(mgr->free_node_pages[i]);
        
        for (uint16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            total_free_pages += node.size;
        }
    }
    
    return total_free_pages * EFLASH_PAGE_SIZE;
}
