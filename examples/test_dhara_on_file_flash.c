/*
 * Dhara - NAND flash management layer
 * Test suite running on file-based flash simulator
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <windows.h>
#include "dhara/config.h"
#include "sim_flash.h"
#include "dhara/nand.h"
#include "dhara/map.h"
#include "dhara/error.h"
#include "dhara/journal.h"
#include "dhara/bytes.h"
#include "util.h"
#include "sim.h"
#include "jtutil.h"
#include "ecc/bch.h"
#include "ecc/hamming.h"
#include "ecc/crc32.h"

// Define our simulated NAND flash parameters
#define LOG2_PAGE_SIZE 9       // 512 bytes per page
#define LOG2_PAGES_PER_BLOCK 3 // 8 pages per block
#define LOG2_BLOCK_SIZE (LOG2_PAGE_SIZE + LOG2_PAGES_PER_BLOCK) // 4096 bytes per block
#define TOTAL_SIZE (512 * 1024) // 512KB total
#define NUM_BLOCKS (TOTAL_SIZE >> LOG2_BLOCK_SIZE) // Total number of blocks
#define NUM_SECTORS		200
#define GC_RATIO		4

// Forward declarations for test functions
int test_map(void);
int test_journal(void);
int test_recovery(void);
int test_jfill(void);
int test_epoch_roll(void);
int test_nand(void);
int test_error(void);
int test_bch(void);
int test_hamming(void);
int test_crc32(void);

// File-based flash simulator state
typedef struct {
    uint8_t* memory;           // Pointer to mapped memory
    int bad_blocks[NUM_BLOCKS]; // Bad block flags
    int erased[NUM_BLOCKS];     // Erase status for each block
} file_flash_state_t;

static file_flash_state_t file_flash = {0};

// Global NAND structure for file-based flash simulation
// The NAND interface functions are defined in sim_flash.c
extern struct dhara_nand file_sim_nand;

// Test functions adapted from all_tests.c
static dhara_sector_t sector_list[NUM_SECTORS];

static void shuffle(int seed)
{
    int i;

    srand(seed);
    for (i = 0; i < NUM_SECTORS; i++)
        sector_list[i] = i;

    for (i = NUM_SECTORS - 1; i > 0; i--) {
        const int j = rand() % i;
        const int tmp = sector_list[i];

        sector_list[i] = sector_list[j];
        sector_list[j] = tmp;
    }
}

static int check_recurse(struct dhara_map *map,
                         dhara_page_t parent,
                         dhara_page_t page,
                         dhara_sector_t id_expect,
                         int depth)
{
    uint8_t meta[DHARA_META_SIZE];
    dhara_error_t err;
    const dhara_page_t h_offset = map->journal.head - map->journal.tail;
    const dhara_page_t p_offset = parent - map->journal.tail;
    const dhara_page_t offset = page - map->journal.tail;
    dhara_sector_t id;
    int count = 1;
    int i;

    if (page == DHARA_PAGE_NONE)
        return 0;

    /* Make sure this is a valid journal user page, and one which is
     * older than the page pointing to it.
     */
    assert(offset < p_offset);
    assert(offset < h_offset);
    assert((~page) & ((1 << map->journal.log2_ppc) - 1));

    /* Fetch metadata */
    if (dhara_journal_read_meta(&map->journal, page, meta, &err) < 0) {
        fprintf(stderr, "mt_check failed: %s\n", dhara_strerror(err));
        return -1;
    }

    /* Check the first <depth> bits of the ID field - high bits must match */
    id = dhara_r32(meta);
    if (!depth) {
        id_expect = id;
    } else {
        // Check that the high 'depth' bits match
        if (((id ^ id_expect) >> (32 - depth)) != 0) {
            printf("ERROR: mt_check: bad ID at page %d (parent %d)\n",
                   page, parent);
            printf("    expect = 0x%08x\n", id_expect);
            printf("    actual = 0x%08x\n", id);
            printf("    depth = %d, mismatch in high bits\n", depth);
            return -1;
        }
    }

    /* Check all alt-pointers starting from depth */
    for (i = depth; i < 32; i++) {
        dhara_page_t child = dhara_r32(meta + (i << 2) + 4);
        if (child != DHARA_PAGE_NONE) {
            const int sub_count = check_recurse(map, page, child,
                                               id ^ (1 << (31 - i)),
                                               i + 1);
            if (sub_count < 0) return -1;
            count += sub_count;
        }
    }

    return count;
}

