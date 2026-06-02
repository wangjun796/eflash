/* eFlash FTL - Long-term Stability Test
 * 长期运行稳定性测试（独立文件）
 *
 * 测试内容：
 *   Phase 1: 执行 100,000 次读写操作
 *   Phase 2: 混合大小写入（1-508 字节）
 *   Phase 3: 周期性断电恢复（10 次）
 *   Phase 4: 最终综合验证
 */

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

#define TEST_FLASH_FILE "test_flash_stability.bin"
#define META_OFFSET USER_DATA_SIZE

static void init_test_flash(void) {
    eflash_deinit();

    for (int i = 0; i < 3; i++) {
        if (remove(TEST_FLASH_FILE) == 0) break;
#ifdef _WIN32
        Sleep(10);
#endif
    }

    int ret = eflash_init(TEST_FLASH_FILE);
    if (ret != 0) {
        fprintf(stderr, "Failed to initialize test flash\n");
        exit(EXIT_FAILURE);
    }
}

static void cleanup_test_flash(void) {
    eflash_deinit();
}

static void dump_all_pages(const char *tag) {
    printf("\n  ====== [DUMP %s] Full Flash Page Scan ======\n", tag);
    printf("  %-6s %-10s %-6s %-12s %-8s %-8s %s\n",
           "PPN", "sector_id", "epoch", "global_count", "status", "is_comm", "notes");
    printf("  ------ ---------- ------ ------------ -------- -------- -----\n");

    int blank_count = 0;
    int valid_count = 0;
    int committed_count = 0;
    int ready_count = 0;
    int invalid_count = 0;

    for (int ppn = 0; ppn < EFLASH_TOTAL_PAGES; ppn++) {
        uint8_t buf[EFLASH_PAGE_SIZE];
        if (eflash_hw_read(ppn, buf) != 0) {
            printf("  %-6d [READ ERROR]\n", ppn);
            continue;
        }

        int is_blank = 1;
        for (int bi = 0; bi < EFLASH_PAGE_SIZE; bi++) {
            if (buf[bi] != 0xFF) { is_blank = 0; break; }
        }

        if (is_blank) {
            blank_count++;
            continue;
        }

        ftl_meta_t *meta = (ftl_meta_t *)(buf + META_OFFSET);
        uint8_t status = meta->status;
        const char *status_str = "???";
        int is_comm = 0;

        switch (status) {
            case 0xFF: status_str = "BLANK"; break;
            case 0x21: status_str = "COMMIT"; is_comm = 1; committed_count++; break;
            case 0xAD: status_str = "READY"; ready_count++; break;
            case 0x00: status_str = "INVALID"; invalid_count++; break;
            case 0xEF: status_str = "PENDING"; break;
            default: status_str = "UNKNOWN"; break;
        }

        valid_count++;

        if (valid_count <= 50 || is_comm) {
            printf("  %-6d %-10d %-6d %-12u 0x%02X(%s) %-8s",
                   ppn, meta->sector_id, meta->epoch, meta->global_count,
                   status, status_str, is_comm ? "YES" : "no");
            if (is_comm) {
                printf(" *** COMMITTED ***");
            }
            printf("\n");
        }
    }

    printf("  ------ ---------- ------ ------------ -------- -------- -----\n");
    printf("  Total: %d pages, Blank: %d, Valid: %d, Committed: %d, Ready: %d, Invalid: %d\n",
           EFLASH_TOTAL_PAGES, blank_count, valid_count, committed_count, ready_count, invalid_count);
    printf("  ====== [DUMP %s END] ======\n\n", tag);
    fflush(stdout);
}

