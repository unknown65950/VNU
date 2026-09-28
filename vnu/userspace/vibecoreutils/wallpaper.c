/* wallpaper — show or set the desktop background (separate binary).
 *
 * The desktop wallpaper is a plain file, /etc/vnu/wallpaper, that the
 * kernel decodes and scales behind the windows. This command hands
 * the kernel a candidate (VNU_SYS_wallpaper) and reports what it
 * decided: the kernel decodes first and only swaps the picture in when
 * it is good, so a file it refuses leaves the desktop untouched.
 *
 * The kernel is the only authority on what a wallpaper may be — BMP or
 * PNG px.h understands, at most 512x384 and 64 KiB, and never JPEG,
 * whose IDCT is the one piece of float code this -mno-80387 kernel
 * cannot link. So this file deliberately does not carry px.h just to
 * sniff headers: --list marks the JPEGs and leaves the verdict to the
 * call.
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
#define PATH_CAP   96

static void join(char* dst, int cap, const char* dir, const char* name)
{
    int i = 0;
    while (dir[i] && i < cap - 2) {
        dst[i] = dir[i];
        ++i;
    }
    dst[i++] = '/';
    int j = 0;
    while (name[j] && i < cap - 1) {
        dst[i] = name[j];
        ++i;
        ++j;
    }
    dst[i] = 0;
}

/* What the desktop is drawing, as /proc/gfx reports it. "none" until a
 * picture has been decoded (the procedural sky is not a file). */
static void show_current(void)
{
    /* The whole file, not one read: /proc/gfx reports more of the
     * display as there is more to report - the mode, the depth, the
     * palette, the mode ladder, the wallpaper - and a line this
     * command needs can end up past a single short read's reach. */
    char buf[512];
    int fd = open(PROC_GFX, 0);
    if (fd < 0) {
        we("wallpaper: cannot read " PROC_GFX "\n");
        return;
    }
    int n = 0;
    while (n < (int)sizeof(buf) - 1) {
        int r = (int)read(fd, buf + n, sizeof(buf) - 1 - n);
        if (r <= 0)
            break;
        n += r;
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
            return;
        }
        while (*p && *p != '\n')
            ++p;
        if (*p == '\n')
            ++p;
    }
    w("none\n");
}

/* Does this file start with a JPEG SOI marker? */
static int is_jpeg(const char* path)
{
    int fd = open(path, 0);
    if (fd < 0)
        return 0;
    unsigned char head[2] = { 0, 0 };
    int n = (int)read(fd, head, sizeof(head));
    close(fd);
    return n == 2 && head[0] == 0xFF && head[1] == 0xD8;
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
        char path[PATH_CAP];
        join(path, sizeof(path), PICS_DIR, e->d_name);
        if (is_jpeg(path))
            w("  (jpeg: the kernel cannot decode it)");
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
    w("With no argument, print the background in use. With a FILE, decode it\n");
    w("and put it on the desktop: the image replaces " SHIPPED "'s\n");
    w("neighbour, /etc/vnu/wallpaper, and the next redraw draws it.\n\n");
    w("BMP and PNG of at most 512x384 pixels and 64 KiB are accepted. The\n");
    w("demo pack in " PICS_DIR " holds the candidates; the shipped\n");
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
            we(": not a usable wallpaper (bmp or png, at most 512x384 "
               "pixels and 64 KiB)\n");
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