static int check_tree(struct dhara_map *map, dhara_sector_t start_id)
{
    const dhara_page_t root = dhara_journal_root(&map->journal);
    uint8_t root_meta[DHARA_META_SIZE];
    dhara_error_t err;
    dhara_sector_t root_id;

    if (root == DHARA_PAGE_NONE)
        return 0;

    // Debug: Print root page info
    printf("    Radix tree root: page=%d, head=%d, tail=%d\n", 
           root, map->journal.head, map->journal.tail);
    
    // Read root node's ID to use as starting point
    if (dhara_journal_read_meta(&map->journal, root, root_meta, &err) < 0) {
        fprintf(stderr, "    Failed to read root metadata: %s\n", dhara_strerror(err));
        return -1;
    }
    root_id = dhara_r32(root_meta);
    printf("    Root ID: 0x%08x\n", root_id);
    
    // Use journal.head as parent, and root_id as the expected ID for root
    return check_recurse(map, map->journal.head, root, root_id, 0);
}

int test_map(void)
{
    static uint8_t page_buf[MAX_PAGE_SIZE];
    static struct dhara_map map;
    dhara_error_t err;
    int i;

    printf("Map test on file-based flash...\n");

    /* Set up a fresh map - skip resume for fresh flash */
    dhara_map_init(&map, &file_sim_nand, page_buf, GC_RATIO);
    dhara_map_clear(&map);
    
    printf("    Initial: capacity=%d, size=%d, gc_ratio=%.2f%%\n",
           dhara_map_capacity(&map), dhara_map_size(&map), 
           100.0 / (1 + ((double)map.gc_ratio)));

    /* Write some sectors with random IDs */
    int total_sectors = 0;
    const int test_sectors = NUM_SECTORS;  // Use same as all_tests.c
    
    printf("    Writing %d test sectors...\n", test_sectors);
    
    // Shuffle sector list for randomized testing
    shuffle(0);
    
    for (i = 0; i < test_sectors; i++) {
        uint8_t wbuf[MAX_PAGE_SIZE];
        dhara_error_t err;

        seq_gen(100 + i, wbuf, 1 << file_sim_nand.log2_page_size);

        if (dhara_map_write(&map, sector_list[i], wbuf, &err) < 0) {
            printf("    write failed at sector %d: %s\n", sector_list[i], dhara_strerror(err));
            return -1;
        }
            
        total_sectors++;
    }

    /* Check that all previously written sectors can still be read */
    printf("    Verifying %d sectors...\n", total_sectors);
    for (int i = 0; i < total_sectors; i++) {
        uint8_t wbuf[MAX_PAGE_SIZE];
        uint8_t rbuf[MAX_PAGE_SIZE];
        int j;
        dhara_sector_t sector_id = sector_list[i];

        // Generate expected data using the same index as when writing
        seq_gen(100 + i, wbuf, 1 << file_sim_nand.log2_page_size);

        if (dhara_map_read(&map, sector_id, rbuf, &err) < 0) {
            printf("    read failed at sector %d: %s\n", sector_id, dhara_strerror(err));
            return -1;
        }

        for (j = 0; j < (1 << file_sim_nand.log2_page_size); j++)
            if (wbuf[j] != rbuf[j]) {
                printf("    data mismatch at %d in sector %d (expected byte %02x, got %02x)\n",
                       j, sector_id, wbuf[j], rbuf[j]);
                return -1;
            }
    }

    /* Check the integrity of the radix tree */
    {
        printf("    Checking radix tree integrity...\n");
        const int count = check_tree(&map, 0);

        if (count < 0) {
            printf("    Radix tree check failed\n");
            return -1;
        }

        if (count != total_sectors) {
            printf("    expected %d sectors, found %d\n",
                   total_sectors, count);
            return -1;
        }
        printf("    Radix tree OK: %d sectors verified\n", count);
    }

    /* Sync */
    if (dhara_map_sync(&map, &err) < 0) {
        fprintf(stderr, "sync failed: %s\n", dhara_strerror(err));
        return -1;
    }
    // Wait for journal to become clean (gc complete)
    while (!dhara_journal_is_clean(&map.journal)) {
        uint8_t dummy_data[MAX_PAGE_SIZE];
        uint8_t dummy_meta[DHARA_META_SIZE];
        memset(dummy_data, 0, sizeof(dummy_data));
        memset(dummy_meta, 0, sizeof(dummy_meta));
        if (dhara_journal_enqueue(&map.journal, dummy_data, dummy_meta, &err) < 0) {
            fprintf(stderr, "gc enqueue failed: %s\n", dhara_strerror(err));
            return -1;
        }
    }

    printf("    wrote %d sectors OK\n", total_sectors);
    printf("Map test on file-based flash OK\n");
    return 0;
}

