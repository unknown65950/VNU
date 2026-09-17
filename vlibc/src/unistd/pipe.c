#include <vlibc/unistd.h>
int pipe(int fds[2]) { return (int)syscall(SYS_pipe, fds); }
