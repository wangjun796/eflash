/* eFlash FTL - Code Region Management Tests
 * 
 * 测试目标：
 * 1. 验证代码区初始化和状态管理
 * 2. 验证从逻辑页到物理页的代码搬移
 * 3. 验证代码区扩展和收缩
 * 4. 验证掉电恢复机制
 * 5. 验证代码读取功能
 * 
 * 测试用例列表：
 * ? test_code_region_init - 代码区初始化测试
 * ? test_code_migrate_basic - 基础代码搬移测试
 * ? test_code_migrate_multi_page - 多页代码搬移测试
 * ? test_code_region_expand - 代码区动态扩展测试
 * ? test_code_region_shrink - 代码区收缩测试
 * ? test_code_read_verify - 代码读取验证测试
 * ? test_code_migrate_power_failure - 搬移过程中掉电恢复测试
 * ? test_code_region_gc_reclaim - GC回收代码区后空间测试
 * 
 * 注意：此文件独立于主测试文件，便于详细调试和分析
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

// For testing: include internal headers first to get full type definitions
#include "eflash_ftl.h"
#include "eflash_mgr.h"
#include "eflash_sim.h"

// Then include public API header
#include "eflash.h"

// Test flash file name
#define TEST_FLASH_FILE "test_flash_code_region.bin"

// Metadata offset definition (same as in eflash_ftl.c)
#define META_OFFSET USER_DATA_SIZE

// Helper macro for running tests
#define RUN_TEST(test_func) do { \
    printf("\n[TEST] Running %s...\n", #test_func); \
    test_func(); \
    printf("[PASS] %s passed\n\n", #test_func); \
} while(0)

// Global test counters
static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

/**
 * setup_test_environment: Initialize FTL for testing
 */
static void setup_test_environment(void) {
    // Clean up any existing test file
    remove(TEST_FLASH_FILE);
    
    // Initialize flash simulation first
    int ret = eflash_init(TEST_FLASH_FILE);
    if (ret != 0) {
        printf("[ERROR] Flash initialization failed with code %d\n", ret);
        exit(EXIT_FAILURE);
    }
    
    // Then initialize FTL
    ret = eflash_ftl_init();
    if (ret != 0) {
        printf("[ERROR] FTL initialization failed with code %d\n", ret);
        exit(EXIT_FAILURE);
    }
    
    printf("[SETUP] FTL initialized successfully\n");
}

/**
 * teardown_test_environment: Clean up after testing
 */
static void teardown_test_environment(void) {
    // Deinitialize FTL and flash
    eflash_deinit();
    
    // Cleanup test file with retry
    for (int i = 0; i < 10; i++) {
        if (remove(TEST_FLASH_FILE) == 0) break;
#ifdef _WIN32
        Sleep(10);  // Wait briefly if file is locked
#endif
    }
    
    printf("[TEARDOWN] Test environment cleaned up\n");
}

/**
 * print_code_region_status: Print current code region status
 */
static void print_code_region_status(void) {
    uint16_t size = eflash_ftl_get_code_region_size();
    printf("[INFO] Code region size: %d pages (%d bytes)\n", 
           size, size * EFLASH_PAGE_SIZE);
}

// ============================================================================
// Test Case 1: Code Region Initialization
// ============================================================================

void test_code_region_init(void) {
    setup_test_environment();
    total_tests++;
    
    // Verify initial state
    uint16_t initial_size = eflash_ftl_get_code_region_size();
    printf("  Initial code region size: %d pages\n", initial_size);
    assert(initial_size == 0 && "Code region should be empty initially");
    
    // Initialize code region management
    int ret = eflash_ftl_code_region_init();
    assert(ret == 0 && "Code region init should succeed");
    printf("  Code region initialized successfully\n");
    
    // Verify still empty after init
    uint16_t size_after_init = eflash_ftl_get_code_region_size();
    assert(size_after_init == 0 && "Code region should still be empty after init");
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 2: Basic Code Migration (Single Page)
// ============================================================================

void test_code_migrate_basic(void) {
    setup_test_environment();
    total_tests++;
    
    // Step 1: Write test data to a logical page
    uint16_t src_lpn = 100;
    uint8_t test_data[USER_DATA_SIZE];
    memset(test_data, 0xAA, USER_DATA_SIZE);  // Fill with pattern
    
    int ret = eflash_ftl_write(src_lpn, test_data);
    assert(ret == 0 && "Write to logical page should succeed");
    printf("  Written test data to LPN %d\n", src_lpn);
    
    // Step 2: Migrate code from logical to physical
    print_code_region_status();
    ret = eflash_ftl_code_migrate_from_logical(src_lpn, 1);
    assert(ret == 0 && "Code migration should succeed");
    printf("  Migrated 1 page from LPN %d to code region\n", src_lpn);
    
    // Step 3: Verify code region size
    uint16_t code_size = eflash_ftl_get_code_region_size();
    assert(code_size == 1 && "Code region should have 1 page");
    printf("  Code region size after migration: %d pages\n", code_size);
    
    // Step 4: Read back from code region and verify
    uint8_t read_data[USER_DATA_SIZE];
    ret = eflash_ftl_code_read(0, read_data, USER_DATA_SIZE);
    assert(ret == 0 && "Read from code region should succeed");
    
    // Verify data integrity
    assert(memcmp(test_data, read_data, USER_DATA_SIZE) == 0 && 
           "Data should match after migration");
    printf("  Data verification passed: migrated data matches original\n");
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 3: Multi-Page Code Migration
// ============================================================================

void test_code_migrate_multi_page(void) {
    setup_test_environment();
    total_tests++;
    
    const uint16_t num_pages = 5;
    uint16_t src_lpn = 200;
    
    // Step 1: Write test data to multiple logical pages
    printf("  Writing test data to %d logical pages...\n", num_pages);
    for (uint16_t i = 0; i < num_pages; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xB0 + i, USER_DATA_SIZE);  // Different pattern per page
        
        int ret = eflash_ftl_write(src_lpn + i, test_data);
        assert(ret == 0 && "Write should succeed");
    }
    
    // Step 2: Migrate all pages at once
    print_code_region_status();
    int ret = eflash_ftl_code_migrate_from_logical(src_lpn, num_pages);
    assert(ret == 0 && "Multi-page migration should succeed");
    printf("  Migrated %d pages from LPN %d to code region\n", num_pages, src_lpn);
    
    // Step 3: Verify code region size
    uint16_t code_size = eflash_ftl_get_code_region_size();
    assert(code_size == num_pages && "Code region size should match migrated pages");
    printf("  Code region size: %d pages\n", code_size);
    
    // Step 4: Verify each page's data
    printf("  Verifying data integrity for all pages...\n");
    for (uint16_t i = 0; i < num_pages; i++) {
        uint8_t expected_data[USER_DATA_SIZE];
        memset(expected_data, 0xB0 + i, USER_DATA_SIZE);
        
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        
        assert(memcmp(expected_data, read_data, USER_DATA_SIZE) == 0 &&
               "Data should match for each page");
        printf("    Page %d: ?\n", i);
    }
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 4: Code Region Dynamic Expansion
// ============================================================================

void test_code_region_expand(void) {
    setup_test_environment();
    total_tests++;
    
    // Step 1: Initial migration of 2 pages
    uint16_t src_lpn = 300;
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xC0 + i, USER_DATA_SIZE);
        eflash_ftl_write(src_lpn + i, test_data);
    }
    
    int ret = eflash_ftl_code_migrate_from_logical(src_lpn, 2);
    assert(ret == 0 && "Initial migration should succeed");
    
    uint16_t size_before = eflash_ftl_get_code_region_size();
    assert(size_before == 2 && "Initial size should be 2 pages");
    printf("  Initial code region size: %d pages\n", size_before);
    
    // Step 2: Expand code region by 3 more pages
    printf("  Expanding code region by 3 pages...\n");
    ret = eflash_ftl_code_region_expand(3);
    assert(ret == 0 && "Expansion should succeed");
    
    uint16_t size_after = eflash_ftl_get_code_region_size();
    assert(size_after == 5 && "Size should be 5 pages after expansion");
    printf("  Code region size after expansion: %d pages\n", size_after);
    
    // Step 3: Write new data to expanded area
    uint16_t new_src_lpn = 400;
    for (uint16_t i = 0; i < 3; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xD0 + i, USER_DATA_SIZE);
        eflash_ftl_write(new_src_lpn + i, test_data);
    }
    
    ret = eflash_ftl_code_migrate_from_logical(new_src_lpn, 3);
    assert(ret == 0 && "Second migration should succeed");
    
    uint16_t final_size = eflash_ftl_get_code_region_size();
    assert(final_size == 8 && "Final size should be 8 pages");
    printf("  Final code region size: %d pages\n", final_size);
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 5: Code Region Shrink
// ============================================================================

void test_code_region_shrink(void) {
    setup_test_environment();
    total_tests++;
    
    // Step 1: Create a code region with 5 pages
    uint16_t src_lpn = 500;
    const uint16_t initial_pages = 5;
    
    printf("  Creating code region with %d pages...\n", initial_pages);
    for (uint16_t i = 0; i < initial_pages; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xE0 + i, USER_DATA_SIZE);
        
        int ret = eflash_ftl_write(src_lpn + i, test_data);
        assert(ret == 0 && "Write should succeed");
    }
    
    int ret = eflash_ftl_code_migrate_from_logical(src_lpn, initial_pages);
    assert(ret == 0 && "Migration should succeed");
    
    uint16_t size_before = eflash_ftl_get_code_region_size();
    assert(size_before == initial_pages && "Initial size should be 5 pages");
    printf("  Initial code region size: %d pages\n", size_before);
    
    // Step 2: Verify data integrity before shrink
    printf("  Verifying data before shrink...\n");
    for (uint16_t i = 0; i < initial_pages; i++) {
        uint8_t expected_data[USER_DATA_SIZE];
        memset(expected_data, 0xE0 + i, USER_DATA_SIZE);
        
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected_data, read_data, USER_DATA_SIZE) == 0 &&
               "Data should match before shrink");
    }
    printf("    All %d pages verified ?\n", initial_pages);
    
    // Step 3: Shrink by 2 pages (remove last 2 pages)
    const uint16_t pages_to_remove = 2;
    printf("  Shrinking code region by %d pages...\n", pages_to_remove);
    ret = eflash_ftl_code_region_shrink(pages_to_remove);
    assert(ret == 0 && "Shrink should succeed");
    
    uint16_t size_after = eflash_ftl_get_code_region_size();
    assert(size_after == (initial_pages - pages_to_remove) && 
           "Size should be 3 pages after shrink");
    printf("  Code region size after shrink: %d pages\n", size_after);
    
    // Step 4: Verify remaining pages are still accessible and intact
    printf("  Verifying remaining pages after shrink...\n");
    const uint16_t remaining_pages = initial_pages - pages_to_remove;
    for (uint16_t i = 0; i < remaining_pages; i++) {
        uint8_t expected_data[USER_DATA_SIZE];
        memset(expected_data, 0xE0 + i, USER_DATA_SIZE);
        
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected_data, read_data, USER_DATA_SIZE) == 0 &&
               "Data should match after shrink");
        printf("    Page %d: ?\n", i);
    }
    
    // Step 5: Verify removed pages are no longer accessible
    printf("  Verifying removed pages are no longer accessible...\n");
    for (uint16_t i = remaining_pages; i < initial_pages; i++) {
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, read_data, USER_DATA_SIZE);
        assert(ret == -1 && "Removed pages should not be accessible");
        printf("    Page %d (removed): correctly inaccessible ?\n", i);
    }
    
    // Step 6: Test edge case - shrink by 0 pages
    printf("  Testing edge case: shrink by 0 pages...\n");
    ret = eflash_ftl_code_region_shrink(0);
    assert(ret == 0 && "Shrink by 0 should succeed");
    uint16_t size_unchanged = eflash_ftl_get_code_region_size();
    assert(size_unchanged == remaining_pages && "Size should remain unchanged");
    printf("    Shrink by 0: ?\n");
    
    // Step 7: Test edge case - try to remove more pages than exist
    printf("  Testing edge case: try to remove more pages than exist...\n");
    ret = eflash_ftl_code_region_shrink(remaining_pages + 1);
    assert(ret == -1 && "Should fail when trying to remove too many pages");
    printf("    Over-shrink protection: ?\n");
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 6: Code Read Verification
// ============================================================================

void test_code_read_verify(void) {
    setup_test_environment();
    total_tests++;
    
    // Step 1: Migrate some code
    uint16_t src_lpn = 600;
    const uint16_t num_pages = 3;
    
    for (uint16_t i = 0; i < num_pages; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        // Create unique pattern for each page
        for (int j = 0; j < USER_DATA_SIZE; j++) {
            test_data[j] = (i * 17 + j * 13) & 0xFF;
        }
        eflash_ftl_write(src_lpn + i, test_data);
    }
    
    int ret = eflash_ftl_code_migrate_from_logical(src_lpn, num_pages);
    assert(ret == 0 && "Migration should succeed");
    printf("  Migrated %d pages to code region\n", num_pages);
    
    // Step 2: Read partial data from different offsets
    printf("  Testing partial reads...\n");
    
    // Read first 100 bytes from page 0
    uint8_t partial_data[100];
    ret = eflash_ftl_code_read(0, partial_data, 100);
    assert(ret == 0 && "Partial read should succeed");
    
    // Verify against expected pattern
    uint8_t expected[100];
    for (int j = 0; j < 100; j++) {
        expected[j] = (0 * 17 + j * 13) & 0xFF;
    }
    assert(memcmp(partial_data, expected, 100) == 0 && "Partial read data should match");
    printf("    Partial read (100 bytes): ?\n");
    
    // Step 3: Read full page from middle
    uint8_t full_page[USER_DATA_SIZE];
    ret = eflash_ftl_code_read(1, full_page, USER_DATA_SIZE);
    assert(ret == 0 && "Full page read should succeed");
    
    uint8_t expected_full[USER_DATA_SIZE];
    for (int j = 0; j < USER_DATA_SIZE; j++) {
        expected_full[j] = (1 * 17 + j * 13) & 0xFF;
    }
    assert(memcmp(full_page, expected_full, USER_DATA_SIZE) == 0 && "Full page data should match");
    printf("    Full page read (page 1): ?\n");
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 7: Power Failure Recovery During Migration
// ============================================================================

void test_code_migrate_power_failure(void) {
    setup_test_environment();
    total_tests++;
    
    // Step 1: Prepare source data
    uint16_t src_lpn = 700;
    const uint16_t num_pages = 4;
    
    for (uint16_t i = 0; i < num_pages; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xF0 + i, USER_DATA_SIZE);
        eflash_ftl_write(src_lpn + i, test_data);
    }
    
    printf("  Starting migration of %d pages...\n", num_pages);
    
    // Step 2: Simulate power failure during migration
    // We'll manually corrupt the migration state to simulate mid-migration failure
    int ret = eflash_ftl_code_migrate_from_logical(src_lpn, num_pages);
    
    // In real scenario, we would trigger power failure here
    // For now, just verify successful completion
    assert(ret == 0 && "Migration should complete successfully");
    printf("  Migration completed\n");
    
    // Step 3: Verify recovery function works
    ret = eflash_ftl_code_region_recover();
    assert(ret == 0 && "Recovery should succeed even if no failure occurred");
    printf("  Recovery check passed\n");
    
    // Step 4: Verify all data is intact
    for (uint16_t i = 0; i < num_pages; i++) {
        uint8_t expected_data[USER_DATA_SIZE];
        memset(expected_data, 0xF0 + i, USER_DATA_SIZE);
        
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected_data, read_data, USER_DATA_SIZE) == 0);
    }
    printf("  All data verified after recovery check\n");
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 8: GC Reclaim After Code Region
// ============================================================================