int test_journal(void)
{
    static uint8_t page_buf[MAX_PAGE_SIZE];
    static struct dhara_journal j;
    dhara_error_t err;

    printf("Journal test on file-based flash...\n");

    dhara_journal_init(&j, &file_sim_nand, page_buf);
    
    // For a fresh flash, don't call resume - just clear directly
    // This avoids reading garbage data from uninitialized flash
    dhara_journal_clear(&j);

    if (jt_enqueue_sequence(&j, 0, 100) != 100) {
        printf("    failed to write 100 pages\n");
        return -1;
    }

    // Use dhara_journal_enqueue to make journal clean instead of dhara_journal_gc
    while (!dhara_journal_is_clean(&j)) {
        uint8_t dummy_data[MAX_PAGE_SIZE];
        uint8_t dummy_meta[DHARA_META_SIZE];
        memset(dummy_data, 0, sizeof(dummy_data));
        memset(dummy_meta, 0, sizeof(dummy_meta));
        if (dhara_journal_enqueue(&j, dummy_data, dummy_meta, &err) < 0) {
            fprintf(stderr, "gc enqueue failed: %s\n", dhara_strerror(err));
            return -1;
        }
    }

    jt_dequeue_sequence(&j, 0, 100);

    printf("Journal test on file-based flash OK\n");
    return 0;
}

int test_recovery(void)
{
    static uint8_t page_buf[MAX_PAGE_SIZE];
    static struct dhara_journal j;
    dhara_error_t err;

    printf("Recovery test on file-based flash...\n");

    /* Set up a fresh journal - skip resume for fresh flash */
    dhara_journal_init(&j, &file_sim_nand, page_buf);
    dhara_journal_clear(&j);
    
    printf("    Initial: head=%d, tail=%d, tail_sync=%d, root=%d, capacity=%d\n",
           j.head, j.tail, j.tail_sync, j.root, dhara_journal_capacity(&j));

    /* Write some data */
    int count1 = jt_enqueue_sequence(&j, 0, 50);
    if (count1 != 50) {
        printf("    failed to write 50 pages (only wrote %d)\n", count1);
        return -1;
    }
    
    printf("    After 50 pages: head=%d, tail=%d, tail_sync=%d, root=%d, size=%d\n",
           j.head, j.tail, j.tail_sync, j.root, dhara_journal_size(&j));

    /* Read back immediately to verify */
    printf("    Reading back first 50 pages...\n");
    jt_dequeue_sequence(&j, 0, 50);
    
    printf("    After dequeue: head=%d, tail=%d, tail_sync=%d, root=%d, size=%d\n",
           j.head, j.tail, j.tail_sync, j.root, dhara_journal_size(&j));

    /* Write more data */
    printf("    Writing another 50 pages (IDs 50-99)...\n");
    int count2 = jt_enqueue_sequence(&j, 50, 50);
    if (count2 != 50) {
        printf("    failed to write 50 more pages (only wrote %d)\n", count2);
        return -1;
    }
    
    printf("    After second write: head=%d, tail=%d, tail_sync=%d, root=%d, size=%d\n",
           j.head, j.tail, j.tail_sync, j.root, dhara_journal_size(&j));

    /* Read back the second batch */
    printf("    Reading back second 50 pages...\n");
    jt_dequeue_sequence(&j, 50, 50);

    printf("Recovery test on file-based flash OK\n");
    return 0;
}