int test_long_term_stability(void) {
    printf("\n========================================\n");
    printf("TEST: Long-term Stability Test\n");
    printf("========================================\n\n");

    int test_passed = 1;

    init_test_flash();
    eflash_ftl_init();

    uint8_t write_buf[USER_DATA_SIZE];
    uint8_t read_buf[USER_DATA_SIZE];

    extern eflash_ftl_t g_ftl_instance;

    #define PRINT_ROOT_STATE(tag) do { \
        printf("      [ROOT_%s] root_page=%d, next_count=%u, valid_page_count=%u\n", \
               tag, g_ftl_instance.root_page, g_ftl_instance.next_count, \
               g_ftl_instance.valid_page_count); \
        printf("      [ROOT_%s] head=%d, tail=%d, epoch=%d\n", \
               tag, g_ftl_instance.gc_head_page, g_ftl_instance.gc_tail_page, \
               (int)g_ftl_instance.current_epoch); \
        if (g_ftl_instance.root_page != PAGE_NONE) { \
            uint8_t root_buf[EFLASH_PAGE_SIZE]; \
            if (eflash_hw_read(g_ftl_instance.root_page, root_buf) == 0) { \
                ftl_meta_t *root_meta = (ftl_meta_t *)(root_buf + META_OFFSET); \
                uint32_t cksum = 0; \
                for (int ci = 0; ci < 256; ci++) cksum += root_buf[ci]; \
                printf("      [ROOT_%s] sector_id=%d, global_count=%u, status=0x%02X, cksum=%u\n", \
                       tag, root_meta->sector_id, root_meta->global_count, \
                       root_meta->status, cksum); \
            } else { \
                printf("      [ROOT_%s] ERROR: Cannot read root page %d\n", \
                       tag, g_ftl_instance.root_page); \
            } \
        } \
    } while(0)

    printf("  [PHASE 1] Executing 100,000 read/write operations...\n");

    #define STABILITY_OPS 100000
    int write_count = 0;
    int read_count = 0;
    int error_count = 0;

    for (int i = 0; i < STABILITY_OPS; i++) {
        uint16_t sector_id = (uint16_t)((i * 7919 + 12345) % 2000);

        memset(write_buf, (uint8_t)(i & 0xFF), USER_DATA_SIZE);
        write_buf[0] = (uint8_t)(i >> 8);

        int write_ret = eflash_ftl_write(sector_id, write_buf);
        if (write_ret == 0) {
            write_count++;
        } else {
            assert(0 && "Phase 1: Write failed - flash full, allocate_physical_page returned -1");
            error_count++;
            printf("    WARNING: Write failed at iteration %d, sector %d\n", i, sector_id);
        }

        if (eflash_ftl_read(sector_id, read_buf) == 0) {
            read_count++;

            if (read_buf[0] != (uint8_t)(i >> 8) || read_buf[1] != (uint8_t)(i & 0xFF)) {
                printf("    ERROR: Data corruption at iteration %d, sector %d\n", i, sector_id);
                printf("      Expected: 0x%02X 0x%02X\n", (i >> 8) & 0xFF, i & 0xFF);
                printf("      Got:      0x%02X 0x%02X\n", read_buf[0], read_buf[1]);
                error_count++;
                assert(0 && "Phase 1: Data corruption detected");
                test_passed = 0;
                goto cleanup_stability;
            }
        } else {
            printf("    ERROR: Read failed at iteration %d, sector %d\n", i, sector_id);
            error_count++;
        }

        if ((i + 1) % 1000 == 0) {
            printf("    Progress: %d / %d operations completed\n", i + 1, STABILITY_OPS);
        }
    }

    printf("    Phase 1 Summary:\n");
    printf("      Writes: %d / %d\n", write_count, STABILITY_OPS);
    printf("      Reads:  %d / %d\n", read_count, STABILITY_OPS);
    printf("      Errors: %d\n", error_count);

    if (error_count == 0) {
        printf("  [PASS] Phase 1: All operations completed successfully\n");
    } else {
        printf("  [FAIL] Phase 1: %d errors occurred\n", error_count);
        test_passed = 0;
    }

    printf("\n  [PHASE 2] Mixed-size writes (1 to 508 bytes)...\n");

    #define MIXED_SIZE_OPS 1000
    int mixed_write_success = 0;
    int mixed_read_success = 0;

    for (int i = 0; i < MIXED_SIZE_OPS; i++) {
        int16_t write_size = (int16_t)((i % (USER_DATA_SIZE - 6)) + 1);
        uint32_t logical_addr = (uint32_t)(i * 100);

        uint8_t *mixed_data = (uint8_t *)malloc(write_size);
        if (!mixed_data) {
            printf("    ERROR: Memory allocation failed at iteration %d\n", i);
            test_passed = 0;
            goto cleanup_stability;
        }

        for (int j = 0; j < write_size; j++) {
            mixed_data[j] = (uint8_t)((i + j) & 0xFF);
        }

        if (eflash_ftl_write_logical(logical_addr, mixed_data, write_size) == 0) {
            mixed_write_success++;

            uint8_t *read_data = (uint8_t *)malloc(write_size);
            if (read_data && eflash_ftl_read_logical(logical_addr, read_data, write_size) == 0) {
                if (memcmp(mixed_data, read_data, write_size) == 0) {
                    mixed_read_success++;
                } else {
                    printf("    ERROR: Data mismatch at addr 0x%06X, size %d\n",
                           logical_addr, write_size);
                    test_passed = 0;
                }
                free(read_data);
            }
        }

        free(mixed_data);

        if ((i + 1) % 200 == 0) {
            printf("    Progress: %d / %d mixed-size operations\n", i + 1, MIXED_SIZE_OPS);
        }
    }

    printf("    Phase 2 Summary:\n");
    printf("      Mixed writes: %d / %d\n", mixed_write_success, MIXED_SIZE_OPS);
    printf("      Mixed reads:  %d / %d\n", mixed_read_success, MIXED_SIZE_OPS);

    if (mixed_read_success == mixed_write_success && mixed_write_success > 0) {
        printf("  [PASS] Phase 2: Mixed-size operations successful\n");
    } else {
        printf("  [FAIL] Phase 2: Some mixed-size operations failed\n");
        test_passed = 0;
    }

    printf("\n  [PHASE 3] Periodic power failure simulation...\n");

    #define POWER_CYCLE_COUNT 10
    int power_cycle_success = 0;
    int first_cycle_dumped = 0;

    for (int cycle = 0; cycle < POWER_CYCLE_COUNT; cycle++) {
        printf("    Power cycle %d / %d...\n", cycle + 1, POWER_CYCLE_COUNT);

        uint32_t free_before = eflash_ftl_get_free_pages();
        printf("      Free pages before GC: %u\n", free_before);
        if (free_before < 50) {
            printf("      Low free space, triggering GC...\n");
            eflash_ftl_gc_collect_all();
            uint32_t free_after = eflash_ftl_get_free_pages();
            printf("      Free pages after GC: %u\n", free_after);
        }

        int write_success = 0;
        int write_failed = 0;

        for (int i = 0; i < 20; i++) {
            uint16_t sector = (uint16_t)(cycle * 20 + i);
            memset(write_buf, (uint8_t)((cycle + 1) * 10 + i), USER_DATA_SIZE);
            int ret = eflash_ftl_write(sector, write_buf);
            assert(ret == 0 && "Phase 3: Write failed - flash full");
            if (ret == 0) {
                write_success++;
            } else {
                write_failed++;
                printf("        WARNING: Write failed for sector %d (ret=%d)\n", sector, ret);
            }
        }
        printf("      Writes: %d succeeded, %d failed\n", write_success, write_failed);

        if (write_success == 0) {
            printf("      Cycle %d: FAILED - all writes failed\n", cycle + 1);
            test_passed = 0;
            continue;
        }

        PRINT_ROOT_STATE("BEFORE_DEINIT");
        if (!first_cycle_dumped) {
            dump_all_pages("BEFORE_DEINIT");
        }
        eflash_deinit();

        eflash_init(TEST_FLASH_FILE);
        eflash_ftl_init();
        PRINT_ROOT_STATE("AFTER_INIT");
        if (!first_cycle_dumped) {
            dump_all_pages("AFTER_INIT");
            first_cycle_dumped = 1;
        }

        int verified = 0;
        for (int i = 0; i < 20; i++) {
            uint16_t sector = (uint16_t)(cycle * 20 + i);
            if (eflash_ftl_read(sector, read_buf) == 0) {
                uint8_t expected = (uint8_t)((cycle + 1) * 10 + i);
                if (read_buf[0] == expected) {
                    verified++;
                }
            }
        }

        if (cycle > 0) {
            uint8_t tmp[USER_DATA_SIZE];
            if (eflash_ftl_read(0, tmp) == 0) {
                printf("      [DEBUG] After cycle %d recovery: sector 0 still readable (first byte=0x%02X)\n", cycle+1, tmp[0]);
            } else {
                printf("      [DEBUG] After cycle %d recovery: sector 0 LOST!\n", cycle+1);
            }
        }

        if (verified > 0) {
            power_cycle_success++;
            printf("      Cycle %d: Verified %d / 20 sectors\n", cycle + 1, verified);
        } else {
            printf("      Cycle %d: FAILED - no data recovered\n", cycle + 1);
            test_passed = 0;
        }
    }

    printf("    Phase 3 Summary:\n");
    printf("      Successful cycles: %d / %d\n", power_cycle_success, POWER_CYCLE_COUNT);

    if (power_cycle_success == POWER_CYCLE_COUNT) {
        printf("  [PASS] Phase 3: All power cycles handled correctly\n");
    } else {
        printf("  [FAIL] Phase 3: Some power cycles failed\n");
        test_passed = 0;
    }

    printf("\n  [PHASE 4] Final comprehensive verification...\n");

    int final_verified = 0;
    int final_errors = 0;

    #define VERIFY_SAMPLE_COUNT 100
    for (int i = 0; i < VERIFY_SAMPLE_COUNT; i++) {
        uint16_t sector = (uint16_t)((i * 19) % 2000);

        if (eflash_ftl_read(sector, read_buf) == 0) {
            final_verified++;
        } else {
            final_errors++;
        }
    }

    printf("    Final Verification Summary:\n");
    printf("      Verified: %d / %d sectors\n", final_verified, VERIFY_SAMPLE_COUNT);
    printf("      Errors:   %d\n", final_errors);
    if (final_errors > 0) {
        printf("      First 10 failed sectors: ");
        int shown = 0;
        for (int i = 0; i < VERIFY_SAMPLE_COUNT && shown < 10; i++) {
            uint16_t sector = (uint16_t)((i * 19) % 2000);
            if (eflash_ftl_read(sector, read_buf) != 0) {
                printf("%d ", sector);
                shown++;
            }
        }
        printf("\n");
    }

    if (final_verified > VERIFY_SAMPLE_COUNT * 0.9) {
        printf("  [PASS] Phase 4: Final verification successful\n");
    } else {
        printf("  [FAIL] Phase 4: Too many verification failures\n");
        test_passed = 0;
    }

