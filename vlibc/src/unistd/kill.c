#include <vlibc/unistd.h>
int kill(int pid, int sig) { return (int)syscall(SYS_kill, pid, sig); }
