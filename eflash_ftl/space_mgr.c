#include "space_mgr.h"
#include "eflash_sim.h"
#include "mini_ftl.h"
#include <string.h>
#include <stdio.h>

#ifndef FTL_DEBUG
#define FTL_DEBUG(...) do {} while(0)  // 关闭调试输出

#endif

// --- 内部辅助函数 ---

// 从3字节数组读取逻辑地址
static uint32_t read_addr_24(const uint8_t addr[3]) {
    return ((uint32_t)addr[0] << 16) | ((uint32_t)addr[1] << 8) | addr[2];
}

// 将逻辑地址写入3字节数组
static void write_addr_24(uint8_t addr[3], uint32_t value) {
    addr[0] = (value >> 16) & 0xFF;
    addr[1] = (value >> 8) & 0xFF;
    addr[2] = value & 0xFF;
}

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
    eflash_hw_read(phys_page, buf);  // 先读取
    
    // count存储在meta区域之后的用户数据区开头
    uint16_t offset = META_SIZE;
    buf[offset] = count & 0xFF;
    buf[offset + 1] = (count >> 8) & 0xFF;
    
    // Flash写入前必须先擦除
    eflash_hw_erase(phys_page);
    eflash_hw_prog(phys_page, buf);
}

// 读取指定索引的free_node
static free_node_t read_free_node(uint16_t phys_page, uint16_t index) {
    free_node_t node = {{0xFF, 0xFF, 0xFF}, 0xFFFF};  // 默认返回无效节点
    
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

// 在free_node表中查找并删除指定页的节点，返回其大小
static uint16_t remove_node_from_table(space_mgr_t *mgr, uint16_t target_page) {
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t count = read_node_count(mgr->free_node_pages[i]);
        
        FTL_DEBUG("[REMOVE_NODE] Searching page=%d in free_node[%d], count=%d\n", 
                 target_page, i, count);
        
        for (uint16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            uint32_t node_addr = read_addr_24(node.addr);
            
            FTL_DEBUG("[REMOVE_NODE] Checking node[%d][%d]: addr=0x%06X, size=%d\n",
                     i, j, node_addr, node.size);
            
            if (node_addr == target_page) {
                FTL_DEBUG("[REMOVE_NODE] Found match at [%d][%d], removing...\n", i, j);
                
                // 找到目标节点，将其移除（用最后一个节点覆盖）
                uint16_t last_idx = count - 1;
                if (j != last_idx) {
                    free_node_t last_node = read_free_node(mgr->free_node_pages[i], last_idx);
                    write_free_node(mgr->free_node_pages[i], j, &last_node);
                    
                    // 验证写入
                    free_node_t verify = read_free_node(mgr->free_node_pages[i], j);
                    FTL_DEBUG("[REMOVE_NODE] Moved last node to [%d][%d]: addr=0x%06X, size=%d\n",
                             i, j, read_addr_24(verify.addr), verify.size);
                }
                
                // 清除最后一个节点
                free_node_t empty_node = {{0xFF, 0xFF, 0xFF}, 0xFFFF};
                write_free_node(mgr->free_node_pages[i], last_idx, &empty_node);
                
                // 更新count
                write_node_count(mgr->free_node_pages[i], count - 1);
                
                // 验证count更新
                uint16_t new_count = read_node_count(mgr->free_node_pages[i]);
                FTL_DEBUG("[REMOVE_NODE] Updated count: %d -> %d\n", count, new_count);
                
                return node.size;
            }
        }
    }
    
    FTL_DEBUG("[REMOVE_NODE] ERROR: Page %d not found in any free_node table!\n", target_page);
    return 0;  // 未找到
}

// 插入新的free_node到表中（保持按size排序）
static void insert_node_to_table(space_mgr_t *mgr, uint16_t page, uint16_t size) {
    // 寻找合适的插入位置（第一个size大于等于新节点的页）
    int best_page_idx = -1;
    uint16_t best_size = 0xFFFF;
    
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
    
    // 向后移动元素
    for (uint16_t j = count; j > insert_pos; j--) {
        free_node_t node = read_free_node(mgr->free_node_pages[best_page_idx], j - 1);
        write_free_node(mgr->free_node_pages[best_page_idx], j, &node);
    }
    
    // 插入新节点
    free_node_t new_node;
    write_addr_24(new_node.addr, page);
    new_node.size = size;
    write_free_node(mgr->free_node_pages[best_page_idx], insert_pos, &new_node);
    
    // 更新count
    write_node_count(mgr->free_node_pages[best_page_idx], count + 1);
}

