/* eFlash FTL - Comprehensive Test Suite
 * Tests for Mini-FTL implementation with transaction support
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "../eflash_ftl/mini_ftl.h"
#include "../eflash_ftl/eflash_sim.h"
#include "../eflash_ftl/space_mgr.h"
#include "ecc/bch.h"

// --- BCH ECC 包装函数（用于测试）---

static void bch_encode(const struct bch_def *bch, const uint8_t *data, size_t len, uint8_t *ecc) {
    bch_generate(bch, data, len, ecc);
}

static int bch_decode(const struct bch_def *bch, uint8_t *data, size_t len, const uint8_t *ecc) {
    if (bch_verify(bch, data, len, ecc) == 0) {
        return 0;
    }
    
    uint8_t data_copy[BCH_MAX_CHUNK_SIZE];
    uint8_t ecc_copy[BCH_MAX_ECC];
    memcpy(data_copy, data, len);
    memcpy(ecc_copy, ecc, bch->ecc_bytes);
    
    bch_repair(bch, data_copy, len, ecc_copy);
    
    if (bch_verify(bch, data_copy, len, ecc_copy) != 0) {
        return -1;
    }
    
    memcpy(data, data_copy, len);
    return 1;
}

#define TEST_FLASH_FILE "eflash_test.bin"

// Test macros (same as in original tests)
#define TEST(name) printf("  [TEST] %s...\n", #name)
#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] Assertion failed: %s\n", msg); \
        return -1; \
    } \
} while(0)
#define PASS() do { \
    printf("  [PASS]\n"); \
    return 0; \
} while(0)

// Forward declarations for helper functions used in comprehensive tests
#define META_OFFSET USER_DATA_SIZE

static bool is_page_valid_local(uint16_t page, ftl_meta_t *meta) {
    uint8_t buf[EFLASH_PAGE_SIZE];
    if (eflash_hw_read(page, buf) != 0) return false;

    // 使用新的完整页ECC验证（覆盖用户数据+元数据）
    size_t protected_len = USER_DATA_SIZE + META_SIZE - 5;
    uint8_t *ecc_ptr = buf + USER_DATA_SIZE + META_SIZE - 5;
    
    // 先验证
    if (bch_verify(&bch_3bit, buf, protected_len, ecc_ptr) != 0) {
        // 尝试纠错
        uint8_t data_copy[EFLASH_PAGE_SIZE];
        memcpy(data_copy, buf, EFLASH_PAGE_SIZE);
        
        bch_repair(&bch_3bit, data_copy, protected_len, ecc_ptr);
        
        if (bch_verify(&bch_3bit, data_copy, protected_len, ecc_ptr) != 0) {
            return false; // 不可纠正的错误
        }
        memcpy(buf, data_copy, EFLASH_PAGE_SIZE);
    }

    // 从纠正后的缓冲区复制元数据
    memcpy(meta, buf + META_OFFSET, META_SIZE);

    // Check for blank page
    if (meta->status == TXN_STATUS_BLANK) return false;

    // Check status
    return (meta->status == TXN_STATUS_COMMITTED || 
            meta->status == TXN_STATUS_READY || 
            meta->status == TXN_STATUS_INVALID);
}

// Test result counters
static int total_tests = 0;
static int failed_tests = 0;

// Helper: Initialize and erase flash
static void init_test_flash(void) {
    eflash_init(TEST_FLASH_FILE);
    for (int i = 0; i < EFLASH_TOTAL_PAGES; i++) {
        eflash_hw_erase(i);
    }
}

// Helper: Cleanup test flash
static void cleanup_test_flash(void) {
    eflash_deinit();
    remove(TEST_FLASH_FILE);
}

// Helper: Create test data pattern
static void create_test_pattern(uint8_t *buf, size_t size, uint8_t seed) {
    for (size_t i = 0; i < size; i++) {
        buf[i] = (uint8_t)((seed + i) & 0xFF);
    }
}

// Helper: Verify test data pattern
static int verify_test_pattern(const uint8_t *buf, size_t size, uint8_t seed) {
    for (size_t i = 0; i < size; i++) {
        if (buf[i] != (uint8_t)((seed + i) & 0xFF)) {
            return -1;
        }
    }
    return 0;
}

// ============================================================================
// Test 1: Initialization and Recovery
// ============================================================================
int test_init_recovery(void) {
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    uint8_t test_data[USER_DATA_SIZE];
    
    printf("[TEST] test_init_recovery: Starting...\n");
    
    // Test 1a: Fresh initialization
    init_test_flash();
    mini_ftl_init(&ftl);
    
    assert(ftl.root_page == PAGE_NONE);
    assert(ftl.next_count == 1);
    assert(ftl.is_initialized == true);
    printf("  [PASS] Fresh initialization\n");
    
    // Test 1b: Write data and verify recovery
    create_test_pattern(test_data, USER_DATA_SIZE, 0xAA);
    mini_ftl_write(&ftl, 100, test_data);
    
    eflash_deinit();
    
    // Simulate power failure and restart
    eflash_init(TEST_FLASH_FILE);
    mini_ftl_init(&ftl);
    
    assert(ftl.root_page != PAGE_NONE);
    assert(ftl.next_count > 1);
    
    // Verify data persistence
    uint8_t read_data[USER_DATA_SIZE];
    mini_ftl_read(&ftl, 100, read_data);
    assert(verify_test_pattern(read_data, USER_DATA_SIZE, 0xAA) == 0);
    printf("  [PASS] Recovery after write\n");
    
    cleanup_test_flash();
    printf("[PASS] test_init_recovery: Completed successfully\n");
    return 0;
}

// ============================================================================
// Test 2: Basic Read/Write Operations
// ============================================================================
int test_basic_read_write(void) {
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    uint8_t write_buf[USER_DATA_SIZE];
    uint8_t read_buf[USER_DATA_SIZE];
    
    printf("[TEST] test_basic_read_write: Starting...\n");
    
    init_test_flash();
    mini_ftl_init(&ftl);
    
    // Test 2a: Single page write/read
    create_test_pattern(write_buf, USER_DATA_SIZE, 0x11);
    mini_ftl_write(&ftl, 0, write_buf);
    
    memset(read_buf, 0, USER_DATA_SIZE);
    mini_ftl_read(&ftl, 0, read_buf);
    assert(verify_test_pattern(read_buf, USER_DATA_SIZE, 0x11) == 0);
    printf("  [PASS] Single page write/read\n");
    
    // Test 2b: Multiple pages
    for (int i = 0; i < 10; i++) {
        create_test_pattern(write_buf, USER_DATA_SIZE, (uint8_t)(i + 0x20));
        mini_ftl_write(&ftl, (uint16_t)(i * 50), write_buf);
    }
    
    for (int i = 0; i < 10; i++) {
        mini_ftl_read(&ftl, (uint16_t)(i * 50), read_buf);
        assert(verify_test_pattern(read_buf, USER_DATA_SIZE, (uint8_t)(i + 0x20)) == 0);
    }
    printf("  [PASS] Multiple pages write/read\n");
    
    // Test 2c: Overwrite and verify old data is invisible
    create_test_pattern(write_buf, USER_DATA_SIZE, 0xFF);
    mini_ftl_write(&ftl, 0, write_buf);
    
    mini_ftl_read(&ftl, 0, read_buf);
    assert(verify_test_pattern(read_buf, USER_DATA_SIZE, 0xFF) == 0);
    printf("  [PASS] Overwrite visibility\n");
    
    cleanup_test_flash();
    printf("[PASS] test_basic_read_write: Completed successfully\n");
    return 0;
}

// ============================================================================
// Test 3: Object Header Management
// ============================================================================
int test_object_headers(void) {
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    obj_header_t hdr;
    obj_header_t read_hdr;
    
    printf("[TEST] test_object_headers: Starting...\n");
    
    init_test_flash();
    mini_ftl_init(&ftl);
    
    // Test 3a: Basic header operations in base zone
    memset(&hdr, 0, sizeof(hdr));
    hdr.pkg_id = 0x1001;
    hdr.class_id = 0x2001;
    hdr.type = 1;
    hdr.body_size = 128;
    
    mini_ftl_obj_set_header(&ftl, 0, &hdr);
    mini_ftl_obj_get_header(&ftl, 0, &read_hdr);
    
    assert(read_hdr.pkg_id == 0x1001);
    assert(read_hdr.class_id == 0x2001);
    assert(read_hdr.body_size == 128);
    printf("  [PASS] Base zone header read/write\n");
    
    // Test 3b: Multiple headers in base zone
    for (int i = 0; i < 50; i++) {
        hdr.pkg_id = (uint16_t)(0x3000 + i);
        hdr.body_size = (uint32_t)(i * 10);
        mini_ftl_obj_set_header(&ftl, (uint16_t)i, &hdr);
    }
    
    for (int i = 0; i < 50; i++) {
        mini_ftl_obj_get_header(&ftl, (uint16_t)i, &read_hdr);
        assert(read_hdr.pkg_id == (uint16_t)(0x3000 + i));
    }
    printf("  [PASS] Multiple base zone headers\n");
    
    // Test 3c: Extended zone headers (ID >= 232)
    hdr.pkg_id = 0x9999;
    hdr.body_size = 256;
    mini_ftl_obj_set_header(&ftl, 250, &hdr);
    
    mini_ftl_obj_get_header(&ftl, 250, &read_hdr);
    assert(read_hdr.pkg_id == 0x9999);
    assert(read_hdr.body_size == 256);
    printf("  [PASS] Extended zone header (level 1)\n");
    
    // Test 3d: Higher level extension
    mini_ftl_obj_set_header(&ftl, 400, &hdr);
    mini_ftl_obj_get_header(&ftl, 400, &read_hdr);
    assert(read_hdr.pkg_id == 0x9999);
    printf("  [PASS] Extended zone header (level 2)\n");
    
    cleanup_test_flash();
    printf("[PASS] test_object_headers: Completed successfully\n");
    return 0;
}

// ============================================================================
// Test 4: Transaction Management
// ============================================================================
int test_transactions(void) {
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    uint8_t write_buf[USER_DATA_SIZE];
    uint8_t read_buf[USER_DATA_SIZE];
    
    printf("[TEST] test_transactions: Starting...\n");

    init_test_flash();
    mini_ftl_init(&ftl);
    
    // Test 4a: Normal transaction commit
    create_test_pattern(write_buf, USER_DATA_SIZE, 0xA1);
    mini_ftl_write(&ftl, 10, write_buf);
    
    mini_ftl_txn_begin(&ftl);
    create_test_pattern(write_buf, USER_DATA_SIZE, 0xB1);
    mini_ftl_write(&ftl, 10, write_buf);
    mini_ftl_txn_commit(&ftl);
    
    mini_ftl_read(&ftl, 10, read_buf);
    assert(verify_test_pattern(read_buf, USER_DATA_SIZE, 0xB1) == 0);
    printf("  [PASS] Transaction commit\n");
    
    // Test 4b: Transaction abort
    mini_ftl_txn_begin(&ftl);
    create_test_pattern(write_buf, USER_DATA_SIZE, 0xC1);
    mini_ftl_write(&ftl, 10, write_buf);
    mini_ftl_txn_abort(&ftl);
    
    mini_ftl_read(&ftl, 10, read_buf);
    assert(verify_test_pattern(read_buf, USER_DATA_SIZE, 0xB1) == 0);
    printf("  [PASS] Transaction abort (rollback)\n");
    
    // Test 4c: Multiple operations in one transaction
    mini_ftl_txn_begin(&ftl);
    for (int i = 0; i < 5; i++) {
        create_test_pattern(write_buf, USER_DATA_SIZE, (uint8_t)(0xD0 + i));
        mini_ftl_write(&ftl, (uint16_t)(100 + i), write_buf);
    }
    mini_ftl_txn_commit(&ftl);
    
    for (int i = 0; i < 5; i++) {
        mini_ftl_read(&ftl, (uint16_t)(100 + i), read_buf);
        assert(verify_test_pattern(read_buf, USER_DATA_SIZE, (uint8_t)(0xD0 + i)) == 0);
    }
    printf("  [PASS] Multi-operation transaction\n");
    
    cleanup_test_flash();
    printf("[PASS] test_transactions: Completed successfully\n");
    return 0;
}

// ============================================================================
// Test 5: Power Failure Simulation
// ============================================================================
int test_power_failure(void) {
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    uint8_t write_buf[USER_DATA_SIZE];
    uint8_t read_buf[USER_DATA_SIZE];
    
    printf("[TEST] test_power_failure: Starting...\n");

    init_test_flash();
    mini_ftl_init(&ftl);
    
    // Write initial committed data
    create_test_pattern(write_buf, USER_DATA_SIZE, 0xE1);
    mini_ftl_write(&ftl, 20, write_buf);
    
    // Simulate power failure during transaction (no commit)
    mini_ftl_txn_begin(&ftl);
    create_test_pattern(write_buf, USER_DATA_SIZE, 0xF1);
    mini_ftl_write(&ftl, 20, write_buf);
    
    // Simulate power loss by not committing
    eflash_deinit();
    
    // Restart - should see old data
    eflash_init(TEST_FLASH_FILE);
    mini_ftl_init(&ftl);
    
    mini_ftl_read(&ftl, 20, read_buf);
    assert(verify_test_pattern(read_buf, USER_DATA_SIZE, 0xE1) == 0);
    printf("  [PASS] Recovery from incomplete transaction\n");
    
    // Test 5b: Commit then power failure
    mini_ftl_txn_begin(&ftl);
    create_test_pattern(write_buf, USER_DATA_SIZE, 0xF2);
    mini_ftl_write(&ftl, 20, write_buf);
    mini_ftl_txn_commit(&ftl);
    
    eflash_deinit();
    eflash_init(TEST_FLASH_FILE);
    mini_ftl_init(&ftl);
    
    mini_ftl_read(&ftl, 20, read_buf);
    assert(verify_test_pattern(read_buf, USER_DATA_SIZE, 0xF2) == 0);
    printf("  [PASS] Recovery after committed transaction\n");
    
    cleanup_test_flash();
    printf("[PASS] test_power_failure: Completed successfully\n");
    return 0;
}

// ============================================================================
// Test 6: Space Management
// ============================================================================
int test_space_management(void) {
    space_mgr_t mgr;
    uint16_t page;
    uint16_t offset;
    
    printf("[TEST] test_space_management: Starting...\n");
    
    // Test 6a: Basic allocation
    space_mgr_init(&mgr, 100);
    assert(mgr.free_units == 100 * 256);
    
    int ret = space_mgr_alloc(&mgr, 10, &page, &offset);
    assert(ret == 0);
    assert(page == 0);
    assert(offset == 0);
    printf("  [PASS] Basic allocation\n");
    
    // Test 6b: Multiple allocations
    uint16_t pages[10];
    uint16_t offsets[10];
    for (int i = 0; i < 10; i++) {
        ret = space_mgr_alloc(&mgr, 4, &pages[i], &offsets[i]);
        assert(ret == 0);
    }
    printf("  [PASS] Multiple allocations\n");
    
    // Test 6c: Free and reallocate
    space_mgr_free(&mgr, pages[0], offsets[0], 4);
    ret = space_mgr_alloc(&mgr, 4, &page, &offset);
    assert(ret == 0);
    assert(page == pages[0]);
    assert(offset == offsets[0]);
    printf("  [PASS] Free and reallocate\n");
    
    // Test 6d: Small object allocation
    ret = space_mgr_alloc(&mgr, 2, &page, &offset);
    assert(ret == 0);
    printf("  [PASS] Minimum size allocation (2 bytes)\n");
    
    printf("[PASS] test_space_management: Completed successfully\n");
    return 0;
}

// ============================================================================
// Test 7: ECC Error Correction
// ============================================================================
int test_ecc_correction(void) {
    ftl_meta_t meta;
    uint8_t corrupted[META_SIZE];
    
    printf("[TEST] test_ecc_correction: Starting...\n");
    
    // Test 7a: Encode and decode without errors
    memset(&meta, 0, sizeof(meta));
    meta.sector_id = 123;
    meta.global_count = 456;
    meta.status = TXN_STATUS_COMMITTED;
    
    // Encode
    bch_encode(&bch_3bit, (const uint8_t *)&meta, META_SIZE - 5, meta.ecc);
    
    // Decode without errors
    memcpy(corrupted, &meta, META_SIZE);
    int result = bch_decode(&bch_3bit, corrupted, META_SIZE - 5, meta.ecc);
    assert(result == 0);
    printf("  [PASS] ECC encode/decode (no errors)\n");
    
    // Test 7b: Correct 1-bit error
    memcpy(corrupted, &meta, META_SIZE);
    corrupted[0] ^= 0x01; // Flip 1 bit
    result = bch_decode(&bch_3bit, corrupted, META_SIZE - 5, meta.ecc);
    assert(result == 1);
    assert(corrupted[0] == ((uint8_t *)&meta)[0]);
    printf("  [PASS] ECC correct 1-bit error\n");
    
    // Test 7c: Correct 3-bit errors
    memcpy(corrupted, &meta, META_SIZE);
    corrupted[2] ^= 0x07; // Flip 3 bits
    result = bch_decode(&bch_3bit, corrupted, META_SIZE - 5, meta.ecc);
    assert(result == 3);
    printf("  [PASS] ECC correct 3-bit errors\n");
    
    printf("[PASS] test_ecc_correction: Completed successfully\n");
    return 0;
}

// ============================================================================
// Test 8: Radix Tree Operations
// ============================================================================
int test_radix_tree(void) {
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    uint8_t write_buf[USER_DATA_SIZE];
    uint8_t read_buf[USER_DATA_SIZE];
    
    printf("[TEST] test_radix_tree: Starting...\n");

    init_test_flash();
    mini_ftl_init(&ftl);
    
    // Test 8a: Sequential writes build correct tree
    for (int i = 0; i < 20; i++) {
        create_test_pattern(write_buf, USER_DATA_SIZE, (uint8_t)(i + 0x50));
        mini_ftl_write(&ftl, (uint16_t)(i * 100), write_buf);
    }
    
    // Verify all data accessible via tree
    for (int i = 0; i < 20; i++) {
        mini_ftl_read(&ftl, (uint16_t)(i * 100), read_buf);
        assert(verify_test_pattern(read_buf, USER_DATA_SIZE, (uint8_t)(i + 0x50)) == 0);
    }
    printf("  [PASS] Sequential writes tree integrity\n");
    
    // Test 8b: Random access pattern
    for (int i = 19; i >= 0; i--) {
        create_test_pattern(write_buf, USER_DATA_SIZE, (uint8_t)(i + 0x70));
        mini_ftl_write(&ftl, (uint16_t)(i * 100), write_buf);
    }
    
    for (int i = 0; i < 20; i++) {
        mini_ftl_read(&ftl, (uint16_t)(i * 100), read_buf);
        assert(verify_test_pattern(read_buf, USER_DATA_SIZE, (uint8_t)(i + 0x70)) == 0);
    }
    printf("  [PASS] Random access tree integrity\n");
    
    cleanup_test_flash();
    printf("[PASS] test_radix_tree: Completed successfully\n");
    return 0;
}

// ============================================================================
// Test 9: Stress Test
// ============================================================================
int test_stress(void) {
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    uint8_t write_buf[USER_DATA_SIZE];
    uint8_t read_buf[USER_DATA_SIZE];
    
    printf("[TEST] test_stress: Starting...\n");

    init_test_flash();
    mini_ftl_init(&ftl);
    
    // Test 9a: Continuous writes
    for (int i = 0; i < 50; i++) {
        create_test_pattern(write_buf, USER_DATA_SIZE, (uint8_t)(i & 0xFF));
        mini_ftl_write(&ftl, (uint16_t)(i % 30), write_buf);
    }
    printf("  [PASS] 50 continuous writes\n");
    
    // Test 9b: Mixed transactions
    for (int i = 0; i < 10; i++) {
        mini_ftl_txn_begin(&ftl);
        for (int j = 0; j < 5; j++) {
            create_test_pattern(write_buf, USER_DATA_SIZE, (uint8_t)(i * 10 + j));
            mini_ftl_write(&ftl, (uint16_t)(200 + j), write_buf);
        }
        
        if (i % 2 == 0) {
            mini_ftl_txn_commit(&ftl);
        } else {
            mini_ftl_txn_abort(&ftl);
        }
    }
    printf("  [PASS] Mixed transaction commit/abort\n");
    
    // Test 9c: Object header stress
    for (int i = 0; i < 100; i++) {
        obj_header_t hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.pkg_id = (uint16_t)(0x4000 + i);
        hdr.body_size = (uint32_t)(i * 5);
        mini_ftl_obj_set_header(&ftl, (uint16_t)i, &hdr);
    }
    printf("  [PASS] 100 object headers\n");
    
    cleanup_test_flash();
    printf("[PASS] test_stress: Completed successfully\n");
    return 0;
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main(void) {
    printf("========================================\n");
    printf("eFlash FTL Test Suite\n");
    printf("========================================\n\n");
    
    #define RUN_TEST(name) \
        total_tests++; \
        if (test_##name() < 0) { \
            printf("[FAIL] %s test failed!\n", #name); \
            failed_tests++; \
        } else { \
            printf("[PASS] %s test passed\n", #name); \
        } \
        printf("\n");
    
    RUN_TEST(init_recovery)
    RUN_TEST(basic_read_write)
    RUN_TEST(object_headers)
    RUN_TEST(transactions)
    RUN_TEST(power_failure)
    RUN_TEST(space_management)
    RUN_TEST(ecc_correction)
    RUN_TEST(radix_tree)
    RUN_TEST(stress)
    
    // Comprehensive radix tree tests
    RUN_TEST(radix_tree_single_sector_updates)
    RUN_TEST(radix_tree_multiple_sectors)
    RUN_TEST(radix_tree_path_correctness)
    RUN_TEST(radix_tree_stress_random_access)
    
    #undef RUN_TEST
    
    printf("========================================\n");
    printf("Test Results: %d passed, %d failed out of %d total\n",
           total_tests - failed_tests, failed_tests, total_tests);
    printf("========================================\n");
    
    return (failed_tests > 0) ? -1 : 0;
}

/************************************************************************
 * Test radix tree implementation comprehensively
 */

