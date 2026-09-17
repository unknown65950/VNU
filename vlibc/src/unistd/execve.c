#include <vlibc/unistd.h>

int execve(const char* path, char* const argv[], char* const envp[]) {
    return (int)syscall(SYS_execve, path, argv, envp);
}
