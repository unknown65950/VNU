#include <vlibc/unistd.h>
int dup(int oldfd) { return (int)syscall(SYS_dup, oldfd); }
int dup2(int oldfd, int newfd) { return (int)syscall(SYS_dup2, oldfd, newfd); }
