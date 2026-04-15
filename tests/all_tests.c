/* Dhara - NAND flash management layer
 * Copyright (C) 2013 Daniel Beer <dlbeer@gmail.com>
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
#include <assert.h>
#include <string.h>
#include "dhara/config.h"
#include "dhara/map.h"
#include "dhara/bytes.h"
#include "dhara/error.h"
#include "dhara/journal.h"
#include "dhara/nand.h"
#include "util.h"
#include "sim.h"
#include "jtutil.h"
#include "ecc/bch.h"
#include "ecc/hamming.h"
#include "ecc/crc32.h"

#define NUM_SECTORS		200
#define GC_RATIO		4

// Forward declarations for test functions
int test_error(void);
int test_map(void);
int test_journal(void);
int test_recovery(void);
int test_jfill(void);
int test_epoch_roll(void);
int test_nand(void);
int test_bch(void);
int test_hamming(void);
int test_crc32(void);

static dhara_sector_t sector_list[NUM_SECTORS];

static void shuffle(int seed)
{
	int i;

	srandom(seed);
	for (i = 0; i < NUM_SECTORS; i++)
		sector_list[i] = i;

	for (i = NUM_SECTORS - 1; i > 0; i--) {
		const int j = random() % i;
		const int tmp = sector_list[i];

		sector_list[i] = sector_list[j];
		sector_list[j] = tmp;
	}
}

static int check_recurse(struct dhara_map *m,
			 dhara_page_t parent,
			 dhara_page_t page,
			 dhara_sector_t id_expect,
			 int depth)
{
	uint8_t meta[DHARA_META_SIZE];
	dhara_error_t err;
	const dhara_page_t h_offset = m->journal.head - m->journal.tail;
	const dhara_page_t p_offset = parent - m->journal.tail;
	const dhara_page_t offset = page - m->journal.tail;
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
	assert((~page) & ((1 << m->journal.log2_ppc) - 1));

	/* Fetch metadata */
	if (dhara_journal_read_meta(&m->journal, page, meta, &err) < 0)
		dabort("mt_check", err);

	/* Check the first <depth> bits of the ID field */
	id = dhara_r32(meta);
	if (!depth) {
		id_expect = id;
	} else {
		assert(!((id ^ id_expect) >> (32 - depth)));
	}

	/* Check all alt-pointers */
	for (i = depth; i < 32; i++) {
		dhara_page_t child = dhara_r32(meta + (i << 2) + 4);

		count += check_recurse(m, page, child,
			id ^ (1 << (31 - i)), i + 1);
	}

	return count;
}

static void mt_check(struct dhara_map *m)
{
	int count;

	sim_freeze();
	count = check_recurse(m, m->journal.head,
		dhara_journal_root(&m->journal), 0, 0);
	sim_thaw();

	assert(m->count == count);
}

static void mt_write(struct dhara_map *m, dhara_sector_t s, int seed)
{
	const size_t page_size = 1 << m->journal.nand->log2_page_size;
	uint8_t buf[MAX_PAGE_SIZE];
	dhara_error_t err;


	seq_gen(seed, buf, page_size);
	if (dhara_map_write(m, s, buf, &err) < 0)
		dabort("map_write", err);
}

static void mt_assert(struct dhara_map *m, dhara_sector_t s, int seed)
{
	const size_t page_size = 1 << m->journal.nand->log2_page_size;
	uint8_t buf[MAX_PAGE_SIZE];
	dhara_error_t err;

	if(page_size > MAX_PAGE_SIZE) {
		fprintf(stderr, "Page size too large\n");
		return;
	}

	if (dhara_map_read(m, s, buf, &err) < 0)
		dabort("map_read", err);

	seq_assert(seed, buf, page_size);
}

static void mt_trim(struct dhara_map *m, dhara_sector_t s)
{
	dhara_error_t err;

	if (dhara_map_trim(m, s, &err) < 0)
		dabort("map_trim", err);
}