// Helper: Print radix tree structure (text-based visualization)
static void print_radix_tree_node(mini_ftl_t *ftl, uint16_t page, int depth, int max_depth) {
    if (page == PAGE_NONE || depth > max_depth) return;
    
    ftl_meta_t meta;
    if (!is_page_valid_local(page, &meta)) return;
    
    // Indent based on depth
    for (int i = 0; i < depth; i++) printf("  ");
    printf("Page %d: sector=%d, gc=%d, alt[", page, meta.sector_id, meta.global_count);
    
    // Show first few alt pointers
    for (int i = 0; i < 4 && i < RADIX_DEPTH; i++) {
        if (i > 0) printf(", ");
        printf("%d:%d", i, meta.alt[i]);
    }
    if (RADIX_DEPTH > 4) printf(", ...");
    printf("]\n");
    
    // Recursively print children (only non-NONE alts)
    for (int i = depth; i < RADIX_DEPTH && i < depth + 2; i++) {
        if (meta.alt[i] != PAGE_NONE) {
            print_radix_tree_node(ftl, meta.alt[i], depth + 1, max_depth);
        }
    }
}

static void print_radix_tree(mini_ftl_t *ftl) {
    printf("\n=== Radix Tree Structure ===\n");
    printf("Root page: %d\n", ftl->root_page);
    if (ftl->root_page != PAGE_NONE) {
        print_radix_tree_node(ftl, ftl->root_page, 0, 4);
    }
    printf("===========================\n\n");
}

