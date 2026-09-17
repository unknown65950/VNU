#include <vlibc/stdio.h>
#include <vlibc/string.h>

int puts(const char* str) {
    if (str == NULL) {
        return -1;
    }
    
    write(STDOUT_FILENO, str, strlen(str));
    putchar('\n');
    return 0;
}
