#include "eflash_sim.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

// Memory-mapped flash simulation
static uint8_t *flash_mem_map = NULL;  // Memory-mapped address (base: 0x80000000)
static size_t flash_file_size = 0;

#ifdef _WIN32
    static HANDLE flash_file_handle = INVALID_HANDLE_VALUE;
    static HANDLE flash_mapping_handle = NULL;
#else
    static int flash_fd = -1;
#endif

int eflash_init(const char *filename) {
    flash_file_size = (size_t)EFLASH_TOTAL_PAGES * EFLASH_PAGE_SIZE;
    
#ifdef _WIN32
    // Windows: Create file if not exists
    flash_file_handle = CreateFileA(
        filename,
        GENERIC_READ | GENERIC_WRITE,
        0,  // No sharing
        NULL,
        OPEN_ALWAYS,  // Open existing or create new
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    
    if (flash_file_handle == INVALID_HANDLE_VALUE) {
        printf("[EFLASH_INIT] ERROR: Failed to open/create file: %s\n", filename);
        return -1;
    }
    
    // Check if file is newly created
    DWORD file_size = GetFileSize(flash_file_handle, NULL);
    if (file_size == 0 || file_size != flash_file_size) {
        printf("[EFLASH_INIT] File not found or wrong size, creating new: %s\n", filename);
        
        // Set file size
        LARGE_INTEGER size;
        size.QuadPart = flash_file_size;
        SetFilePointerEx(flash_file_handle, size, NULL, FILE_BEGIN);
        SetEndOfFile(flash_file_handle);
        
        // Initialize to all 0xFF
        uint8_t *blank_buf = (uint8_t *)malloc(EFLASH_PAGE_SIZE);
        memset(blank_buf, 0xFF, EFLASH_PAGE_SIZE);
        
        DWORD bytes_written;
        SetFilePointer(flash_file_handle, 0, NULL, FILE_BEGIN);
        for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
            WriteFile(flash_file_handle, blank_buf, EFLASH_PAGE_SIZE, &bytes_written, NULL);
        }
        FlushFileBuffers(flash_file_handle);
        free(blank_buf);
        
        printf("[EFLASH_INIT] Initialized %d pages to 0xFF\n", EFLASH_TOTAL_PAGES);
    } else {
        printf("[EFLASH_INIT] Opening existing file: %s (size=%lu bytes)\n", filename, file_size);
    }
    
    // Create file mapping
    flash_mapping_handle = CreateFileMapping(
        flash_file_handle,
        NULL,
        PAGE_READWRITE,
        0,  // High DWORD of size
        (DWORD)flash_file_size,  // Low DWORD of size
        NULL
    );
    
    if (flash_mapping_handle == NULL) {
        printf("[EFLASH_INIT] ERROR: Failed to create file mapping!\n");
        CloseHandle(flash_file_handle);
        return -1;
    }
    
    // Map view of file at base address 0x80000000
    flash_mem_map = (uint8_t *)MapViewOfFileEx(
        flash_mapping_handle,
        FILE_MAP_ALL_ACCESS,
        0,  // High DWORD of offset
        0,  // Low DWORD of offset
        flash_file_size,
        (LPVOID)0x80000000  // Requested base address
    );
    
    if (flash_mem_map == NULL) {
        printf("[EFLASH_INIT] WARNING: Cannot map to 0x80000000, using system-assigned address\n");
        flash_mem_map = (uint8_t *)MapViewOfFile(
            flash_mapping_handle,
            FILE_MAP_ALL_ACCESS,
            0, 0, flash_file_size
        );
        if (flash_mem_map == NULL) {
            printf("[EFLASH_INIT] ERROR: Failed to map view of file!\n");
            CloseHandle(flash_mapping_handle);
            CloseHandle(flash_file_handle);
            return -1;
        }
    } else {
        printf("[EFLASH_INIT] Successfully mapped to base address 0x%08X\n", (unsigned int)0x80000000);
    }
    
#else
    // Linux/Unix: Use mmap
    flash_fd = open(filename, O_RDWR | O_CREAT, 0666);
    if (flash_fd < 0) {
        printf("[EFLASH_INIT] ERROR: Failed to open file: %s\n", filename);
        return -1;
    }
    
    // Check file size
    off_t file_size = lseek(flash_fd, 0, SEEK_END);
    if (file_size == 0 || file_size != (off_t)flash_file_size) {
        printf("[EFLASH_INIT] File not found or wrong size, creating new: %s\n", filename);
        
        // Set file size
        ftruncate(flash_fd, flash_file_size);
        
        // Initialize to all 0xFF
        uint8_t *blank_buf = (uint8_t *)malloc(EFLASH_PAGE_SIZE);
        memset(blank_buf, 0xFF, EFLASH_PAGE_SIZE);
        
        lseek(flash_fd, 0, SEEK_SET);
        for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
            write(flash_fd, blank_buf, EFLASH_PAGE_SIZE);
        }
        fsync(flash_fd);
        free(blank_buf);
        
        printf("[EFLASH_INIT] Initialized %d pages to 0xFF\n", EFLASH_TOTAL_PAGES);
    } else {
        printf("[EFLASH_INIT] Opening existing file: %s (size=%ld bytes)\n", filename, file_size);
    }
    
    // Map file to memory at base address 0x80000000
    flash_mem_map = (uint8_t *)mmap(
        (void *)0x80000000,  // Requested address
        flash_file_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        flash_fd,
        0  // Offset
    );
    
    if (flash_mem_map == MAP_FAILED) {
        printf("[EFLASH_INIT] WARNING: Cannot map to 0x80000000, using system-assigned address\n");
        flash_mem_map = (uint8_t *)mmap(
            NULL,  // Let system choose address
            flash_file_size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            flash_fd,
            0
        );
        if (flash_mem_map == MAP_FAILED) {
            printf("[EFLASH_INIT] ERROR: Failed to mmap file!\n");
            close(flash_fd);
            return -1;
        }
    } else {
        printf("[EFLASH_INIT] Successfully mapped to base address 0x%08X\n", (unsigned int)0x80000000);
    }
