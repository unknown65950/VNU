/* wallpaper — show or set the desktop background (separate binary).
 *
 * The desktop wallpaper is a plain file, /etc/vnu/wallpaper, that the
 * kernel reads behind the windows. This command hands the kernel a
 * candidate (VNU_SYS_wallpaper) and reports what it decided: the kernel
 * accepts it first and only swaps it in when it is good, so a file it
 * refuses leaves the desktop untouched.
 *
 * The kernel is the only authority on what a wallpaper may be — a BMP,
 * PNG or JPEG px.h understands up to the top of the mode ladder, or a
 * flat colour. So this file deliberately does not carry px.h just to
 * sniff headers: the verdict is the call's, and a file the kernel
 * refuses leaves the desktop exactly as it was.
 *
 * A flat colour is not an image: it is the seven-byte marker the kernel
 * ships as the default (see include/vnu/wallpaper.h), and it is what
 * makes a fresh desktop paint instantly. This command only has to pass
 * the file through — the marker and the picture go the same way.
 */
#include "cu.h"
#include <vnu/abi.h>
#include <vlibc/sys/syscall.h>

/* The kernel seeds the demo pack and both wallpaper files; see
 * include/vnu/media.h on the kernel side. */
#define PICS_DIR   "/etc/vnu/pics"
#define SHIPPED    "/etc/vnu/wallpaper.default"
#define PROC_GFX   "/proc/gfx"
#define NAME_CAP   64

/* What the desktop is drawing, as /proc/gfx reports it. "none" until a
 * wallpaper has been loaded (the procedural sky is not a file). */
static void show_current(void)
{
    /* All of /proc/gfx, and not a fixed buffer: it reports the mode,
     * the depth, the whole palette and then the wallpaper, which is
     * kilobytes of text, and the line this command needs is the last
     * one in it. A buffer that stops short of that reports "none" for
     * a desktop that is drawing a picture perfectly well. */
    unsigned long cap = 4096;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        we("wallpaper: out of memory reading " PROC_GFX "\n");
        return;
    }
    int fd = open(PROC_GFX, 0);
    if (fd < 0) {
        we("wallpaper: cannot read " PROC_GFX "\n");
        free(buf);
        return;
    }
    unsigned long n = 0;
    for (;;) {
        if (n + 1 >= cap) {
            char* bigger = (char*)malloc(cap * 2);
            if (!bigger)
                break;
            for (unsigned long i = 0; i < n; ++i)
                bigger[i] = buf[i];
            free(buf);
            buf = bigger;
            cap *= 2;
        }
        int r = (int)read(fd, buf + n, cap - 1 - n);
        if (r <= 0)
            break;
        n += (unsigned long)r;
    }
    close(fd);
    buf[n] = 0;

    const char* p = buf;
    while (p && *p) {
        if (strncmp(p, "wallpaper\t", 10) == 0) {
            const char* v = p + 10;
            int i = 0;
            while (v[i] && v[i] != '\n')
                ++i;
            char name[NAME_CAP];
            int j = 0;
            while (v[j] && v[j] != '\n' && j < NAME_CAP - 1) {
                name[j] = v[j];
                ++j;
            }
            name[j] = 0;
            w(name);
            w("\n");
            free(buf);
            return;
        }
        while (*p && *p != '\n')
            ++p;
        if (*p == '\n')
            ++p;
    }
    w("none\n");
    free(buf);
}

static void list_candidates(void)
{
    DIR* d = opendir(PICS_DIR);
    if (!d) {
        we("wallpaper: cannot open " PICS_DIR "\n");
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        if (e->d_type != 8) /* regular file */
            continue;
        w(e->d_name);
        w("\n");
    }
    closedir(d);
    w(SHIPPED);
    w("  (the desktop VNU ships with)\n");
}

static void wallpaper_help(void)
{
    w("Usage: wallpaper [OPTION]... [FILE]\n");
    w("Show or set the desktop background.\n\n");
    w("  -l, --list     list the pictures that can be used\n");
    w("      --help     display this help and exit\n");
    w("      --version  output version information and exit\n\n");
    w("With no argument, print the background in use. With a FILE, accept it\n");
    w("first and, only if it works, put it on the desktop: it replaces\n");
    w("/etc/vnu/wallpaper and the next redraw draws it. A flat colour is set\n");
    w("by a FILE just as a picture is.\n\n");
    w("BMP, PNG and JPEG pictures of at most 1280x1024 pixels are accepted.\n");
    w("The demo pack in " PICS_DIR " holds the candidates; the shipped\n");
    w("desktop is " SHIPPED ", which stays untouched so it can\n");
    w("always come back. The choice lives in the RAM filesystem and is\n");
    w("therefore forgotten at reboot.\n");
}

int main(int argc, char** argv)
{
    const char* file = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0) {
            wallpaper_help();
            return 0;
        }
        if (strcmp(a, "--version") == 0) {
            w("wallpaper (VNU coreutils) 0.1\n");
            return 0;
        }
        if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) {
            list_candidates();
            return 0;
        }
        if (a[0] == '-' && a[1]) {
            we("wallpaper: unknown option ");
            we(a);
            we("\n");
            wallpaper_help();
            return 1;
        }
        file = a;
    }

    if (!file) {
        show_current();
        return 0;
    }

    long rc = syscall(SYS_wallpaper, (unsigned long)file);
    if (rc < 0) {
        we("wallpaper: ");
        we(file);
        if (rc == -VNU_EINVAL)
            we(": not a usable wallpaper (bmp, png or jpeg, at most "
               "1280x1024 pixels)\n");
        else if (rc == -VNU_ENOENT)
            we(": no such file\n");
        else
            we(": the kernel refused it\n");
        return 1;
    }
    w(base(file));
    w("\n");
    return 0;
}