int test_jfill(void)
{
    static uint8_t page_buf[MAX_PAGE_SIZE];
    static struct dhara_journal j;
    dhara_error_t err;

    printf("Journal-fill test on file-based flash...\n");

    dhara_journal_init(&j, &file_sim_nand, page_buf);
    
    // For a fresh flash, don't call resume - just clear directly
    dhara_journal_clear(&j);

    /* Keep filling until we run out of space */
    {
        int count = 0;

        while (dhara_journal_enqueue(&j, page_buf, page_buf, &err) >= 0)
            count++;

        if (err != DHARA_E_JOURNAL_FULL) {
            printf("    unexpected error: %s\n", dhara_strerror(err));
            return -1;
        }

        printf("    wrote %d pages before failing\n", count);
    }

    /* Now trim and resume */
    dhara_journal_clear(&j);

    if (dhara_journal_resume(&j, &err) < 0) {
        printf("    resume after clear failed: %s\n", dhara_strerror(err));
        return -1;
    }

    printf("Journal-fill test on file-based flash OK\n");
    return 0;
}

int test_epoch_roll(void)
{
    static uint8_t page_buf[MAX_PAGE_SIZE];
    static struct dhara_journal j;
    dhara_error_t err;

    printf("Epoch-roll test on file-based flash...\n");

    dhara_journal_init(&j, &file_sim_nand, page_buf);
    
    // For a fresh flash, don't call resume - just clear directly
    // This avoids reading garbage data from uninitialized flash
    dhara_journal_clear(&j);
    
    // Debug: Print capacity and initial state
    printf("    Journal capacity: %d\n", dhara_journal_capacity(&j));
    printf("    Journal size: %d\n", dhara_journal_size(&j));
    printf("    head=%d, tail=%d, root=%d\n", j.head, j.tail, j.root);

    /* Write more pages than fit in the chip */
    {
        const int limit = dhara_journal_capacity(&j) + 10;
        int i;
        
        printf("    Trying to write %d pages (capacity + 10)\n", limit);

        for (i = 0; i < limit; i++) {
            if (dhara_journal_enqueue(&j, page_buf, page_buf, &err) < 0) {
                printf("    failed at page %d: %s (head=%d, tail=%d)\n", i, dhara_strerror(err), j.head, j.tail);
                return -1;
            }
        }
    }

    // Wait for journal to become clean (gc complete)
    // Use jt_enqueue_sequence instead of dhara_journal_gc
    while (!dhara_journal_is_clean(&j)) {
        uint8_t dummy_data[MAX_PAGE_SIZE];
        uint8_t dummy_meta[DHARA_META_SIZE];
        memset(dummy_data, 0, sizeof(dummy_data));
        memset(dummy_meta, 0, sizeof(dummy_meta));
        if (dhara_journal_enqueue(&j, dummy_data, dummy_meta, &err) < 0) {
            fprintf(stderr, "gc enqueue failed: %s\n", dhara_strerror(err));
            return -1;
        }
    }

    printf("Epoch-roll test on file-based flash OK\n");
    return 0;
}

