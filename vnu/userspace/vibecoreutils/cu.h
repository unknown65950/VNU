/*
 * cu.h — helpers shared by the VNU coreutils binaries.
 *
 * Every command is its own separate, self-contained ELF in this
 * directory (echo, cat, ls, ...); the toolchain builds one source
 * file per binary, so instead of copy-pasting the tiny helpers below
 * into 21 files they live here once. `static inline` so an unused
 * helper simply disappears from the final binary.
 */
#ifndef VNU_CU_H_
#define VNU_CU_H_

#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>
#include <vlibc/fcntl.h>
#include <vlibc/dirent.h>
#include <vlibc/sys/stat.h>
#include <vlibc/sys/utsname.h>
#include <vlibc/sys/wait.h>

/* Last path component of p (basename). */
static inline const char* base(const char* p)
{
    const char* s = p;
    for (; *p; ++p)
        if (*p == '/')
            s = p + 1;
    return s;
}

static inline void w(const char* s) { write(1, s, strlen(s)); }
static inline void we(const char* s) { write(2, s, strlen(s)); }

/* wc/seq: decimal printing without snprintf. */
static inline void put_uint(unsigned long v)
{
    char digits[12];
    int n = 0;
    if (v == 0) {
        w("0");
        return;
    }
    while (v > 0 && n < 12) {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char out[13];
    for (int i = 0; i < n; ++i)
        out[i] = digits[n - 1 - i];
    out[n] = 0;
    w(out);
}

/* head/tail/grep/sort share a line-oriented reader that copies at most
 * MAX_LINES lines (each up to LINE_CAP-1 bytes) into a fixed buffer —
 * enough for this OS's small text files, no dynamic allocation. Kept
 * deliberately small: these live in the command's own .bss, which has
 * to fit inside the small private app-image region every process gets
 * (see kernel/proc/process.cpp) alongside the actual code. */
#define MAX_LINES 128
#define LINE_CAP 100

static inline int read_lines(int fd, char lines[MAX_LINES][LINE_CAP], int* out_count)
{
    int count = 0;
    int col = 0;
    char buf[256];
    for (;;) {
        /* Stop once the buffer is full rather than reading on and
         * discarding: /dev/zero and /dev/urandom never return EOF, so
         * without this, head/tail/grep/sort would spin forever on
         * them. Means these tools only ever see the first MAX_LINES
         * lines of a file, which is the documented trade-off of the
         * fixed-size buffer. */
        if (count >= MAX_LINES)
            break;
        long n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\n') {
                if (count < MAX_LINES) {
                    lines[count][col < LINE_CAP - 1 ? col : LINE_CAP - 1] = 0;
                    ++count;
                }
                col = 0;
            } else if (count < MAX_LINES && col < LINE_CAP - 1) {
                lines[count][col++] = c;
                lines[count][col] = 0;
            }
        }
    }
    if (col > 0 && count < MAX_LINES) {
        lines[count][col < LINE_CAP - 1 ? col : LINE_CAP - 1] = 0;
        ++count;
    }
    *out_count = count;
    return 0;
}

static inline int open_or_stdin(int argc, char** argv, int arg_index, int* is_stdin)
{
    if (argc <= arg_index) {
        *is_stdin = 1;
        return 0;
    }
    *is_stdin = 0;
    int fd = open(argv[arg_index], O_RDONLY);
    if (fd < 0) {
        we("cannot open ");
        we(argv[arg_index]);
        we("\n");
    }
    return fd;
}

/* grep: plain substring match (no regex). */
static inline int contains(const char* hay, const char* needle)
{
    if (!*needle)
        return 1;
    for (int i = 0; hay[i]; ++i) {
        int j = 0;
        while (hay[i + j] && needle[j] && hay[i + j] == needle[j])
            ++j;
        if (!needle[j])
            return 1;
    }
    return 0;
}

/* cp/mv: copy bytes between two paths. */
static inline int copy_file(const char* src, const char* dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        we("cannot open ");
        we(src);
        we("\n");
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        we("cannot create ");
        we(dst);
        we("\n");
        close(in);
        return -1;
    }
    char buf[256];
    for (;;) {
        long n = read(in, buf, sizeof(buf));
        if (n <= 0)
            break;
        write(out, buf, (unsigned long)n);
    }
    close(in);
    close(out);
    return 0;
}

#endif /* VNU_CU_H_ */