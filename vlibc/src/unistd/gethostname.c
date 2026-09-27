/*
 * gethostname — the node name of the machine (POSIX).
 *
 * This kernel has no gethostname(2): the name is a file, /etc/hostname,
 * so the call is made straight on top of open/read. The result is the
 * file's first line without its trailing newline, and the buffer is
 * always NUL-terminated. Returns 0 on success, -1 when the file cannot
 * be read or holds no usable name.
 */
#include <vlibc/unistd.h>
#include <vlibc/fcntl.h>

#define HOSTNAME_F "/etc/hostname"

int gethostname(char* name, unsigned long len)
{
    if (!name || len == 0)
        return -1;

    int fd = open(HOSTNAME_F, O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[80];
    long got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got < 0)
        return -1;
    buf[got] = 0;

    /* Keep the first line only, and trim what an editor may have left
     * around it (CR, spaces). */
    int n = 0;
    while (buf[n] == ' ' || buf[n] == '\t')
        ++n;
    int i = 0;
    while (buf[n + i] && buf[n + i] != '\n' && buf[n + i] != '\r' &&
           i < (int)len - 1)
        ++i;
    if (i == 0)
        return -1;
    for (int j = 0; j < i; ++j)
        name[j] = buf[n + j];
    name[i] = 0;
    return 0;
}