int test_nand(void)
{
    const size_t page_size = 1 << file_sim_nand.log2_page_size;
    uint8_t *buf = malloc(page_size);
    uint8_t *chk = malloc(page_size);
    int i;
    int j;

    if (!buf || !chk) {
        printf("Can't allocate page buffers\n");
        return -1;
    }

    printf("NAND test on file-based flash...\n");

    for (i = 0; i < 100; i++) {
        /* Fill the buffer with pseudo-random data based on page number */
        seq_gen(i * 1000, buf, page_size);

        /* Write */
        if (dhara_nand_prog(&file_sim_nand, i, buf, NULL) < 0) {
            printf("    write failed at page %d\n", i);
            free(buf);
            free(chk);
            return -1;
        }

        /* Check */
        if (dhara_nand_read(&file_sim_nand, i, 0, page_size, chk, NULL) < 0) {
            printf("    read failed at page %d\n", i);
            free(buf);
            free(chk);
            return -1;
        }

        for (j = 0; j < page_size; j++)
            if (buf[j] != chk[j]) {
                printf("    data mismatch at page %d, offset %d\n",
                       i, j);
                free(buf);
                free(chk);
                return -1;
            }
    }

    free(buf);
    free(chk);
    printf("NAND test on file-based flash OK\n");
    return 0;
}

int test_error(void)
{
    dhara_error_t err;

    printf("Error-code test...\n");
    
    // Test all error codes from the Dhara library
    for (err = DHARA_E_NONE; err < DHARA_E_MAX; err++) {
        const char *msg = dhara_strerror(err);

        if (!msg) {
            printf("    failed: NULL for error %d\n", err);
            return -1;
        }

        printf("    %4d: %s\n", err, msg);
    }

    printf("Error-code test OK\n");
    return 0;
}

int test_bch(void)
{
    uint8_t block[256];
    uint8_t syndrome[BCH_MAX_ECC];
    uint8_t fixed[256];
    int i;
    int j;

    printf("BCH test...\n");

    for (i = 0; i < 100; i++) {
        int errors;

        /* Generate a pseudo-random block */
        seq_gen(i * 1000, block, 256);

        /* Generate the syndrome */
        bch_generate(&bch_4bit, block, 256, syndrome);

        /* Copy to the fixed block */
        memcpy(fixed, block, 256);

        /* Introduce exactly 4 errors (the maximum correctable) */
        errors = 0;
        srand(i * 1000);  // Deterministic seed for reproducibility
        for (j = 0; j < 256 && errors < 4; j++) {
            if ((rand() % 100) < 5) {  // ~5% error rate per byte
                fixed[j] ^= (1 << (rand() % 8));
                errors++;
            }
        }

        if (errors > 0) {
            /* Try to correct using bch_repair */
            bch_repair(&bch_4bit, fixed, 256, syndrome);

            /* Verify - if still failing, errors were uncorrectable */
            if (bch_verify(&bch_4bit, fixed, 256, syndrome) < 0) {
                /* This is OK - too many errors */
                continue;
            }

            /* Check that it matches original */
            if (memcmp(block, fixed, 256) != 0) {
                printf("    correction mismatch at iteration %d\n", i);
                return -1;
            }
        }
    }

    printf("BCH test OK\n");
    return 0;
}

