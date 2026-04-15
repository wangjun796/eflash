/*
 * Windows rand replacement functions
 * Since Visual Studio doesn't support random/srandom functions
 */

#include <stdlib.h>
#include <time.h>

#ifdef _WIN32

// Define srandom and random functions for Windows
void srandom(unsigned int seed) {
    srand(seed);
}

long int random(void) {
    return rand();
}

#endif