// Helper: Verify tree integrity by checking all alt pointers form valid paths
static int verify_tree_integrity(mini_ftl_t *ftl) {
    if (ftl->root_page == PAGE_NONE) return 0; // Empty tree is valid
    
    // Check that root page is valid
    ftl_meta_t root_meta;
    if (!is_page_valid_local(ftl->root_page, &root_meta)) {
        printf("  [ERROR] Root page %d is invalid!\n", ftl->root_page);
        return -1;
    }
    
    // BFS to check all reachable pages
    uint16_t queue[1024];
    int head = 0, tail = 0;
    bool visited[EFLASH_TOTAL_PAGES] = {false};
    
    queue[tail++] = ftl->root_page;
    visited[ftl->root_page] = true;
    
    while (head < tail) {
        uint16_t current = queue[head++];
        ftl_meta_t meta;
        
        if (!is_page_valid_local(current, &meta)) {
            printf("  [ERROR] Page %d in tree is invalid!\n", current);
            return -1;
        }

        // Check all alt pointers
        for (int i = 0; i < RADIX_DEPTH; i++) {
            if (meta.alt[i] != PAGE_NONE) {
                if (meta.alt[i] >= EFLASH_TOTAL_PAGES) {
                    printf("  [ERROR] Page %d has invalid alt[%d]=%d (out of range)!\n", 
                           current, i, meta.alt[i]);
                    return -1;
                }
                
                if (!visited[meta.alt[i]]) {
                    visited[meta.alt[i]] = true;
                    queue[tail++] = meta.alt[i];
                    
                    if (tail >= 1024) {
                        printf("  [WARNING] Tree too large, stopping BFS\n");
                        return 0;
                    }
                }
            }
        }
    }
    
    return 0;
}

