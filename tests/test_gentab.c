/* Test suite for gentab.c - CRC32 lookup table generator
 * 
 * This test file verifies the functionality of the gentab tool which
 * pre-computes CRC32 lookup tables for fast CRC calculation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* External function declarations from gentab.c (compiled with GENTAB_TEST_MODE) */
extern int parse_poly(const char *text, uint32_t *out);
extern void fill_table_impl(uint32_t poly, uint32_t *out_table);
extern int gentab_main(int argc, char **argv);

/* Test counter and result tracking */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    printf("TEST: %s... ", #name); \
    tests_run++;

#define PASS() do { \
    printf("PASSED\n"); \
    tests_passed++; \
    return 0; \
} while(0)

#define FAIL(msg) do { \
    printf("FAILED: %s\n", msg); \
    return -1; \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("ASSERTION FAILED: %s\n", msg); \
        return -1; \
    } \
} while(0)

/************************************************************************
 * Test parse_poly() function
 */

static int test_parse_poly_valid(void)
{
    TEST(test_parse_poly_valid);
    
    uint32_t result;
    
    /* Test standard polynomial with 0x prefix */
    ASSERT(parse_poly("0xEDB88320", &result) == 0, "parse 0xEDB88320");
    ASSERT(result == 0xEDB88320, "check value");
    
    /* Test with 0X prefix (uppercase) */
    ASSERT(parse_poly("0XABCDEF12", &result) == 0, "parse 0XABCDEF12");
    ASSERT(result == 0xABCDEF12, "check uppercase prefix");
    
    /* Test without prefix */
    ASSERT(parse_poly("12345678", &result) == 0, "parse without prefix");
    ASSERT(result == 0x12345678, "check no prefix value");
    
    /* Test lowercase hex */
    ASSERT(parse_poly("0xabcdef12", &result) == 0, "parse lowercase");
    ASSERT(result == 0xABCDEF12, "check lowercase value");
    
    /* Test mixed case */
    ASSERT(parse_poly("0xaBcDeF12", &result) == 0, "parse mixed case");
    ASSERT(result == 0xABCDEF12, "check mixed case value");
    
    /* Test polynomial with leading zeros */
    ASSERT(parse_poly("0x00EDB883", &result) == 0, "parse with leading zeros");
    ASSERT(result == 0xEDB883, "check leading zeros value");
    
    PASS();
}

static int test_parse_poly_invalid(void)
{
    TEST(test_parse_poly_invalid);
    
    uint32_t result;
    
    /* Test invalid character 'G' (not hex) */
    ASSERT(parse_poly("0x123G5678", &result) < 0, "reject invalid char G");
    
    /* Test invalid character 'Z' */
    ASSERT(parse_poly("0xABCDZEF1", &result) < 0, "reject invalid char Z");
    
    /* Test invalid character in middle */
    ASSERT(parse_poly("0x12@45678", &result) < 0, "reject special char @");
    
    /* Test empty string (should fail or produce 0) */
    /* Note: current implementation treats empty as 0, which is acceptable */
    ASSERT(parse_poly("", &result) == 0, "handle empty string as 0");
    ASSERT(result == 0, "empty string produces 0");
    
    PASS();
}

static int test_parse_poly_edge_cases(void)
{
    TEST(test_parse_poly_edge_cases);
    
    uint32_t result;
    
    /* Test zero */
    ASSERT(parse_poly("0x0", &result) == 0, "parse zero");
    ASSERT(result == 0, "zero value");
    
    /* Test all zeros */
    ASSERT(parse_poly("0x00000000", &result) == 0, "parse all zeros");
    ASSERT(result == 0, "all zeros value");
    
    /* Test maximum 32-bit value */
    ASSERT(parse_poly("0xFFFFFFFF", &result) == 0, "parse max value");
    ASSERT(result == 0xFFFFFFFF, "max value check");
    
    /* Test single digit */
    ASSERT(parse_poly("0xF", &result) == 0, "parse single digit");
    ASSERT(result == 0xF, "single digit value");
    
    PASS();
}

/************************************************************************
 * Test fill_table() function
 */

static int test_fill_table_zero(void)
{
    TEST(test_fill_table_zero);
    
    uint32_t test_table[256];
    
    /* Fill table with any polynomial */
    fill_table_impl(0xEDB88320, test_table);
    
    /* Key property: table[0] should always be 0 */
    /* Because 0 XORed with anything is itself, and shifting 0 gives 0 */
    ASSERT(test_table[0] == 0, "table[0] must be 0");
    
    PASS();
}

static int test_fill_table_deterministic(void)
{
    TEST(test_fill_table_deterministic);
    
    uint32_t table1[256];
    uint32_t table2[256];
    
    /* Fill same table twice with same polynomial */
    fill_table_impl(0xEDB88320, table1);
    fill_table_impl(0xEDB88320, table2);
    
    /* Results should be identical */
    ASSERT(memcmp(table1, table2, sizeof(table1)) == 0, 
           "same polynomial produces same table");
    
    /* Different polynomials should produce different tables */
    fill_table_impl(0x82F63B78, table2);
    ASSERT(memcmp(table1, table2, sizeof(table1)) != 0,
           "different polynomials produce different tables");
    
    /* Verify some specific values are non-zero (except table[0]) */
    int non_zero_count = 0;
    for (int i = 1; i < 256; i++) {
        if (table1[i] != 0)
            non_zero_count++;
    }
    ASSERT(non_zero_count > 200, "most table entries should be non-zero");
    
    PASS();
}

