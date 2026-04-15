/*
 * Dhara - NAND flash management layer
 * File-based Flash simulator NAND interface for Windows
 * Copyright (C) 2026 Daniel Beer <dlbeer@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "dhara/nand.h"
#include "dhara/error.h"
#include "sim_flash.h"

// ============================================================================
// File-based Flash NAND Interface
// This provides the dhara_nand_* functions for file-based flash simulation
// ============================================================================

#define FILE_FLASH_LOG2_PAGE_SIZE    9
#define FILE_FLASH_LOG2_PPB          3
#define FILE_FLASH_NUM_BLOCKS        128
#define FILE_FLASH_NUM_PAGES         (FILE_FLASH_NUM_BLOCKS * (1 << FILE_FLASH_LOG2_PPB))

// Global NAND structure for file-based flash simulation
const struct dhara_nand file_sim_nand = {
    .log2_page_size = FILE_FLASH_LOG2_PAGE_SIZE,
    .log2_ppb = FILE_FLASH_LOG2_PPB,
    .num_blocks = FILE_FLASH_NUM_BLOCKS
};

// File-based flash state
static struct {
    int bad_blocks[FILE_FLASH_NUM_BLOCKS];
    int erased[FILE_FLASH_NUM_BLOCKS];
} file_flash_state = {0};

int dhara_nand_is_bad(const struct dhara_nand *n, dhara_block_t bno)
{
    (void)n;
    if (bno >= FILE_FLASH_NUM_BLOCKS) {
        fprintf(stderr, "file_flash: is_bad called on invalid block: %d\n", bno);
        return 0;
    }
    // Debug output
    // printf("dhara_nand_is_bad(%d) = %d\n", bno, file_flash_state.bad_blocks[bno]);
    return file_flash_state.bad_blocks[bno];
}

void dhara_nand_mark_bad(const struct dhara_nand *n, dhara_block_t bno)
{
    (void)n;
    if (bno >= FILE_FLASH_NUM_BLOCKS) {
        fprintf(stderr, "file_flash: mark_bad called on invalid block: %d\n", bno);
        return;
    }
    printf("file_flash: Marking block %d as bad\n", bno);
    file_flash_state.bad_blocks[bno] = 1;
}

int dhara_nand_erase(const struct dhara_nand *n, dhara_block_t bno,
                     dhara_error_t *err)
{
    const size_t block_size = 1 << (n->log2_page_size + n->log2_ppb);
    const size_t offset = bno * block_size;
    
    if (bno >= n->num_blocks) {
        fprintf(stderr, "file_flash: erase called on invalid block: %d\n", bno);
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    if (file_flash_state.bad_blocks[bno]) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    // Erase the block (set to 0xFF)
    if (!erase_flash_block(offset, block_size)) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    file_flash_state.erased[bno] = 1;
    return 0;
}

int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t pno,
                    const uint8_t *data, dhara_error_t *err)
{
    const size_t page_size = 1 << n->log2_page_size;
    const size_t file_offset = pno * page_size;
    
    // Debug: Check for invalid accesses
    if (pno >= FILE_FLASH_NUM_PAGES) {
        fprintf(stderr, "file_flash: prog called on invalid page: %d\n", pno);
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    if (!write_flash(file_offset, data, page_size)) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    return 0;
}

int dhara_nand_read(const struct dhara_nand *n, dhara_page_t pno,
                    size_t offset, size_t len, uint8_t *buf,
                    dhara_error_t *err)
{
    const size_t page_size = 1 << n->log2_page_size;
    const size_t file_offset = pno * page_size + offset;
    
    // Debug: Check for invalid accesses
    if (pno >= FILE_FLASH_NUM_PAGES) {
        fprintf(stderr, "file_flash: read called on invalid page: %d\n", pno);
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    if ((offset + len) > page_size) {
        fprintf(stderr, "file_flash: read out of bounds: page=%d, offset=%zu, len=%zu\n", 
                pno, offset, len);
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    if (!read_flash(file_offset, buf, len)) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    return 0;
}

int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src,
                    dhara_page_t dst, dhara_error_t *err)
{
    const size_t page_size = 1 << n->log2_page_size;
    uint8_t *buf = malloc(page_size);
    
    if (!buf) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    if (dhara_nand_read(n, src, 0, page_size, buf, err) < 0) {
        free(buf);
        return -1;
    }
    
    if (dhara_nand_prog(n, dst, buf, err) < 0) {
        free(buf);
        return -1;
    }
    
    free(buf);
    return 0;
}

int dhara_nand_is_erased(const struct dhara_nand *n, dhara_page_t pno)
{
    const size_t page_size = 1 << n->log2_page_size;
    const size_t offset = pno * page_size;
    uint8_t *buf = malloc(page_size);
    int i;
    
    if (!buf || !read_flash(offset, buf, page_size)) {
        if (buf) free(buf);
        return 0;
    }
    
    for (i = 0; i < page_size; i++) {
        if (buf[i] != 0xFF) {
            free(buf);
            return 0;
        }
    }
    
    free(buf);
    return 1;
}

int dhara_nand_is_free(const struct dhara_nand *n, dhara_page_t pno)
{
    // For file-based flash simulation, we consider a page free if it's erased
    return dhara_nand_is_erased(n, pno);
}

// Helper function to check if flash is properly initialized
int check_flash_initialization(const struct dhara_nand *n)
{
    const size_t page_size = 1 << n->log2_page_size;
    uint8_t *buf = malloc(page_size);
    int i, j;
    
    if (!buf) return -1;
    
    printf("Checking flash initialization...\n");
    for (i = 0; i < 10; i++) {
        if (!read_flash(i * page_size, buf, page_size)) {
            printf("  Page %d: read failed\n", i);
            free(buf);
            return -1;
        }
        
        int all_ff = 1;
        for (j = 0; j < page_size && all_ff; j++) {
            if (buf[j] != 0xFF) {
                all_ff = 0;
                printf("  Page %d: not erased (byte %d = 0x%02x)\n", i, j, buf[j]);
            }
        }
        if (all_ff) {
            printf("  Page %d: OK (erased)\n", i);
        }
    }
    
    free(buf);
    return 0;
}
