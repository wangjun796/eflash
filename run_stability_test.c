#include <stdio.h>
#include <stdlib.h>
#include "eflash_ftl.h"
#include "eflash_sim.h"

extern int test_long_term_stability(void);

int main(void) {
    printf("========================================\n");
    printf(" Cache-ON: test_long_term_stability\n");
    printf("========================================\n\n");

    int result = test_long_term_stability();

    printf("\n========================================\n");
    printf(" Result: %s\n", result == 0 ? "PASSED" : "FAILED");
    printf("========================================\n");

    return (result != 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}