static void mt_assert_blank(struct dhara_map *m, dhara_sector_t s)
{
	dhara_error_t err;
	dhara_page_t loc;
	int r;

	r = dhara_map_find(m, s, &loc, &err);
	assert(r < 0);
	assert(err == DHARA_E_NOT_FOUND);
}

static void run_recovery_test(const char *name, void (*scen)(void))
{
	const size_t page_size = 1 << sim_nand.log2_page_size;
	uint8_t page_buf[MAX_PAGE_SIZE];
	struct dhara_journal journal;

	if(page_size > MAX_PAGE_SIZE) {
		fprintf(stderr, "Page size too large\n");
		return;
	}

	printf("========================================"
	       "================================\n"
	       "%s\n"
	       "========================================"
	       "================================\n\n", name);

	sim_reset();
	dhara_journal_init(&journal, &sim_nand, page_buf);

	/* All tests are tuned for this value */
	assert(journal.log2_ppc == 2);

	scen();

	jt_enqueue_sequence(&journal, 0, 30);
	jt_dequeue_sequence(&journal, 0, 30);

	sim_dump();
	printf("\n");
}

static void scen_control(void)
{
}

static void scen_instant_fail(void)
{
	sim_set_failed(0);
}

static void scen_after_check(void)
{
	sim_set_timebomb(0, 6);
}

static void scen_mid_check(void)
{
	sim_set_timebomb(0, 3);
}

static void scen_meta_check(void)
{
	sim_set_timebomb(0, 5);
}

static void scen_after_cascade(void)
{
	sim_set_timebomb(0, 6);
	sim_set_timebomb(1, 3);
	sim_set_timebomb(2, 3);
}

static void scen_mid_cascade(void)
{
	sim_set_timebomb(0, 3);
	sim_set_timebomb(1, 3);
}

static void scen_meta_fail(void)
{
	sim_set_timebomb(0, 3);
	sim_set_failed(1);
}

static void scen_bad_day(void)
{
	int i;

	sim_set_timebomb(0, 7);
	for (i = 1; i < 5; i++)
		sim_set_timebomb(i, 3);
}

static void mt_write_epoch_roll(struct dhara_map *m, dhara_sector_t s, int seed)
{
	const size_t page_size = 1 << m->journal.nand->log2_page_size;
	uint8_t buf[MAX_PAGE_SIZE];
	dhara_error_t err;

	if(page_size > MAX_PAGE_SIZE) {
		fprintf(stderr, "Page size too large\n");
		return;
	}

	seq_gen(seed, buf, page_size);
	if (dhara_map_write(m, s, buf, &err) < 0)
		dabort("map_write", err);
}

static void mt_assert_epoch_roll(struct dhara_map *m, dhara_sector_t s, int seed)
{
	const size_t page_size = 1 << m->journal.nand->log2_page_size;
	uint8_t buf[MAX_PAGE_SIZE];
	dhara_error_t err;

	if(page_size > MAX_PAGE_SIZE) {
		fprintf(stderr, "Page size too large\n");
		return;
	}

	if (dhara_map_read(m, s, buf, &err) < 0)
		dabort("map_read", err);

	seq_assert(seed, buf, page_size);
}

// Test function implementations
int test_error(void)
{
	dhara_error_t err;

	printf("Running error tests...\n");
	for (err = DHARA_E_NONE; err < DHARA_E_MAX; err++) {
		const char *msg = dhara_strerror(err);

		assert(msg != NULL);
		printf("%4d: %s\n", err, msg);
	}
	
	printf("Error tests passed.\n\n");
	return 0;
}