cleanup_stability:
    printf("\n========================================\n");
    printf("Long-term Stability Test Summary:\n");
    printf("  Total operations: ~%d\n", STABILITY_OPS + MIXED_SIZE_OPS);
    printf("  Power cycles: %d\n", POWER_CYCLE_COUNT);
    printf("  Overall result: %s\n", test_passed ? "PASSED" : "FAILED");
    printf("========================================\n");

    if (test_passed) {
        printf("[PASSED] test_long_term_stability\n");
        printf("System demonstrated long-term stability!\n");
    } else {
        printf("[FAILED] test_long_term_stability\n");
        printf("Some stability tests failed!\n");
    }
    printf("========================================\n");

    cleanup_test_flash();
    return test_passed ? 0 : 1;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf(" eFlash FTL - Stability Test\n");
    printf("========================================\n\n");

    int passed_count = 0;
    int failed_count = 0;

    printf("\n");
    printf("========================================\n");
    printf(" Running: test_long_term_stability\n");
    printf("========================================\n");
    int ret = test_long_term_stability();
    if (ret != 0) {
        printf("[FAILED] test_long_term_stability returned %d\n", ret);
        failed_count++;
    } else {
        printf("[PASSED] test_long_term_stability\n");
        passed_count++;
    }

    printf("\n========================================\n");
    printf(" Test Summary\n");
    printf("========================================\n");
    printf(" Passed: %d\n", passed_count);
    printf(" Failed: %d\n", failed_count);
    printf(" Total:  %d\n", passed_count + failed_count);
    printf("========================================\n\n");

    return (failed_count > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}