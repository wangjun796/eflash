#include "eflash_sim.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static FILE *flash_file = NULL;

int eflash_init(const char *filename) {
    flash_file = fopen(filename, "rb+");
    if (!flash_file) {
        flash_file = fopen(filename, "wb+");
        if (!flash_file) return -1;
        // 初始化为全 0xFF
        uint8_t blank[EFLASH_PAGE_SIZE];
        memset(blank, 0xFF, EFLASH_PAGE_SIZE);
        for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
            fwrite(blank, 1, EFLASH_PAGE_SIZE, flash_file);
        }
        fflush(flash_file);
    }
    return 0;
}

void eflash_deinit() {
    if (flash_file) fclose(flash_file);
}

int eflash_hw_erase(uint16_t page_addr) {
    if (!flash_file || page_addr >= EFLASH_TOTAL_PAGES) return -1;
    uint8_t blank[EFLASH_PAGE_SIZE];
    memset(blank, 0xFF, EFLASH_PAGE_SIZE);
    fseek(flash_file, page_addr * EFLASH_PAGE_SIZE, SEEK_SET);
    fwrite(blank, 1, EFLASH_PAGE_SIZE, flash_file);
    return 0;
}

int eflash_hw_prog(uint16_t page_addr, const uint8_t *data) {
    if (!flash_file || page_addr >= EFLASH_TOTAL_PAGES) return -1;
    fseek(flash_file, page_addr * EFLASH_PAGE_SIZE, SEEK_SET);
    fwrite(data, 1, EFLASH_PAGE_SIZE, flash_file);
    return 0;
}

int eflash_hw_read(uint16_t page_addr, uint8_t *data) {
    if (!flash_file || page_addr >= EFLASH_TOTAL_PAGES) return -1;
    fseek(flash_file, page_addr * EFLASH_PAGE_SIZE, SEEK_SET);
    fread(data, 1, EFLASH_PAGE_SIZE, flash_file);
    return 0;
}

// 模拟字更新：检查是否满足 1->0 规律
int eflash_hw_word_update(uint16_t page_addr, uint16_t offset, uint16_t data) {
    if (offset + 2 > EFLASH_PAGE_SIZE) return -1;

    uint8_t current[2];
    fseek(flash_file, page_addr * EFLASH_PAGE_SIZE + offset, SEEK_SET);
    fread(current, 1, 2, flash_file);

    uint16_t cur_val = (current[1] << 8) | current[0];

    // 检查是否所有要变 0 的位当前都是 1
    if ((cur_val & data) != data) {
        return -1; // 违反 1->0 规律
    }

    uint16_t new_val = cur_val & data;
    uint8_t new_bytes[2] = { (uint8_t)(new_val & 0xFF), (uint8_t)((new_val >> 8) & 0xFF) };

    fseek(flash_file, page_addr * EFLASH_PAGE_SIZE + offset, SEEK_SET);
    fwrite(new_bytes, 1, 2, flash_file);
    return 0;
}