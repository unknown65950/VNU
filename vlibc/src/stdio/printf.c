#include <vlibc/stdio.h>
#include <vlibc/string.h>

static void print_hex(unsigned long num) {
    const char* hex = "0123456789abcdef";
    if (num >= 16) {
        print_hex(num / 16);
    }
    putchar(hex[num % 16]);
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
// by the time write() is called.
static int print_unsigned(unsigned long num) {
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

    int len = (int)((buffer + sizeof(buffer) - 1) - pos);
    write(STDOUT_FILENO, pos, (unsigned long)len);
    return len;
}

static int print_signed(long num) {
    if (num < 0) {
        putchar('-');
        // -num would overflow for LONG_MIN (there is no positive
        // equivalent in the same type), so the magnitude is computed as
        // (-(num+1))+1, which is safe for any num.
        unsigned long magnitude = (unsigned long)(-(num + 1)) + 1UL;
        return 1 + print_unsigned(magnitude);
    }

    return print_unsigned((unsigned long)num);
}

int vprintf(const char* format, va_list args) {
    int count = 0;

    for (const char* p = format; *p; p++) {
        if (*p != '%') {
            putchar(*p);
            count++;
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
                unsigned long len = strlen(str);
                write(STDOUT_FILENO, str, len);
                count += (int)len;
                break;
            }
            case 'd':
            case 'i': {
                long num = is_long ? va_arg(args, long) : (long)va_arg(args, int);
                count += print_signed(num);
                break;
            }
            case 'u': {
                unsigned long num = is_long
                    ? va_arg(args, unsigned long)
                    : (unsigned long)va_arg(args, unsigned int);
                count += print_unsigned(num);
                break;
            }
            case 'x': {
                unsigned long num = is_long
                    ? va_arg(args, unsigned long)
                    : (unsigned long)va_arg(args, unsigned int);
                print_hex(num);
                count += 8; // approximate - print_hex does not report its exact length yet
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                putchar(c);
                count++;
                break;
            }
            case '%': {
                putchar('%');
                count++;
                break;
            }
            default:
                putchar('%');
                if (is_long) {
                    putchar('l');
                    count++;
                }
                putchar(*p);
                count += 2;
                break;
        }
    }

    return count;
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    return result;
}