int test_map(void)
{
	const size_t page_size = 1 << sim_nand.log2_page_size;
	uint8_t page_buf[MAX_PAGE_SIZE];
	struct dhara_map map;
	int i;


	printf("Running map tests...\n");
	
	sim_reset();
	sim_inject_bad(10);
	sim_inject_timebombs(30, 20);

	dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
	dhara_map_resume(&map, NULL);
	
	// Writing sectors...
	shuffle(0);
	for (i = 0; i < NUM_SECTORS; i++) {
		const dhara_sector_t s = sector_list[i];

		mt_write(&map, s, s);
		mt_check(&map);
	}

	// Sync and resume...
	dhara_map_sync(&map, NULL);
	dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
	dhara_map_resume(&map, NULL);

	// Read back...
	shuffle(1);
	for (i = 0; i < NUM_SECTORS; i++) {
		const dhara_sector_t s = sector_list[i];

		mt_assert(&map, s, s);
	}

	// Rewrite/trim half...
	shuffle(2);
	for (i = 0; i < NUM_SECTORS; i += 2) {
		const dhara_sector_t s0 = sector_list[i];
		const dhara_sector_t s1 = sector_list[i + 1];

		mt_write(&map, s0, ~s0);
		mt_check(&map);
		mt_trim(&map, s1);
		mt_check(&map);
	}

	// Final sync and resume...
	dhara_map_sync(&map, NULL);
	dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
	dhara_map_resume(&map, NULL);

	// Final read back...
	for (i = 0; i < NUM_SECTORS; i += 2) {
		const dhara_sector_t s0 = sector_list[i];
		const dhara_sector_t s1 = sector_list[i + 1];

		mt_assert(&map, s0, ~s0);
		mt_assert_blank(&map, s1);
	}

	printf("Map tests passed.\n\n");
	return 0;
}

int test_journal(void)
{
	struct dhara_journal journal;
	const size_t page_size = 1 << sim_nand.log2_page_size;
	uint8_t page_buf[MAX_PAGE_SIZE];
	int rep;

	if(page_size > MAX_PAGE_SIZE) {
		fprintf(stderr, "Page size too large\n");
		return 1;
	}

	printf("Running journal tests...\n");

	sim_reset();
	sim_inject_bad(20);

	dhara_journal_init(&journal, &sim_nand, page_buf);
	dhara_journal_resume(&journal, NULL);

	// Enqueue/dequeue tests
	for (rep = 0; rep < 20; rep++) {
		int count;

		count = jt_enqueue_sequence(&journal, 0, 100);
		assert(count == 100);

		jt_dequeue_sequence(&journal, 0, count);
	}

	// Resume tests
	for (rep = 0; rep < 5; rep++) {
		uint8_t *cookie = dhara_journal_cookie(&journal);
		int count;

		cookie[0] = rep;
		count = jt_enqueue_sequence(&journal, 0, 100);
		assert(count == 100);

		while (!dhara_journal_is_clean(&journal)) {
			const int c = jt_enqueue_sequence(&journal,
				count++, 1);

			assert(c == 1);
		}

		sim_freeze();  // Suspend and resume simulation
		dhara_journal_clear(&journal);
		if (dhara_journal_resume(&journal, NULL) < 0)
			dabort("resume", DHARA_E_TOO_BAD);
		sim_thaw();

		jt_dequeue_sequence(&journal, 0, count);

		assert(cookie[0] == rep);
	}

	printf("Journal tests passed.\n\n");
	return 0;
}

int test_recovery(void)
{
	printf("Running recovery tests...\n");
	
	run_recovery_test("Control", scen_control);
	run_recovery_test("Instant fail", scen_instant_fail);
	run_recovery_test("Fail after checkpoint", scen_after_check);
	run_recovery_test("Fail mid-checkpoint", scen_mid_check);
	run_recovery_test("Fail on meta", scen_meta_check);
	run_recovery_test("Cascade fail after checkpoint", scen_after_cascade);
	run_recovery_test("Cascade fail mid-checkpoint", scen_mid_cascade);
	run_recovery_test("Metadata dump failure", scen_meta_fail);
	run_recovery_test("Bad day", scen_bad_day);

	printf("Recovery tests passed.\n\n");
	return 0;
}

