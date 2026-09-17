#include <vlibc/unistd.h>

long lseek(int fd, long offset, int whence) {
    return syscall(SYS_lseek, fd, offset, whence);
}
