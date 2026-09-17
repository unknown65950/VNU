#include <vlibc/unistd.h>

int open(const char* path, int flags, ...) {
    /* mode (3rd arg) ignored for now — kernel open takes path + flags only */
    return (int)syscall(SYS_open, path, flags);
}
