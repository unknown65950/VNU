#include <vlibc/sys/stat.h>
#include <vlibc/sys/syscall.h>
int stat(const char* path, struct stat* st) {
    return (int)syscall(SYS_stat, path, st);
}
int fstat(int fd, struct stat* st) {
    return (int)syscall(SYS_fstat, fd, st);
}