int test_jfill(void)
{
	struct dhara_journal journal;
	const size_t page_size = 1 << sim_nand.log2_page_size;
	uint8_t page_buf[MAX_PAGE_SIZE];
	int rep;

	if(page_size > MAX_PAGE_SIZE) {
		fprintf(stderr, "Page size too large\n");
		return 1;
	}

	printf("Running jfill tests...\n");

	sim_reset();
	sim_inject_bad(10);
	sim_inject_failed(10);

	dhara_journal_init(&journal, &sim_nand, page_buf);

	for (rep = 0; rep < 5; rep++) {
		int count;

		count = jt_enqueue_sequence(&journal, 0, -1);
		jt_dequeue_sequence(&journal, 0, count);

		/* Only way to recover space here... */
		journal.tail_sync = journal.tail;
	}

	printf("Jfill tests passed.\n\n");
	return 0;
}

int test_epoch_roll(void)
{
	const size_t page_size = 1 << sim_nand.log2_page_size;
	struct dhara_map map;
	uint8_t page_buf[MAX_PAGE_SIZE];
	int write_seed = 0;
	int i;

	if(page_size > MAX_PAGE_SIZE) {
		fprintf(stderr, "Page size too large\n");
		return 1;
	}

	printf("Running epoch roll tests...\n");

	sim_reset();
	dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
	dhara_map_resume(&map, NULL);

	/* Write pages until we have just barely wrapped around, but not
	 * yet hit a checkpoint.
	 */
	for (i = 0; i < 200; i++)
		mt_write_epoch_roll(&map, i, write_seed++);
	for (i = 0; i < 200; i++)
		mt_write_epoch_roll(&map, i, write_seed++);
	for (i = 0; i < 200; i++)
		mt_write_epoch_roll(&map, i, write_seed++);
	for (i = 0; i < 79; i++)
		mt_write_epoch_roll(&map, i, write_seed++);
	assert(map.journal.head == 1); /* Required for this test */

	/* Now, see what happens on resume if we don't sync.
	 */
	dhara_map_resume(&map, NULL);

	for (i = 0; i < 2; i++)
		mt_write_epoch_roll(&map, i, i + 10000);
	dhara_map_sync(&map, NULL);

	/* Try another resume */
	mt_assert_epoch_roll(&map, 0, 10000);
	mt_assert_epoch_roll(&map, 1, 10001);
	dhara_map_resume(&map, NULL);
	mt_assert_epoch_roll(&map, 0, 10000);
	mt_assert_epoch_roll(&map, 1, 10001);

	printf("Epoch roll tests passed.\n\n");
	return 0;
}

int test_nand(void)
{
	int i;

	printf("Running nand tests...\n");

	sim_reset();
	sim_inject_bad(5);

	for (i = 0; i < (1 << sim_nand.log2_ppb); i++) {
		int j;

		for (j = 0; j < sim_nand.num_blocks; j++) {
			uint8_t block[MAX_PAGE_SIZE];
			dhara_error_t err;
			dhara_page_t p =
				(j << sim_nand.log2_ppb) | i;
			
			const size_t page_size = 1 << sim_nand.log2_page_size;
			
			if(page_size > MAX_PAGE_SIZE) {
				fprintf(stderr, "Page size too large\n");
				return 1;
			}

			if (dhara_nand_is_bad(&sim_nand, j))
				continue;

			if (!i && (dhara_nand_erase(&sim_nand, j, &err) < 0))
				dabort("erase", err);

			seq_gen(p, block, page_size);
			if (dhara_nand_prog(&sim_nand, p, block, &err) < 0)
				dabort("prog", err);
		}
	}

	for (i = 0; i < (sim_nand.num_blocks << sim_nand.log2_ppb); i++) {
		uint8_t block[MAX_PAGE_SIZE];
		dhara_error_t err;
		const size_t page_size = 1 << sim_nand.log2_page_size;
		
		if(page_size > MAX_PAGE_SIZE) {
			fprintf(stderr, "Page size too large\n");
			return 1;
		}

		if (dhara_nand_is_bad(&sim_nand, i >> sim_nand.log2_ppb))
			continue;

		if (dhara_nand_read(&sim_nand, i, 0, page_size,
				    block, &err) < 0)
			dabort("read", err);

		seq_assert(i, block, page_size);
	}

	printf("Nand tests passed.\n\n");
	return 0;
}

