#include <vlibc/stdio.h>
#include <vlibc/string.h>

// Where formatted output goes.
//
// One struct serves both entry points, and the difference between them is
// only what happens to a character: vprintf() puts it straight on stdout,
// and snprintf() puts it in the caller's buffer, where the last byte is
// kept for the terminating NUL. Every emitter below asks the sink what to
// do and neither entry point has to know which one it is serving.
//
// The two are told apart by `kind` rather than by `buf` being null,
// because POSIX lets snprintf() be called with a null buffer to measure a
// format without writing it (snprintf(NULL, 0, ...) returns the length and
// touches nothing). A null `buf` therefore cannot mean stdout, or that
// call would measure the format by printing it.
//
// `len` is how much the format asked for and `pos` is how much fitted.
// They are the same number until a bounded buffer runs out, which is the
// whole reason they are counted separately: POSIX has snprintf() return
// the length the output *would* have had, so a caller can size a second
// buffer instead of discovering the truncation by looking at it.
typedef enum {
    SINK_STDOUT,
    SINK_BUFFER,
} sink_kind;

typedef struct {
    sink_kind kind;
    char* buf;
    unsigned long cap;
    unsigned long len;
    unsigned long pos;
} sink;

static void sink_char(sink* s, char c) {
    if (s->kind == SINK_STDOUT) {
        putchar(c);
    } else if (s->pos + 1 < s->cap) {
        // pos + 1 < cap, not pos < cap: the byte at cap - 1 belongs to
        // the NUL that snprintf() writes whenever cap is not zero. With
        // a null buf nothing is stored at all, which is what measuring
        // without writing has to mean.
        s->buf[s->pos] = c;
    }
    s->pos++;
    s->len++;
}

static void sink_str(sink* s, const char* str, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) {
        sink_char(s, str[i]);
    }
}

// Prints an unsigned number in base 16, most significant digit first.
static void print_hex(sink* s, unsigned long num) {
    const char* hex = "0123456789abcdef";
    if (num >= 16) {
        print_hex(s, num / 16);
    }
    sink_char(s, hex[num % 16]);
}

// Prints an unsigned number in base 10.
//
// There used to be an off-by-one here (in the %d body): pos was
// advanced one byte AFTER the last (most significant) digit had been
// written, while write() still started reading at pos - one byte
// before the start of the buffer. In practice that read whatever
// happened to sit on the stack instead of the first digit ("42"
// became "\0" + "4", losing the 2). The buffer is filled from the
// back here, so pos always points at the very first character written
// by the time sink_str() is called.
static void print_unsigned(sink* s, unsigned long num) {
    char buffer[24];
    char* pos = buffer + sizeof(buffer);
    *--pos = '\0';

    if (num == 0) {
        *--pos = '0';
    } else {
        while (num) {
            *--pos = (char)('0' + (num % 10));
            num /= 10;
        }
    }

    sink_str(s, pos, (unsigned long)((buffer + sizeof(buffer) - 1) - pos));
}

static void print_signed(sink* s, long num) {
    if (num < 0) {
        sink_char(s, '-');
        // -num would overflow for LONG_MIN (there is no positive
        // equivalent in the same type), so the magnitude is computed as
        // (-(num+1))+1, which is safe for any num.
        unsigned long magnitude = (unsigned long)(-(num + 1)) + 1UL;
        print_unsigned(s, magnitude);
        return;
    }

    print_unsigned(s, (unsigned long)num);
}

// The one formatter both entry points share.
static void vformat(sink* s, const char* fmt, va_list args) {
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            sink_char(s, *p);
            continue;
        }

        p++;

        // The 'l' length modifier (needed by %ld/%lu - used, for
        // example, by getpid()/getuid(), which return
        // unsigned long).
        int is_long = 0;
        if (*p == 'l') {
            is_long = 1;
            p++;
        }

        switch (*p) {
            case 's': {
                const char* str = va_arg(args, const char*);
                if (str == NULL) str = "(null)";
                sink_str(s, str, strlen(str));
                break;
            }
            case 'd':
            case 'i': {
                long num = is_long ? va_arg(args, long) : (long)va_arg(args, int);
                print_signed(s, num);
                break;
            }
            case 'u': {
                unsigned long num = is_long
                    ? va_arg(args, unsigned long)
                    : (unsigned long)va_arg(args, unsigned int);
                print_unsigned(s, num);
                break;
            }
            case 'x': {
                unsigned long num = is_long
                    ? va_arg(args, unsigned long)
                    : (unsigned long)va_arg(args, unsigned int);
                print_hex(s, num);
                break;
            }
            case 'c': {
                sink_char(s, (char)va_arg(args, int));
                break;
            }
            case '%': {
                sink_char(s, '%');
                break;
            }
            default:
                sink_char(s, '%');
                if (is_long) {
                    sink_char(s, 'l');
                }
                sink_char(s, *p);
                break;
        }
    }
}

int vprintf(const char* format, va_list args) {
    sink s = {0};
    s.kind = SINK_STDOUT;
    vformat(&s, format, args);
    return (int)s.len;
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    return result;
}

// snprintf() and vsnprintf() are here because formatted output into a
// buffer is a capability a program needs the moment it builds a message
// longer than a string literal - a request line, a path, a line of
// formatted output for a screen - and because vprintf() cannot be used
// there: it writes to stdout as it goes, with no way to stop it at the
// end of the buffer. Rolling a second formatter inside one program
// instead would have put the same formatter in every program that needs
// it, which is what a library is for.
//
// `size` is the size of `buf` in bytes and is counted the POSIX way: it
// includes the byte the NUL goes in, so snprintf(buf, 4, "abc") writes
// "abc" and snprintf(buf, 3, "abc") writes "ab" - both return 3, the
// length the output would have had. `buf` may be null when `size` is
// zero, which measures without writing.
int vsnprintf(char* buf, unsigned long size, const char* format, va_list args) {
    sink s = {0};
    s.kind = SINK_BUFFER;
    s.buf = buf;
    s.cap = size;
    vformat(&s, format, args);
    if (buf && size > 0) {
        buf[s.pos < size ? s.pos : size - 1] = '\0';
    }
    return (int)s.len;
}

int snprintf(char* buf, unsigned long size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buf, size, format, args);
    va_end(args);
    return result;
}
