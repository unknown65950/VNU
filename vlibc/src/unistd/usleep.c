#include <vlibc/unistd.h>
#include <vnu/abi.h>

int usleep(unsigned long usec)
{
    unsigned long ms = (usec + 999) / 1000;
    return (int)syscall(SYS_sleep, ms);
}

unsigned int sleep(unsigned int seconds)
{
    unsigned long ms = (unsigned long)seconds * 1000;
    if (ms > 0xFFFFFFFFul)
        ms = 0xFFFFFFFFul;
    return syscall(SYS_sleep, ms) < 0 ? 1 : 0;
}