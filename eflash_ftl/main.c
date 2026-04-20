#include "mini_ftl.h"
#include "eflash_sim.h"
#include <stdio.h>
#include <string.h>

// Define object header structure (16 bytes)
// Note: If this structure is already defined in mini_ftl.h, delete this definition to avoid redefinition errors
typedef struct {
    uint16_t pkg_id;
    uint16_t class_id;
    uint8_t  type;
    uint8_t  reserved[3];
    uint32_t body_addr;
    uint32_t body_size;
} obj_header_t;

int main() {
    mini_ftl_t ftl;
    const char *flash_file = "test_flash.bin";

    // 1. Initialize and erase
    printf("Initializing simulated eFlash (Erasing to 0xFF)...\n");
    eflash_init(flash_file);
    for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
        eflash_hw_erase(i);
    }

    // 2. Start FTL
    mini_ftl_init(&ftl);
    printf("FTL Init: Base Header at Logic Page %d\n", ftl.base_hdr_addr);

    // 3. Write base area object header (ID: 0)
    obj_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.pkg_id = 0x1000;
    hdr.class_id = 0x2000;
    mini_ftl_obj_set_header(&ftl, 0, &hdr);
    printf("Set header for ID 0 (Base Level)\n");

    // 4. Write extended area object header (ID: 250 -> triggers first level extension)
    hdr.pkg_id = 0x9999;
    mini_ftl_obj_set_header(&ftl, 250, &hdr);
    printf("Set header for ID 250 (Extended Level 1)\n");

    // 5. Demonstrate transaction commit methods
    printf("\n=== Transaction Commit Demo ===\n");

    // Method 1: Universal version (works on all hardware)
    printf("\n--- Method 1: Full Page Rewrite (Universal) ---\n");
    mini_ftl_txn_begin(&ftl);

    uint8_t test_data[USER_DATA_SIZE];
    memset(test_data, 0xAA, USER_DATA_SIZE);
    mini_ftl_write(&ftl, 100, test_data);

    int ret = mini_ftl_txn_commit(&ftl);
    printf("Commit result (universal): %s\n", ret == 0 ? "SUCCESS" : "FAILED");

    // Method 2: Optimized version (requires hardware support for word update)
    printf("\n--- Method 2: Word Update (Optimized, requires hardware support) ---\n");
    mini_ftl_txn_begin(&ftl);

    memset(test_data, 0xBB, USER_DATA_SIZE);
    mini_ftl_write(&ftl, 101, test_data);

    ret = mini_ftl_txn_commit_with_update(&ftl);
    printf("Commit result (optimized): %s\n", ret == 0 ? "SUCCESS" : "FAILED");
    printf("Note: This method avoids full page erase, extending Flash lifespan!\n");

    // 6. Read verification
    printf("\n=== Read Verification ===\n");
    obj_header_t read_hdr;
    if (mini_ftl_obj_get_header(&ftl, 250, &read_hdr) == 0) {
        printf("Read ID 250: PkgID=0x%04X\n", read_hdr.pkg_id);
    } else {
        printf("Failed to read ID 250\n");
    }

    // Verify transaction data
    uint8_t read_data[USER_DATA_SIZE];
    if (mini_ftl_read(&ftl, 100, read_data) == 0) {
        printf("Read sector 100: first byte=0x%02X (expected 0xAA)\n", read_data[0]);
    }
    if (mini_ftl_read(&ftl, 101, read_data) == 0) {
        printf("Read sector 101: first byte=0x%02X (expected 0xBB)\n", read_data[0]);
    }

    // 7. Simulate power failure recovery
    printf("\nSimulating Power Cycle...\n");
    eflash_deinit();
    eflash_init(flash_file);
    mini_ftl_init(&ftl);

    // 8. Read again after reboot
    printf("\n=== After Reboot ===\n");
    if (mini_ftl_obj_get_header(&ftl, 250, &read_hdr) == 0) {
        printf("After Reboot - Read ID 250: PkgID=0x%04X\n", read_hdr.pkg_id);
    } else {
        printf("After Reboot - Failed to read ID 250\n");
    }

    eflash_deinit();
    printf("\nDemo completed successfully!\n");
    return 0;
}
