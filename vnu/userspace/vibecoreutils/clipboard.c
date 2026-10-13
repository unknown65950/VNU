/* clipboard — read and write the system clipboard.
 *
 *   clipboard                 print the clipboard
 *   clipboard paste           print the clipboard
 *   clipboard copy [TEXT...]   replace it with TEXT, or with stdin
 *   clipboard clear            empty it
 *
 * The clipboard is the /dev/clipboard character device: writing it
 * replaces the text, reading it walks the text from the start to the
 * end. This command is that device made convenient - a shell can copy
 * with `ls | clipboard copy`, paste with `clipboard paste > notes`, and
 * any windowed program's Ctrl+C lands here too.
 */
#include "cu.h"
#include <vlibc/clipboard.h>

static void usage(void)
{
    we("usage: clipboard [copy [TEXT...] | paste | clear]\n");
    we("  paste (default)   print the clipboard\n");
    we("  copy [TEXT...]    set it from TEXT, or from standard input\n");
    we("  clear             empty it\n");
}

/* Copy the device out to standard output, a chunk at a time. */
static int do_paste(void)
{
    int fd = open(VCLIP_DEV, O_RDONLY);
    if (fd < 0) {
        we("clipboard: cannot open " VCLIP_DEV "\n");
        return 1;
    }
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(1, buf, (unsigned long)n) < 0)
            break;
    }
    close(fd);
    return 0;
}

/* Replace the clipboard with the arguments, which read as one line
 * (separated by spaces, newline at the end) - the way `echo a b c` is
 * one line, so `clipboard copy a b c` copies that same text. */
static int do_copy_args(int argc, char** argv)
{
    int fd = open(VCLIP_DEV, O_WRONLY);
    if (fd < 0) {
        we("clipboard: cannot open " VCLIP_DEV "\n");
        return 1;
    }
    for (int i = 2; i < argc; ++i) {
        if (i > 2)
            write(fd, " ", 1);
        write(fd, argv[i], strlen(argv[i]));
    }
    write(fd, "\n", 1);
    close(fd);
    return 0;
}

/* Replace the clipboard with standard input, which is what makes
 * `command | clipboard copy` and `clipboard copy < file` work. */
static int do_copy_stdin(void)
{
    int fd = open(VCLIP_DEV, O_WRONLY);
    if (fd < 0) {
        we("clipboard: cannot open " VCLIP_DEV "\n");
        return 1;
    }
    char buf[512];
    long n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        if (write(fd, buf, (unsigned long)n) < 0)
            break;
    }
    close(fd);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2) {
        const char* c = argv[1];
        if (!strcmp(c, "-h") || !strcmp(c, "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(c, "paste") || !strcmp(c, "show") || !strcmp(c, "cat"))
            return do_paste();
        if (!strcmp(c, "clear") || !strcmp(c, "empty")) {
            vclip_clear();
            return 0;
        }
        if (!strcmp(c, "copy") || !strcmp(c, "set"))
            return argc > 2 ? do_copy_args(argc, argv) : do_copy_stdin();
        we("clipboard: unknown command \"");
        we(c);
        we("\"\n");
        usage();
        return 1;
    }
    return do_paste();
}