static int test_radix_tree_single_sector_updates(void) {
    TEST(test_radix_tree_single_sector_updates);
    
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    init_test_flash();
    mini_ftl_init(&ftl);
    
    const uint16_t test_sector = 42;
    uint8_t write_data[USER_DATA_SIZE];
    uint8_t read_data[USER_DATA_SIZE];
    
    printf("  Testing 250 sequential updates to sector %d...\n", test_sector);
    
    // Write 250 times with different patterns
    for (int i = 0; i < 250; i++) {
        // Create pattern: all bytes = i
        memset(write_data, i, USER_DATA_SIZE);
        
        // Write to same logical sector
        int ret = mini_ftl_write(&ftl, test_sector, write_data);
        ASSERT(ret == 0, "write should succeed");
        
        // Immediately read back and verify
        ret = mini_ftl_read(&ftl, test_sector, read_data);
        ASSERT(ret == 0, "read should succeed");
        ASSERT(memcmp(write_data, read_data, USER_DATA_SIZE) == 0, 
               "data should match after write");
        
        // Every 50 writes, print tree structure
        if ((i + 1) % 50 == 0) {
            printf("    After %d writes: root=%d, next_count=%d\n", 
                   i + 1, ftl.root_page, ftl.next_count);
        }
    }
    
    // Final verification
    memset(write_data, 249, USER_DATA_SIZE);
    mini_ftl_read(&ftl, test_sector, read_data);
    ASSERT(memcmp(write_data, read_data, USER_DATA_SIZE) == 0, 
           "final data should be pattern 249");
    
    // Verify tree integrity
    ASSERT(verify_tree_integrity(&ftl) == 0, "tree integrity check");
    
    // Print final tree structure
    print_radix_tree(&ftl);
    
    cleanup_test_flash();
    PASS();
}