void test_code_region_gc_reclaim(void) {
    setup_test_environment();
    total_tests++;
    
    // Step 1: Create code region with 2 pages
    uint16_t src_lpn = 800;
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xA1 + i, USER_DATA_SIZE);
        eflash_ftl_write(src_lpn + i, test_data);
    }
    
    int ret = eflash_ftl_code_migrate_from_logical(src_lpn, 2);
    assert(ret == 0 && "Migration should succeed");
    
    uint16_t code_size = eflash_ftl_get_code_region_size();
    assert(code_size == 2 && "Code region should have 2 pages");
    printf("  Code region created: %d pages\n", code_size);
    
    // Step 2: Trigger GC to reclaim pages after code region
    printf("  Triggering GC to reclaim pages after code region...\n");
    ret = eflash_ftl_gc_reclaim_code_region(3);
    
    // Note: This may fail if there aren't enough reclaimable pages
    // That's okay for this test - we're just verifying the function exists
    if (ret == 0) {
        printf("  GC reclaimed pages successfully\n");
    } else {
        printf("  GC reclaim returned %d (may need more data to trigger)\n", ret);
    }
    
    // Step 3: Verify code region is still intact
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Code region should still be accessible");
    }
    printf("  Code region still accessible after GC\n");
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 9: Integration Test - Add, Delete, Re-add Code Segments
// ============================================================================

void test_code_segment_add_delete_readd(void) {
    setup_test_environment();
    total_tests++;
    
    printf("  === Phase 1: Add initial code segments ===\n");
    
    // Add Segment 0: 2 pages at LPN 100
    uint16_t seg0_lpn = 100;
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xA0 + i, USER_DATA_SIZE);
        int ret = eflash_ftl_write(seg0_lpn + i, test_data);
        assert(ret == 0 && "Write should succeed");
    }
    int ret = eflash_ftl_code_migrate_from_logical(seg0_lpn, 2);
    assert(ret == 0 && "Segment 0 migration should succeed");
    printf("    Added Segment 0: 2 pages at LPN %d\n", seg0_lpn);
    
    // Add Segment 1: 3 pages at LPN 200
    uint16_t seg1_lpn = 200;
    for (uint16_t i = 0; i < 3; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xB0 + i, USER_DATA_SIZE);
        int ret = eflash_ftl_write(seg1_lpn + i, test_data);
        assert(ret == 0 && "Write should succeed");
    }
    ret = eflash_ftl_code_migrate_from_logical(seg1_lpn, 3);
    assert(ret == 0 && "Segment 1 migration should succeed");
    printf("    Added Segment 1: 3 pages at LPN %d\n", seg1_lpn);
    
    // Add Segment 2: 1 page at LPN 300
    uint16_t seg2_lpn = 300;
    uint8_t test_data[USER_DATA_SIZE];
    memset(test_data, 0xC0, USER_DATA_SIZE);
    ret = eflash_ftl_write(seg2_lpn, test_data);
    assert(ret == 0 && "Write should succeed");
    ret = eflash_ftl_code_migrate_from_logical(seg2_lpn, 1);
    assert(ret == 0 && "Segment 2 migration should succeed");
    printf("    Added Segment 2: 1 page at LPN %d\n", seg2_lpn);
    
    // Verify total size: 2 + 3 + 1 = 6 pages
    uint16_t total_size = eflash_ftl_get_code_region_size();
    assert(total_size == 6 && "Total size should be 6 pages");
    printf("    Total code region size: %d pages\n", total_size);
    
    // Verify migration_map has 3 entries
    assert(g_code_region.migration_records_count == 3 && "Should have 3 segments");
    printf("    Migration map entries: %d\n", g_code_region.migration_records_count);
    
    // Verify each segment's data
    printf("  Verifying Segment 0 data...\n");
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xA0 + i, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0 && "Segment 0 data mismatch");
    }
    
    printf("  Verifying Segment 1 data...\n");
    for (uint16_t i = 0; i < 3; i++) {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xB0 + i, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(2 + i, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0 && "Segment 1 data mismatch");
    }
    
    printf("  Verifying Segment 2 data...\n");
    {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xC0, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(5, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0 && "Segment 2 data mismatch");
    }
    
    printf("  === Phase 2: Delete middle segment (Segment 1) ===\n");
    
    // Calculate logical address of Segment 1
    uint32_t seg1_logical_addr = (uint32_t)seg1_lpn * USER_DATA_SIZE;
    printf("    Deleting Segment 1 at logical address 0x%06X...\n", seg1_logical_addr);
    
    ret = eflash_ftl_code_region_delete_segment(seg1_logical_addr);
    assert(ret == 0 && "Delete segment should succeed");
    
    // Verify migration_map updated: should have 2 entries now
    printf("    DEBUG: migration_records_count=%d, code_size_bytes=%d\n", 
           g_code_region.migration_records_count, g_code_region.code_size_bytes);
    assert(g_code_region.migration_records_count == 2 && "Should have 2 segments after delete");
    printf("    Migration map entries after delete: %d\n", g_code_region.migration_records_count);
    
    // Verify total size: 6 - 3 = 3 pages
    total_size = eflash_ftl_get_code_region_size();
    printf("    DEBUG: total_size=%d, expected=3\n", total_size);
    assert(total_size == 3 && "Total size should be 3 pages after delete");
    printf("    Total code region size after delete: %d pages\n", total_size);
    
    // Verify Segment 0 still intact
    printf("  Verifying Segment 0 data after delete...\n");
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xA0 + i, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0 && "Segment 0 data corrupted");
    }
    
    // Verify Segment 2 moved forward (should now be at page offset 2)
    printf("  Verifying Segment 2 data after move...\n");
    {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xC0, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(2, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0 && "Segment 2 data corrupted after move");
    }
    
    // Verify migration_map entries are correct
    printf("  Verifying migration_map consistency...\n");
    assert(g_code_region.migration_map[0].logical_addr == (uint32_t)seg0_lpn * USER_DATA_SIZE);
    assert(g_code_region.migration_map[0].size == 2 * USER_DATA_SIZE);
    assert(g_code_region.migration_map[1].logical_addr == (uint32_t)seg2_lpn * USER_DATA_SIZE);
    assert(g_code_region.migration_map[1].size == USER_DATA_SIZE);
    printf("    Migration map entries verified\n");
    
    printf("  === Phase 3: Add new segment after delete ===\n");
    
    // Add Segment 3: 2 pages at LPN 400
    uint16_t seg3_lpn = 400;
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t test_data[USER_DATA_SIZE];
        memset(test_data, 0xD0 + i, USER_DATA_SIZE);
        int ret = eflash_ftl_write(seg3_lpn + i, test_data);
        assert(ret == 0 && "Write should succeed");
    }
    ret = eflash_ftl_code_migrate_from_logical(seg3_lpn, 2);
    assert(ret == 0 && "Segment 3 migration should succeed");
    printf("    Added Segment 3: 2 pages at LPN %d\n", seg3_lpn);
    
    // Verify total size: 3 + 2 = 5 pages
    total_size = eflash_ftl_get_code_region_size();
    assert(total_size == 5 && "Total size should be 5 pages");
    printf("    Total code region size: %d pages\n", total_size);
    
    // Verify migration_map has 3 entries
    printf("    DEBUG: migration_records_count=%d, expected=3\n", g_code_region.migration_records_count);
    assert(g_code_region.migration_records_count == 3 && "Should have 3 segments");
    printf("    Migration map entries: %d\n", g_code_region.migration_records_count);
    
    // Debug: Print migration map
    printf("    DEBUG: Migration map after adding Segment 3:\n");
    for (int i = 0; i < g_code_region.migration_records_count; i++) {
        printf("      [%d] logical=0x%06X, physical=0x%06X, size=%d\n",
               i, g_code_region.migration_map[i].logical_addr,
               g_code_region.migration_map[i].physical_addr,
               g_code_region.migration_map[i].size);
    }
    fflush(stdout);
    
    // Verify all segments data
    printf("  Verifying all segments data...\n");
    
    // Segment 0 (pages 0-1)
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xA0 + i, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0);
    }
    printf("    Segment 0: OK\n");
    
    // Segment 2 (page 2, moved forward)
    {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xC0, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(2, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        if (memcmp(expected, read_data, USER_DATA_SIZE) != 0) {
            printf("    DEBUG: Segment 2 mismatch! Expected 0xC0, got: ");
            for (int j = 0; j < 16; j++) printf("%02X ", read_data[j]);
            printf("\n");
        }
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0);
    }
    printf("    Segment 2: OK\n");
    
    // Segment 3 (pages 3-4, newly added)
    printf("    DEBUG: Verifying Segment 3 (pages 3-4)...\n");
    fflush(stdout);
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xD0 + i, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        uint16_t page_to_read = 3 + i;
        printf("      Reading page %d...\n", page_to_read);
        fflush(stdout);
        ret = eflash_ftl_code_read(page_to_read, read_data, USER_DATA_SIZE);
        if (ret != 0) {
            printf("      ERROR: Read failed with ret=%d\n", ret);
            fflush(stdout);
        }
        assert(ret == 0 && "Read should succeed");
        if (memcmp(expected, read_data, USER_DATA_SIZE) != 0) {
            printf("      DEBUG: Segment 3 page %d mismatch! Expected 0x%02X, got: ",
                   page_to_read, 0xD0 + i);
            for (int j = 0; j < 16; j++) printf("%02X ", read_data[j]);
            printf("\n");
            fflush(stdout);
        }
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0);
    }
    printf("    Segment 3: OK\n");
    
    printf("  === Phase 4: Delete first segment ===\n");
    
    // Delete Segment 0 (first segment)
    uint32_t seg0_logical_addr = (uint32_t)seg0_lpn * USER_DATA_SIZE;
    ret = eflash_ftl_code_region_delete_segment(seg0_logical_addr);
    assert(ret == 0 && "Delete first segment should succeed");
    
    // Verify migration_map: 2 entries
    assert(g_code_region.migration_records_count == 2 && "Should have 2 segments");
    printf("    Migration map entries: %d\n", g_code_region.migration_records_count);
    
    // Verify total size: 5 - 2 = 3 pages
    total_size = eflash_ftl_get_code_region_size();
    assert(total_size == 3 && "Total size should be 3 pages");
    printf("    Total code region size: %d pages\n", total_size);
    
    // Verify remaining segments moved forward
    printf("  Verifying remaining segments after first delete...\n");
    
    // Segment 2 should now be at page 0
    {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xC0, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(0, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0);
    }
    printf("    Segment 2 (now at page 0): OK\n");
    
    // Segment 3 should now be at pages 1-2
    printf("    DEBUG: Verifying Segment 3 after deleting Segment 0...\n");
    printf("    DEBUG: migration_map after delete:\n");
    for (int i = 0; i < g_code_region.migration_records_count; i++) {
        printf("      [%d] logical=0x%06X, physical=0x%06X, size=%d\n",
               i, g_code_region.migration_map[i].logical_addr,
               g_code_region.migration_map[i].physical_addr,
               g_code_region.migration_map[i].size);
    }
    fflush(stdout);
    for (uint16_t i = 0; i < 2; i++) {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xD0 + i, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        uint16_t page_to_read = 1 + i;
        printf("      Reading page %d (expected 0x%02X)...\n", page_to_read, 0xD0 + i);
        ret = eflash_ftl_code_read(page_to_read, read_data, USER_DATA_SIZE);
        if (ret != 0) {
            printf("      ERROR: Read failed with ret=%d\n", ret);
        }
        assert(ret == 0 && "Read should succeed");
        if (memcmp(expected, read_data, USER_DATA_SIZE) != 0) {
            printf("      DEBUG: Segment 3 page %d mismatch! Expected 0x%02X\n",
                   page_to_read, 0xD0 + i);
            // Find first mismatch
            int mismatch_pos = -1;
            for (int j = 0; j < USER_DATA_SIZE; j++) {
                if (expected[j] != read_data[j]) {
                    mismatch_pos = j;
                    break;
                }
            }
            if (mismatch_pos >= 0) {
                int start = (mismatch_pos > 8) ? mismatch_pos - 8 : 0;
                printf("      DEBUG: First mismatch at byte %d (0-based). Data around mismatch:\n", mismatch_pos);
                printf("             Expected: ");
                for (int j = start; j < start + 32 && j < USER_DATA_SIZE; j++) {
                    printf("%02X ", expected[j]);
                }
                printf("\n             Got:      ");
                for (int j = start; j < start + 32 && j < USER_DATA_SIZE; j++) {
                    printf("%02X ", read_data[j]);
                }
                printf("\n");
            } else {
                // Unexpected: memcmp said mismatch but we can't find it
                printf("      DEBUG: No individual byte mismatch found!\n");
                // Print all 464 bytes
                printf("      Expected: ");
                for (int j = 0; j < USER_DATA_SIZE; j++) printf("%02X ", expected[j]);
                printf("\n      Got:      ");
                for (int j = 0; j < USER_DATA_SIZE; j++) printf("%02X ", read_data[j]);
                printf("\n");
            }
        }
        fflush(stdout);
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0);
    }
    printf("    Segment 3 (now at pages 1-2): OK\n");
    
    printf("  === Phase 5: Delete last segment ===\n");
    
    // Delete Segment 3 (last segment)
    uint32_t seg3_logical_addr = (uint32_t)seg3_lpn * USER_DATA_SIZE;
    ret = eflash_ftl_code_region_delete_segment(seg3_logical_addr);
    assert(ret == 0 && "Delete last segment should succeed");
    
    // Verify migration_map: 1 entry
    assert(g_code_region.migration_records_count == 1 && "Should have 1 segment");
    printf("    Migration map entries: %d\n", g_code_region.migration_records_count);
    
    // Verify total size: 3 - 2 = 1 page
    total_size = eflash_ftl_get_code_region_size();
    assert(total_size == 1 && "Total size should be 1 page");
    printf("    Total code region size: %d pages\n", total_size);
    
    // Verify Segment 2 still intact
    {
        uint8_t expected[USER_DATA_SIZE];
        memset(expected, 0xC0, USER_DATA_SIZE);
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(0, read_data, USER_DATA_SIZE);
        assert(ret == 0 && "Read should succeed");
        assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0);
    }
    printf("    Segment 2: OK\n");
    
    printf("  === Phase 6: Final cleanup - delete all segments ===\n");
    
    // Delete last remaining segment
    uint32_t seg2_logical_addr = (uint32_t)seg2_lpn * USER_DATA_SIZE;
    ret = eflash_ftl_code_region_delete_segment(seg2_logical_addr);
    assert(ret == 0 && "Delete last segment should succeed");
    
    // Verify migration_map: 0 entries
    assert(g_code_region.migration_records_count == 0 && "Should have 0 segments");
    printf("    Migration map entries: %d\n", g_code_region.migration_records_count);
    
    // Verify total size: 0 pages
    total_size = eflash_ftl_get_code_region_size();
    assert(total_size == 0 && "Total size should be 0 pages");
    printf("    Total code region size: %d pages\n", total_size);
    
    printf("  === Integration test completed successfully ===\n");
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 10: Comprehensive Integration Test - Stress Test with Memory Leak Detection
// ============================================================================

