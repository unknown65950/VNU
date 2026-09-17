#include <vlibc/unistd.h>

int chdir(const char* path) {
    return (int)syscall(SYS_chdir, path);
}
