#include <vlibc/string.h>

char* strncpy(char* dest, const char* src, unsigned long n) {
    unsigned long i = 0;

    for (; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }

    for (; i < n; i++) {
        dest[i] = '\0';
    }

    return dest;
}
