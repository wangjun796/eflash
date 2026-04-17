#ifndef EFLASH_SIM_H
#define EFLASH_SIM_H

#include <stdint.h>
#include <stddef.h>

#define EFLASH_PAGE_SIZE    512
#define EFLASH_TOTAL_PAGES  2048 // 1MB / 512B (从8192减小到2048以加速测试)

// 初始化模拟 Flash 文件
int eflash_init(const char *filename);

// 硬件原语
int eflash_hw_erase(uint16_t page_addr);
int eflash_hw_prog(uint16_t page_addr, const uint8_t *data);
int eflash_hw_read(uint16_t page_addr, uint8_t *data);

// eFlash 操作，位更新 (仅 1->0)
int eflash_hw_word_update(uint16_t page_addr, uint16_t offset, uint16_t data);

#endif