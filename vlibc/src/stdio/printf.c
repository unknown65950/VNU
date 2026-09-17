#include <vlibc/stdio.h>
#include <vlibc/string.h>

static void print_hex(unsigned long num) {
    const char* hex = "0123456789abcdef";
    if (num >= 16) {
        print_hex(num / 16);
    }
    putchar(hex[num % 16]);
}

// Печатает беззнаковое число в десятичной системе.
//
// Раньше здесь (в теле %d) была ошибка на единицу: указатель pos
// сдвигался на один байт ПОСЛЕ записи последней (старшей) цифры,
// а write() всё равно начинал читать именно с pos — то есть с байта
// перед началом буфера. На практике это читало случайный мусор со
// стека вместо первой цифры (например, "42" превращалось в
// "\0" + "4", теряя двойку). Здесь буфер заполняется с конца,
// и pos в момент вызова write() всегда указывает ровно на первый
// записанный символ.
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
        // -num переполнился бы для LONG_MIN (нет положительного
        // эквивалента в том же типе); вместо этого считаем модуль
        // через (-(num+1))+1, что безопасно для любого num.
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

        // Модификатор длины 'l' (нужен для %ld/%lu — используется,
        // например, в getpid()/getuid(), которые возвращают
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
                count += 8; // приблизительно — точную длину print_hex пока не возвращает
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
