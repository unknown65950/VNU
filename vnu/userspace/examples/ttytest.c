/* Small POSIX-compat smoke test: exercises isatty(), getppid() and
 * stat()'s new file-type bits against /dev and /proc. */
#include <vlibc/unistd.h>
#include <vlibc/stdio.h>
#include <vlibc/fcntl.h>
#include <vlibc/sys/stat.h>

static void show(const char* path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        printf("%s: stat failed\n", path);
        return;
    }
    const char* kind = S_ISDIR(st.st_mode) ? "dir"
                     : S_ISCHR(st.st_mode) ? "chardev"
                     : S_ISREG(st.st_mode) ? "regular"
                                           : "other";
    printf("%s: %s size=%d\n", path, kind, (int)st.st_size);
}

int main(void)
{
    printf("isatty(stdout)=%d\n", isatty(1));
    printf("getpid=%d getppid=%d\n", (int)getpid(), (int)getppid());
    show("/dev/null");
    show("/dev");
    show("/etc/motd");
    show("/proc/version");
    int fd = open("/dev/tty", O_RDONLY);
    printf("isatty(/dev/tty)=%d\n", isatty(fd));
    close(fd);
    fd = open("/etc/motd", O_RDONLY);
    printf("isatty(/etc/motd)=%d\n", isatty(fd));
    close(fd);
    return 0;
}
