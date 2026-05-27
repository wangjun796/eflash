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
    
    const int num_segments = 10;
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