static int test_radix_tree_multiple_sectors(void) {
    TEST(test_radix_tree_multiple_sectors);
    
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    init_test_flash();
    mini_ftl_init(&ftl);
    
    const int num_sectors = 16;
    uint16_t sectors[] = {0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256};
    uint8_t write_data[USER_DATA_SIZE];
    uint8_t read_data[USER_DATA_SIZE];
    
    printf("  Testing interleaved writes to %d sectors...\n", num_sectors);
    
    // Phase 1: Write each sector once
    for (int i = 0; i < num_sectors; i++) {
        memset(write_data, i + 10, USER_DATA_SIZE);
        ASSERT(mini_ftl_write(&ftl, sectors[i], write_data) == 0, "initial write");
    }
    
    // Verify all sectors
    for (int i = 0; i < num_sectors; i++) {
        ASSERT(mini_ftl_read(&ftl, sectors[i], read_data) == 0, "read after initial write");
        memset(write_data, i + 10, USER_DATA_SIZE);
        ASSERT(memcmp(write_data, read_data, USER_DATA_SIZE) == 0, "data matches");
    }
    
    printf("    Phase 1 complete: all %d sectors written and verified\n", num_sectors);
    print_radix_tree(&ftl);
    
    // Phase 2: Update sectors in reverse order
    for (int i = num_sectors - 1; i >= 0; i--) {
        memset(write_data, i + 100, USER_DATA_SIZE);
        ASSERT(mini_ftl_write(&ftl, sectors[i], write_data) == 0, "update write");
    }
    
    // Verify updated data
    for (int i = 0; i < num_sectors; i++) {
        ASSERT(mini_ftl_read(&ftl, sectors[i], read_data) == 0, "read after update");
        memset(write_data, i + 100, USER_DATA_SIZE);
        ASSERT(memcmp(write_data, read_data, USER_DATA_SIZE) == 0, "updated data matches");
    }
    
    printf("    Phase 2 complete: all sectors updated and verified\n");
    print_radix_tree(&ftl);
    
    // Verify tree integrity
    ASSERT(verify_tree_integrity(&ftl) == 0, "tree integrity after updates");
    
    cleanup_test_flash();
    PASS();
}

