/*
 * Windows compatibility functions
 * Implements functions that are missing on Windows
 */

#ifdef _WIN32
#include <string.h>
#include <ctype.h>

// Implement strcasecmp for Windows
int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        if (tolower(*s1) != tolower(*s2)) {
            return tolower(*s1) - tolower(*s2);
        }
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

// Implement strncasecmp for Windows
int strncasecmp(const char *s1, const char *s2, size_t n) {
    size_t i = 0;
    while (i < n && *s1 && *s2) {
        if (tolower(*s1) != tolower(*s2)) {
            return tolower(*s1) - tolower(*s2);
        }
        s1++;
        s2++;
        i++;
    }
    
    if (i == n) {
        return 0;
    } else {
        return *s1 - *s2;
    }
}

#endif