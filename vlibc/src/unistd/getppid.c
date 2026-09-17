#include <vlibc/unistd.h>

unsigned long getppid(void) {
    return (unsigned long)syscall(SYS_getppid);
}
