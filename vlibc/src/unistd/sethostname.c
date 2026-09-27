/*
 * sethostname — set the node name of the machine (POSIX).
 *
 * The name lives in /etc/hostname, so this rewrites that file: the same
 * rule the kernel applies when it records a name on an installed disk
 * (see vnu/kernel/include/vnu/host.h) is checked here first, so a bad
 * name never reaches the disk.
 *
 * A name is 1..63 characters of letters, digits, '-', '_' and '.', and
 * must start and end with a letter or digit.
 *
 * Returns 0 on success, -1 when the name is refused or the file cannot
 * be written. Only root can write it (the file is root-owned 0644).
 */
#include <vlibc/unistd.h>
#include <vlibc/fcntl.h>

#define HOSTNAME_F "/etc/hostname"
#define HOSTNAME_MAX 64 /* 63 characters plus the NUL */

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static int is_mid(char c)
{
    return is_alnum(c) || c == '-' || c == '_' || c == '.';
}

int sethostname(const char* name)
{
    if (!name)
        return -1;
    int n = 0;
    for (; name[n]; ++n) {
        if (n >= HOSTNAME_MAX - 1 || !is_mid(name[n]))
            return -1;
        if (n == 0 && !is_alnum(name[n]))
            return -1;
    }
    if (n == 0 || !is_alnum(name[n - 1]))
        return -1;

    char line[HOSTNAME_MAX];
    for (int i = 0; i < n; ++i)
        line[i] = name[i];
    line[n] = '\n';

    int fd = open(HOSTNAME_F, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    long put = write(fd, line, (unsigned long)n + 1);
    close(fd);
    return put == n + 1 ? 0 : -1;
}
