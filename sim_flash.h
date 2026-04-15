/*
 * Dhara - NAND flash management layer
 * Memory mapped Flash simulator header
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

#ifndef SIM_FLASH_H_
#define SIM_FLASH_H_

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Constants for the simulated flash
#define SIM_FLASH_SIZE (512 * 1024)  // 512KB
#define SIM_FLASH_BASE_ADDR 0x80000000ULL  // 2GB address
#define SIM_FLASH_FILE_NAME "sim_flash.bin"

/**
 * Initialize the memory mapped flash simulator
 * Creates a 512KB file and maps it to memory at 0x80000000
 * @return True on success, false on failure
 */
bool init_flash_simulator(void);

/**
 * Clean up and close the flash simulator
 */
void cleanup_flash_simulator(void);

/**
 * Read data from the simulated flash
 * @param addr Address in the flash to read from
 * @param buffer Buffer to store the read data
 * @param size Number of bytes to read
 * @return True on success, false on failure
 */
bool read_flash(uint32_t addr, void* buffer, size_t size);

/**
 * Write data to the simulated flash
 * Note: Flash writing typically can only change 1s to 0s, but for simulation purposes
 * we'll allow arbitrary writes. In real flash, you'd need to implement page/block erase logic.
 * @param addr Address in the flash to write to
 * @param data Data to write
 * @param size Number of bytes to write
 * @return True on success, false on failure
 */
bool write_flash(uint32_t addr, const void* data, size_t size);

/**
 * Erase a block in the simulated flash (set to 0xFF)
 * @param addr Address of the block to erase (should be block-aligned)
 * @param size Size of the block to erase
 * @return True on success, false on failure
 */
bool erase_flash_block(uint32_t addr, size_t size);

/**
 * Get direct access to the flash memory for performance-critical operations
 * @return Pointer to the mapped flash memory
 */
uint8_t* get_flash_ptr(void);

#endif /* SIM_FLASH_H_ */