// --- 接口实现 ---

void space_mgr_init(space_mgr_t *mgr, uint16_t total_pages) {
    mgr->total_pages = total_pages;
    
    // 分配free_node表物理页（使用前4页）
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        mgr->free_node_pages[i] = i;
        // 先擦除页面
        eflash_hw_erase(i);
        write_node_count(i, 0);  // 初始化为空
    }
    
    // 分配对象头表物理页（接下来的8页）
    for (int i = 0; i < BASE_HEADER_PAGES; i++) {
        mgr->header_pages[i] = FREE_NODE_PAGE_COUNT + i;
        // 擦除对象头页
        eflash_hw_erase(mgr->header_pages[i]);
    }
    
    // 初始化整个空闲空间为一个大的free_node
    uint16_t first_user_page = FREE_NODE_PAGE_COUNT + BASE_HEADER_PAGES;
    uint16_t user_pages = total_pages - first_user_page;
    
    if (user_pages > 0) {
        insert_node_to_table(mgr, first_user_page, user_pages);
    }
    
    mgr->next_alloc_page = first_user_page;
}

int space_mgr_alloc(space_mgr_t *mgr, uint32_t size, uint16_t *out_page, uint16_t *out_offset) {
    // 计算需要的页数
    uint16_t pages_needed = (size + EFLASH_PAGE_SIZE - 1) / EFLASH_PAGE_SIZE;
    
    // 遍历所有free_node页，寻找第一个满足大小的节点
    for (int i = 0; i < FREE_NODE_PAGE_COUNT; i++) {
        uint16_t count = read_node_count(mgr->free_node_pages[i]);
        
        for (uint16_t j = 0; j < count; j++) {
            free_node_t node = read_free_node(mgr->free_node_pages[i], j);
            
            if (node.size >= pages_needed) {
                // 找到合适的节点
                uint32_t alloc_addr = read_addr_24(node.addr);
                uint16_t remaining = node.size - pages_needed;
                
                FTL_DEBUG("[SPACE_ALLOC] Allocating page=%d, size=%d from free_node[%d][%d]\n", 
                         (uint16_t)alloc_addr, node.size, i, j);
                
                // 从表中移除原节点
                remove_node_from_table(mgr, (uint16_t)alloc_addr);
                
                // 如果有剩余空间，插入剩余节点
                if (remaining > 0) {
                    insert_node_to_table(mgr, (uint16_t)(alloc_addr + pages_needed), remaining);
                }
                
                *out_page = (uint16_t)alloc_addr;
                *out_offset = 0;  // 按页分配，偏移始终为0
                
                // 验证：读取刚被移除的节点位置，确认已被清除
                free_node_t verify_node = read_free_node(mgr->free_node_pages[i], j);
                uint32_t verify_addr = read_addr_24(verify_node.addr);
                FTL_DEBUG("[SPACE_ALLOC] After removal: node[%d][%d] addr=0x%06X, size=%d\n",
                         i, j, verify_addr, verify_node.size);
                
                return 0;
            }
        }
    }
    
    FTL_DEBUG("[SPACE_ALLOC] ERROR: No suitable free node found for size=%d\n", pages_needed);
    return -1;  // 空间不足
}

void space_mgr_free(space_mgr_t *mgr, uint16_t page, uint16_t offset, uint32_t size) {
    uint16_t pages_freed = (size + EFLASH_PAGE_SIZE - 1) / EFLASH_PAGE_SIZE;
    
    // 检查是否可以与前一个空闲块合并
    uint16_t prev_size = remove_node_from_table(mgr, page - 1);
    if (prev_size > 0) {
        page = page - 1;
        pages_freed += prev_size;
    }
    
    // 检查是否可以与后一个空闲块合并
    uint16_t next_size = remove_node_from_table(mgr, page + pages_freed);
    if (next_size > 0) {
        pages_freed += next_size;
    }
    
    // 插入合并后的节点
    insert_node_to_table(mgr, page, pages_freed);
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
