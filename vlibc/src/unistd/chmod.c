#include <vlibc/sys/syscall.h>

int chmod(const char* path, unsigned int mode) {
    return (int)syscall(VNU_SYS_chmod, path, mode);
}