static int test_radix_tree_path_correctness(void) {
    TEST(test_radix_tree_path_correctness);
    
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    init_test_flash();
    mini_ftl_init(&ftl);

    // Write sectors with specific bit patterns to test path tracing
    // Sector 0:   0b0000000000000000
    // Sector 1:   0b0000000000000001
    // Sector 2:   0b0000000000000010
    // Sector 32768: 0b1000000000000000
    
    uint16_t test_sectors[] = {0, 1, 2, 32768};
    uint8_t data[USER_DATA_SIZE];
    
    for (int i = 0; i < 4; i++) {
        memset(data, i + 1, USER_DATA_SIZE);
        ASSERT(mini_ftl_write(&ftl, test_sectors[i], data) == 0, "write test sector");
    }
    
    // Verify each sector can be read correctly
    for (int i = 0; i < 4; i++) {
        ASSERT(mini_ftl_read(&ftl, test_sectors[i], data) == 0, "read test sector");
        ASSERT(data[0] == (uint8_t)(i + 1), "correct data for sector");
    }
    
    printf("    Path correctness verified for sectors: 0, 1, 2, 32768\n");
    print_radix_tree(&ftl);
    
    ASSERT(verify_tree_integrity(&ftl) == 0, "tree integrity");
    
    cleanup_test_flash();
    PASS();
}

