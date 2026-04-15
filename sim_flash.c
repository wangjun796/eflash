/*
 * Dhara - NAND flash management layer
 * Memory mapped Flash simulator for Windows
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

#define FLASH_SIZE (512 * 1024)  // 512KB
#define MAP_BASE_ADDR 0x80000000ULL  // 2GB address
#define FILE_NAME "sim_flash.bin"

// Flash simulator structure
typedef struct {
    HANDLE file_handle;
    HANDLE mapping_handle;
    uint8_t* mapped_memory;
    bool initialized;
} flash_simulator_t;

static flash_simulator_t flash_sim = {0};

/**
 * Initialize the memory mapped flash simulator
 * Creates a 512KB file and maps it to memory at 0x80000000
 */
bool init_flash_simulator(void) {
    // Create or open the file with desired size
    flash_sim.file_handle = CreateFile(
        FILE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,  // Always create a new file
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    
    if (flash_sim.file_handle == INVALID_HANDLE_VALUE) {
        printf("Error creating flash simulation file: %lu\n", GetLastError());
        return false;
    }

    // Set file size to 512KB
    LARGE_INTEGER file_size;
    file_size.QuadPart = FLASH_SIZE;
    if (!SetFilePointerEx(flash_sim.file_handle, file_size, NULL, FILE_BEGIN) || 
        !SetEndOfFile(flash_sim.file_handle)) {
        printf("Error setting file size: %lu\n", GetLastError());
        CloseHandle(flash_sim.file_handle);
        return false;
    }

    // Create a file mapping object
    flash_sim.mapping_handle = CreateFileMapping(
        flash_sim.file_handle,
        NULL,
        PAGE_READWRITE,
        0,  // High-order size
        FLASH_SIZE,  // Low-order size (512KB)
        NULL  // Name of mapping object
    );

    if (flash_sim.mapping_handle == NULL) {
        printf("Error creating file mapping: %lu\n", GetLastError());
        CloseHandle(flash_sim.file_handle);
        return false;
    }

    // Map the file to memory at the specified address
    flash_sim.mapped_memory = (uint8_t*)MapViewOfFileEx(
        flash_sim.mapping_handle,
        FILE_MAP_ALL_ACCESS,
        0,  // Offset high
        0,  // Offset low
        FLASH_SIZE,  // Size to map
        (LPVOID)MAP_BASE_ADDR  // Preferred address
    );

    if (flash_sim.mapped_memory == NULL) {
        // Try without preferred address if the preferred address is not available
        flash_sim.mapped_memory = (uint8_t*)MapViewOfFile(
            flash_sim.mapping_handle,
            FILE_MAP_ALL_ACCESS,
            0,  // Offset high
            0,  // Offset low
            FLASH_SIZE  // Size to map
        );
        
        if (flash_sim.mapped_memory == NULL) {
            printf("Error mapping view of file: %lu\n", GetLastError());
            CloseHandle(flash_sim.mapping_handle);
            CloseHandle(flash_sim.file_handle);
            return false;
        } else {
            printf("Warning: Could not map to preferred address 0x%p, using default address 0x%p\n", 
                   (void*)MAP_BASE_ADDR, (void*)flash_sim.mapped_memory);
        }
    } else {
        printf("Successfully mapped flash simulation to address 0x%p\n", (void*)flash_sim.mapped_memory);
    }

    // Initialize the memory with 0xFF (representing erased state)
    memset(flash_sim.mapped_memory, 0xFF, FLASH_SIZE);
    
    flash_sim.initialized = true;
    printf("Flash simulator initialized: %s (%d KB)\n", FILE_NAME, FLASH_SIZE / 1024);
    
    return true;
}

/**
 * Clean up and close the flash simulator
 */
void cleanup_flash_simulator(void) {
    if (flash_sim.initialized) {
        // Unmap the view
        if (flash_sim.mapped_memory) {
            UnmapViewOfFile(flash_sim.mapped_memory);
            flash_sim.mapped_memory = NULL;
        }
        
        // Close handles
        if (flash_sim.mapping_handle) {
            CloseHandle(flash_sim.mapping_handle);
            flash_sim.mapping_handle = NULL;
        }
        
        if (flash_sim.file_handle) {
            CloseHandle(flash_sim.file_handle);
            flash_sim.file_handle = NULL;
        }
        
        flash_sim.initialized = false;
        printf("Flash simulator cleaned up\n");
    }
}

/**
 * Read data from the simulated flash
 * @param addr Address in the flash to read from
 * @param buffer Buffer to store the read data
 * @param size Number of bytes to read
 * @return True on success, false on failure
 */
bool read_flash(uint32_t addr, void* buffer, size_t size) {
    if (!flash_sim.initialized) {
        printf("Error: Flash simulator not initialized\n");
        return false;
    }
    
    if (addr + size > FLASH_SIZE) {
        printf("Error: Read beyond flash boundary\n");
        return false;
    }
    
    memcpy(buffer, flash_sim.mapped_memory + addr, size);
    return true;
}

/**
 * Write data to the simulated flash
 * Note: Flash writing typically can only change 1s to 0s, but for simulation purposes
 * we'll allow arbitrary writes. In real flash, you'd need to implement page/block erase logic.
 * @param addr Address in the flash to write to
 * @param data Data to write
 * @param size Number of bytes to write
 * @return True on success, false on failure
 */
bool write_flash(uint32_t addr, const void* data, size_t size) {
    if (!flash_sim.initialized) {
        printf("Error: Flash simulator not initialized\n");
        return false;
    }
    
    if (addr + size > FLASH_SIZE) {
        printf("Error: Write beyond flash boundary\n");
        return false;
    }
    
    // Copy data to the mapped memory
    memcpy(flash_sim.mapped_memory + addr, data, size);
    
    // Force a flush to ensure data is written to the file
    if (!FlushViewOfFile(flash_sim.mapped_memory + addr, size)) {
        printf("Warning: Could not flush view to file: %lu\n", GetLastError());
    }
    
    return true;
}

/**
 * Erase a block in the simulated flash (set to 0xFF)
 * @param addr Address of the block to erase (should be block-aligned)
 * @param size Size of the block to erase
 * @return True on success, false on failure
 */
bool erase_flash_block(uint32_t addr, size_t size) {
    if (!flash_sim.initialized) {
        printf("Error: Flash simulator not initialized\n");
        return false;
    }
    
    if (addr + size > FLASH_SIZE) {
        printf("Error: Erase beyond flash boundary\n");
        return false;
    }
    
    // Set memory to 0xFF (erased state)
    memset(flash_sim.mapped_memory + addr, 0xFF, size);
    
    // Force a flush to ensure data is written to the file
    if (!FlushViewOfFile(flash_sim.mapped_memory + addr, size)) {
        printf("Warning: Could not flush view to file: %lu\n", GetLastError());
    }
    
    return true;
}

/**
 * Get direct access to the flash memory for performance-critical operations
 * @return Pointer to the mapped flash memory
 */
uint8_t* get_flash_ptr(void) {
    return flash_sim.initialized ? flash_sim.mapped_memory : NULL;
}

#ifdef SIM_FLASH_MAIN
// Example usage
int main(void) {
    printf("Initializing Flash Simulator...\n");
    
    if (!init_flash_simulator()) {
        printf("Failed to initialize flash simulator\n");
        return -1;
    }
    
    // Example: Write some data to flash
    const char* test_data = "Hello, Flash Simulator!";
    if (!write_flash(0x0, test_data, strlen(test_data) + 1)) {
        printf("Write failed\n");
        cleanup_flash_simulator();
        return -1;
    }
    
    // Example: Read the data back
    char read_buffer[100];
    if (!read_flash(0x0, read_buffer, strlen(test_data) + 1)) {
        printf("Read failed\n");
        cleanup_flash_simulator();
        return -1;
    }
    
    printf("Read from flash: %s\n", read_buffer);
    
    // Example: Erase a block
    if (!erase_flash_block(0x1000, 0x1000)) {  // Erase 4KB block at 0x1000
        printf("Erase failed\n");
    }
    
    // Verify the erase
    uint8_t verify_byte;
    if (read_flash(0x1000, &verify_byte, 1) && verify_byte == 0xFF) {
        printf("Block successfully erased\n");
    } else {
        printf("Block not properly erased\n");
    }
    
    cleanup_flash_simulator();
    printf("Flash simulator demo completed\n");
    
    return 0;
}
#endif