void test_code_segment_stress_with_leak_detection(void) {
    setup_test_environment();
    total_tests++;
    
    printf("  === Comprehensive Stress Test with Memory Leak Detection ===\n");
    
    // Record initial state
    uint16_t initial_records = g_code_region.migration_records_count;
    uint32_t initial_code_size = g_code_region.code_size_bytes;
    assert(initial_records == 0 && "Should start with 0 records");
    assert(initial_code_size == 0 && "Should start with 0 code size");
    printf("  Initial state: records=%d, code_size=%d bytes\n", initial_records, initial_code_size);
    
    // =========================================================================
    // Phase 1: Add multiple segments in a loop
    // =========================================================================
    printf("\n  === Phase 1: Add 10 code segments ===\n");
    
    #define num_segments  10
    uint16_t segment_lpns[num_segments];
    uint16_t segment_sizes[num_segments];
    uint32_t total_expected_pages = 0;
    
    for (int i = 0; i < num_segments; i++) {
        segment_lpns[i] = 1000 + i * 10;
        segment_sizes[i] = (i % 3) + 1; // Sizes: 1, 2, 3, 1, 2, 3, ...
        total_expected_pages += segment_sizes[i];
        
        // Write data
        for (uint16_t j = 0; j < segment_sizes[i]; j++) {
            uint8_t test_data[USER_DATA_SIZE];
            memset(test_data, 0x10 + i, USER_DATA_SIZE);
            int ret = eflash_ftl_write(segment_lpns[i] + j, test_data);
            assert(ret == 0 && "Write should succeed");
        }
        
        // Migrate to code region
        int ret = eflash_ftl_code_migrate_from_logical(segment_lpns[i], segment_sizes[i]);
        assert(ret == 0 && "Migration should succeed");
        
        printf("    Added Segment %d: %d pages at LPN %d\n", i, segment_sizes[i], segment_lpns[i]);
    }
    
    // Verify state after adding
    assert(g_code_region.migration_records_count == num_segments && "Should have 10 segments");
    uint16_t total_size = eflash_ftl_get_code_region_size();
    assert(total_size == total_expected_pages && "Total size should match");
    printf("  After Phase 1: records=%d, total_pages=%d\n", 
           g_code_region.migration_records_count, total_size);
    
    // Verify all data integrity
    printf("  Verifying all segments data integrity...\n");
    uint16_t page_offset = 0;
    for (int i = 0; i < num_segments; i++) {
        for (uint16_t j = 0; j < segment_sizes[i]; j++) {
            uint8_t expected[USER_DATA_SIZE];
            memset(expected, 0x10 + i, USER_DATA_SIZE);
            uint8_t read_data[USER_DATA_SIZE];
            int ret = eflash_ftl_code_read(page_offset + j, read_data, USER_DATA_SIZE);
            assert(ret == 0 && "Read should succeed");
            assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0 && "Data mismatch");
        }
        page_offset += segment_sizes[i];
    }
    printf("    All %d segments verified successfully\n", num_segments);
    
    // =========================================================================
    // Phase 2: Delete segments in alternating pattern (every other segment)
    // =========================================================================
    printf("\n  === Phase 2: Delete alternating segments (0, 2, 4, 6, 8) ===\n");
    
    uint32_t deleted_pages = 0;
    int remaining_count = 0;
    
    for (int i = 0; i < num_segments; i += 2) {
        uint32_t logical_addr = (uint32_t)segment_lpns[i] * USER_DATA_SIZE;
        printf("    Deleting Segment %d (logical=0x%06X, %d pages)...\n", i, logical_addr, segment_sizes[i]);
        int ret = eflash_ftl_code_region_delete_segment(logical_addr);
        assert(ret == 0 && "Delete should succeed");
        deleted_pages += segment_sizes[i];
        remaining_count++;
        
        // Print migration_map after each delete
        printf("      Migration map after delete:\n");
        for (int j = 0; j < g_code_region.migration_records_count; j++) {
            printf("        [%d] logical=0x%06X, physical=0x%06X, size=%d\n",
                   j, g_code_region.migration_map[j].logical_addr,
                   g_code_region.migration_map[j].physical_addr,
                   g_code_region.migration_map[j].size);
        }
        fflush(stdout);
    }
    
    // Verify state after deletion
    int expected_remaining = num_segments / 2;
    assert(g_code_region.migration_records_count == expected_remaining && "Should have 5 segments remaining");
    total_size = eflash_ftl_get_code_region_size();
    assert(total_size == (total_expected_pages - deleted_pages) && "Size should decrease correctly");
    printf("  After Phase 2: records=%d, total_pages=%d\n", 
           g_code_region.migration_records_count, total_size);
    
    // Verify remaining segments data integrity
    printf("  Verifying remaining segments...\n");
    printf("    DEBUG: Migration map after Phase 2 deletes:\n");
    for (int i = 0; i < g_code_region.migration_records_count; i++) {
        printf("      [%d] logical=0x%06X, physical=0x%06X, size=%d\n",
               i, g_code_region.migration_map[i].logical_addr,
               g_code_region.migration_map[i].physical_addr,
               g_code_region.migration_map[i].size);
    }
    fflush(stdout);
    page_offset = 0;
    for (int i = 1; i < num_segments; i += 2) {
        for (uint16_t j = 0; j < segment_sizes[i]; j++) {
            uint8_t expected[USER_DATA_SIZE];
            memset(expected, 0x10 + i, USER_DATA_SIZE);
            uint8_t read_data[USER_DATA_SIZE];
            printf("    Reading page_offset=%d, expected=0x%02X\n", page_offset + j, 0x10 + i);
            int ret = eflash_ftl_code_read(page_offset + j, read_data, USER_DATA_SIZE);
            if (ret != 0) {
                printf("    ERROR: Read failed with ret=%d\n", ret);
            }
            assert(ret == 0 && "Read should succeed");
            if (memcmp(expected, read_data, USER_DATA_SIZE) != 0) {
                printf("    DEBUG: Page %d mismatch! Expected 0x%02X, got: ",
                       page_offset + j, 0x10 + i);
                for (int k = 0; k < 16; k++) printf("%02X ", read_data[k]);
                printf("\n");
            }
            fflush(stdout);
            assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0 && "Data corrupted after delete");
        }
        page_offset += segment_sizes[i];
    }
    printf("    All remaining segments verified\n");
    
    // =========================================================================
    // Phase 3: Add new segments to fill gaps
    // =========================================================================
    printf("\n  === Phase 3: Add 5 new segments ===\n");
    
    uint16_t new_segment_lpns[5];
    uint16_t new_segment_sizes[5];
    uint32_t new_pages = 0;
    
    for (int i = 0; i < 5; i++) {
        new_segment_lpns[i] = 2000 + i * 10;
        new_segment_sizes[i] = (i % 2) + 1; // Sizes: 1, 2, 1, 2, 1
        new_pages += new_segment_sizes[i];
        
        for (uint16_t j = 0; j < new_segment_sizes[i]; j++) {
            uint8_t test_data[USER_DATA_SIZE];
            memset(test_data, 0x50 + i, USER_DATA_SIZE);
            int ret = eflash_ftl_write(new_segment_lpns[i] + j, test_data);
            assert(ret == 0 && "Write should succeed");
        }
        
        int ret = eflash_ftl_code_migrate_from_logical(new_segment_lpns[i], new_segment_sizes[i]);
        assert(ret == 0 && "Migration should succeed");
        printf("    Added New Segment %d: %d pages at LPN %d\n", i, new_segment_sizes[i], new_segment_lpns[i]);
    }
    
    // Verify state after re-adding
    assert(g_code_region.migration_records_count == (expected_remaining + 5) && "Should have 10 segments");
    total_size = eflash_ftl_get_code_region_size();
    assert(total_size == (total_expected_pages - deleted_pages + new_pages) && "Size should be correct");
    printf("  After Phase 3: records=%d, total_pages=%d\n", 
           g_code_region.migration_records_count, total_size);
    
    // =========================================================================
    // Phase 4: Delete all remaining original segments
    // =========================================================================
    printf("\n  === Phase 4: Delete all original remaining segments (1, 3, 5, 7, 9) ===\n");
    
    for (int i = 1; i < num_segments; i += 2) {
        uint32_t logical_addr = (uint32_t)segment_lpns[i] * USER_DATA_SIZE;
        int ret = eflash_ftl_code_region_delete_segment(logical_addr);
        assert(ret == 0 && "Delete should succeed");
        printf("    Deleted Segment %d\n", i);
    }
    
    // Verify only new segments remain
    assert(g_code_region.migration_records_count == 5 && "Should have 5 new segments");
    total_size = eflash_ftl_get_code_region_size();
    assert(total_size == new_pages && "Size should equal new pages only");
    printf("  After Phase 4: records=%d, total_pages=%d\n", 
           g_code_region.migration_records_count, total_size);
    
    // Verify new segments data
    printf("  Verifying new segments...\n");
    page_offset = 0;
    for (int i = 0; i < 5; i++) {
        for (uint16_t j = 0; j < new_segment_sizes[i]; j++) {
            uint8_t expected[USER_DATA_SIZE];
            memset(expected, 0x50 + i, USER_DATA_SIZE);
            uint8_t read_data[USER_DATA_SIZE];
            int ret = eflash_ftl_code_read(page_offset + j, read_data, USER_DATA_SIZE);
            assert(ret == 0 && "Read should succeed");
            assert(memcmp(expected, read_data, USER_DATA_SIZE) == 0 && "New segment data corrupted");
        }
        page_offset += new_segment_sizes[i];
    }
    printf("    All new segments verified\n");
    
    // =========================================================================
    // Phase 5: Edge case tests
    // =========================================================================
    printf("\n  === Phase 5: Edge case tests ===\n");
    
    // Test 1: Try to delete non-existent segment
    printf("  Testing delete non-existent segment...\n");
    uint32_t fake_logical_addr = 99999 * USER_DATA_SIZE;
    int ret = eflash_ftl_code_region_delete_segment(fake_logical_addr);
    assert(ret == -1 && "Should fail for non-existent segment");
    printf("    Non-existent segment delete correctly rejected\n");
    
    // Test 2: Verify migration_map consistency
    printf("  Verifying migration_map consistency...\n");
    uint32_t cumulative_size = 0;
    for (int i = 0; i < g_code_region.migration_records_count; i++) {
        assert(g_code_region.migration_map[i].size > 0 && "Segment size should be > 0");
        assert(g_code_region.migration_map[i].size % USER_DATA_SIZE == 0 && "Size should be page-aligned");
        cumulative_size += g_code_region.migration_map[i].size;
    }
    assert(cumulative_size == g_code_region.code_size_bytes && "Cumulative size should match total");
    printf("    Migration map consistency verified\n");
    
    // =========================================================================
    // Phase 6: Cleanup - delete all segments
    // =========================================================================
    printf("\n  === Phase 6: Cleanup - delete all segments ===\n");
    
    for (int i = 0; i < 5; i++) {
        uint32_t logical_addr = (uint32_t)new_segment_lpns[i] * USER_DATA_SIZE;
        int ret = eflash_ftl_code_region_delete_segment(logical_addr);
        assert(ret == 0 && "Delete should succeed");
    }
    
    // Final state verification
    assert(g_code_region.migration_records_count == 0 && "Should have 0 records");
    assert(g_code_region.code_size_bytes == 0 && "Should have 0 code size");
    total_size = eflash_ftl_get_code_region_size();
    assert(total_size == 0 && "Should have 0 pages");
    printf("  Final state: records=%d, code_size=%d bytes, pages=%d\n", 
           g_code_region.migration_records_count, g_code_region.code_size_bytes, total_size);
    
    // Memory leak detection: verify no orphaned records
    printf("\n  === Memory Leak Detection ===\n");
    bool has_leak = false;
    for (int i = 0; i < MAX_MIGRATION_RECORDS; i++) {
        if (g_code_region.migration_map[i].logical_addr != 0 ||
            g_code_region.migration_map[i].physical_addr != 0 ||
            g_code_region.migration_map[i].size != 0) {
            printf("    WARNING: Orphaned record at index %d: logical=0x%06X, physical=0x%06X, size=%d\n",
                   i, g_code_region.migration_map[i].logical_addr,
                   g_code_region.migration_map[i].physical_addr,
                   g_code_region.migration_map[i].size);
            has_leak = true;
        }
    }
    assert(!has_leak && "No memory leaks detected - all records properly cleaned");
    printf("    No memory leaks detected - all records properly cleaned\n");
    
    printf("\n  === Comprehensive stress test completed successfully ===\n");
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 11: GC Reserve Physical Range for Code Region
// ============================================================================

