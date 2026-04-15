/*
 * Dhara - NAND flash management layer
 * Example using memory mapped flash simulator with Dhara library
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <windows.h>
#include "dhara/config.h"
#include "sim_flash.h"
#include "dhara/nand.h"
#include "dhara/map.h"
#include "dhara/error.h"

// Define our simulated NAND flash parameters
#define LOG2_PAGE_SIZE 9       // 512 bytes per page
#define LOG2_PAGES_PER_BLOCK 3 // 8 pages per block
#define LOG2_BLOCK_SIZE (LOG2_PAGE_SIZE + LOG2_PAGES_PER_BLOCK) // 4096 bytes per block
#define TOTAL_SIZE (512 * 1024) // 512KB total
#define NUM_BLOCKS (TOTAL_SIZE >> LOG2_BLOCK_SIZE) // Total number of blocks

// Define our custom NAND flash interface
typedef struct {
    struct dhara_nand base;
    uint8_t *flash_memory;
    unsigned char bad_blocks[NUM_BLOCKS / 8 + 1];  // Bit array for bad blocks
} simulated_nand_t;

// Implement the NAND interface functions
int dhara_nand_is_bad(const struct dhara_nand *nand, dhara_block_t block)
{
    const simulated_nand_t *sim = (const simulated_nand_t *)nand;
    
    if (block >= NUM_BLOCKS) {
        fprintf(stderr, "NAND is_bad called on invalid block: %u\n", block);
        abort();
    }
    
    const unsigned int byte_idx = block / 8;
    const unsigned int bit_idx = block % 8;
    return (sim->bad_blocks[byte_idx] >> bit_idx) & 1;
}

void dhara_nand_mark_bad(const struct dhara_nand *nand, dhara_block_t block)
{
    simulated_nand_t *sim = (simulated_nand_t *)nand;
    
    if (block >= NUM_BLOCKS) {
        fprintf(stderr, "NAND mark_bad called on invalid block: %u\n", block);
        abort();
    }
    
    const unsigned int byte_idx = block / 8;
    const unsigned int bit_idx = block % 8;
    sim->bad_blocks[byte_idx] |= (1 << bit_idx);
    
    printf("Marked block %u as bad\n", block);
}

int dhara_nand_erase(const struct dhara_nand *nand, dhara_block_t block, dhara_error_t *err)
{
    simulated_nand_t *sim = (simulated_nand_t *)nand;
    
    if (block >= NUM_BLOCKS) {
        fprintf(stderr, "NAND erase called on invalid block: %u\n", block);
        abort();
    }
    
    if (dhara_nand_is_bad(nand, block)) {
        fprintf(stderr, "NAND erase called on bad block: %u\n", block);
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    // Calculate the start position in our flash memory
    const size_t block_start = block << LOG2_BLOCK_SIZE;
    const size_t block_size = 1 << LOG2_BLOCK_SIZE;
    
    // Erase by setting all bytes to 0xFF
    memset(sim->flash_memory + block_start, 0xFF, block_size);
    
    // Flush to ensure the changes are written to the file
    if (!FlushViewOfFile(sim->flash_memory + block_start, block_size)) {
        fprintf(stderr, "Could not flush view to file: %lu\n", GetLastError());
    }
    
    return 0;
}

int dhara_nand_prog(const struct dhara_nand *nand, dhara_page_t page, 
                    const uint8_t *data, dhara_error_t *err)
{
    simulated_nand_t *sim = (simulated_nand_t *)nand;
    
    const dhara_block_t block = page >> LOG2_PAGES_PER_BLOCK;
    const unsigned int page_in_block = page & ((1 << LOG2_PAGES_PER_BLOCK) - 1);
    
    if (block >= NUM_BLOCKS) {
        fprintf(stderr, "NAND prog called on invalid block: %u\n", block);
        abort();
    }
    
    if (dhara_nand_is_bad(nand, block)) {
        fprintf(stderr, "NAND prog called on bad block: %u\n", block);
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    
    // Calculate the start position in our flash memory
    const size_t page_start = page << LOG2_PAGE_SIZE;
    const size_t page_size = 1 << LOG2_PAGE_SIZE;
    
    // In real NAND flash, programming can only change 1s to 0s, but for simulation we allow any change
    memcpy(sim->flash_memory + page_start, data, page_size);
    
    // Flush to ensure the changes are written to the file
    if (!FlushViewOfFile(sim->flash_memory + page_start, page_size)) {
        fprintf(stderr, "Could not flush view to file: %lu\n", GetLastError());
    }
    
    return 0;
}

int dhara_nand_is_free(const struct dhara_nand *nand, dhara_page_t page)
{
    simulated_nand_t *sim = (simulated_nand_t *)nand;
    
    const dhara_block_t block = page >> LOG2_PAGES_PER_BLOCK;
    if (block >= NUM_BLOCKS) {
        fprintf(stderr, "NAND is_free called on invalid block: %u\n", block);
        abort();
    }
    
    // Calculate the start position in our flash memory
    const size_t page_start = page << LOG2_PAGE_SIZE;
    const size_t page_size = 1 << LOG2_PAGE_SIZE;
    
    // Check if the page is completely filled with 0xFF (erased state)
    const uint8_t *page_data = sim->flash_memory + page_start;
    for (size_t i = 0; i < page_size; i++) {
        if (page_data[i] != 0xFF) {
            return 0;  // Not free if any byte is not 0xFF
        }
    }
    
    return 1;  // Free if all bytes are 0xFF
}

int dhara_nand_read(const struct dhara_nand *nand, dhara_page_t page,
                    size_t offset, size_t length,
                    uint8_t *data, dhara_error_t *err)
{
    simulated_nand_t *sim = (simulated_nand_t *)nand;
    
    if ((offset > (1 << LOG2_PAGE_SIZE)) || (length > (1 << LOG2_PAGE_SIZE)) ||
        (offset + length > (1 << LOG2_PAGE_SIZE))) {
        fprintf(stderr, "NAND read called on invalid range: offset = %zu, length = %zu\n", offset, length);
        abort();
    }
    
    // Calculate the start position in our flash memory
    const size_t page_start = page << LOG2_PAGE_SIZE;
    
    memcpy(data, sim->flash_memory + page_start + offset, length);
    return 0;
}

int dhara_nand_copy(const struct dhara_nand *nand,
                    dhara_page_t src, dhara_page_t dst,
                    dhara_error_t *err)
{
    uint8_t buf[1 << LOG2_PAGE_SIZE];
    
    if ((dhara_nand_read(nand, src, 0, 1 << LOG2_PAGE_SIZE, buf, err) < 0) ||
        (dhara_nand_prog(nand, dst, buf, err) < 0))
        return -1;
    
    return 0;
}

int main(void)
{
    printf("Dhara with Memory-Mapped Flash Simulator Demo\n");
    printf("=============================================\n\n");
    
    // Initialize the flash simulator
    if (!init_flash_simulator()) {
        printf("Failed to initialize flash simulator\n");
        return -1;
    }
    
    // Get pointer to the mapped flash memory
    uint8_t *flash_ptr = get_flash_ptr();
    if (!flash_ptr) {
        printf("Failed to get flash pointer\n");
        cleanup_flash_simulator();
        return -1;
    }
    
    // Initialize our simulated NAND flash
    static simulated_nand_t nand = {
        .base = {
            .log2_page_size = LOG2_PAGE_SIZE,
            .log2_ppb = LOG2_PAGES_PER_BLOCK,
            .num_blocks = NUM_BLOCKS
        }
    };
    nand.flash_memory = flash_ptr;
    memset(nand.bad_blocks, 0, sizeof(nand.bad_blocks));
    
    printf("Simulated NAND flash parameters:\n");
    printf("- Page size: %u bytes\n", 1 << LOG2_PAGE_SIZE);
    printf("- Pages per block: %u\n", 1 << LOG2_PAGES_PER_BLOCK);
    printf("- Block size: %u bytes\n", 1 << LOG2_BLOCK_SIZE);
    printf("- Total blocks: %u\n", NUM_BLOCKS);
    printf("- Total size: %u KB\n", TOTAL_SIZE / 1024);
    printf("\n");
    
    // Initialize Dhara map
    const size_t page_size = 1 << nand.base.log2_page_size;
    uint8_t page_buf[MAX_PAGE_SIZE];  // Fixed-size buffer
    
    struct dhara_map map;
    dhara_map_init(&map, &nand.base, page_buf, 4);  // GC ratio of 4
    
    // Initialize and resume the map
    printf("Resuming map...\n");
    if (dhara_map_resume(&map, NULL) < 0) {
        printf("Failed to resume map\n");
        cleanup_flash_simulator();
        return -1;
    }
    
    printf("Map resumed successfully\n");
    printf("Available sectors: %llu\n", (unsigned long long)dhara_map_capacity(&map));
    printf("\n");
    
    // Write some data to the map
    printf("Writing test data...\n");
    const char *test_data = "Hello from memory-mapped flash!";
    dhara_error_t err;
    
    if (dhara_map_write(&map, 0, (uint8_t*)test_data, &err) < 0) {
        printf("Write failed: %s\n", dhara_strerror(err));
        cleanup_flash_simulator();
        return -1;
    }
    
    // Read the data back
    printf("Reading test data...\n");
    char read_buffer[MAX_PAGE_SIZE];
    if (dhara_map_read(&map, 0, (uint8_t*)read_buffer, &err) < 0) {
        printf("Read failed: %s\n", dhara_strerror(err));
        cleanup_flash_simulator();
        return -1;
    }
    
    printf("Read from flash: %s\n", read_buffer);
    
    // Synchronize the map
    printf("Syncing map...\n");
    if (dhara_map_sync(&map, NULL) < 0) {
        printf("Sync failed\n");
        cleanup_flash_simulator();
        return -1;
    }
    
    printf("Demo completed successfully!\n");
    
    // Cleanup
    cleanup_flash_simulator();
    
    return 0;
}