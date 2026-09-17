#include <vlibc/string.h>

void* memset(void* s, int c, unsigned long n) {
    unsigned char* p = (unsigned char*)s;

    while (n--) {
        *p++ = (unsigned char)c;
    }

    return s;
}
