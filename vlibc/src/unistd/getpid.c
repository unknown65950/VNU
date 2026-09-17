#include <vlibc/unistd.h>

unsigned long getpid(void) {
    return (unsigned long)syscall(SYS_getpid);
}