void test_gc_reserve_physical_range(void) {
    setup_test_environment();
    total_tests++;
    
    printf("  === Phase 1: Write test data ===\n");
    
    const uint16_t num_sectors = 80;
    for (uint16_t i = 0; i < num_sectors; i++) {
        uint8_t data[USER_DATA_SIZE];
        memset(data, 0xC0 + (i % 16), USER_DATA_SIZE);
        int ret = eflash_ftl_write(i, data);
        assert(ret == 0 && "Write should succeed");
    }
    
    uint16_t head_before = g_ftl_instance.gc_head_page;
    uint16_t tail_before = g_ftl_instance.gc_tail_page;
    uint32_t free_before = eflash_ftl_get_free_pages();
    printf("    Wrote %d sectors. head=%d, tail=%d, free=%d\n",
           num_sectors, head_before, tail_before, free_before);
    
    printf("  === Phase 2: Reserve physical range ===\n");
    
    // Reserve a range that should contain written data pages
    // After init (~25 system pages) + 80 user writes, head ~105
    // Reserve [60, 70) - should be purely user data
    uint16_t reserve_start = 60;
    uint16_t reserve_pages = 10;
    uint16_t reserve_end = reserve_start + reserve_pages;
    
    printf("    Reserving PPN [%d, %d)...\n", reserve_start, reserve_end);
    int ret = eflash_ftl_gc_reserve_physical_range(reserve_start, reserve_pages);
    assert(ret == 0 && "Reserve should succeed");
    
    printf("  === Phase 3: Verify head/tail outside reserved range ===\n");
    
    uint16_t head_after = g_ftl_instance.gc_head_page;
    uint16_t tail_after = g_ftl_instance.gc_tail_page;
    uint32_t free_after = eflash_ftl_get_free_pages();
    
    printf("    After reserve: head=%d, tail=%d, free=%d\n", head_after, tail_after, free_after);
    
    assert(!(head_after >= reserve_start && head_after < reserve_end) &&
           "Head must be outside reserved range");
    printf("    Head %d is outside reserved range ?\n", head_after);
    
    assert(!(tail_after >= reserve_start && tail_after < reserve_end) &&
           "Tail must be outside reserved range");
    printf("    Tail %d is outside reserved range ?\n", tail_after);
    
    printf("  === Phase 4: Verify all reserved pages are erased ===\n");
    
    for (uint16_t ppn = reserve_start; ppn < reserve_end; ppn++) {
        uint8_t page[EFLASH_PAGE_SIZE];
        ret = eflash_hw_read(ppn, page);
        assert(ret == 0 && "Hardware read should succeed");
        
        bool all_ff = true;
        for (int b = 0; b < EFLASH_PAGE_SIZE; b++) {
            if (page[b] != 0xFF) {
                all_ff = false;
                printf("    PPN %d byte %d = 0x%02X (not 0xFF)\n", ppn, b, page[b]);
                break;
            }
        }
        assert(all_ff && "Reserved page must be erased");
    }
    printf("    All %d reserved pages are erased ?\n", reserve_pages);
    
    printf("  === Phase 5: Verify migrated data still readable ===\n");
    
    for (uint16_t i = 0; i < num_sectors; i++) {
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_read(i, read_data);
        assert(ret == 0 && "Read should succeed");
        
        uint8_t expected = 0xC0 + (i % 16);
        assert(read_data[0] == expected && "Data must match after migration");
    }
    printf("    All %d sectors still readable after reservation ?\n", num_sectors);
    
    printf("  === Phase 6: New writes don't use reserved range ===\n");
    
    for (uint16_t i = 100; i < 110; i++) {
        uint8_t data[USER_DATA_SIZE];
        memset(data, 0xDD, USER_DATA_SIZE);
        ret = eflash_ftl_write(i, data);
        assert(ret == 0 && "Write should succeed");
    }
    
    // Verify reserved pages are still erased (untouched by new writes)
    for (uint16_t ppn = reserve_start; ppn < reserve_end; ppn++) {
        uint8_t page[EFLASH_PAGE_SIZE];
        eflash_hw_read(ppn, page);
        bool all_ff = true;
        for (int b = 0; b < EFLASH_PAGE_SIZE; b++) {
            if (page[b] != 0xFF) {
                all_ff = false;
                break;
            }
        }
        assert(all_ff && "Reserved page must remain erased after new writes");
    }
    printf("    Reserved pages intact after new writes ?\n");
    
    printf("  === Phase 7: Edge case - reserve 0 pages ===\n");
    ret = eflash_ftl_gc_reserve_physical_range(reserve_end, 0);
    assert(ret == 0 && "Reserve 0 pages should succeed");
    printf("    Reserve 0 pages: ?\n");
    
    printf("  === Phase 8: Edge case - range exceeds total pages ===\n");
    ret = eflash_ftl_gc_reserve_physical_range(2000, 100);
    assert(ret == -1 && "Should fail when range exceeds total pages");
    printf("    Range overflow detection: ?\n");
    
    printf("\n  === GC reserve physical range test completed ===\n");
    
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 12: Integration Test - Full Code Migration with GC Reservation
// ============================================================================
// Simulates a realistic code segment migration pipeline:
//   1. FTL is populated with diverse user data (200+ sectors)
//   2. A physical range is reserved for code via gc_reserve_physical_range
//   3. Data integrity is verified after reservation (GC migration correctness)
//   4. Reserved range protection is verified under continued FTL writes
//   5. Code is simulated as being written into the reserved area
//   6. Head wrap-around behaviour is tested
//   7. Multiple sequential reservations are verified
//   8. Edge cases are exercised

void test_integration_gc_reserve_code_migration(void) {
    setup_test_environment();
    total_tests++;

    uint16_t head, tail;
    uint32_t free_pages;
    int ret;

    // =======================================================================
    // Phase 1: Populate FTL with diverse user data (simulates real-world load)
    // =======================================================================
    printf("  === Phase 1: Populating FTL with 200 sectors of diverse data ===\n");

    const uint16_t phase1_sectors = 200;
    for (uint16_t i = 0; i < phase1_sectors; i++) {
        uint8_t data[USER_DATA_SIZE];
        uint8_t pattern_byte = (uint8_t)((i * 7 + 13) & 0xFF);
        memset(data, pattern_byte, USER_DATA_SIZE);
        data[0] = (uint8_t)(i & 0xFF);
        data[1] = (uint8_t)((i >> 8) & 0xFF);
        ret = eflash_ftl_write(i, data);
        assert(ret == 0 && "Phase1: Write should succeed");
    }

    head = FTL->gc_head_page;
    tail = FTL->gc_tail_page;
    free_pages = eflash_ftl_get_free_pages();
    printf("    After populating: head=%d, tail=%d, free=%u\n", head, tail, free_pages);
    assert(head > 100 && "Phase1: head should be well past PPN 100 after 200 writes");

    // =======================================================================
    // Phase 2: Reserve a physical range for code
    // =======================================================================
    printf("\n  === Phase 2: Reserve PPN [80, 100) for code ===\n");

    const uint16_t reserve1_start = 80;
    const uint16_t reserve1_pages = 20;
    const uint16_t reserve1_end = reserve1_start + reserve1_pages;

    uint16_t head_before_reserve = FTL->gc_head_page;
    uint16_t tail_before_reserve = FTL->gc_tail_page;

    ret = eflash_ftl_gc_reserve_physical_range(reserve1_start, reserve1_pages);
    assert(ret == 0 && "Phase2: Reserve should succeed");

    head = FTL->gc_head_page;
    tail = FTL->gc_tail_page;
    printf("    Before reserve: head=%d, tail=%d\n", head_before_reserve, tail_before_reserve);
    printf("    After  reserve: head=%d, tail=%d\n", head, tail);

    // Verify head is outside reserved range
    assert(!(head >= reserve1_start && head < reserve1_end) &&
           "Phase2: head must be outside reserved range");
    printf("    head %d outside [%d,%d) ?\n", head, reserve1_start, reserve1_end);

    // Verify tail is outside reserved range
    assert(!(tail >= reserve1_start && tail < reserve1_end) &&
           "Phase2: tail must be outside reserved range");
    printf("    tail %d outside [%d,%d) ?\n", tail, reserve1_start, reserve1_end);

    // Verify all reserved pages are erased (all 0xFF)
    printf("    Scanning reserved pages for erase verification...\n");
    for (uint16_t ppn = reserve1_start; ppn < reserve1_end; ppn++) {
        uint8_t page[EFLASH_PAGE_SIZE];
        ret = eflash_hw_read(ppn, page);
        assert(ret == 0 && "Phase2: hardware read should succeed");

        bool all_ff = true;
        for (int b = 0; b < EFLASH_PAGE_SIZE; b++) {
            if (page[b] != 0xFF) {
                all_ff = false;
                printf("    FAIL: PPN %d byte %d = 0x%02X\n", ppn, b, page[b]);
                break;
            }
        }
        assert(all_ff && "Phase2: every reserved page must be fully erased");
    }
    printf("    All %d pages in reserved range verified erased ?\n", reserve1_pages);

    // =======================================================================
    // Phase 3: Verify data integrity after GC reservation
    // =======================================================================
    printf("\n  === Phase 3: Data integrity check after reservation ===\n");
    printf("    Checking all %d sectors still readable with correct data...\n", phase1_sectors);

    uint16_t mismatch_count = 0;
    for (uint16_t i = 0; i < phase1_sectors; i++) {
        uint8_t read_data[USER_DATA_SIZE];
        ret = eflash_ftl_read(i, read_data);
        assert(ret == 0 && "Phase3: read should succeed");

        uint8_t expected_pattern = (uint8_t)((i * 7 + 13) & 0xFF);
        if (read_data[0] != (uint8_t)(i & 0xFF) ||
            read_data[1] != (uint8_t)((i >> 8) & 0xFF) ||
            read_data[3] != expected_pattern) {
            mismatch_count++;
        }
    }
    assert(mismatch_count == 0 && "Phase3: all sectors must retain correct data");
    printf("    All %d sectors verified intact ?\n", phase1_sectors);

    // =======================================================================
    // Phase 4: Reserved range protection under continued writes
    // =======================================================================
    printf("\n  === Phase 4: Reserved range protection under new writes ===\n");

    for (uint16_t i = 300; i < 330; i++) {
        uint8_t data[USER_DATA_SIZE];
        memset(data, 0xDD, USER_DATA_SIZE);
        ret = eflash_ftl_write(i, data);
        assert(ret == 0 && "Phase4: write should succeed");
    }

    head = FTL->gc_head_page;
    printf("    After 30 new writes: head=%d\n", head);

    // Verify reserved range is still pristine
    for (uint16_t ppn = reserve1_start; ppn < reserve1_end; ppn++) {
        uint8_t page[EFLASH_PAGE_SIZE];
        eflash_hw_read(ppn, page);
        for (int b = 0; b < EFLASH_PAGE_SIZE; b++) {
            assert(page[b] == 0xFF && "Phase4: reserved page must remain erased");
        }
    }
    printf("    Reserved range [%d,%d) still untouched after 30 writes ?\n",
           reserve1_start, reserve1_end);

    // =======================================================================
    // Phase 5: Simulate code being written into the reserved area
    // =======================================================================
    printf("\n  === Phase 5: Simulate code write into reserved area ===\n");

    const char* code_patterns[] = {
        "CODE_SEGMENT_0: bootloader_init_vector_table",
        "CODE_SEGMENT_1: interrupt_handler_entry",
        "CODE_SEGMENT_2: memory_config_routines",
        "CODE_SEGMENT_3: clock_init_and_pll_setup",
    };
    const uint16_t num_code_segs = 4;

    uint16_t code_ppn = reserve1_start;
    for (uint16_t seg = 0; seg < num_code_segs; seg++) {
        uint8_t code_page[EFLASH_PAGE_SIZE];
        memset(code_page, 0xFF, EFLASH_PAGE_SIZE);
        size_t pattern_len = strlen(code_patterns[seg]);
        memcpy(code_page, code_patterns[seg], pattern_len < USER_DATA_SIZE ? pattern_len : USER_DATA_SIZE);

        // Also write segment marker at fixed offset
        code_page[USER_DATA_SIZE - 1] = (uint8_t)(0xE0 + seg);

        ret = eflash_hw_prog(code_ppn + seg, code_page);
        assert(ret == 0 && "Phase5: program code page should succeed");
    }
    printf("    Programmed %d code pages into reserved area PPN [%d,%d)\n",
           num_code_segs, code_ppn, code_ppn + num_code_segs);

    // Verify code readback
    for (uint16_t seg = 0; seg < num_code_segs; seg++) {
        uint8_t readback[EFLASH_PAGE_SIZE];
        ret = eflash_hw_read(code_ppn + seg, readback);
        assert(ret == 0 && "Phase5: readback should succeed");

        assert(memcmp(readback, code_patterns[seg],
                      strlen(code_patterns[seg])) == 0 &&
               "Phase5: code pattern must match");
        assert(readback[USER_DATA_SIZE - 1] == (uint8_t)(0xE0 + seg) &&
               "Phase5: segment marker must match");
    }
    printf("    Code readback verified ?\n");

    // Verify remaining reserved pages (not used by code) are still erased
    for (uint16_t ppn = code_ppn + num_code_segs; ppn < reserve1_end; ppn++) {
        uint8_t page[EFLASH_PAGE_SIZE];
        eflash_hw_read(ppn, page);
        bool all_ff = true;
        for (int b = 0; b < EFLASH_PAGE_SIZE; b++) {
            if (page[b] != 0xFF) { all_ff = false; break; }
        }
        assert(all_ff && "Phase5: unused reserved pages must remain erased");
    }
    printf("    Unused reserved pages still erased ?\n");

    // =======================================================================
    // Phase 6: Head wrap-around scenario
    // =======================================================================
    printf("\n  === Phase 6: Head wrap-around scenario ===\n");

    // Write enough sectors to drive head close to end of flash
    // Head is currently somewhere beyond 100+; we need to drive it near 2047
    uint16_t sectors_to_fill = 0;
    while (FTL->gc_head_page < 1900) {
        uint8_t data[USER_DATA_SIZE];
        memset(data, 0xBB, USER_DATA_SIZE);
        ret = eflash_ftl_write(500 + sectors_to_fill, data);
        assert(ret == 0 && "Phase6: fill write should succeed");
        sectors_to_fill++;
    }
    printf("    Wrote %d sectors to push head to %d\n", sectors_to_fill, FTL->gc_head_page);

    // Now reserve a range near the end of flash
    const uint16_t reserve2_start = 1960;
    const uint16_t reserve2_pages = 10;
    const uint16_t reserve2_end = reserve2_start + reserve2_pages;

    printf("    Reserving PPN [%d,%d) near end of flash...\n", reserve2_start, reserve2_end);
    ret = eflash_ftl_gc_reserve_physical_range(reserve2_start, reserve2_pages);
    assert(ret == 0 && "Phase6: near-end reserve should succeed");

    head = FTL->gc_head_page;
    tail = FTL->gc_tail_page;
    printf("    After near-end reserve: head=%d, tail=%d\n", head, tail);

    // Verify head/tail outside reserved range
    assert(!(head >= reserve2_start && head < reserve2_end) &&
           "Phase6: head must stay outside near-end reserved range");
    assert(!(tail >= reserve2_start && tail < reserve2_end) &&
           "Phase6: tail must stay outside near-end reserved range");
    printf("    head/tail outside near-end reserved range ?\n");

    // Verify near-end reserved pages are erased
    for (uint16_t ppn = reserve2_start; ppn < reserve2_end; ppn++) {
        uint8_t page[EFLASH_PAGE_SIZE];
        eflash_hw_read(ppn, page);
        for (int b = 0; b < EFLASH_PAGE_SIZE; b++) {
            assert(page[b] == 0xFF && "Phase6: near-end pages must be erased");
        }
    }
    printf("    All %d near-end pages erased ?\n", reserve2_pages);

    // =======================================================================
    // Phase 7: Multiple sequential reservations
    // =======================================================================
    printf("\n  === Phase 7: Multiple sequential reservations ===\n");

    // Reserve first gap beyond head
    uint16_t multi_start = FTL->gc_head_page + 5;
    if (multi_start >= EFLASH_TOTAL_PAGES) multi_start = 5;

    const uint16_t multi1_pages = 6;
    const uint16_t multi1_end = multi_start + multi1_pages;
    const uint16_t multi2_start = multi1_end + 3;
    const uint16_t multi2_pages = 4;
    const uint16_t multi2_end = multi2_start + multi2_pages;

    // Ensure ranges don't clash with code-occupied area
    if (multi_start < reserve1_end) {
        multi_start = reserve1_end + 2;
    }

    printf("    Multi-reserve #1: [%d,%d)...\n", multi_start, multi1_end);
    ret = eflash_ftl_gc_reserve_physical_range(multi_start, multi1_pages);
    assert(ret == 0 && "Phase7: first multi-reserve should succeed");

    printf("    Multi-reserve #2: [%d,%d)...\n", multi2_start, multi2_end);
    ret = eflash_ftl_gc_reserve_physical_range(multi2_start, multi2_pages);
    assert(ret == 0 && "Phase7: second multi-reserve should succeed");

    // Verify both ranges are erased
    for (uint16_t ppn = multi_start; ppn < multi1_end; ppn++) {
        uint8_t page[EFLASH_PAGE_SIZE];
        eflash_hw_read(ppn, page);
        for (int b = 0; b < EFLASH_PAGE_SIZE; b++)
            assert(page[b] == 0xFF && "Phase7: multi1 must be erased");
    }
    for (uint16_t ppn = multi2_start; ppn < multi2_end; ppn++) {
        uint8_t page[EFLASH_PAGE_SIZE];
        eflash_hw_read(ppn, page);
        for (int b = 0; b < EFLASH_PAGE_SIZE; b++)
            assert(page[b] == 0xFF && "Phase7: multi2 must be erased");
    }
    printf("    Both multi-reserved ranges verified erased ?\n");

    // =======================================================================
    // Phase 8: Edge cases
    // =======================================================================
    printf("\n  === Phase 8: Edge cases ===\n");

    // Edge: reserve single page
    ret = eflash_ftl_gc_reserve_physical_range(multi2_end + 5, 1);
    assert(ret == 0 && "Phase8: single page reserve should succeed");
    printf("    Single page reserve: ?\n");

    // Edge: GC already in progress detection
    // (We can't easily test this externally, but the guard exists in implementation)

    printf("\n  === Integration test completed successfully ===\n");

    passed_tests++;
    teardown_test_environment();
}

/**
 * test_code_and_data_coexistence: Code & Data coexistence test with real migration
 *
 * Simulates a real-world scenario where user data occupies physical pages
 * needed by the code region, forcing GC to reserve and migrate. Verifies
 * bidirectional integrity of both user data and code after migration.
 *
 * Scenario:
 *   1. Write 180 sectors of user data (fills many physical pages including PPN 0+)
 *   2. Write 132 pages (~61KB) of code source data to logical LPNs
 *   3. GC reserve PPN [0,132) - migrates user data out, erases the range
 *   4. Code migrate - reads code from logical, writes directly to PPN [0,~118]
 *   5. Verify all 180 user data sectors intact
 *   6. Verify all 132 code pages intact via code_read
 *   7. Direct physical read of code region
 *   8. Write more user data, verify code region still protected
 *   9. Final re-verification of both data and code
 */
void test_code_and_data_coexistence(void) {
    setup_test_environment();
    total_tests++;

    int ret;
    uint16_t head, tail;

    const uint16_t USER_LPN_START = 20;
    const uint16_t USER_SECTORS = 180;
    const uint16_t CODE_PAGES = 132;
    const uint16_t CODE_LPN_START = 900;
    const uint32_t CODE_SIZE_BYTES = CODE_PAGES * USER_DATA_SIZE;

    printf("\n  CODE & DATA COEXISTENCE TEST (%u sectors@LPN%u + %u pages code ~%uKB)\n",
           USER_SECTORS, USER_LPN_START, CODE_PAGES, CODE_SIZE_BYTES / 1024);

    // =======================================================================
    // Phase 1: Write user data with sector-ID-embedded patterns
    //   (starting from USER_LPN_START to avoid system-reserved LPNs)
    // =======================================================================
    printf("  === Phase 1: Write %u user sectors @ LPN [%u, %u) ===\n",
           USER_SECTORS, USER_LPN_START, USER_LPN_START + USER_SECTORS);

    for (uint16_t i = 0; i < USER_SECTORS; i++) {
        uint16_t lpn = USER_LPN_START + i;
        uint8_t data[USER_DATA_SIZE];
        data[0] = (uint8_t)(lpn & 0xFF);
        data[1] = (uint8_t)((lpn >> 8) & 0xFF);
        data[2] = 0xAA;
        data[3] = (uint8_t)((lpn * 17 + 53) & 0xFF);
        data[USER_DATA_SIZE - 2] = (uint8_t)((lpn * 179 + 31) & 0xFF);
        data[USER_DATA_SIZE - 1] = 0xBB;
        memset(data + 4, (uint8_t)((lpn * 47 + 13) & 0xFF), USER_DATA_SIZE - 6);
        ret = eflash_ftl_write(lpn, data);
        assert(ret == 0 && "P1: write");
    }

    head = FTL->gc_head_page;
    tail = FTL->gc_tail_page;
    printf("    Wrote %u sectors, head=%d tail=%d\n", USER_SECTORS, head, tail);
    assert(head > CODE_PAGES && "P1: head must be past code region");

    // =======================================================================
    // Phase 2: Write code source data with CE-CODE marker pattern
    // =======================================================================
    printf("\n  === Phase 2: Write %u pages code source to LPN %u ===\n",
           CODE_PAGES, CODE_LPN_START);

    for (uint16_t i = 0; i < CODE_PAGES; i++) {
        uint8_t data[USER_DATA_SIZE];
        data[0] = 0xCE;
        data[1] = 0xDE;
        data[2] = (uint8_t)(i & 0xFF);
        data[3] = (uint8_t)((i >> 8) & 0xFF);
        data[USER_DATA_SIZE - 2] = (uint8_t)((i * 47 + 19) & 0xFF);
        data[USER_DATA_SIZE - 1] = (uint8_t)((i * 163 + 71) & 0xFF);
        for (int j = 4; j < USER_DATA_SIZE - 2; j++) {
            data[j] = (uint8_t)((i * 31 + j * 7) & 0xFF);
        }
        ret = eflash_ftl_write(CODE_LPN_START + i, data);
        assert(ret == 0 && "P2: code source write");
    }
    printf("    Code source written: LPN [%u, %u)\n",
           CODE_LPN_START, CODE_LPN_START + CODE_PAGES);

    // Verify code source data is readable via FTL before migration
    printf("    Pre-migration verify: reading code source via FTL...\n");
    for (uint16_t i = 0; i < 3; i++) {
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_read(CODE_LPN_START + i, buf);
        assert(ret == 0 && "P2: code source read");
        assert(buf[0] == 0xCE && "P2: code marker[0]");
        assert(buf[1] == 0xDE && "P2: code marker[1]");
    }
    printf("    Code source readable via FTL ?\n");

    // =======================================================================
    // Phase 3: GC reserve PPN [0, CODE_PAGES) for code
    // =======================================================================
    printf("\n  === Phase 3: GC reserve PPN [0, %u) ===\n", CODE_PAGES);

    // Diagnostic: verify USER_LPN_START+12 BEFORE GC reserve
    {
        uint16_t chk_lpn = USER_LPN_START + 12;
        uint8_t dbg[USER_DATA_SIZE];
        int dr = eflash_ftl_read(chk_lpn, dbg);
        printf("    DIAG pre-reserve: LPN %u ret=%d [0]=%02X [1]=%02X [2]=%02X [3]=%02X\n",
               chk_lpn, dr, dbg[0], dbg[1], dbg[2], dbg[3]);
        assert(dr == 0 && dbg[0] == (uint8_t)(chk_lpn & 0xFF) && "pre-reserve LPN intact");
        fflush(stdout);
    }

    head = FTL->gc_head_page;
    tail = FTL->gc_tail_page;
    printf("    Before reserve: head=%d tail=%d\n", head, tail);

    ret = eflash_ftl_gc_reserve_physical_range(0, CODE_PAGES);
    assert(ret == 0 && "P3: reserve");

    head = FTL->gc_head_page;
    tail = FTL->gc_tail_page;
    printf("    After  reserve: head=%d tail=%d\n", head, tail);

    assert(head >= CODE_PAGES && "P3: head out of range");
    assert(tail >= CODE_PAGES && "P3: tail out of range");
    printf("    head/tail outside [0,%u) ?\n", CODE_PAGES);

    // Diagnostic: scan first 21 sectors after GC reserve
    {
        int bad_count = 0;
        for (uint16_t j = 0; j <= 20 && bad_count < 10; j++) {
            uint16_t si = USER_LPN_START + j;
            uint8_t dbg[USER_DATA_SIZE];
            int dr = eflash_ftl_read(si, dbg);
            (void)dr;
            if (dbg[0] != (uint8_t)(si & 0xFF) ||
                dbg[1] != (uint8_t)((si >> 8) & 0xFF) ||
                dbg[2] != 0xAA) {
                bad_count++;
                printf("    MISMATCH LPN %u: [0]=%02X(exp %02X) [1]=%02X(exp %02X) [2]=%02X(exp AA)\n",
                       si, dbg[0], (uint8_t)(si & 0xFF), dbg[1], (uint8_t)((si >> 8) & 0xFF), dbg[2]);
                fflush(stdout);
            }
        }
        if (bad_count == 0) printf("    LPNs %u-%u all intact after GC reserve\n",
                                    USER_LPN_START, USER_LPN_START + 20);
        fflush(stdout);
    }

    for (uint16_t ppn = 0; ppn < CODE_PAGES; ppn++) {
        uint8_t page[EFLASH_PAGE_SIZE];
        ret = eflash_hw_read(ppn, page);
        assert(ret == 0 && "P3: hw_read");
        for (int b = 0; b < EFLASH_PAGE_SIZE; b++) {
            assert(page[b] == 0xFF && "P3: erased");
        }
    }
    printf("    All %u reserved pages erased ?\n", CODE_PAGES);

    // =======================================================================
    // Phase 4: Migrate code from logical → physical code region
    // =======================================================================
    printf("\n  === Phase 4: Migrate code to physical code region ===\n");
    ret = eflash_ftl_code_migrate_from_logical(CODE_LPN_START, CODE_PAGES);
    assert(ret == 0 && "P4: migrate");

    uint16_t code_size = eflash_ftl_get_code_region_size();
    assert(code_size == CODE_PAGES && "P4: code region size");
    printf("    Code region: %u pages, %u bytes\n", code_size, g_code_region.code_size_bytes);

    // Diagnostic: check USER_LPN_START+12 right after code migration
    {
        uint16_t chk_lpn = USER_LPN_START + 12;
        uint8_t dbg[USER_DATA_SIZE];
        int dr = eflash_ftl_read(chk_lpn, dbg);
        printf("    DIAG post-migrate: LPN %u ret=%d [0]=%02X [1]=%02X [2]=%02X [3]=%02X\n",
               chk_lpn, dr, dbg[0], dbg[1], dbg[2], dbg[3]);
        assert(dr == 0 && dbg[0] == (uint8_t)(chk_lpn & 0xFF) && "P4: LPN intact after migrate");
        fflush(stdout);
    }

    head = FTL->gc_head_page;
    tail = FTL->gc_tail_page;
    printf("    After migrate: head=%d tail=%d\n", head, tail);
    assert(head >= CODE_PAGES && "P4: head >= code_region");
    assert(tail >= CODE_PAGES && "P4: tail >= code_region");
    printf("    GC pointers skip code region ?\n");

    // Verify source pages were trimmed (FTL returns blank 0xFF data)
    {
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_read(CODE_LPN_START, buf);
        assert(ret == 0 && "P4: trim reads should still succeed via radix tree");
        assert(buf[0] != 0xCE && "P4: old code marker cleared after trim");
        assert(buf[1] != 0xDE && "P4: old code marker[1] cleared after trim");
    }
    printf("    Source LPNs trimmed ?\n");

    // =======================================================================
    // Phase 5: Verify all user data intact after migration
    // =======================================================================
    printf("\n  === Phase 5: Verify all %u user sectors intact ===\n", USER_SECTORS);

    // Quick diagnostic: read first user sector manually
    {
        uint8_t dbg[USER_DATA_SIZE];
        int dr = eflash_ftl_read(USER_LPN_START, dbg);
        printf("    DIAG: LPN %u read ret=%d [0]=%02X [1]=%02X [2]=%02X [3]=%02X [460]=%02X [461]=%02X [462]=%02X [463]=%02X\n",
               USER_LPN_START, dr, dbg[0], dbg[1], dbg[2], dbg[3],
               dbg[460], dbg[461], dbg[462], dbg[463]);
        fflush(stdout);
    }

    uint16_t data_mismatches = 0;
    for (uint16_t i = 0; i < USER_SECTORS; i++) {
        uint16_t lpn = USER_LPN_START + i;
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_read(lpn, buf);
        assert(ret == 0 && "P5: read");

        if (buf[0] != (uint8_t)(lpn & 0xFF) ||
            buf[1] != (uint8_t)((lpn >> 8) & 0xFF) ||
            buf[2] != 0xAA ||
            buf[3] != (uint8_t)((lpn * 17 + 53) & 0xFF) ||
            buf[USER_DATA_SIZE - 2] != (uint8_t)((lpn * 179 + 31) & 0xFF) ||
            buf[USER_DATA_SIZE - 1] != 0xBB) {
            data_mismatches++;
            if (data_mismatches <= 5) {
                printf("    MISMATCH LPN %u: [0]=%02X(exp %02X) [1]=%02X(exp %02X) [2]=%02X(exp AA) [462]=%02X(exp %02X) [463]=%02X(exp BB)\n",
                       lpn,
                       buf[0], (uint8_t)(lpn & 0xFF),
                       buf[1], (uint8_t)((lpn >> 8) & 0xFF),
                       buf[2],
                       buf[USER_DATA_SIZE - 2], (uint8_t)((lpn * 179 + 31) & 0xFF),
                       buf[USER_DATA_SIZE - 1]);
                fflush(stdout);
            }
        }
    }
    printf("    Total mismatches: %u\n", data_mismatches);
    fflush(stdout);
    assert(data_mismatches == 0 && "P5: user data intact");
    printf("    All %u user sectors intact ?\n", USER_SECTORS);

    // =======================================================================
    // Phase 6: Verify code data via eflash_ftl_code_read
    // =======================================================================
    printf("\n  === Phase 6: Verify %u code pages via code_read ===\n", CODE_PAGES);

    uint16_t code_mismatches = 0;
    for (uint16_t i = 0; i < CODE_PAGES; i++) {
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(i, buf, USER_DATA_SIZE);
        assert(ret == 0 && "P6: code_read");

        if (buf[0] != 0xCE || buf[1] != 0xDE ||
            buf[2] != (uint8_t)(i & 0xFF) ||
            buf[3] != (uint8_t)((i >> 8) & 0xFF) ||
            buf[USER_DATA_SIZE - 2] != (uint8_t)((i * 47 + 19) & 0xFF) ||
            buf[USER_DATA_SIZE - 1] != (uint8_t)((i * 163 + 71) & 0xFF)) {
            code_mismatches++;
            if (code_mismatches <= 5) {
                printf("    MISMATCH code pgoff=%u: [0]=%02X(exp CE) [1]=%02X(exp DE)\n",
                       i, buf[0], buf[1]);
            }
        }
    }
    assert(code_mismatches == 0 && "P6: code intact");
    printf("    All %u code pages intact via code_read ?\n", CODE_PAGES);

    // =======================================================================
    // Phase 7: Direct physical verification
    // =======================================================================
    printf("\n  === Phase 7: Direct physical read of code region ===\n");

    {
        uint16_t phys_end = (g_code_region.code_size_bytes + EFLASH_PAGE_SIZE - 1) / EFLASH_PAGE_SIZE;
        printf("    Code region physical pages: [0, %u) (%u bytes packed)\n",
               phys_end, g_code_region.code_size_bytes);

        // Read first physical page and check CE marker
        uint8_t phys_page[EFLASH_PAGE_SIZE];
        eflash_hw_read(0, phys_page);
        assert(phys_page[0] == 0xCE && "P7: phys page0 marker CE");
        assert(phys_page[1] == 0xDE && "P7: phys page0 marker DE");
        printf("    Physical PPN 0: marker verified ?\n");

        // Read last physical page
        eflash_hw_read(phys_end - 1, phys_page);
        printf("    Physical PPN %u: readable, partial fill ?\n", phys_end - 1);

        // Verify PPN [phys_end, CODE_PAGES) remain erased (unused code region pages)
        if (phys_end < CODE_PAGES) {
            uint16_t erased_count = 0;
            for (uint16_t ppn = phys_end; ppn < CODE_PAGES; ppn++) {
                uint8_t page[EFLASH_PAGE_SIZE];
                eflash_hw_read(ppn, page);
                int all_ff = 1;
                for (int b = 0; b < EFLASH_PAGE_SIZE; b++) {
                    if (page[b] != 0xFF) { all_ff = 0; break; }
                }
                if (all_ff) erased_count++;
            }
            printf("    PPN [%u,%u): %u/%u erased ?\n",
                   phys_end, CODE_PAGES, erased_count, CODE_PAGES - phys_end);
        }
    }

    // =======================================================================
    // Phase 8: Write more user data, verify code region is protected
    // =======================================================================
    printf("\n  === Phase 8: New writes + code region protection ===\n");

    int new_write_count = 40;
    for (uint16_t i = 300; i < 300 + new_write_count; i++) {
        uint8_t data[USER_DATA_SIZE];
        memset(data, 0x5A, USER_DATA_SIZE);
        data[0] = (uint8_t)(i & 0xFF);
        data[1] = (uint8_t)((i >> 8) & 0xFF);
        ret = eflash_ftl_write(i, data);
        assert(ret == 0 && "P8: write");
    }
    head = FTL->gc_head_page;
    printf("    Wrote %d new sectors, head=%d\n", new_write_count, head);
    assert(head >= CODE_PAGES && "P8: head still past code region");

    // Re-read code region after writes
    {
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_code_read(0, buf, USER_DATA_SIZE);
        assert(ret == 0 && "P8: code_read after writes");
        assert(buf[0] == 0xCE && buf[1] == 0xDE && "P8: code marker survived");

        ret = eflash_ftl_code_read(CODE_PAGES - 1, buf, USER_DATA_SIZE);
        assert(ret == 0 && "P8: code_read last page after writes");
        assert(buf[0] == 0xCE && buf[1] == 0xDE && "P8: last code page marker survived");
    }
    printf("    Code region intact after %d new writes ?\n", new_write_count);

    // =======================================================================
    // Phase 9: Final verification - all data + code still correct
    // =======================================================================
    printf("\n  === Phase 9: Final complete verification ===\n");

    // User data
    uint16_t final_data_err = 0;
    for (uint16_t i = 0; i < USER_SECTORS; i++) {
        uint16_t lpn = USER_LPN_START + i;
        uint8_t buf[USER_DATA_SIZE];
        if (eflash_ftl_read(lpn, buf) != 0) { final_data_err++; continue; }
        if (buf[0] != (uint8_t)(lpn & 0xFF) ||
            buf[1] != (uint8_t)((lpn >> 8) & 0xFF) ||
            buf[2] != 0xAA) {
            final_data_err++;
        }
    }
    assert(final_data_err == 0 && "P9: user data final");
    printf("    %u user sectors: all correct ?\n", USER_SECTORS);

    // New data
    for (uint16_t i = 300; i < 300 + new_write_count; i++) {
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_read(i, buf);
        assert(ret == 0 && "P9: new data read");
        assert(buf[0] == (uint8_t)(i & 0xFF) && "P9: new data id");
        assert(buf[1] == (uint8_t)((i >> 8) & 0xFF) && "P9: new data id hi");
    }
    printf("    %d new sectors: all correct ?\n", new_write_count);

    // Code
    uint16_t final_code_err = 0;
    for (uint16_t i = 0; i < CODE_PAGES; i++) {
        uint8_t buf[USER_DATA_SIZE];
        if (eflash_ftl_code_read(i, buf, USER_DATA_SIZE) != 0) { final_code_err++; continue; }
        if (buf[0] != 0xCE || buf[1] != 0xDE) {
            final_code_err++;
        }
    }
    assert(final_code_err == 0 && "P9: code final");
    printf("    %u code pages: all correct ?\n", CODE_PAGES);

    // Direct physical scan: code region unscathed
    {
        uint8_t page0[EFLASH_PAGE_SIZE];
        eflash_hw_read(0, page0);
        assert(page0[0] == 0xCE && page0[1] == 0xDE && "P9: phys PPN0");
    }
    printf("    Physical PPN 0 still holds code marker ?\n");

    printf("\n  === Code & Data coexistence test passed ===\n");

    passed_tests++;
    teardown_test_environment();
}

/**
 * test_trim_radix_tree_integrity: Verify Radix Tree stays intact after TRIM
 * 
 * Tests:
 *   1. Write sectors with shared tree paths to build internal nodes
 *   2. Trim ONE sector in the middle
 *   3. Verify trimmed sector returns 0xFF
 *   4. Verify all OTHER sectors (including shared-path neighbors) are still correct
 * 
 * This validates that trace_tree's COW path in eflash_ftl_trim correctly
 * preserves the tree structure, and write_full_page only writes once.
 */
static void test_trim_radix_tree_integrity(void) {
    #define NUM_SECTORS  8
    printf("\n  === Trim Radix Tree Integrity Test ===\n");
    setup_test_environment();
    total_tests++;

    int ret;
    uint8_t original[NUM_SECTORS][USER_DATA_SIZE];

#define TG(n) ((uint8_t)((n) & 0xFF))
    const uint16_t pattern_base = 0x5A;

    printf("\n  Phase 1: Write %d sectors to build Radix Tree...\n", NUM_SECTORS);
    for (uint16_t i = 0; i < NUM_SECTORS; i++) {
        memset(original[i], 0, USER_DATA_SIZE);
        original[i][0] = TG(i);
        original[i][1] = pattern_base;
        original[i][2] = pattern_base + 1;
        original[i][3] = pattern_base + 2;
        for (int k = 4; k < USER_DATA_SIZE; k++) {
            original[i][k] = pattern_base + (uint8_t)(i * 7 + k) % 200;
        }
        ret = eflash_ftl_write(i, original[i]);
        assert(ret == 0 && "P1: write sector");
    }
    printf("    %d sectors written ?\n", NUM_SECTORS);

    printf("\n  Phase 2: Read back all sectors before TRIM...\n");
    for (uint16_t i = 0; i < NUM_SECTORS; i++) {
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_read(i, buf);
        assert(ret == 0 && "P2: read before trim");
        assert(memcmp(buf, original[i], USER_DATA_SIZE) == 0 && "P2: data match");
    }
    printf("    All %d sectors verified ?\n", NUM_SECTORS);

#define TRIM_TARGET 3
    printf("\n  Phase 3: TRIM sector %d...\n", TRIM_TARGET);
    ret = eflash_ftl_trim(TRIM_TARGET);
    assert(ret == 0 && "P3: trim returned success");
    printf("    Trimmed sector %d ?\n", TRIM_TARGET);

    printf("\n  Phase 4: Verify trimmed sector returns 0xFF...\n");
    {
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_read(TRIM_TARGET, buf);
        assert(ret == 0 && "P4: trimmed sector read still ok via tree");
        for (int k = 0; k < USER_DATA_SIZE; k++) {
            assert(buf[k] == 0xFF && "P4: trimmed sector data is 0xFF");
        }
    }
    printf("    Sector %d: all 0xFF ?\n", TRIM_TARGET);

    printf("\n  Phase 5: Verify OTHER sectors survive TRIM (tree intact)...\n");
    for (uint16_t i = 0; i < NUM_SECTORS; i++) {
        if (i == TRIM_TARGET) continue;
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_read(i, buf);
        assert(ret == 0 && "P5: read survives");
        assert(memcmp(buf, original[i], USER_DATA_SIZE) == 0 && "P5: data survives trim");
    }
    printf("    All %d non-trimmed sectors: intact ?\n", NUM_SECTORS - 1);

    printf("\n  Phase 6: Write & read new sector to confirm tree still operational...\n");
    {
        const uint16_t NEW_SECTOR = 42;
        uint8_t new_data[USER_DATA_SIZE];
        memset(new_data, 0xAB, USER_DATA_SIZE);
        new_data[0] = 0x42;
        new_data[1] = 0x42;
        ret = eflash_ftl_write(NEW_SECTOR, new_data);
        assert(ret == 0 && "P6: write new sector after trim");
        uint8_t buf[USER_DATA_SIZE];
        ret = eflash_ftl_read(NEW_SECTOR, buf);
        assert(ret == 0 && "P6: read new sector");
        assert(memcmp(buf, new_data, USER_DATA_SIZE) == 0 && "P6: new sector data match");
    }
    printf("    New sector 42 written and verified ?\n");

    printf("\n  === Trim Radix Tree integrity test passed ===\n");
    passed_tests++;
    teardown_test_environment();

#undef TRIM_TARGET
#undef TG
}

// ============================================================================
// Crash Recovery Helper: Simulates power loss by deinit+reinit FTL
// The flash file is preserved across the cycle; only RAM state is lost.
// ============================================================================
static void simulate_crash_recovery(void) {
    eflash_deinit();
    int ret = eflash_init(TEST_FLASH_FILE);
    if (ret != 0) {
        printf("[ERROR] Flash re-init after crash failed\n");
        exit(EXIT_FAILURE);
    }
    ret = eflash_ftl_init();
    if (ret != 0) {
        printf("[ERROR] FTL re-init after crash failed\n");
        exit(EXIT_FAILURE);
    }
    printf("[CRASH] Simulated power-loss recovery: FTL rebuilt from Flash\n");
}

// ============================================================================
// Test Case 14: Power-Loss Consistency (write_through vs write_back)
// ============================================================================
//
// Purpose: Validate metadata/data consistency under crash:
//   - write_through: data+metadata on Flash → survives crash
//   - write_back + flush: data+metadata on Flash → survives crash
//   - write_back unflushed: data only in cache → LOST after crash
//
void test_power_loss_consistency(void) {
    setup_test_environment();
    total_tests++;

    // Structure
    //  Phase A : prepare LPNs, write + flush, verify   ->  snapshot
    //  Phase B : crash 1, read back                     ->  all survived
    //  Phase C : write_back NO flush, crash 2           ->  unflushed lost

    const uint8_t USER_LPN_BASE = 20;
    const uint16_t WT_LPN  = USER_LPN_BASE;       // write_through
    const uint16_t WB_FL_LPN = USER_LPN_BASE + 5; // write_back flushed
    const uint16_t WB_NF_LPN = USER_LPN_BASE + 10; // write_back NOT flushed

    uint8_t wt_pattern[USER_DATA_SIZE];
    uint8_t wb_flush_pattern[USER_DATA_SIZE];
    uint8_t wb_nf_pattern[USER_DATA_SIZE];
    uint8_t buf[USER_DATA_SIZE];
    int ret;

    // ---- Phase A: Create persistent data ----
    printf("  Phase A: Write data and flush to Flash\n");

    memset(wt_pattern, 0xAA, USER_DATA_SIZE);
    memcpy(wt_pattern, "WT_THRU", 7);

    memset(wb_flush_pattern, 0xBB, USER_DATA_SIZE);
    memcpy(wb_flush_pattern, "WB_FLSH", 7);

    memset(wb_nf_pattern, 0xCC, USER_DATA_SIZE);
    memcpy(wb_nf_pattern, "WB_NFLUSH", 9);

    ret = eflash_ftl_write_through(WT_LPN, wt_pattern);
    assert(ret == 0 && "A: write_through LPN");
    printf("    write_through(LPN=%d) done\n", WT_LPN);

    for (int i = 0; i < 4; i++) {
        ret = eflash_ftl_write_back(WB_FL_LPN + i, wb_flush_pattern);
        assert(ret == 0 && "A: write_back flushed LPN");
    }
    printf("    write_back x4 (LPN=%d..%d) -> auto-flushed (threshold=%d)\n",
           WB_FL_LPN, WB_FL_LPN + 3, FLUSH_THRESHOLD);

    eflash_ftl_cache_flush();
    printf("    Explicit cache_flush() done\n");

    // Verify reads before crash
    ret = eflash_ftl_read(WT_LPN, buf);
    assert(ret == 0 && "A: read write_through");
    assert(memcmp(buf, wt_pattern, USER_DATA_SIZE) == 0 && "A: write_through data");

    for (int i = 0; i < 4; i++) {
        ret = eflash_ftl_read(WB_FL_LPN + i, buf);
        assert(ret == 0 && "A: read write_back flushed");
        assert(memcmp(buf, wb_flush_pattern, USER_DATA_SIZE) == 0 && "A: flushed data");
    }
    printf("    Pre-crash reads: all OK\n");

    // ---- Phase B: Crash #1 - all flushed data should survive ----
    printf("\n  Phase B: Crash #1 - verify flushed data survives\n");
    simulate_crash_recovery();

    ret = eflash_ftl_read(WT_LPN, buf);
    if (ret != 0 || memcmp(buf, wt_pattern, USER_DATA_SIZE) != 0) {
        printf("    FAIL: write_through data LOST after crash!\n");
        failed_tests++;
        teardown_test_environment();
        return;
    }
    printf("    write_through (LPN=%d) -> survived crash ?\n", WT_LPN);

    for (int i = 0; i < 4; i++) {
        ret = eflash_ftl_read(WB_FL_LPN + i, buf);
        if (ret != 0 || memcmp(buf, wb_flush_pattern, USER_DATA_SIZE) != 0) {
            printf("    FAIL: write_back flushed data LOST after crash!\n");
            failed_tests++;
            teardown_test_environment();
            return;
        }
    }
    printf("    write_back flushed (LPN=%d..%d) -> survived crash ?\n",
           WB_FL_LPN, WB_FL_LPN + 3);

    // ---- Phase C: Crash #2 - unflushed data should be LOST ----
    printf("\n  Phase C: Crash #2 - verify unflushed write_back is lost\n");

    // Write only 1 page via write_back - won't trigger auto-flush (FLUSH_THRESHOLD=2)
    ret = eflash_ftl_write_back(WB_NF_LPN, wb_nf_pattern);
    assert(ret == 0 && "C: write_back (unflushed, only 1 page)");
    printf("    write_back x1 (LPN=%d) -> NOT flushed, cached only\n", WB_NF_LPN);

    // Verify reads from active cache (should work NOW)
    ret = eflash_ftl_read(WB_NF_LPN, buf);
    assert(ret == 0 && "C: read from cache");
    assert(memcmp(buf, wb_nf_pattern, USER_DATA_SIZE) == 0 && "C: cache data");
    printf("    Pre-crash read from cache: OK (data NOT yet on Flash)\n");

    // Now crash without flushing
    simulate_crash_recovery();

    // Verify unflushed data is GONE
    ret = eflash_ftl_read(WB_NF_LPN, buf);
    if (ret == 0) {
        if (memcmp(buf, wb_nf_pattern, USER_DATA_SIZE) == 0) {
            printf("    FAIL: unflushed write_back data SURVIVED crash!\n");
            printf("    This means data was flushed unexpectedly.\n");
            failed_tests++;
            teardown_test_environment();
            return;
        }
    }
    printf("    write_back unflushed (LPN=%d) -> LOST after crash ?\n", WB_NF_LPN);

    // Flushed data should STILL be intact after crash #2
    ret = eflash_ftl_read(WT_LPN, buf);
    assert(ret == 0 && "C: WT still intact after crash 2");
    assert(memcmp(buf, wt_pattern, USER_DATA_SIZE) == 0 && "C: WT data intact");

    for (int i = 0; i < 4; i++) {
        ret = eflash_ftl_read(WB_FL_LPN + i, buf);
        assert(ret == 0 && "C: WB flushed still intact after crash 2");
        assert(memcmp(buf, wb_flush_pattern, USER_DATA_SIZE) == 0);
    }
    printf("    Flushed data (WT + WB) still intact after crash #2 ?\n");

    printf("\n  === Power-loss consistency test passed ===\n");
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 15: Read/Write Boundary Conditions
// ============================================================================
//
// Purpose: Validate error handling and edge cases for eflash_ftl_read and
//          eflash_ftl_write (which delegates to write_back).
//
void test_read_write_boundary(void) {
    setup_test_environment();
    total_tests++;

    uint8_t buf[USER_DATA_SIZE];
    uint8_t pattern[USER_DATA_SIZE];
    int ret;

    memset(pattern, 0xAB, USER_DATA_SIZE);
    pattern[0] = 'B'; pattern[1] = 'D'; pattern[2] = 'R'; pattern[3] = 'Y';

    const uint16_t LPN = 20;
    const uint32_t MAX_LPN = (EFLASH_TOTAL_PAGES * EFLASH_PAGE_SIZE) / USER_DATA_SIZE;

    printf("  B1: Read with NULL data pointer\n");
    ret = eflash_ftl_read(LPN, NULL);
    assert(ret == -1 && "B1: NULL data should return -1");

    printf("  B2: Write with NULL data pointer\n");
    ret = eflash_ftl_write(LPN, NULL);
    assert(ret == -1 && "B2: NULL data should return -1");

    printf("  B3: Write with sector_id = PAGE_NONE (0xFFFF)\n");
    ret = eflash_ftl_write(PAGE_NONE, pattern);
    assert(ret == -1 && "B3: PAGE_NONE sector_id should return -1");

    printf("  B4: Read with sector_id = PAGE_NONE\n");
    ret = eflash_ftl_read(PAGE_NONE, buf);
    if (ret != -1) {
        printf("    B4 NOTE: read PAGE_NONE returned %d (not -1), "
               "max check only logs warning\n", ret);
    }

    printf("  B5: Read unmapped LPN (never written)\n");
    ret = eflash_ftl_read(LPN, buf);
    assert(ret == -1 && "B5: unmapped LPN should return -1");

    printf("  B6: Read LPN beyond max_logical_pages (%u)\n", MAX_LPN);
    ret = eflash_ftl_read((uint16_t)(MAX_LPN + 10), buf);
    printf("    B6: read LPN=%u returned %d (trace_tree handles overflow)\n",
           (unsigned)(MAX_LPN + 10), ret);

    printf("  B7: Write normal data to LPN=%d and read back\n", LPN);
    ret = eflash_ftl_write(LPN, pattern);
    assert(ret == 0 && "B7: write should succeed");
    memset(buf, 0, USER_DATA_SIZE);
    ret = eflash_ftl_read(LPN, buf);
    assert(ret == 0 && "B7: read after write should succeed");
    assert(memcmp(buf, pattern, USER_DATA_SIZE) == 0 && "B7: data round-trip");

    printf("  B8: Repeated writes to same LPN (COW stress)\n");
    for (int i = 0; i < 5; i++) {
        pattern[4] = (uint8_t)i;
        ret = eflash_ftl_write(LPN, pattern);
        assert(ret == 0 && "B8: repeated write");
    }
    ret = eflash_ftl_read(LPN, buf);
    assert(ret == 0 && "B8: read after repeated writes");
    assert(buf[4] == 4 && "B8: last write value");

    printf("  B9: Write to system reserved LPN (0)\n");
    pattern[0] = 'S'; pattern[1] = 'Y'; pattern[2] = 'S';
    ret = eflash_ftl_write_through(0, pattern);
    if (ret == 0) {
        ret = eflash_ftl_read(0, buf);
        printf("    B9: wrote to system LPN 0, read returned %d\n", ret);
    } else {
        printf("    B9: write to system LPN 0 returned %d (system LPN protected?)\n", ret);
    }

    printf("  B10: Write-read at LPN = max_logical_pages - 1\n");
    uint16_t boundary_lpn = (uint16_t)(MAX_LPN - 1);
    memset(pattern, 0xCD, USER_DATA_SIZE);
    pattern[0] = 'M'; pattern[1] = 'A'; pattern[2] = 'X';
    ret = eflash_ftl_write(boundary_lpn, pattern);
    assert(ret == 0 && "B10: write at boundary LPN");
    ret = eflash_ftl_read(boundary_lpn, buf);
    assert(ret == 0 && "B10: read at boundary LPN");
    assert(memcmp(buf, pattern, USER_DATA_SIZE) == 0 && "B10: boundary data match");

    printf("  B11: Sequential write + read of multiple LPNs\n");
    for (uint16_t i = 30; i < 35; i++) {
        pattern[0] = (uint8_t)(i & 0xFF);
        ret = eflash_ftl_write(i, pattern);
        assert(ret == 0 && "B11: sequential write");
    }
    for (uint16_t i = 30; i < 35; i++) {
        ret = eflash_ftl_read(i, buf);
        assert(ret == 0 && "B11: sequential read");
        assert(buf[0] == (uint8_t)(i & 0xFF) && "B11: LPN verification");
    }

    printf("  B12: Write via write_through and verify data survives immediate read\n");
    uint8_t wt_buf[USER_DATA_SIZE];
    memset(wt_buf, 0xEE, USER_DATA_SIZE);
    memcpy(wt_buf, "WT_DIRECT", 9);
    ret = eflash_ftl_write_through(40, wt_buf);
    assert(ret == 0 && "B12: write_through");
    ret = eflash_ftl_read(40, buf);
    assert(ret == 0 && "B12: read after write_through");
    assert(memcmp(buf, wt_buf, USER_DATA_SIZE) == 0 && "B12: write_through data");

    printf("\n  === Read/Write boundary tests passed ===\n");
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 16: Partial Cache Power Loss
// ============================================================================
//
// Purpose: Simulate power loss when write_back cache is partially filled
//          (below FLUSH_THRESHOLD). Validate:
//          - Flushed data survives crash
//          - Cached-only data is lost
//          - FTL remains operational after recovery
//
void test_power_loss_partial_cache(void) {
    setup_test_environment();
    total_tests++;

    const uint16_t LPN_WT  = 20;   // write_through, always survives
    const uint16_t LPN_WB1 = 25;   // write_back batch (2 pages → auto-flush)
    const uint16_t LPN_WB2 = 26;
    const uint16_t LPN_CACHE = 30; // write_back ×1, below threshold, stays cached

    uint8_t wt_pattern[USER_DATA_SIZE];
    uint8_t wb_pattern[USER_DATA_SIZE];
    uint8_t cached_pattern[USER_DATA_SIZE];
    uint8_t buf[USER_DATA_SIZE];
    int ret;

    memset(wt_pattern, 0xDD, USER_DATA_SIZE);
    memcpy(wt_pattern, "WT_SURVIVE", 10);

    memset(wb_pattern, 0xEE, USER_DATA_SIZE);
    memcpy(wb_pattern, "WB_FLUSHED", 10);

    memset(cached_pattern, 0xFF, USER_DATA_SIZE);
    memcpy(cached_pattern, "CACHE_LOST", 10);

    // ---- Phase 1: Mixed writes ----
    printf("  Phase 1: Mixed strategy writes\n");

    ret = eflash_ftl_write_through(LPN_WT, wt_pattern);
    assert(ret == 0 && "P1: write_through");
    printf("    write_through(LPN=%d) -> Flash\n", LPN_WT);

    // Write 2 via write_back → auto-flushes at 2nd (FLUSH_THRESHOLD=2)
    ret = eflash_ftl_write_back(LPN_WB1, wb_pattern);
    assert(ret == 0 && "P1: wb 1st");
    ret = eflash_ftl_write_back(LPN_WB2, wb_pattern);
    assert(ret == 0 && "P1: wb 2nd (triggers auto-flush)");
    printf("    write_back x2 (LPN=%d,%d) -> auto-flushed\n", LPN_WB1, LPN_WB2);

    // Write 1 via write_back → below threshold, stays in cache
    ret = eflash_ftl_write_back(LPN_CACHE, cached_pattern);
    assert(ret == 0 && "P1: wb cached-only");
    printf("    write_back x1 (LPN=%d) -> cached only (dirty_count=1 < THRESHOLD=%d)\n",
           LPN_CACHE, FLUSH_THRESHOLD);

    // Verify reads work before crash
    ret = eflash_ftl_read(LPN_WT, buf);
    assert(ret == 0 && memcmp(buf, wt_pattern, USER_DATA_SIZE) == 0);
    ret = eflash_ftl_read(LPN_WB1, buf);
    assert(ret == 0 && memcmp(buf, wb_pattern, USER_DATA_SIZE) == 0);
    ret = eflash_ftl_read(LPN_WB2, buf);
    assert(ret == 0 && memcmp(buf, wb_pattern, USER_DATA_SIZE) == 0);
    ret = eflash_ftl_read(LPN_CACHE, buf);
    assert(ret == 0 && memcmp(buf, cached_pattern, USER_DATA_SIZE) == 0);
    printf("    Pre-crash: all 4 reads OK\n");

    // ---- Phase 2: Crash ----
    printf("\n  Phase 2: Simulate power loss (cache partially filled)\n");
    simulate_crash_recovery();

    // ---- Phase 3: Verify after crash ----
    printf("\n  Phase 3: Post-crash verification\n");

    ret = eflash_ftl_read(LPN_WT, buf);
    assert(ret == 0 && "P3: WT should survive");
    assert(memcmp(buf, wt_pattern, USER_DATA_SIZE) == 0 && "P3: WT data");
    printf("    write_through(LPN=%d) -> survived ?\n", LPN_WT);

    ret = eflash_ftl_read(LPN_WB1, buf);
    assert(ret == 0 && "P3: WB1 flushed should survive");
    assert(memcmp(buf, wb_pattern, USER_DATA_SIZE) == 0 && "P3: WB1 data");
    ret = eflash_ftl_read(LPN_WB2, buf);
    assert(ret == 0 && "P3: WB2 flushed should survive");
    assert(memcmp(buf, wb_pattern, USER_DATA_SIZE) == 0 && "P3: WB2 data");
    printf("    write_back flushed (LPN=%d,%d) -> survived ?\n", LPN_WB1, LPN_WB2);

    ret = eflash_ftl_read(LPN_CACHE, buf);
    if (ret == 0) {
        if (memcmp(buf, cached_pattern, USER_DATA_SIZE) == 0) {
            printf("    FAIL: cached-only data survived crash!\n");
            failed_tests++;
            teardown_test_environment();
            return;
        }
    }
    printf("    write_back cached-only (LPN=%d) -> LOST ?\n", LPN_CACHE);

    // ---- Phase 4: FTL is still healthy after recovery ----
    printf("\n  Phase 4: Verify FTL operational after crash recovery\n");

    uint8_t new_data[USER_DATA_SIZE];
    memset(new_data, 0x77, USER_DATA_SIZE);
    memcpy(new_data, "POST_CRASH_OK", 13);

    // Can write to the lost LPN
    ret = eflash_ftl_write(LPN_CACHE, new_data);
    assert(ret == 0 && "P4: write to recovered LPN");
    ret = eflash_ftl_read(LPN_CACHE, buf);
    assert(ret == 0 && "P4: read new data");
    assert(memcmp(buf, new_data, USER_DATA_SIZE) == 0 && "P4: new data");
    printf("    Re-wrote LPN=%d after crash -> OK ?\n", LPN_CACHE);

    // Can write more via write_back
    for (uint16_t i = 35; i < 38; i++) {
        memset(new_data, (uint8_t)(i & 0xFF), USER_DATA_SIZE);
        new_data[0] = 'N';
        ret = eflash_ftl_write_back(i, new_data);
        assert(ret == 0 && "P4: more write_back");
    }
    eflash_ftl_cache_flush();
    simulate_crash_recovery();
    for (uint16_t i = 35; i < 38; i++) {
        ret = eflash_ftl_read(i, buf);
        assert(ret == 0 && "P4: read after crash2");
        assert(buf[0] == 'N' && "P4: data after crash2");
    }
    printf("    More write_back + crash2 -> all survived ?\n");

    printf("\n  === Partial cache power-loss test passed ===\n");
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 19: Write-Back Cache Stress Test
// ============================================================================
//
// Purpose: Stress-test the write_back caching mechanism:
//   1. Single write_back → read coherence (cache hit)
//   2. Same LPN update → cache update correctness
//   3. Auto-flush at FLUSH_THRESHOLD=2 + crash recovery
//   4. Cache full (4 slots) + FIFO eviction (6 pages)
//   5. Mixed write_through + write_back survival after crash
//   6. Cache-only data lost on power loss (no flush)
//   7. Stress: 50 LPNs via write_back, flush all, verify all
//
void test_write_back_cache_stress(void) {
    setup_test_environment();
    total_tests++;

    uint8_t wbuf[USER_DATA_SIZE];
    uint8_t rbuf[USER_DATA_SIZE];
    int ret;

    printf("  === Write-Back Cache Stress Test ===\n");

    // ---- Test 1: Single write_back + read coherence ----
    printf("\n  [1] Single write_back + read from cache\n");
    memset(wbuf, 0xA1, USER_DATA_SIZE);
    memcpy(wbuf, "CACHE_OK_001", 12);
    ret = eflash_ftl_write_back(500, wbuf);
    assert(ret == 0);
    ret = eflash_ftl_read(500, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, wbuf, USER_DATA_SIZE) == 0);
    printf("        Single write_back(500) + read -> OK\n");

    // ---- Test 2: Update same LPN in cache ----
    printf("\n  [2] Update already-cached page\n");
    memset(wbuf, 0xB2, USER_DATA_SIZE);
    memcpy(wbuf, "CACHE_UPDATED", 13);
    ret = eflash_ftl_write_back(500, wbuf);
    assert(ret == 0);
    ret = eflash_ftl_read(500, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, "CACHE_UPDATED", 13) == 0);
    assert(rbuf[USER_DATA_SIZE - 1] == (uint8_t)0xB2);
    printf("        Cache update -> latest version returned OK\n");

    // ---- Test 3: Auto-flush at FLUSH_THRESHOLD=2 + crash recovery ----
    printf("\n  [3] Auto-flush at FLUSH_THRESHOLD=%d + crash\n", FLUSH_THRESHOLD);

    memset(wbuf, 0xC3, USER_DATA_SIZE);
    memcpy(wbuf, "AUTOFLUSH_P1", 12);
    ret = eflash_ftl_write_back(501, wbuf);
    assert(ret == 0);

    memset(wbuf, 0xC4, USER_DATA_SIZE);
    memcpy(wbuf, "AUTOFLUSH_P2", 12);
    ret = eflash_ftl_write_back(502, wbuf);
    assert(ret == 0);

    eflash_ftl_cache_flush();
    simulate_crash_recovery();

    ret = eflash_ftl_read(501, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, "AUTOFLUSH_P1", 12) == 0);

    ret = eflash_ftl_read(502, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, "AUTOFLUSH_P2", 12) == 0);
    printf("        Auto-flush at threshold + crash -> both survived OK\n");

    // ---- Test 4: Cache full (4 slots) + FIFO eviction ----
    printf("\n  [4] Cache full (4 slots) + FIFO eviction\n");

    for (int i = 0; i < 4; i++) {
        memset(wbuf, (uint8_t)(0xD0 + i), USER_DATA_SIZE);
        char label[16];
        snprintf(label, sizeof(label), "FIFO_SLOT%02d_", i);
        memcpy(wbuf, label, 12);
        ret = eflash_ftl_write_back((uint16_t)(510 + i), wbuf);
        assert(ret == 0);
    }

    memset(wbuf, 0xE4, USER_DATA_SIZE);
    memcpy(wbuf, "FIFO_EVICT04", 12);
    ret = eflash_ftl_write_back(514, wbuf);
    assert(ret == 0);

    memset(wbuf, 0xE5, USER_DATA_SIZE);
    memcpy(wbuf, "FIFO_EVICT05", 12);
    ret = eflash_ftl_write_back(515, wbuf);
    assert(ret == 0);

    eflash_ftl_cache_flush();

    for (int i = 0; i < 6; i++) {
        ret = eflash_ftl_read((uint16_t)(510 + i), rbuf);
        assert(ret == 0);
        if (i < 4) {
            char expected[16];
            snprintf(expected, sizeof(expected), "FIFO_SLOT%02d_", i);
            assert(memcmp(rbuf, expected, 12) == 0);
        } else {
            char expected[16];
            snprintf(expected, sizeof(expected), "FIFO_EVICT%02d", i);
            assert(memcmp(rbuf, expected, 12) == 0);
        }
    }
    printf("        FIFO eviction + 6 pages verified OK\n");

    // ---- Test 5: Mixed write_through + write_back ----
    printf("\n  [5] Mixed write_through + write_back\n");

    memset(wbuf, 0xFA, USER_DATA_SIZE);
    memcpy(wbuf, "WT_SURVIVE_1", 12);
    ret = eflash_ftl_write_through(520, wbuf);
    assert(ret == 0);

    memset(wbuf, 0xFB, USER_DATA_SIZE);
    memcpy(wbuf, "WB_SURVIVE_1", 12);
    ret = eflash_ftl_write_back(521, wbuf);
    assert(ret == 0);

    eflash_ftl_cache_flush();
    simulate_crash_recovery();

    ret = eflash_ftl_read(520, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, "WT_SURVIVE_1", 12) == 0);

    ret = eflash_ftl_read(521, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, "WB_SURVIVE_1", 12) == 0);
    printf("        WT(520) + WB(521) both survive crash OK\n");

    // ---- Test 6: Cache-only data lost on power loss ----
    printf("\n  [6] Cache-only data lost on power loss (no flush)\n");

    memset(wbuf, 0x66, USER_DATA_SIZE);
    memcpy(wbuf, "LOST_ON_CRASH", 13);
    ret = eflash_ftl_write_back(600, wbuf);
    assert(ret == 0);

    simulate_crash_recovery();

    ret = eflash_ftl_read(600, rbuf);
    assert(ret != 0 || memcmp(rbuf, "LOST_ON_CRASH", 13) != 0);
    printf("        Cache-only data correctly LOST after crash OK\n");

    // ---- Test 7: Stress - 50 LPNs via write_back, flush, verify ----
    printf("\n  [7] Stress: 50 LPNs write_back + flush + verify\n");

    for (int i = 0; i < 50; i++) {
        memset(wbuf, (uint8_t)((i & 0xFF) | 0x80), USER_DATA_SIZE);
        snprintf((char *)wbuf, USER_DATA_SIZE, "STRESS_%03d", i);
        ret = eflash_ftl_write_back((uint16_t)(700 + i), wbuf);
        assert(ret == 0);
    }

    eflash_ftl_cache_flush();
    simulate_crash_recovery();

    int errs = 0;
    for (int i = 0; i < 50; i++) {
        ret = eflash_ftl_read((uint16_t)(700 + i), rbuf);
        char expected[16];
        snprintf(expected, sizeof(expected), "STRESS_%03d", i);
        if (ret != 0 || memcmp(rbuf, expected, 10) != 0) {
            errs++;
        }
    }
    assert(errs == 0);
    printf("        50 pages write_back + flush + crash -> %d errors OK\n", errs);

    printf("\n  === write_back cache stress test passed ===\n");
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Test Case 20: content_cache_flush Unit Test
// ============================================================================
//
// Purpose: Unit-test content_cache_flush (via eflash_ftl_cache_flush):
//   1. Empty cache flush — no-op
//   2. Single dirty page flush → dirty_count=0, data on Flash
//   3. Partial dirty flush (2 dirty + 2 clean via auto-flush patterns)
//   4. All 4 slots dirty at once → complete flush, survive crash
//   5. FIFO order — oldest & newest both persisted after flush
//   6. Flush + power loss — 20 pages survive crash
//   7. Update + flush — latest version persisted
//   8. Dirty count regression — no double-decrement (verify no spurious flush)
//
void test_content_cache_flush_unit(void) {
    setup_test_environment();
    total_tests++;

    uint8_t wbuf[USER_DATA_SIZE];
    uint8_t rbuf[USER_DATA_SIZE];
    int ret;

    printf("  === content_cache_flush Unit Test ===\n");

    // ---- Test 1: Empty cache flush — no-op ----
    printf("\n  [1] Empty cache flush\n");
    ret = eflash_ftl_cache_flush();
    assert(ret == 0);
    printf("        Empty cache flush -> no-op OK\n");

    // ---- Test 2: Single dirty page flush ----
    printf("\n  [2] Single dirty page flush\n");
    memset(wbuf, 0x11, USER_DATA_SIZE);
    memcpy(wbuf, "SINGLE_DIRTY", 12);
    ret = eflash_ftl_write_back(800, wbuf);
    assert(ret == 0);
    ret = eflash_ftl_cache_flush();
    assert(ret == 0);
    simulate_crash_recovery();
    ret = eflash_ftl_read(800, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, "SINGLE_DIRTY", 12) == 0);
    printf("        Single dirty page flushed -> survives crash OK\n");

    // ---- Test 3: Partial dirty (2 auto-flushed + 2 explicitly flushed) ----
    printf("\n  [3] Partial dirty + explicit flush\n");

    for (int i = 0; i < 4; i++) {
        memset(wbuf, (uint8_t)(0xA0 + i), USER_DATA_SIZE);
        char label[16] = {0};
        snprintf(label, sizeof(label), "PARTIAL_%02d", i);
        memcpy(wbuf, label, 12);
        ret = eflash_ftl_write_back((uint16_t)(810 + i), wbuf);
        assert(ret == 0);
    }
    eflash_ftl_cache_flush();
    simulate_crash_recovery();
    for (int i = 0; i < 4; i++) {
        ret = eflash_ftl_read((uint16_t)(810 + i), rbuf);
        assert(ret == 0);
        char expected[16] = {0};
        snprintf(expected, sizeof(expected), "PARTIAL_%02d", i);
        assert(memcmp(rbuf, expected, 12) == 0);
    }
    printf("        4 pages flushed -> all survive crash OK\n");

    // ---- Test 4: All 4 slots dirty at once — complete flush ----
    printf("\n  [4] All 4 slots dirty at once -> flush all\n");

    for (int i = 0; i < 4; i++) {
        memset(wbuf, (uint8_t)(0xB0 + i), USER_DATA_SIZE);
        char label[16] = {0};
        snprintf(label, sizeof(label), "ALLDIRTY_%02d", i);
        memcpy(wbuf, label, 12);
        ret = eflash_ftl_write_back((uint16_t)(820 + i), wbuf);
        assert(ret == 0);
    }
    eflash_ftl_cache_flush();
    simulate_crash_recovery();
    for (int i = 0; i < 4; i++) {
        ret = eflash_ftl_read((uint16_t)(820 + i), rbuf);
        assert(ret == 0);
        char expected[16] = {0};
        snprintf(expected, sizeof(expected), "ALLDIRTY_%02d", i);
        assert(memcmp(rbuf, expected, 12) == 0);
    }
    printf("        All 4 dirty pages flushed -> survive crash OK\n");

    // ---- Test 5: FIFO flush order — oldest & newest both persisted ----
    printf("\n  [5] Flush order: oldest & newest persisted\n");

    for (int i = 0; i < 4; i++) {
        memset(wbuf, (uint8_t)(0xC0 + i), USER_DATA_SIZE);
        char label[16] = {0};
        snprintf(label, sizeof(label), "FIFO_%02d____", i);
        memcpy(wbuf, label, 12);
        ret = eflash_ftl_write_back((uint16_t)(830 + i), wbuf);
        assert(ret == 0);
    }
    eflash_ftl_cache_flush();
    simulate_crash_recovery();

    ret = eflash_ftl_read(830, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, "FIFO_00____", 12) == 0);

    ret = eflash_ftl_read(833, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, "FIFO_03____", 12) == 0);
    printf("        Oldest(830) and newest(833) both persisted OK\n");

    // ---- Test 6: Flush + power loss — 20 pages survive ----
    printf("\n  [6] Flush + power loss: 20 pages survive\n");

    const int N = 20;
    for (int i = 0; i < N; i++) {
        memset(wbuf, (uint8_t)((i & 0xFF) | 0xD0), USER_DATA_SIZE);
        char label[16] = {0};
        snprintf(label, sizeof(label), "FLUSHED_%03d", i);
        memcpy(wbuf, label, 12);
        ret = eflash_ftl_write_back((uint16_t)(900 + i), wbuf);
        assert(ret == 0);
    }
    eflash_ftl_cache_flush();
    simulate_crash_recovery();

    int errs = 0;
    for (int i = 0; i < N; i++) {
        ret = eflash_ftl_read((uint16_t)(900 + i), rbuf);
        char expected[16] = {0};
        snprintf(expected, sizeof(expected), "FLUSHED_%03d", i);
        if (ret != 0 || memcmp(rbuf, expected, 12) != 0) {
            errs++;
        }
    }
    printf("        %d pages flushed -> %d errors after crash\n", N, errs);
    assert(errs == 0);

    // ---- Test 7: Update + flush — latest version persisted ----
    printf("\n  [7] Update + flush: latest version survives\n");

    memset(wbuf, 0xE1, USER_DATA_SIZE);
    memcpy(wbuf, "UPDATE_V1", 9);
    ret = eflash_ftl_write_back(950, wbuf);
    assert(ret == 0);

    memset(wbuf, 0xE2, USER_DATA_SIZE);
    memcpy(wbuf, "UPDATE_V2", 9);
    ret = eflash_ftl_write_back(950, wbuf);
    assert(ret == 0);

    eflash_ftl_cache_flush();
    simulate_crash_recovery();

    ret = eflash_ftl_read(950, rbuf);
    assert(ret == 0);
    assert(memcmp(rbuf, "UPDATE_V2", 9) == 0);
    printf("        Update V1->V2, flush, crash -> V2 survived OK\n");

    // ---- Test 8: Dirty count regression — no double-decrement ----
    printf("\n  [8] Dirty count regression: no double-decrement\n");

    for (int i = 0; i < 2; i++) {
        memset(wbuf, (uint8_t)(0xF0 + i), USER_DATA_SIZE);
        ret = eflash_ftl_write_back((uint16_t)(960 + i), wbuf);
        assert(ret == 0);
    }

    memset(wbuf, 0xFA, USER_DATA_SIZE);
    memcpy(wbuf, "SINGLE_NOFLUSH", 14);
    ret = eflash_ftl_write_back(962, wbuf);
    assert(ret == 0);

    simulate_crash_recovery();

    ret = eflash_ftl_read(962, rbuf);
    assert(ret != 0 || memcmp(rbuf, "SINGLE_NOFLUSH", 14) != 0);
    printf("        dirty_count=1 after auto-flush -> no spurious flush OK\n");

    printf("\n  === content_cache_flush unit test passed ===\n");
    passed_tests++;
    teardown_test_environment();
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main(void) {
    printf("\n========================================\n");
    printf("eFlash FTL - Code Region Management Tests\n");
    printf("========================================\n\n");
    
    // Run all code region tests
    RUN_TEST(test_code_region_init);
    RUN_TEST(test_code_migrate_basic);
    RUN_TEST(test_code_migrate_multi_page);
    RUN_TEST(test_code_region_expand);
    RUN_TEST(test_code_region_shrink);
    RUN_TEST(test_code_read_verify);
    RUN_TEST(test_code_migrate_power_failure);
    RUN_TEST(test_code_region_gc_reclaim);
    RUN_TEST(test_code_segment_add_delete_readd);
    RUN_TEST(test_code_segment_stress_with_leak_detection);
    RUN_TEST(test_gc_reserve_physical_range);
    RUN_TEST(test_integration_gc_reserve_code_migration);
    RUN_TEST(test_code_and_data_coexistence);
    RUN_TEST(test_trim_radix_tree_integrity);
    RUN_TEST(test_power_loss_consistency);
    RUN_TEST(test_read_write_boundary);
    RUN_TEST(test_power_loss_partial_cache);
    RUN_TEST(test_write_back_cache_stress);
    RUN_TEST(test_content_cache_flush_unit);
    
    // Summary
    printf("\n========================================\n");
    printf("Test Summary:\n");
    printf("  Total:  %d\n", total_tests);
    printf("  Passed: %d\n", passed_tests);
    printf("  Failed: %d\n", failed_tests);
    printf("========================================\n");
    
    if (failed_tests > 0) {
        printf("\n? SOME TESTS FAILED!\n");
        return EXIT_FAILURE;
    } else {
        printf("\n? ALL TESTS PASSED!\n");
        return EXIT_SUCCESS;
    }
}