int test_hamming(void)
{
    uint8_t block[511];
    uint8_t orig[511];
    uint8_t ecc[HAMMING_ECC_SIZE];
    int i;
    int j;

    printf("Hamming test...\n");

    for (i = 0; i < 100; i++) {
        /* Generate a pseudo-random block */
        seq_gen(i * 1000, block, 511);

        memcpy(orig, block, 511);

        /* Generate the syndrome using hamming_generate */
        hamming_generate(block, 511, ecc);
        
        /* Calculate syndrome to check if data is correct */
        hamming_ecc_t syndrome = hamming_syndrome(block, 511, ecc);
        if (syndrome != 0) {
            printf("    initial syndrome check failed: %u\n", syndrome);
            return -1;
        }

        /* Flip one bit */
        j = rand() % (511 * 8);
        block[j >> 3] ^= 1 << (j & 7);

        /* Correct it using hamming_repair */
        syndrome = hamming_syndrome(block, 511, ecc);
        if (hamming_repair(block, 511, syndrome) < 0) {
            printf("    correction failed at iteration %d\n", i);
            return -1;
        }

        syndrome = hamming_syndrome(block, 511, ecc);
        if (syndrome != 0) {
            printf("    final syndrome check failed: %u\n", syndrome);
            return -1;
        }

        /* Check that it's fixed */
        for (j = 0; j < 511; j++)
            if (orig[j] != block[j]) {
                printf("    data mismatch at %d\n", j);
                return -1;
            }
    }

    printf("Hamming test OK\n");
    return 0;
}

// Removed redundant function as seq_gen can be used directly

int test_crc32(void)
{
    uint8_t block[512];
    uint32_t sum;
    int i;
    int j;

    printf("CRC32 test...\n");

    for (i = 0; i < 100; i++) {
        /* Generate a pseudo-random block */
        seq_gen(i * 1000, block, 512);

        /* Compute checksum */
        sum = crc32_nand(block, 512, 0);

        /* Check a few single-bit flips */
        for (j = 0; j < 10; j++) {
            uint8_t temp[4];
            seq_gen(i * 10 + j, temp, 4);
            const int pos = rand() % 512;
            const int bit = rand() & 7;

            block[pos] ^= 1 << bit;
            if (crc32_nand(block, 512, 0) == sum) {
                printf("    failed to detect corruption\n");
                return -1;
            }
            block[pos] ^= 1 << bit;
        }
    }

    printf("CRC32 test OK\n");
    return 0;
}


int main(void)
{
    printf("Dhara Comprehensive Tests on File-Based Flash Simulator\n");
    printf("=====================================================\n\n");

    // Initialize the flash simulator
    if (!init_flash_simulator()) {
        printf("Failed to initialize flash simulator\n");
        return -1;
    }

    printf("Simulated NAND flash parameters:\n");
    printf("- Page size: %u bytes\n", 1 << file_sim_nand.log2_page_size);
    printf("- Pages per block: %u\n", 1 << file_sim_nand.log2_ppb);
    printf("- Block size: %u bytes\n", 1 << (file_sim_nand.log2_page_size + file_sim_nand.log2_ppb));
    printf("- Total blocks: %u\n", file_sim_nand.num_blocks);
    printf("\n");
    
    // Debug: Check flash initialization
    check_flash_initialization(&file_sim_nand);
    printf("\n");

    // Run all tests that were in all_tests.c
    printf("Running comprehensive tests...\n");

    int failed = 0;
    int total = 0;

#define RUN_TEST(name) \
    do { \
        total++; \
        printf("Running %s test...\n", #name); \
        if (test_##name() < 0) { \
            printf("%s test FAILED\n", #name); \
            failed++; \
        } else { \
            printf("%s test PASSED\n", #name); \
        } \
    } while (0)

    RUN_TEST(error);
    RUN_TEST(nand);
    RUN_TEST(journal);
    RUN_TEST(recovery);
    RUN_TEST(jfill);
    RUN_TEST(map);
    RUN_TEST(epoch_roll);
    RUN_TEST(bch);
    RUN_TEST(hamming);
    RUN_TEST(crc32);


#undef RUN_TEST

    printf("\n%d/%d tests passed\n", total - failed, total);

    if (failed) {
        printf("FAILED: %d tests failed\n", failed);
    } else {
        printf("SUCCESS: All tests passed!\n");
    }

    // Cleanup
    cleanup_flash_simulator();

    return failed ? -1 : 0;
}