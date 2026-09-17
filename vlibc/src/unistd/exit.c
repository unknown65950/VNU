#include <vlibc/unistd.h>
#include <vlibc/sys/syscall.h>

void exit(int status) {
    syscall(SYS_exit, status);
    // Никогда не возвращаемся
}