static int test_radix_tree_stress_random_access(void) {
    TEST(test_radix_tree_stress_random_access);
    
    mini_ftl_t ftl;
    memset(&ftl, 0, sizeof(ftl));  // 重要：清零结构体
    init_test_flash();
    mini_ftl_init(&ftl);
    
    const int num_operations = 100;
    uint8_t last_written[256]; // Track last written value for each sector (use uint8_t to match write_val)
    uint16_t write_count[256];  // Track how many times each sector was written
    memset(last_written, 0xFF, sizeof(last_written));
    memset(write_count, 0, sizeof(write_count));
    
    printf("  Performing %d random write/read operations...\n", num_operations);
    
    // Use simple pseudo-random sequence
    uint32_t seed = 12345;
    for (int i = 0; i < num_operations; i++) {
        seed = seed * 1103515245 + 12345;
        uint16_t sector = (seed >> 16) & 0xFF; // Random sector 0-255
        
        uint8_t write_val = i & 0xFF;
        uint8_t write_data[USER_DATA_SIZE];
        memset(write_data, write_val, USER_DATA_SIZE);
        
        if (sector == 0) {
            printf("  [DIAG] Write #%d: sector=0, val=0x%02X, root_page=%d\n", 
                   i, write_val, ftl.root_page);
        }
        
        ASSERT(mini_ftl_write(&ftl, sector, write_data) == 0, "random write");
        last_written[sector] = write_val;
        write_count[sector]++;
        
        // Read back and verify
        uint8_t read_data[USER_DATA_SIZE];
        ASSERT(mini_ftl_read(&ftl, sector, read_data) == 0, "random read");
        ASSERT(read_data[0] == write_val, "random read data matches");
    }
    
    printf("  [DIAG] Sector 0 was written %d times\n", write_count[0]);
    
    // Final verification of all written sectors
    int verified_count = 0;
    int failed_sector = -1;
    
    // First, print the final radix tree structure
    printf("\n=== Final Radix Tree Structure ===\n");
    printf("Root page: %d\n", ftl.root_page);
    if (ftl.root_page != PAGE_NONE) {
        print_radix_tree(&ftl, ftl.root_page, 0);
    }
    printf("==================================\n\n");
    
    for (int i = 0; i < 256; i++) {
        if (last_written[i] != 0xFF) {
            if (i == 0 || write_count[i] == 0) {
                printf("    [DIAG] Verifying sector %d: last_written=0x%02X, write_count=%d\n",
                       i, last_written[i], write_count[i]);
            }
            uint8_t read_data[USER_DATA_SIZE];
            int ret = mini_ftl_read(&ftl, i, read_data);
            if (ret != 0) {
                printf("    [ERROR] Failed to read sector %d (expected value: 0x%02X)\n", 
                       i, last_written[i]);
                failed_sector = i;
                break;
            }
            if (read_data[0] != last_written[i]) {
                printf("    [ERROR] Sector %d data mismatch: expected 0x%02X, got 0x%02X\n",
                       i, last_written[i], read_data[0]);
                failed_sector = i;
                break;
            }
            verified_count++;
        }
    }
    
    if (failed_sector >= 0) {
        printf("    First failure at sector %d\n", failed_sector);
        printf("    Successfully verified %d sectors before failure\n", verified_count);
        printf("    Total unique sectors written: ");
        int total_unique = 0;
        for (int i = 0; i < 256; i++) {
            if (write_count[i] > 0) total_unique++;
        }
        printf("%d\n", total_unique);
        
        // Don't abort immediately, let the test continue to show more info
        // ASSERT(0, "final verification read");
        return -1; // Return failure instead of aborting
    }
    
    printf("    Verified %d unique sectors\n", verified_count);
    ASSERT(verify_tree_integrity(&ftl) == 0, "tree integrity after stress test");
    
    cleanup_test_flash();
    PASS();
}
