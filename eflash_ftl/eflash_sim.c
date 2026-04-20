#include "eflash_sim.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static FILE *flash_file = NULL;

int eflash_init(const char *filename) {
    flash_file = fopen(filename, "rb+");
    if (!flash_file) {
        printf("[EFLASH_INIT] File not found, creating new: %s\n", filename);
        flash_file = fopen(filename, "wb+");
        if (!flash_file) {
            printf("[EFLASH_INIT] ERROR: Failed to create file!\n");
            return -1;
        }
        // Initialize to all 0xFF
        uint8_t blank[EFLASH_PAGE_SIZE];
        memset(blank, 0xFF, EFLASH_PAGE_SIZE);
        for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
            fwrite(blank, 1, EFLASH_PAGE_SIZE, flash_file);
        }
        fflush(flash_file);
        printf("[EFLASH_INIT] Initialized %d pages to 0xFF\n", EFLASH_TOTAL_PAGES);
    } else {
        printf("[EFLASH_INIT] WARNING: File already exists, opening in rb+ mode: %s\n", filename);
    }
    return 0;
}

void eflash_deinit() {
    if (flash_file) fclose(flash_file);
}

int eflash_hw_erase(uint16_t page_addr) {
    if (!flash_file || page_addr >= EFLASH_TOTAL_PAGES) return -1;
    
    // Optimization: Check if page is already all 0xFF, skip erase if so
    if (eflash_hw_is_blank(page_addr) == 1) {
        // Page is already all 0xFF, no need to erase
        return 0;
    }
    
    // Perform erase operation
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

// Simulate rule: Update a 16-bit word at specified offset
// This function simulates Flash's ability to update specific bytes without full erase
// It directly writes the new value (bypassing 1->0 rule for simulation purposes)
int eflash_hw_word_update(uint16_t page_addr, uint16_t offset, uint16_t data) {
    if (offset + 2 > EFLASH_PAGE_SIZE) return -1;

    // Write the new value directly in big-endian format
    uint8_t new_bytes[2] = { (uint8_t)((data >> 8) & 0xFF), (uint8_t)(data & 0xFF) };

    fseek(flash_file, page_addr * EFLASH_PAGE_SIZE + offset, SEEK_SET);
    fwrite(new_bytes, 1, 2, flash_file);
    return 0;
}

// Check if page is all 0xFF (blank page)
int eflash_hw_is_blank(uint16_t page_addr) {
    if (!flash_file || page_addr >= EFLASH_TOTAL_PAGES) return -1;

    uint8_t buf[EFLASH_PAGE_SIZE];
    fseek(flash_file, page_addr * EFLASH_PAGE_SIZE, SEEK_SET);
    size_t bytes_read = fread(buf, 1, EFLASH_PAGE_SIZE, flash_file);

    if (bytes_read != EFLASH_PAGE_SIZE) return -1;

    // Quick check: verify each byte is 0xFF
    for (int i = 0; i < EFLASH_PAGE_SIZE; i++) {
        if (buf[i] != 0xFF) {
            return 0; // Not a blank page
        }
    }

    return 1; // Is a blank page
}
