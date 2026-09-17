#include <vlibc/unistd.h>

unsigned long getuid(void) {
    return (unsigned long)syscall(SYS_getuid);
}