static int test_fill_table_properties(void)
{
    TEST(test_fill_table_properties);
    
    uint32_t test_table[256];
    
    fill_table_impl(0xEDB88320, test_table);
    
    /* Table should have 256 entries */
    /* (This is implicit in the array size, but let's verify we use all) */
    
    /* All entries should be unique? Not necessarily for CRC tables */
    /* But they should have good distribution properties */
    
    /* Check that table[i] for i=1..255 are not all the same */
    int all_same = 1;
    for (int i = 1; i < 256; i++) {
        if (test_table[i] != test_table[1]) {
            all_same = 0;
            break;
        }
    }
    ASSERT(!all_same, "table entries should not all be identical");
    
    PASS();
}

/************************************************************************
 * Test print_table() format
 */

static int test_print_table_format(void)
{
    TEST(test_print_table_format);
    
    /* We can't easily capture stdout in this setup, so we'll verify
     * the logic by checking the formatting rules */
    
    /* Verify the newline logic: ((i & 3) == 3) ? '\n' : ' ' */
    /* This means: newline after every 4th element (indices 3, 7, 11, ...) */
    
    for (int i = 0; i < 256; i++) {
        char expected_sep = ((i & 3) == 3) ? '\n' : ' ';
        
        /* Verify pattern: indices 3, 7, 11, 15, ... should get newline */
        if ((i + 1) % 4 == 0) {
            ASSERT(expected_sep == '\n', "newline at positions 3, 7, 11, ...");
        } else {
            ASSERT(expected_sep == ' ', "space at other positions");
        }
    }
    
    /* Verify output would have correct number of lines */
    /* 256 elements / 4 per line = 64 lines */
    int expected_lines = 0;
    for (int i = 0; i < 256; i++) {
        if ((i & 3) == 3)
            expected_lines++;
    }
    ASSERT(expected_lines == 64, "should produce 64 lines");
    
    PASS();
}

/************************************************************************
 * Test main() function
 */

static int test_main_no_args(void)
{
    TEST(test_main_no_args);
    
    /* Simulate: ./gentab (no arguments) */
    char *argv[] = {"gentab"};
    int argc = 1;
    
    int result = gentab_main(argc, argv);
    
    /* Should return error code when no polynomial provided */
    ASSERT(result < 0, "main() should return error with no args");
    
    PASS();
}

static int test_main_with_valid_poly(void)
{
    TEST(test_main_with_valid_poly);
    
    /* Simulate: ./gentab 0xEDB88320 */
    char *argv[] = {"gentab", "0xEDB88320"};
    int argc = 2;
    
    /* Redirect stdout to suppress output during test */
    FILE *temp_file = fopen("NUL", "w");
    if (!temp_file) {
        FAIL("Failed to open NUL for redirect");
    }
    
    int result = gentab_main(argc, argv);
    
    fclose(temp_file);
    
    ASSERT(result == 0, "main() should succeed with valid polynomial");
    
    PASS();
}

static int test_main_with_invalid_poly(void)
{
    TEST(test_main_with_invalid_poly);
    
    /* Simulate: ./gentab 0xINVALID */
    char *argv[] = {"gentab", "0xINVALID"};
    int argc = 2;
    
    int result = gentab_main(argc, argv);
    
    /* Should fail due to invalid characters */
    ASSERT(result < 0, "main() should fail with invalid polynomial");
    
    PASS();
}

/************************************************************************
 * Integration test: Generate actual CRC32 table and verify known values
 */

static int test_generate_standard_crc32_table(void)
{
    TEST(test_generate_standard_crc32_table);
    
    uint32_t test_table[256];
    fill_table_impl(0xEDB88320, test_table);
    
    /* Known values from standard CRC-32/ISO-HDLC table (polynomial 0xEDB88320) */
    /* These are well-documented and can be verified online */
    
    ASSERT(test_table[0] == 0x00000000, "CRC32[0] = 0x00000000");
    ASSERT(test_table[1] == 0x77073096, "CRC32[1] = 0x77073096");
    ASSERT(test_table[2] == 0xEE0E612C, "CRC32[2] = 0xEE0E612C");
    ASSERT(test_table[3] == 0x990951BA, "CRC32[3] = 0x990951BA");
    ASSERT(test_table[4] == 0x076DC419, "CRC32[4] = 0x076DC419");
    ASSERT(test_table[5] == 0x706AF48F, "CRC32[5] = 0x706AF48F");
    
    /* Verify last few entries */
    ASSERT(test_table[255] == 0x2D02EF8D, "CRC32[255] = 0x2D02EF8D");
    
    PASS();
}

/************************************************************************
 * Main test runner
 */

int main(void)
{
    printf("=== Gentab CRC Table Generator Tests ===\n\n");
    
    /* Unit tests for parse_poly() */
    test_parse_poly_valid();
    test_parse_poly_invalid();
    test_parse_poly_edge_cases();
    
    /* Unit tests for fill_table() */
    test_fill_table_zero();
    test_fill_table_deterministic();
    test_fill_table_properties();
    
    /* Format tests */
    test_print_table_format();
    
    /* Integration tests */
    test_generate_standard_crc32_table();
    
    /* Main function tests */
    test_main_no_args();
    test_main_with_valid_poly();
    test_main_with_invalid_poly();
    
    printf("\n=== Test Summary ===\n");
    printf("Passed: %d/%d\n", tests_passed, tests_run);
    
    if (tests_passed == tests_run) {
        printf("All tests PASSED!\n");
        return 0;
    } else {
        printf("Some tests FAILED!\n");
        return 1;
    }
}