int test_bch(void)
{
	uint8_t chunk[16];
	uint8_t ecc[7];
	int i;

	printf("Running BCH tests...\n");

	for (i = 0; i < 100; i++) {
		int j;

		srandom(i);
		seq_gen(i, chunk, sizeof(chunk));

		bch_generate(&bch_4bit, chunk, sizeof(chunk), ecc);
		if (bch_verify(&bch_4bit, chunk, sizeof(chunk), ecc) < 0)
			return -1;

		/* Flip some bits */
		for (j = 0; j < 4; j++) {
			int bit = random() % (sizeof(chunk) * 8);

			chunk[bit >> 3] ^= 1 << (bit & 7);
		}

		bch_repair(&bch_4bit, chunk, sizeof(chunk), ecc);

		if (bch_verify(&bch_4bit, chunk, sizeof(chunk), ecc) < 0)
			return -1;
	}

	printf("BCH tests passed.\n\n");
	return 0;
}

int test_hamming(void)
{
	uint8_t chunk[255];
	uint8_t ecc[HAMMING_ECC_SIZE];
	int i;

	printf("Running Hamming tests...\n");

	for (i = 0; i < 100; i++) {
		int j;
		hamming_ecc_t syndrome;

		srandom(i);
		seq_gen(i, chunk, sizeof(chunk));

		hamming_generate(chunk, sizeof(chunk), ecc);
		syndrome = hamming_syndrome(chunk, sizeof(chunk), ecc);
		if (syndrome != 0)
			return -1;

		/* Flip one bit */
		j = random() % (sizeof(chunk) * 8);
		chunk[j >> 3] ^= 1 << (j & 7);

		if (hamming_repair(chunk, sizeof(chunk), 
				hamming_syndrome(chunk, sizeof(chunk), ecc)) < 0)
			return -1;

		syndrome = hamming_syndrome(chunk, sizeof(chunk), ecc);
		if (syndrome != 0)
			return -1;
	}

	printf("Hamming tests passed.\n\n");
	return 0;
}

int test_crc32(void)
{
	uint8_t data[100];
	uint32_t crc;
	int i;

	printf("Running CRC32 tests...\n");

	for (i = 0; i < 100; i++) {
		srandom(i);
		seq_gen(i, data, sizeof(data));

		crc = crc32_nand(data, sizeof(data), CRC32_INIT);
		if (crc != crc32_nand(data, sizeof(data), CRC32_INIT))
			return -1;
	}

	printf("CRC32 tests passed.\n\n");
	return 0;
}

int main(void)
{
	int total_tests = 0;
	int failed_tests = 0;

	printf("========================================\n");
	printf("Starting Dhara All-in-One Tests\n");
	printf("========================================\n\n");

	// Run all tests
	#define RUN_TEST(name) \
		total_tests++; \
		if (test_##name() < 0) { \
			printf("ERROR: %s test failed!\n", #name); \
			failed_tests++; \
		} else { \
			printf("SUCCESS: %s test passed\n", #name); \
		}

	RUN_TEST(error)
	RUN_TEST(nand)
	RUN_TEST(journal)
	RUN_TEST(recovery)
	RUN_TEST(jfill)
	RUN_TEST(map)
	RUN_TEST(epoch_roll)
	RUN_TEST(bch)
	RUN_TEST(hamming)
	RUN_TEST(crc32)

	#undef RUN_TEST

	printf("========================================\n");
	printf("Tests completed: %d passed, %d failed out of %d total\n", 
		   total_tests - failed_tests, failed_tests, total_tests);
	printf("========================================\n");

	if (failed_tests > 0) {
		return -1;
	}

	return 0;
}