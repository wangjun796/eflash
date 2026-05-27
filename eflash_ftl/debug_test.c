#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "eflash_ftl.h"
#include "eflash_mgr.h"
#include "eflash_sim.h"
#include "eflash.h"

#define TEST_FLASH_FILE "test_flash_debug.bin"

static void setup(void) {
    remove(TEST_FLASH_FILE);
    int ret = eflash_init(TEST_FLASH_FILE);
    if (ret != 0) {
        printf("[ERROR] Flash initialization failed with code %d\n", ret);
        exit(EXIT_FAILURE);
    }
    ret = eflash_ftl_init();
    if (ret != 0) {
        printf("[ERROR] FTL initialization failed with code %d\n", ret);
        exit(EXIT_FAILURE);
    }
}

static void teardown(void) {
    eflash_deinit();
    for (int i = 0; i < 10; i++) {
        if (remove(TEST_FLASH_FILE) == 0) break;
#ifdef _WIN32
        Sleep(10);
#endif
    }
}

int main(void) {
    setup();
    
    printf("=== Debug Test: Add, Delete, Check Size ===\n\n");
    
    // Add Segment 0: 2 pages at LPN 100
    printf("Phase 1: Add Segment 0 (2 pages at LPN 100)\n");
    uint16_t seg0_lpn = 100;
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xA0 + i, USER_DATA_SIZE);
        int ret = eflash_ftl_write(seg0_lpn + i, test_data);
        assert(ret == 0);
    }
    int ret = eflash_ftl_code_migrate_from_logical(seg0_lpn, 2);
    assert(ret == 0);
    printf("  After adding Segment 0:\n");
    printf("    migration_records_count = %d\n", g_code_region.migration_records_count);
    printf("    code_size_bytes = %d\n", g_code_region.code_size_bytes);
    printf("    num_pages = %d\n", g_code_region.num_pages);
    printf("    get_code_region_size() = %d\n", eflash_ftl_get_code_region_size());
    
    // Add Segment 1: 3 pages at LPN 200
    printf("\nPhase 2: Add Segment 1 (3 pages at LPN 200)\n");
    uint16_t seg1_lpn = 200;
    for (uint16_t i = 0; i < 3; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xB0 + i, USER_DATA_SIZE);
        int ret = eflash_ftl_write(seg1_lpn + i, test_data);
        assert(ret == 0);
    }
    ret = eflash_ftl_code_migrate_from_logical(seg1_lpn, 3);
    assert(ret == 0);
    printf("  After adding Segment 1:\n");
    printf("    migration_records_count = %d\n", g_code_region.migration_records_count);
    printf("    code_size_bytes = %d\n", g_code_region.code_size_bytes);
    printf("    num_pages = %d\n", g_code_region.num_pages);
    printf("    get_code_region_size() = %d\n", eflash_ftl_get_code_region_size());
    
    // Add Segment 2: 1 page at LPN 300
    printf("\nPhase 3: Add Segment 2 (1 page at LPN 300)\n");
    uint16_t seg2_lpn = 300;
    uint8_t test_data[USER_DATA_SIZE];
    memset(test_data, 0xC0, USER_DATA_SIZE);
    ret = eflash_ftl_write(seg2_lpn, test_data);
    assert(ret == 0);
    ret = eflash_ftl_code_migrate_from_logical(seg2_lpn, 1);
    assert(ret == 0);
    printf("  After adding Segment 2:\n");
    printf("    migration_records_count = %d\n", g_code_region.migration_records_count);
    printf("    code_size_bytes = %d\n", g_code_region.code_size_bytes);
    printf("    num_pages = %d\n", g_code_region.num_pages);
    printf("    get_code_region_size() = %d\n", eflash_ftl_get_code_region_size());
    
    // Print migration_map
    printf("\nMigration Map:\n");
    for (int i = 0; i < g_code_region.migration_records_count; i++) {
        printf("  [%d] logical=0x%06X, physical=0x%06X, size=%d\n",
               i, g_code_region.migration_map[i].logical_addr,
               g_code_region.migration_map[i].physical_addr,
               g_code_region.migration_map[i].size);
    }
    
    // Delete Segment 1 (middle segment)
    printf("\nPhase 4: Delete Segment 1 (middle segment)\n");
    uint32_t seg1_logical_addr = (uint32_t)seg1_lpn * USER_DATA_SIZE;
    printf("  Deleting logical address 0x%06X...\n", seg1_logical_addr);
    
    // Read Segment 2 data before delete
    printf("  Reading Segment 2 data BEFORE delete (at page offset 5)...\n");
    uint8_t seg2_before[USER_DATA_SIZE];
    ret = eflash_ftl_code_read(5, seg2_before, USER_DATA_SIZE);
    assert(ret == 0);
    printf("    First 16 bytes: ");
    for (int i = 0; i < 16; i++) printf("%02X ", seg2_before[i]);
    printf("\n");
    
    ret = eflash_ftl_code_region_delete_segment(seg1_logical_addr);
    printf("  delete_segment returned: %d\n", ret);
    
    printf("  After deleting Segment 1:\n");
    printf("    migration_records_count = %d\n", g_code_region.migration_records_count);
    printf("    code_size_bytes = %d\n", g_code_region.code_size_bytes);
    printf("    num_pages = %d\n", g_code_region.num_pages);
    printf("    get_code_region_size() = %d\n", eflash_ftl_get_code_region_size());
    
    // Print migration_map after delete
    printf("\nMigration Map after delete:\n");
    for (int i = 0; i < g_code_region.migration_records_count; i++) {
        printf("  [%d] logical=0x%06X, physical=0x%06X, size=%d\n",
               i, g_code_region.migration_map[i].logical_addr,
               g_code_region.migration_map[i].physical_addr,
               g_code_region.migration_map[i].size);
    }
    
    // Read Segment 2 data after delete (should now be at page offset 2)
    printf("\n  Reading Segment 2 data AFTER delete (at page offset 2)...\n");
    uint8_t seg2_after[USER_DATA_SIZE];
    ret = eflash_ftl_code_read(2, seg2_after, USER_DATA_SIZE);
    assert(ret == 0);
    printf("    First 16 bytes: ");
    for (int i = 0; i < 16; i++) printf("%02X ", seg2_after[i]);
    printf("\n");
    
    // Compare
    printf("\n  Comparing Segment 2 data before and after delete...\n");
    if (memcmp(seg2_before, seg2_after, USER_DATA_SIZE) == 0) {
        printf("    [PASS] Data matches!\n");
    } else {
        printf("    [FAIL] Data mismatch!\n");
        printf("    Expected (before): ");
        for (int i = 0; i < 16; i++) printf("%02X ", seg2_before[i]);
        printf("\n");
        printf("    Actual (after):    ");
        for (int i = 0; i < 16; i++) printf("%02X ", seg2_after[i]);
        printf("\n");
    }
    
    // Expected values
    printf("\nExpected values:\n");
    printf("    migration_records_count = 2\n");
    printf("    code_size_bytes = %d (6 - 3 = 3 pages * 464 = 1392 bytes)\n", 3 * USER_DATA_SIZE);
    printf("    num_pages = 3\n");
    printf("    get_code_region_size() = 3\n");
    
    // Verify
    printf("\nVerification:\n");
    if (g_code_region.migration_records_count == 2) {
        printf("  [PASS] migration_records_count == 2\n");
    } else {
        printf("  [FAIL] migration_records_count == %d (expected 2)\n", g_code_region.migration_records_count);
    }
    
    uint16_t total_size = eflash_ftl_get_code_region_size();
    if (total_size == 3) {
        printf("  [PASS] total_size == 3\n");
    } else {
        printf("  [FAIL] total_size == %d (expected 3)\n", total_size);
    }
    
    teardown();
    
    printf("\n=== Debug Test Complete ===\n");
    return 0;
}
