/**
 * eFlash Library Example - Public API Demonstration
 * 
 * This example shows how to use eFlash library with ONLY the public API.
 * No internal headers needed! Suitable for embedded systems.
 * 
 * Features demonstrated:
 * - Basic initialization and cleanup
 * - Transaction management (begin/commit/abort)
 * - Data read/write operations
 * - Object header management
 * - Two commit methods (universal vs optimized)
 * - Power failure recovery simulation
 * - Garbage collection status
 */

#include "eflash.h"
#include <stdio.h>
#include <string.h>

int main() {
    // Get the global FTL instance (static allocation - no malloc needed)
    eflash_ftl_t *ftl = eflash_get_ftl();
    const char *flash_file = "test_flash.bin";
    
    printf("=== eFlash Library Public API Example ===\n\n");
    
    // ========================================================================
    // Step 1: Initialize simulated flash
    // ========================================================================
    printf("1. Initializing flash simulation...\n");
    if (eflash_init(flash_file) != 0) {
        printf("   ERROR: Failed to init flash\n");
        return -1;
    }
    
    // Erase all pages
    for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
        eflash_hw_erase(i);
    }
    printf("   Flash erased (%d pages)\n", EFLASH_TOTAL_PAGES);
    
    // ========================================================================
    // Step 2: Initialize FTL
    // ========================================================================
    printf("\n2. Initializing FTL...\n");
    if (eflash_ftl_init(ftl) != 0) {
        printf("   ERROR: FTL initialization failed\n");
        eflash_deinit();
        return -1;
    }
    printf("   FTL initialized successfully\n");
    
    // ========================================================================
    // Step 3: Write object headers
    // ========================================================================
    printf("\n3. Writing object headers...\n");
    
    // Write base area object header (ID: 0)
    obj_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.pkg_id = 0x1000;
    hdr.class_id = 0x2000;
    hdr.type = OBJ_TYPE_NORMAL;
    hdr.body_size = 128;
    
    if (eflash_ftl_obj_set_header(0, &hdr) == 0) {
        printf("   Set header for ID 0 (Base Level)\n");
    }
    
    // Write extended area object header (ID: 250 -> triggers first level extension)
    hdr.pkg_id = 0x9999;
    if (eflash_ftl_obj_set_header(250, &hdr) == 0) {
        printf("   Set header for ID 250 (Extended Level 1)\n");
    }
    
    // ========================================================================
    // Step 4: Demonstrate transaction commit methods
    // ========================================================================
    printf("\n4. Transaction Commit Methods Demo\n");
    
    // Method 1: Universal version (works on all hardware)
    printf("\n   --- Method 1: Full Page Rewrite (Universal) ---\n");
    uint8_t test_data[USER_DATA_SIZE];
    memset(test_data, 0xAA, USER_DATA_SIZE);
    
    eflash_ftl_txn_begin(ftl);
    if (eflash_ftl_write( 100, test_data) != 0) {
        printf("   ERROR: Write failed\n");
        eflash_ftl_txn_abort(ftl);
        eflash_deinit();
        return -1;
    }
    
    int ret = eflash_ftl_txn_commit(ftl);
    printf("   Commit result (universal): %s\n", ret == 0 ? "SUCCESS" : "FAILED");
    
    // Method 2: Optimized version (requires hardware support for word update)
    printf("\n   --- Method 2: Word Update (Optimized) ---\n");
    memset(test_data, 0xBB, USER_DATA_SIZE);
    
    eflash_ftl_txn_begin(ftl);
    if (eflash_ftl_write( 101, test_data) != 0) {
        printf("   ERROR: Write failed\n");
        eflash_ftl_txn_abort(ftl);
        eflash_deinit();
        return -1;
    }
    
    ret = eflash_ftl_txn_commit_with_update(ftl);
    if (ret != 0) {
        printf("   Note: Word update not supported, using universal commit instead\n");
        // Fallback to universal commit
        ret = eflash_ftl_txn_commit(ftl);
    }
    printf("   Commit result (optimized): %s\n", ret == 0 ? "SUCCESS" : "FAILED");
    printf("   Note: Word update avoids full page erase, extending Flash lifespan!\n");
    
    // ========================================================================
    // Step 5: Read verification
    // ========================================================================
    printf("\n5. Read Verification\n");
    
    // Verify object headers
    obj_header_t read_hdr;
    if (eflash_ftl_obj_get_header( 250, &read_hdr) == 0) {
        printf("   Read ID 250: PkgID=0x%04X, ClassID=0x%04X\n", 
               read_hdr.pkg_id, read_hdr.class_id);
    } else {
        printf("   ERROR: Failed to read ID 250\n");
    }
    
    // Verify transaction data
    uint8_t read_data[USER_DATA_SIZE];
    if (eflash_ftl_read( 100, read_data) == 0) {
        printf("   Read sector 100: first byte=0x%02X (expected 0xAA) %s\n", 
               read_data[0], (read_data[0] == 0xAA) ? "✓" : "✗");
    }
    if (eflash_ftl_read( 101, read_data) == 0) {
        printf("   Read sector 101: first byte=0x%02X (expected 0xBB) %s\n", 
               read_data[0], (read_data[0] == 0xBB) ? "✓" : "✗");
    }
    
    // ========================================================================
    // Step 6: Simulate power failure recovery
    // ========================================================================
    printf("\n6. Simulating Power Cycle...\n");
    eflash_deinit();
    
    // Re-initialize (simulates reboot after power loss)
    eflash_init(flash_file);
    if (eflash_ftl_init(ftl) != 0) {
        printf("   ERROR: FTL re-initialization failed\n");
        return -1;
    }
    printf("   FTL recovered after power cycle\n");
    
    // Read again after reboot
    printf("\n7. After Reboot Verification\n");
    if (eflash_ftl_obj_get_header( 250, &read_hdr) == 0) {
        printf("   After Reboot - Read ID 250: PkgID=0x%04X ✓\n", read_hdr.pkg_id);
    } else {
        printf("   After Reboot - Failed to read ID 250 ✗\n");
    }
    
    if (eflash_ftl_read( 100, read_data) == 0) {
        printf("   After Reboot - Read sector 100: 0x%02X %s\n", 
               read_data[0], (read_data[0] == 0xAA) ? "✓" : "✗");
    }
    
    // ========================================================================
    // Step 8: Check GC status
    // ========================================================================
    printf("\n8. Garbage Collection Status\n");
    uint32_t free_pages = eflash_ftl_get_free_pages(ftl);
    printf("   Free pages: %lu\n", (unsigned long)free_pages);
    
    // ========================================================================
    // Step 9: Cleanup
    // ========================================================================
    printf("\n9. Cleaning up...\n");
    eflash_deinit();
    // No need to destroy FTL - uses static allocation
    
    printf("\n=== Example completed successfully! ===\n");
    return 0;
}
