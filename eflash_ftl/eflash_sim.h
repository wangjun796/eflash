#ifndef EFLASH_SIM_H
#define EFLASH_SIM_H

#include <stdint.h>
#include <stddef.h>

#define EFLASH_PAGE_SIZE    512
#define EFLASH_TOTAL_PAGES  2048 // 1MB / 512B (reduced from 8192 to 2048 to speed up testing)
#define FLASH_FILE_REMAP_ADDR 0x80000000
// Initialize simulated Flash file
int eflash_init(const char *filename);

// Hardware primitives
int eflash_hw_erase(uint16_t ppn);
int eflash_hw_prog(uint16_t ppn, const uint8_t *data);
int eflash_hw_read(uint16_t ppn, uint8_t *data);

// eFlash operation: bit update (1->0 only)
int eflash_hw_word_update(uint16_t ppn, uint16_t offset, uint16_t data);

// Helper function: Check if page is all 0xFF (blank page)
int eflash_hw_is_blank(uint16_t ppn);

#endif