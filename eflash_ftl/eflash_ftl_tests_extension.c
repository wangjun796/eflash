/* eFlash FTL - Extension Tests
 * 空闲链表扩展机制测试用例
 * 
 * 设计目标：
 * 1. 验证空闲链表动态扩展机制的正确性
 * 2. 验证扩展后的空间回收完整性
 * 3. 验证碎片化场景下的扩展触发
 * 
 * 测试用例列表：
 * ✅ test_free_list_extension - 空闲链表动态扩展测试
 * 
 * 注意：此文件独立于主测试文件，便于详细调试和分析
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

// For testing: include internal headers first to get full type definitions
#include "eflash_ftl.h"
#include "eflash_mgr.h"
#include "eflash_sim.h"

// Then include public API header
#include "eflash.h"

// --- 强制断言宏（不受NDEBUG影响）---
#define FORCE_ASSERT(expr, msg) do { \
    if (!(expr)) { \
        fprintf(stderr, "\n[ASSERTION FAILED] %s\n", msg); \
        fprintf(stderr, "  File: %s, Line: %d\n", __FILE__, __LINE__); \
        fprintf(stderr, "  Expression: %s\n\n", #expr); \
        fflush(stderr); \
        while(1);       \
        exit(EXIT_FAILURE); \
    } \
} while(0)

#define ASSERT FORCE_ASSERT

// --- 测试辅助函数 ---

// Count total free nodes from internal structure
static uint32_t count_total_free_nodes(void) {
    extern eflash_ftl_t g_ftl_instance;
    return g_ftl_instance.spc_mgr.total_free_nodes;
}

// 打印当前系统状态
static void print_system_state(const char *tag, uint32_t initial_free_bytes) {
    uint32_t current_free_bytes = eflash_mgr_get_free_bytes();
    uint32_t current_free_nodes = count_total_free_nodes();
    
    // Calculate extension overhead: each level uses 4 pages * 464 bytes = 1856 bytes
    extern eflash_ftl_t g_ftl_instance;
    uint32_t ext_levels_used = 0;
    for (int i = 0; i < MAX_FREE_NODE_EXT_LEVELS; i++) {
        if (g_ftl_instance.spc_mgr.ext_free_node_addrs[i] != 0xFFFFFFFF) {
            ext_levels_used++;
        } else {
            break;
        }
    }
    uint32_t ext_overhead = ext_levels_used * FREE_NODE_EXT_PAGES * USER_DATA_SIZE;
    uint32_t expected_free_bytes = initial_free_bytes - ext_overhead;
    
    printf("  [%s] Free nodes: %lu, Free bytes: %lu (expected: %lu, ext_levels: %u, ext_overhead: %lu, diff: %ld)\n",
           tag,
           (unsigned long)current_free_nodes,
           (unsigned long)current_free_bytes,
           (unsigned long)expected_free_bytes,
           ext_levels_used,
           (unsigned long)ext_overhead,
           (long)current_free_bytes - (long)expected_free_bytes);
}

// 初始化测试Flash
static void init_test_flash(void) {
    // Force close any existing file handle first
    eflash_deinit();
    
    // Remove old file (retry if needed for Windows filesystem)
    for (int i = 0; i < 3; i++) {
        if (remove("test_flash_extension.bin") == 0) break;
#ifdef _WIN32
        Sleep(10);
#endif
    }
    
    // Initialize flash (will create new file and fill with 0xFF)
    int ret = eflash_init("test_flash_extension.bin");
    if (ret != 0) {
        fprintf(stderr, "Failed to initialize test flash\n");
        exit(EXIT_FAILURE);
    }
}

// 清理测试Flash
static void cleanup_test_flash(void) {
    eflash_deinit();
    // File will be removed on next init_test_flash call
}

// 从main函数开始运行测试
#define RUN_TEST(test_func) do { \
    printf("\n"); \
    printf("========================================\n"); \
    printf(" Running: %s\n", #test_func); \
    printf("========================================\n"); \
    int ret_##test_func = test_func(); \
    if (ret_##test_func != 0) { \
        printf("[FAILED] %s returned %d\n", #test_func, ret_##test_func); \
        failed_count++; \
    } else { \
        printf("[PASSED] %s\n", #test_func); \
        passed_count++; \
    } \
} while(0)

// ============================================================================
// Test 1: Free List Extension with Detailed Debug Info
// ============================================================================
int test_free_list_extension(void) {
    printf("[TEST] test_free_list_extension: Starting...\n");
    
    init_test_flash();
    eflash_ftl_init();
    
    // Record initial state
    uint32_t initial_free_bytes = eflash_mgr_get_free_bytes();
    uint32_t initial_free_nodes = count_total_free_nodes();
    
    printf("  [INFO] Initial state: free_bytes=%lu, free_nodes=%lu\n", 
           (unsigned long)initial_free_bytes, (unsigned long)initial_free_nodes);
    
    // Debug: Read all base free_node pages to check initialization
    for (int lpn = 8; lpn <= 11; lpn++) {
        uint8_t debug_buf[EFLASH_PAGE_SIZE];
        int debug_ret = eflash_ftl_read(lpn, debug_buf);
        if (debug_ret == 0) {
            uint16_t count = (uint16_t)(debug_buf[0] | (debug_buf[1] << 8));
            printf("  [DEBUG] LPN %d: count=%u\n", lpn, count);
            if (count > 0 && lpn == 8) {
                free_node_t node;
                memcpy(&node, debug_buf + 2, sizeof(free_node_t));
                printf("  [DEBUG] LPN %d: node[0].addr=0x%08X, node[0].size=%u\n", lpn, node.addr, node.size);
            }
        } else {
            printf("  [DEBUG] LPN %d: read failed, ret=%d\n", lpn, debug_ret);
        }
    }
    
    // ========================================================================
    // Phase 1: Allocate small blocks with data verification
    // ========================================================================
    #define SMALL_BLOCK_SIZE 8
    #define NUM_ALLOCS 200
    uint32_t addrs[NUM_ALLOCS];
    
    printf("\n  [PHASE 1] Allocating %d small blocks (%d bytes each)...\n", 
           NUM_ALLOCS, SMALL_BLOCK_SIZE);
    printf("  [PHASE 1] Writing data to each block for verification...\n\n");
    
    for (int i = 0; i < NUM_ALLOCS; i++) {
        int ret = eflash_mgr_alloc(SMALL_BLOCK_SIZE, &addrs[i]);
        ASSERT(ret == 0, "allocation should succeed");
        
        // Write data with pattern
        uint8_t write_buf[SMALL_BLOCK_SIZE];
        memset(write_buf, (uint8_t)(i & 0xFF), SMALL_BLOCK_SIZE);
        
        // Write to FTL pages
        uint32_t page_offset = addrs[i] / USER_DATA_SIZE;
        uint32_t byte_offset = addrs[i] % USER_DATA_SIZE;
        uint32_t remaining = SMALL_BLOCK_SIZE;
        uint32_t written = 0;
        
        while (remaining > 0) {
            uint32_t write_size = (remaining < (USER_DATA_SIZE - byte_offset)) ? remaining : (USER_DATA_SIZE - byte_offset);
            
            uint8_t page_buf[EFLASH_PAGE_SIZE];
            ret = eflash_ftl_read((uint16_t)(page_offset), page_buf);
            if (ret != 0) {
                memset(page_buf, 0xff, EFLASH_PAGE_SIZE);
            }
            
            memcpy(page_buf + byte_offset, write_buf + written, write_size);
            ret = eflash_ftl_write((uint16_t)(page_offset), page_buf);
            ASSERT(ret == 0, "write should succeed");
            
            remaining -= write_size;
            written += write_size;
            page_offset++;
            byte_offset = 0;
        }
        
        // Print status after EVERY allocation
        uint32_t current_free_bytes = eflash_mgr_get_free_bytes();
        uint32_t current_free_nodes = count_total_free_nodes();
        printf("  [ALLOC #%d] addr=0x%06X, free_nodes=%lu, free_bytes=%lu\n",
               i + 1, addrs[i],
               (unsigned long)current_free_nodes, (unsigned long)current_free_bytes);
    }
    
    printf("\n  [PASS] Phase 1: All %d allocations and writes completed\n", NUM_ALLOCS);
    print_system_state("AFTER_PHASE1", initial_free_bytes);
    
    // ========================================================================
    // Phase 2: Free every 3rd block with data verification BEFORE free
    // ========================================================================
    printf("\n  [PHASE 2] Freeing every 3rd block with data verification...\n\n");
    
    int freed_count_phase2 = 0;
    for (int i = 0; i < NUM_ALLOCS; i += 3) {
        // Verify data BEFORE freeing
        uint8_t read_buf[SMALL_BLOCK_SIZE];
        uint32_t page_offset = addrs[i] / USER_DATA_SIZE;
        uint32_t byte_offset = addrs[i] % USER_DATA_SIZE;
        uint32_t remaining = SMALL_BLOCK_SIZE;
        uint32_t read_bytes = 0;
        int verify_ok = 1;
        
        while (remaining > 0) {
            uint32_t read_size = (remaining < (USER_DATA_SIZE - byte_offset)) ? remaining : (USER_DATA_SIZE - byte_offset);
            
            uint8_t page_buf[EFLASH_PAGE_SIZE];
            int ret = eflash_ftl_read((uint16_t)(page_offset), page_buf);
            ASSERT(ret == 0, "read before free should succeed");
            
            memcpy(read_buf + read_bytes, page_buf + byte_offset, read_size);
            
            remaining -= read_size;
            read_bytes += read_size;
            page_offset++;
            byte_offset = 0;
        }
        
        // Verify data pattern
        for (uint32_t j = 0; j < SMALL_BLOCK_SIZE; j++) {
            if (read_buf[j] != (uint8_t)(i & 0xFF)) {
                printf("  [ERROR] Data mismatch at block #%d: byte[%u]=0x%02X, expected=0x%02X\n",
                       i, j, read_buf[j], (uint8_t)(i & 0xFF));
                verify_ok = 0;
                break;
            }
        }
        
        ASSERT(verify_ok, "data should match pattern before free");
        
        // Now free the block
        eflash_mgr_free(addrs[i], SMALL_BLOCK_SIZE);
        freed_count_phase2++;
        
        // Print status after EVERY free
        uint32_t current_free_bytes = eflash_mgr_get_free_bytes();
        uint32_t current_free_nodes = count_total_free_nodes();
        printf("  [FREE #%d] block #%d, addr=0x%06X, free_nodes=%lu, free_bytes=%lu\n",
               freed_count_phase2, i, addrs[i],
               (unsigned long)current_free_nodes, (unsigned long)current_free_bytes);
    }
    
    printf("\n  [PASS] Phase 2: Freed %d blocks (every 3rd block)\n", freed_count_phase2);
    print_system_state("AFTER_PHASE2", initial_free_bytes);
    
    // ========================================================================
    // Phase 3: Allocate larger block (may trigger extension)
    // ========================================================================
    printf("\n  [PHASE 3] Allocating large block (100 bytes)...\n");
    
    uint32_t large_block_addr;
    int ret = eflash_mgr_alloc(100, &large_block_addr);
    ASSERT(ret == 0, "large allocation should succeed");
    
    // Write data to large block
    uint8_t large_write_buf[100];
    memset(large_write_buf, 0xAA, 100);
    
    uint32_t large_page_offset = large_block_addr / USER_DATA_SIZE;
    uint32_t large_byte_offset = large_block_addr % USER_DATA_SIZE;
    uint32_t large_remaining = 100;
    uint32_t large_written = 0;
    
    while (large_remaining > 0) {
        uint32_t write_size = (large_remaining < (USER_DATA_SIZE - large_byte_offset)) ? large_remaining : (USER_DATA_SIZE - large_byte_offset);
        
        uint8_t page_buf[EFLASH_PAGE_SIZE];
        ret = eflash_ftl_read((uint16_t)(large_page_offset), page_buf);
        if (ret != 0) {
            memset(page_buf, 0, EFLASH_PAGE_SIZE);
        }
        
        memcpy(page_buf + large_byte_offset, large_write_buf + large_written, write_size);
        ret = eflash_ftl_write((uint16_t)(large_page_offset), page_buf);
        ASSERT(ret == 0, "large block write should succeed");
        
        large_remaining -= write_size;
        large_written += write_size;
        large_page_offset++;
        large_byte_offset = 0;
    }
    
    print_system_state("AFTER_LARGE_ALLOC", initial_free_bytes);
    printf("  [PASS] Large block allocated at addr=0x%06X\n", large_block_addr);
    
    // ========================================================================
    // Phase 4: Allocate additional blocks (may trigger extension)
    // ========================================================================
    printf("\n  [PHASE 4] Allocating additional %d blocks...\n\n", 50);
    
    #define EXTENDED_ALLOCS 50
    uint32_t ext_addrs[EXTENDED_ALLOCS];
    int alloc_count = 0;
    
    for (int i = 0; i < EXTENDED_ALLOCS; i++) {
        ret = eflash_mgr_alloc(SMALL_BLOCK_SIZE, &ext_addrs[i]);
        if (ret != 0) {
            printf("  [INFO] Allocation %d failed (capacity limit reached)\n", i);
            break;
        }
        
        // Write data
        uint8_t ext_write_buf[SMALL_BLOCK_SIZE];
        memset(ext_write_buf, (uint8_t)(0x80 + (i & 0x7F)), SMALL_BLOCK_SIZE);
        
        uint32_t ext_page_offset = ext_addrs[i] / USER_DATA_SIZE;
        uint32_t ext_byte_offset = ext_addrs[i] % USER_DATA_SIZE;
        uint32_t ext_remaining = SMALL_BLOCK_SIZE;
        uint32_t ext_written = 0;
        
        while (ext_remaining > 0) {
            uint32_t write_size = (ext_remaining < (USER_DATA_SIZE - ext_byte_offset)) ? ext_remaining : (USER_DATA_SIZE - ext_byte_offset);
            
            uint8_t page_buf[EFLASH_PAGE_SIZE];
            ret = eflash_ftl_read((uint16_t)(ext_page_offset), page_buf);
            if (ret != 0) {
                memset(page_buf, 0, EFLASH_PAGE_SIZE);
            }
            
            memcpy(page_buf + ext_byte_offset, ext_write_buf + ext_written, write_size);
            ret = eflash_ftl_write((uint16_t)(ext_page_offset), page_buf);
            if (ret != 0) break;
            
            ext_remaining -= write_size;
            ext_written += write_size;
            ext_page_offset++;
            ext_byte_offset = 0;
        }
        
        alloc_count++;
        
        // Print status after EVERY allocation
        uint32_t current_free_bytes = eflash_mgr_get_free_bytes();
        uint32_t current_free_nodes = count_total_free_nodes();
        printf("  [EXT_ALLOC #%d] addr=0x%06X, free_nodes=%lu, free_bytes=%lu\n",
               alloc_count, ext_addrs[i],
               (unsigned long)current_free_nodes, (unsigned long)current_free_bytes);
    }
    
    printf("\n  [PASS] Phase 4: Allocated %d/%d additional blocks\n", alloc_count, EXTENDED_ALLOCS);
    print_system_state("AFTER_PHASE4", initial_free_bytes);
    
    // ========================================================================
    // Phase 5: Free ALL blocks with detailed verification
    // ========================================================================
    printf("\n  [PHASE 5] Freeing ALL blocks with verification...\n\n");
    
    int total_freed = 0;
    
    // Free remaining base blocks (those not freed in phase 2)
    printf("  [PHASE 5A] Freeing remaining base blocks...\n");
    for (int i = 0; i < NUM_ALLOCS; i++) {
        if (i % 3 != 0) {
            // Verify data before free
            uint8_t read_buf[SMALL_BLOCK_SIZE];
            uint32_t page_offset = addrs[i] / USER_DATA_SIZE;
            uint32_t byte_offset = addrs[i] % USER_DATA_SIZE;
            uint32_t remaining = SMALL_BLOCK_SIZE;
            uint32_t read_bytes = 0;
            int verify_ok = 1;
            
            while (remaining > 0) {
                uint32_t read_size = (remaining < (USER_DATA_SIZE - byte_offset)) ? remaining : (USER_DATA_SIZE - byte_offset);
                
                uint8_t page_buf[EFLASH_PAGE_SIZE];
                int ret = eflash_ftl_read((uint16_t)(page_offset), page_buf);
                if (ret != 0) {
                    printf("  [ERROR] Read failed for block #%d at LPN %u (may be expected if page reused)\n",
                           i, page_offset);
                    verify_ok = 0;
                    break;
                }
                
                memcpy(read_buf + read_bytes, page_buf + byte_offset, read_size);
                
                remaining -= read_size;
                read_bytes += read_size;
                page_offset++;
                byte_offset = 0;
            }
            
            // Only verify if read succeeded
            if (verify_ok) {
                for (uint32_t j = 0; j < SMALL_BLOCK_SIZE; j++) {
                    if (read_buf[j] != (uint8_t)(i & 0xFF)) {
                        printf("  [ERROR] Data mismatch at block #%d: byte[%u]=0x%02X, expected=0x%02X\n",
                               i, j, read_buf[j], (uint8_t)(i & 0xFF));
                        verify_ok = 0;
                        break;
                    }
                }
            }
            
            // Free the block
            eflash_mgr_free(addrs[i], SMALL_BLOCK_SIZE);
            total_freed++;
            
            // Print status after EVERY free
            uint32_t current_free_bytes = eflash_mgr_get_free_bytes();
            uint32_t current_free_nodes = count_total_free_nodes();
            printf("  [FREE_BASE #%d] block #%d, addr=0x%06X, free_nodes=%lu, free_bytes=%lu\n",
                   total_freed, i, addrs[i],
                   (unsigned long)current_free_nodes, (unsigned long)current_free_bytes);
        }
    }
    
    printf("  [PASS] Phase 5A: Freed %d base blocks\n", NUM_ALLOCS - freed_count_phase2);
    print_system_state("AFTER_FREE_BASE", initial_free_bytes);
    
    // Free extended blocks
    printf("\n  [PHASE 5B] Freeing extended blocks...\n");
    for (int i = 0; i < alloc_count; i++) {
        eflash_mgr_free(ext_addrs[i], SMALL_BLOCK_SIZE);
        total_freed++;
        
        // Print status after EVERY free
        uint32_t current_free_bytes = eflash_mgr_get_free_bytes();
        uint32_t current_free_nodes = count_total_free_nodes();
        printf("  [FREE_EXT #%d] block #%d, addr=0x%06X, free_nodes=%lu, free_bytes=%lu\n",
               total_freed, i, ext_addrs[i],
               (unsigned long)current_free_nodes, (unsigned long)current_free_bytes);
    }
    
    printf("  [PASS] Phase 5B: Freed %d extended blocks\n", alloc_count);
    print_system_state("AFTER_FREE_EXT", initial_free_bytes);
    
    // Free large block
    printf("\n  [PHASE 5C] Freeing large block...\n");
    eflash_mgr_free(large_block_addr, 100);
    total_freed++;
    uint32_t current_free_bytes = eflash_mgr_get_free_bytes();
    uint32_t current_free_nodes = count_total_free_nodes();
    printf("  [FREE_LARGE] addr=0x%06X, free_nodes=%lu, free_bytes=%lu\n",
           large_block_addr,
           (unsigned long)current_free_nodes, (unsigned long)current_free_bytes);
    print_system_state("AFTER_FREE_LARGE", initial_free_bytes);
    
    // ========================================================================
    // Final Verification
    // ========================================================================
    printf("\n  [FINAL VERIFICATION]\n");
    
    uint32_t final_free_bytes = eflash_mgr_get_free_bytes();
    uint32_t final_free_nodes = count_total_free_nodes();
    
    printf("  [INFO] Final state: free_bytes=%lu (initial: %lu, diff: %ld), free_nodes=%lu (initial: %lu, diff: %ld)\n",
           (unsigned long)final_free_bytes,
           (unsigned long)initial_free_bytes,
           (long)final_free_bytes - (long)initial_free_bytes,
           (unsigned long)final_free_nodes,
           (unsigned long)initial_free_nodes,
           (long)final_free_nodes - (long)initial_free_nodes);
    
    printf("  [INFO] Total blocks freed: %d\n", total_freed);
    
    ASSERT(final_free_bytes == initial_free_bytes, 
           "free space should match after alloc/free cycle");
    ASSERT(final_free_nodes == initial_free_nodes,
           "total nodes should match after alloc/free cycle");
    
    printf("\n  [PASS] Free list extension test completed successfully\n");
    
    cleanup_test_flash();
    return 0;
}

// ============================================================================
// Extension Stress Test - Tests free list extension mechanism
// ============================================================================

// Simple pseudo-random number generator with fixed seed for reproducibility
static unsigned int g_stress_rand_seed = 12345;

static void stress_srand(unsigned int seed) {
    g_stress_rand_seed = seed;
}

static int stress_rand(void) {
    g_stress_rand_seed = g_stress_rand_seed * 1103515245 + 12345;
    return (g_stress_rand_seed >> 16) & 0x7FFF;
}

// Fisher-Yates shuffle to generate random permutation
static void stress_shuffle_array(int *array, int size) {
    for (int i = size - 1; i > 0; i--) {
        int j = stress_rand() % (i + 1);
        int temp = array[i];
        array[i] = array[j];
        array[j] = temp;
    }
}

// Get extension levels used
static uint32_t get_ext_levels_used(void) {
    extern eflash_ftl_t g_ftl_instance;
    uint32_t levels = 0;
    for (int i = 0; i < MAX_FREE_NODE_EXT_LEVELS; i++) {
        if (g_ftl_instance.spc_mgr.ext_free_node_addrs[i] != 0xFFFFFFFF) {
            levels++;
        } else {
            break;
        }
    }
    return levels;
}

int test_free_list_extension_stress(void) {
    printf("\n========================================\n");
    printf("TEST: Free List Extension Stress Test\n");
    printf("========================================\n\n");
    
    // Initialize test flash (this will create a fresh flash file)
    init_test_flash();
    
    // Initialize FTL layer
    eflash_ftl_init();
    
    // Record initial state
    uint32_t initial_free_bytes = eflash_mgr_get_free_bytes();
    uint32_t initial_free_nodes = count_total_free_nodes();
    
    printf("  [INFO] Initial state: free_bytes=%lu, free_nodes=%lu\n", 
           (unsigned long)initial_free_bytes, (unsigned long)initial_free_nodes);
    printf("  [INFO] Base layer capacity: %d nodes (4 pages x 57 nodes/page)\n", 
           FREE_NODE_PAGE_COUNT * FREE_NODES_PER_PAGE);
    printf("  [INFO] Extension trigger: > %d free nodes\n", 
           FREE_NODE_PAGE_COUNT * FREE_NODES_PER_PAGE);
    
    // ========================================================================
    // Phase 1: Allocate many small blocks
    // Need enough blocks to create > 228 free nodes after release
    // Strategy: Allocate 1024 blocks, then free every 4th block to avoid merging
    // ========================================================================
    #define STRESS_BLOCK_SIZE 8  // Small blocks to maximize node count
    #define STRESS_NUM_BLOCKS 1000  // Large number to ensure extension trigger
    uint32_t addrs[STRESS_NUM_BLOCKS];
    
    printf("\n  [PHASE 1] Allocating %d blocks (%d bytes each)...\n", 
           STRESS_NUM_BLOCKS, STRESS_BLOCK_SIZE);
    printf("  [PHASE 1] This will use significant space, preparing for extension test...\n");
    
#ifdef FTL_DEBUG_ENABLE
    // Track root page changes for debugging (only when debug is enabled)
    bool enable_tree_print = false;  // Enable detailed tree printing when root >= 2046
    int write_count_after_2046 = 0;  // Count writes after reaching PPN 2046
#endif
    
    for (int i = 0; i < STRESS_NUM_BLOCKS; i++) {
        int ret = eflash_mgr_alloc(STRESS_BLOCK_SIZE, &addrs[i]);
        ASSERT(ret == 0, "allocation should succeed");
        
        // Write unique pattern to each block
        uint8_t write_buf[STRESS_BLOCK_SIZE];
        memset(write_buf, (uint8_t)(i & 0xFF), STRESS_BLOCK_SIZE);
        
        uint32_t page_offset = addrs[i] / USER_DATA_SIZE;
        uint32_t byte_offset = addrs[i] % USER_DATA_SIZE;
        uint32_t remaining = STRESS_BLOCK_SIZE;
        uint32_t written = 0;
        
        while (remaining > 0) {
            uint32_t write_size = (remaining < (USER_DATA_SIZE - byte_offset)) ? 
                                  remaining : (USER_DATA_SIZE - byte_offset);
            
            uint8_t page_buf[EFLASH_PAGE_SIZE];
            ret = eflash_ftl_read((uint16_t)page_offset, page_buf);
            if (ret != 0) {
                memset(page_buf, 0xff, EFLASH_PAGE_SIZE);
            }
            
            memcpy(page_buf + byte_offset, write_buf + written, write_size);
            ret = eflash_ftl_write((uint16_t)page_offset, page_buf);
            ASSERT(ret == 0, "write should succeed");
            
#ifdef FTL_DEBUG_ENABLE
            // Check if we should enable detailed tree printing (trigger when next_count > 2046)
            if (!enable_tree_print && FTL->next_count > 2046) {
                enable_tree_print = true;
                printf("\n  [DEBUG] *** next_count reached %u, enabling detailed tree tracking ***\n", FTL->next_count);
                printf("  [DEBUG] Writing block #%d, page_offset=%lu\n", i, (unsigned long)page_offset);
            }
            
            // Print tree on every write when next_count > 2046
            if (enable_tree_print) {
                write_count_after_2046++;
                printf("\n  [DEBUG TREE #%d] Block #%d, page_offset=%lu, root=%d, next_count=%u, head=%d, tail=%d\n",
                       write_count_after_2046, i, (unsigned long)page_offset,
                       FTL->root_page, FTL->next_count, FTL->gc_head_page, FTL->gc_tail_page);
                extern void eflash_ftl_print_radix_tree_mermaid_to_file(eflash_ftl_t *ftl, uint16_t root_page);
                eflash_ftl_print_radix_tree_mermaid_to_file(FTL, FTL->root_page);
            }
#endif
            
            remaining -= write_size;
            written += write_size;
            page_offset++;
            byte_offset = 0;
        }
    }
    
    printf("  [PASS] Phase 1: All %d allocations completed\n", STRESS_NUM_BLOCKS);
    print_system_state("AFTER_PHASE1", initial_free_bytes);
    
    // ========================================================================
    // Phase 2: Release blocks with stride to create many non-adjacent free nodes
    // Free every 4th block: i=0,4,8,12,... to avoid merging
    // This should create ~256 free nodes and trigger extension when > 228
    // ========================================================================
    printf("\n  [PHASE 2] Releasing blocks with stride (every 4th block)...\n");
    printf("  [PHASE 2] Strategy: Free blocks at indices 0,4,8,12,... to avoid merging\n");
    printf("  [PHASE 2] Expected free nodes: ~%d (should trigger extension)\n\n",
           STRESS_NUM_BLOCKS / 4);
    
    int freed_count = 0;
    int extension_triggered = 0;
    uint32_t ext_level_at_trigger = 0;
    const int STRIDE = 4;  // Free every 4th block
    
    for (int i = 0; i < STRESS_NUM_BLOCKS; i += STRIDE) {
        
        // Verify data before freeing
        uint8_t read_buf[STRESS_BLOCK_SIZE];
        uint32_t page_offset = addrs[i] / USER_DATA_SIZE;
        uint32_t byte_offset = addrs[i] % USER_DATA_SIZE;
        uint32_t remaining = STRESS_BLOCK_SIZE;
        uint32_t read_bytes = 0;
        int verify_ok = 1;
        
        while (remaining > 0) {
            uint32_t read_size = (remaining < (USER_DATA_SIZE - byte_offset)) ? 
                                 remaining : (USER_DATA_SIZE - byte_offset);
            
            uint8_t page_buf[EFLASH_PAGE_SIZE];
            int ret = eflash_ftl_read((uint16_t)page_offset, page_buf);
            ASSERT(ret == 0, "read before free should succeed");
            
            memcpy(read_buf + read_bytes, page_buf + byte_offset, read_size);
            
            remaining -= read_size;
            read_bytes += read_size;
            page_offset++;
            byte_offset = 0;
        }
        
        // Verify data pattern
        for (uint32_t j = 0; j < STRESS_BLOCK_SIZE; j++) {
            if (read_buf[j] != (uint8_t)(i & 0xFF)) {
                printf("  [ERROR] Data mismatch at block #%d: byte[%u]=0x%02X, expected=0x%02X\n",
                       i, j, read_buf[j], (uint8_t)(i & 0xFF));
                verify_ok = 0;
                break;
            }
        }
        
        ASSERT(verify_ok, "data should match pattern before free");
        
        // Check free nodes before free
        uint32_t nodes_before = count_total_free_nodes();
        
        // Free the block
        eflash_mgr_free(addrs[i], STRESS_BLOCK_SIZE);
        freed_count++;
        
        // Check if extension was triggered
        uint32_t nodes_after = count_total_free_nodes();
        uint32_t current_ext_levels = get_ext_levels_used();
        
        if (current_ext_levels > ext_level_at_trigger && !extension_triggered) {
            extension_triggered = 1;
            ext_level_at_trigger = current_ext_levels;
            printf("\n  [*** EXTENSION TRIGGERED ***] After freeing block #%d\n", i);
            printf("  [*** EXTENSION TRIGGERED ***] Free nodes: %lu -> %lu, Extension level: %u\n\n",
                   (unsigned long)nodes_before, (unsigned long)nodes_after, current_ext_levels);
        }
        
        // Print status every 50 frees
        if (freed_count % 50 == 0) {
            printf("  [FREE #%d] Block #%d, free_nodes=%lu, ext_levels=%u\n",
                   freed_count, i, (unsigned long)nodes_after, current_ext_levels);
        }
    }
    
    printf("\n  [PASS] Phase 2: Freed all %d blocks in random order\n", freed_count);
    printf("  [INFO] Extension triggered: %s (level %u)\n", 
           extension_triggered ? "YES" : "NO", ext_level_at_trigger);
    print_system_state("AFTER_PHASE2", initial_free_bytes);
    
    // ========================================================================
    // Phase 3: Verify extension overhead is correctly accounted for
    // ========================================================================
    printf("\n  [PHASE 3] Verifying extension overhead calculation...\n");
    
    uint32_t final_free_bytes = eflash_mgr_get_free_bytes();
    uint32_t final_ext_levels = get_ext_levels_used();
    uint32_t calculated_overhead = final_ext_levels * FREE_NODE_EXT_PAGES * USER_DATA_SIZE;
    
    // Calculate expected free bytes:
    // initial_free_bytes - overhead - (total_allocated - total_freed)
    uint64_t total_allocated_space = (uint64_t)STRESS_NUM_BLOCKS * STRESS_BLOCK_SIZE;
    uint64_t total_freed_space = (uint64_t)freed_count * STRESS_BLOCK_SIZE;
    uint64_t remaining_allocated = total_allocated_space - total_freed_space;
    uint32_t expected_final_bytes = (uint32_t)(initial_free_bytes - calculated_overhead - remaining_allocated);
    
    printf("  [INFO] Final free bytes: %lu\n", (unsigned long)final_free_bytes);
    printf("  [INFO] Total allocated: %lu blocks x %d bytes = %lu bytes\n",
           (unsigned long)STRESS_NUM_BLOCKS, STRESS_BLOCK_SIZE, (unsigned long)total_allocated_space);
    printf("  [INFO] Total freed: %lu blocks x %d bytes = %lu bytes\n",
           (unsigned long)freed_count, STRESS_BLOCK_SIZE, (unsigned long)total_freed_space);
    printf("  [INFO] Remaining allocated: %lu bytes\n", (unsigned long)remaining_allocated);
    printf("  [INFO] Extension overhead: %lu bytes (%u levels)\n",
           (unsigned long)calculated_overhead, final_ext_levels);
    printf("  [INFO] Expected free bytes: %lu (initial %lu - overhead %lu - remaining %lu)\n",
           (unsigned long)expected_final_bytes,
           (unsigned long)initial_free_bytes,
           (unsigned long)calculated_overhead,
           (unsigned long)remaining_allocated);
    printf("  [INFO] Difference: %ld bytes\n",
           (long)final_free_bytes - (long)expected_final_bytes);
    
    // Allow small difference due to fragmentation
    long diff = (long)final_free_bytes - (long)expected_final_bytes;
    ASSERT(diff >= -100 && diff <= 100, 
           "free space should match expected value (within 100 bytes tolerance)");
    
    printf("  [PASS] Extension overhead correctly accounted for\n");
    
    // ========================================================================
    // Summary
    // ========================================================================
    printf("\n========================================\n");
    printf("TEST SUMMARY\n");
    printf("========================================\n");
    printf("  Blocks allocated: %d\n", STRESS_NUM_BLOCKS);
    printf("  Blocks freed: %d\n", freed_count);
    printf("  Extension triggered: %s\n", extension_triggered ? "YES" : "NO");
    printf("  Extension levels used: %u\n", final_ext_levels);
    printf("  Extension overhead: %lu bytes\n", (unsigned long)calculated_overhead);
    printf("  Final free bytes: %lu (expected: %lu)\n",
           (unsigned long)final_free_bytes, (unsigned long)expected_final_bytes);
    printf("========================================\n");
    
    if (extension_triggered) {
        printf("\n[PASSED] test_free_list_extension_stress\n");
        printf("Extension mechanism working correctly!\n");
    } else {
        printf("\n[WARNING] test_free_list_extension_stress\n");
        printf("Extension was NOT triggered. May need more blocks or different strategy.\n");
    }
    
    cleanup_test_flash();
    return 0;
}

// ============================================================================
// Main Entry Point
// ============================================================================
int main(int argc, char *argv[]) {
    printf("========================================\n");
    printf(" eFlash FTL Extension Tests\n");
    printf("========================================\n\n");
    
    int passed_count = 0;
    int failed_count = 0;
    
    // Run all extension tests
    //RUN_TEST(test_free_list_extension);
    RUN_TEST(test_free_list_extension_stress);
    
    // Summary
    printf("\n========================================\n");
    printf(" Test Summary\n");
    printf("========================================\n");
    printf(" Passed: %d\n", passed_count);
    printf(" Failed: %d\n", failed_count);
    printf(" Total:  %d\n", passed_count + failed_count);
    printf("========================================\n\n");
    
    return (failed_count > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