#endif
    
    printf("[EFLASH_INIT] Flash memory mapped at: 0x%p (size=%zu bytes)\n", 
           (void *)flash_mem_map, flash_file_size);
    
    return 0;
}

void eflash_deinit() {
    if (flash_mem_map) {
#ifdef _WIN32
        UnmapViewOfFile(flash_mem_map);
        if (flash_mapping_handle) {
            CloseHandle(flash_mapping_handle);
        }
        if (flash_file_handle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(flash_file_handle);
            CloseHandle(flash_file_handle);
        }
#else
        msync(flash_mem_map, flash_file_size, MS_SYNC);  // Sync to file before unmap
        munmap(flash_mem_map, flash_file_size);
        if (flash_fd >= 0) {
            fsync(flash_fd);
            close(flash_fd);
        }
#endif
        flash_mem_map = NULL;
        printf("[EFLASH_DEINIT] Flash memory unmapped and file closed\n");
    }
}

int eflash_hw_erase(uint16_t page_addr) {
    if (!flash_mem_map || page_addr >= EFLASH_TOTAL_PAGES) return -1;
    
    // Optimization: Check if page is already all 0xFF, skip erase if so
    if (eflash_hw_is_blank(page_addr) == 1) {
        // Page is already all 0xFF, no need to erase
        return 0;
    }
    
    // Perform erase operation via memory-mapped access
    uint8_t *page_ptr = flash_mem_map + (page_addr * EFLASH_PAGE_SIZE);
    memset(page_ptr, 0xFF, EFLASH_PAGE_SIZE);
    
    // Sync to file immediately
#ifdef _WIN32
    FlushViewOfFile(page_ptr, EFLASH_PAGE_SIZE);
#else
    msync(page_ptr, EFLASH_PAGE_SIZE, MS_SYNC);
#endif
    
    return 0;
}

int eflash_hw_prog(uint16_t page_addr, const uint8_t *data) {
    if (!flash_mem_map || page_addr >= EFLASH_TOTAL_PAGES) return -1;
    
    // Write via memory-mapped access
    uint8_t *page_ptr = flash_mem_map + (page_addr * EFLASH_PAGE_SIZE);
    memcpy(page_ptr, data, EFLASH_PAGE_SIZE);
    
    // Sync to file immediately
#ifdef _WIN32
    FlushViewOfFile(page_ptr, EFLASH_PAGE_SIZE);
#else
    msync(page_ptr, EFLASH_PAGE_SIZE, MS_SYNC);
#endif
    
    return 0;
}

int eflash_hw_read(uint16_t page_addr, uint8_t *data) {
    if (!flash_mem_map || page_addr >= EFLASH_TOTAL_PAGES) return -1;
    
    // Read via memory-mapped access
    uint8_t *page_ptr = flash_mem_map + (page_addr * EFLASH_PAGE_SIZE);
    memcpy(data, page_ptr, EFLASH_PAGE_SIZE);
    
    return 0;
}

// Simulate rule: Update a 16-bit word at specified offset
// This function simulates Flash's ability to update specific bytes without full erase
// It directly writes the new value (bypassing 1->0 rule for simulation purposes)
int eflash_hw_word_update(uint16_t page_addr, uint16_t offset, uint16_t data) {
    if (!flash_mem_map || page_addr >= EFLASH_TOTAL_PAGES) return -1;
    if (offset + 2 > EFLASH_PAGE_SIZE) return -1;

    // Write the new value directly in big-endian format via memory-mapped access
    uint8_t *page_ptr = flash_mem_map + (page_addr * EFLASH_PAGE_SIZE);
    page_ptr[offset] = (uint8_t)((data >> 8) & 0xFF);
    page_ptr[offset + 1] = (uint8_t)(data & 0xFF);
    
    // Sync to file immediately
#ifdef _WIN32
    FlushViewOfFile(page_ptr + offset, 2);
#else
    msync(page_ptr + offset, 2, MS_SYNC);
#endif
    
    return 0;
}

// Check if page is all 0xFF (blank page)
int eflash_hw_is_blank(uint16_t page_addr) {
    if (!flash_mem_map || page_addr >= EFLASH_TOTAL_PAGES) return -1;

    // Check via memory-mapped access
    uint8_t *page_ptr = flash_mem_map + (page_addr * EFLASH_PAGE_SIZE);
    
    // Quick check: verify each byte is 0xFF
    for (int i = 0; i < EFLASH_PAGE_SIZE; i++) {
        if (page_ptr[i] != 0xFF) {
            return 0; // Not a blank page
        }
    }

    return 1; // Is a blank page
}
