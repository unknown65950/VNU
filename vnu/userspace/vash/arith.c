/*
 * arith.c — $(( ... )) and (( ... )) arithmetic.
 *
 * The shell's integer language: the C operators, with the shell's
 * meaning for the pieces C has none of. A bare name is a variable (0
 * when it has no value yet), `x++' and `++x' step a variable, and `x=3'
 * assigns one. Everything is `long'; a divide by zero — or anything the
 * parser cannot make sense of — leaves *ok at 0, which is what $(( ))
 * reports as an error and (( )) reports as a false result.
 */
#include "shell.h"

#define ARITH_MAX_DEPTH 64

typedef struct {
    const char* p;
    int depth;
    int bad;
} Ar;

static long ar_assign(Ar* a);
static long ar_ternary(Ar* a);
static long ar_comma(Ar* a);
static long ar_or(Ar* a);
static long ar_and(Ar* a);
static long ar_bitor(Ar* a);
static long ar_bitxor(Ar* a);
static long ar_bitand(Ar* a);
static long ar_eq(Ar* a);
static long ar_rel(Ar* a);
static long ar_pow(Ar* a);
static long ar_mul(Ar* a);
static long ar_add(Ar* a);
static long ar_unary(Ar* a);

/* --- the characters ------------------------------------------------- */

static void skip_space(Ar* a)
{
    for (;;) {
        while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n')
            ++a->p;
        if (a->p[0] == '\\' && a->p[1] == '\n') {
            a->p += 2;
            continue;
        }
        return;
    }
}

static int is_var_start(int c)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_var_char(int c)
{
    return is_var_start(c) || (c >= '0' && c <= '9');
}

static int digit_val(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int read_name(Ar* a, char* out, int size)
{
    int n = 0;
    while (is_var_char((unsigned char)*a->p) && n < size - 1)
        out[n++] = *a->p++;
    out[n] = 0;
    return n;
}

/* Is the operator at the cursor this one? When it is, step over it. */
static int take_op(Ar* a, const char* op)
{
    skip_space(a);
    int n = 0;
    while (op[n])
        ++n;
    for (int i = 0; i < n; ++i)
        if (a->p[i] != op[i])
            return 0;
    a->p += n;
    return 1;
}

/* `base#digits', `0x1f' and plain decimal, in that order of
 * preference — the shell's own spellings of a number. */
static int read_number(Ar* a, long* out)
{
    long base = 10;
    long v = 0;
    int any = 0;

    /* base#digits — the digits in front of the `#' are the base, and
     * they are read before the number itself, so `16#ff' is 255 and not
     * the sixteen that `16' would be. */
    {
        const char* q = a->p;
        long b = 0;
        while (*q >= '0' && *q <= '9')
            b = b * 10 + (*q++ - '0');
        if (q > a->p && *q == '#') {
            if (b < 2 || b > 36) {
                a->bad = 1;
                *out = 0;
                return 1;
            }
            a->p = q + 1;
            v = 0;
            for (;;) {
                int d = digit_val((unsigned char)*a->p);
                if (d < 0 || (long)d >= b)
                    break;
                v = v * b + d;
                ++a->p;
            }
            *out = v;
            return 1;
        }
    }

    if (a->p[0] == '0' && (a->p[1] == 'x' || a->p[1] == 'X')) {
        base = 16;
        a->p += 2;
    } else if (a->p[0] == '0' && (a->p[1] == 'b' || a->p[1] == 'B')) {
        base = 2;
        a->p += 2;
    } else if (a->p[0] == '0' && a->p[1] >= '0' && a->p[1] <= '7') {
        base = 8;
        ++a->p;
    }
    for (;;) {
        int d = digit_val((unsigned char)*a->p);
        if (d < 0 || (long)d >= base)
            break;
        v = v * base + d;
        ++a->p;
        any = 1;
    }
    if (base != 10) {
        if (!any)
            a->bad = 1;
        *out = v;
        return 1;
    }

    if (!any)
        return 0;
    *out = v;
    return 1;
}

/* --- the levels, loosest first -------------------------------------- */

static long ar_comma(Ar* a)
{
    long v = ar_assign(a);
    while (take_op(a, ","))
        v = ar_assign(a);
    return v;
}

static long ar_ternary(Ar* a)
{
    long c = ar_or(a);
    if (take_op(a, "?")) {
        long t = ar_assign(a);
        if (!take_op(a, ":")) {
            a->bad = 1;
            return 0;
        }
        long f = ar_assign(a);
        return c ? t : f;
    }
    return c;
}

static long ar_or(Ar* a)
{
    long v = ar_and(a);
    while (take_op(a, "||")) {
        long r = ar_and(a);
        v = (v || r) ? 1 : 0;
    }
    return v;
}

static long ar_and(Ar* a)
{
    long v = ar_bitor(a);
    while (take_op(a, "&&")) {
        long r = ar_bitor(a);
        v = (v && r) ? 1 : 0;
    }
    return v;
}

static long ar_bitor(Ar* a)
{
    long v = ar_bitxor(a);
    while (take_op(a, "|")) {
        long r = ar_bitxor(a);
        v |= r;
    }
    return v;
}

static long ar_bitxor(Ar* a)
{
    long v = ar_bitand(a);
    while (take_op(a, "^")) {
        long r = ar_bitand(a);
        v ^= r;
    }
    return v;
}

static long ar_bitand(Ar* a)
{
    long v = ar_eq(a);
    while (take_op(a, "&")) {
        long r = ar_eq(a);
        v &= r;
    }
    return v;
}

static long ar_eq(Ar* a)
{
    long v = ar_rel(a);
    for (;;) {
        if (take_op(a, "==")) {
            v = (v == ar_rel(a)) ? 1 : 0;
            continue;
        }
        if (take_op(a, "!=")) {
            v = (v != ar_rel(a)) ? 1 : 0;
            continue;
        }
        return v;
    }
}

static long ar_rel(Ar* a)
{
    long v = ar_pow(a);
    for (;;) {
        /* The two-character forms are tried first: a `<=' read as `<'
         * would leave an `=' where the value belongs. */
        if (take_op(a, "<=")) {
            v = (v <= ar_pow(a)) ? 1 : 0;
            continue;
        }
        if (take_op(a, ">=")) {
            v = (v >= ar_pow(a)) ? 1 : 0;
            continue;
        }
        if (take_op(a, "<<")) {
            long r = ar_pow(a);
            v = (r >= 0 && r < 31) ? (v << r) : 0;
            continue;
        }
        if (take_op(a, ">>")) {
            long r = ar_pow(a);
            v = (r >= 0 && r < 31) ? (v >> r) : 0;
            continue;
        }
        if (take_op(a, "<")) {
            v = (v < ar_pow(a)) ? 1 : 0;
            continue;
        }
        if (take_op(a, ">")) {
            v = (v > ar_pow(a)) ? 1 : 0;
            continue;
        }
        return v;
    }
}

static long ar_add(Ar* a)
{
    long v = ar_unary(a);
    for (;;) {
        if (take_op(a, "+")) {
            v += ar_unary(a);
            continue;
        }
        if (take_op(a, "-")) {
            v -= ar_unary(a);
            continue;
        }
        return v;
    }
}

static long ar_mul(Ar* a)
{
    long v = ar_add(a);
    for (;;) {
        /* A `**' is the power operator, not a multiplication. */
        skip_space(a);
        if (a->p[0] == '*' && a->p[1] != '*') {
            ++a->p;
            v *= ar_add(a);
            continue;
        }
        if (take_op(a, "/")) {
            long r = ar_add(a);
            if (r == 0) {
                a->bad = 1;
                return 0;
            }
            v /= r;
            continue;
        }
        if (take_op(a, "%")) {
            long r = ar_add(a);
            if (r == 0) {
                a->bad = 1;
                return 0;
            }
            v %= r;
            continue;
        }
        return v;
    }
}

static long ar_pow(Ar* a)
{
    long v = ar_mul(a);
    if (take_op(a, "**")) {
        long r = ar_pow(a);
        long acc = 1;
        for (long i = 0; i < r; ++i)
            acc *= v;
        return acc;
    }
    return v;
}

/* Step a variable the operator names: `x++', `++x' and their `--' forms
 * all end up here, differing only in whether the expression is worth the
 * value before the step or after it. */
static long step_name(const char* name, int plus, int prefix)
{
    if (!name[0])
        return 0;
    long cur = (long)sh_atol(var_get(name));
    long next = plus ? cur + 1 : cur - 1;
    Str num;
    str_init(&num);
    sh_itoa(next, &num);
    var_set(name, str_cstr(&num));
    str_free(&num);
    return prefix ? next : cur;
}

static long ar_unary(Ar* a)
{
    if (++a->depth > ARITH_MAX_DEPTH) {
        a->bad = 1;
        return 0;
    }
    long v;
    char name[64];
    /* The two-character steppers are tried first: reading `++x' as a
     * unary plus on `+x' would leave the variable alone. */
    if (take_op(a, "++")) {
        read_name(a, name, sizeof(name));
        v = step_name(name, 1, 1);
    } else if (take_op(a, "--")) {
        read_name(a, name, sizeof(name));
        v = step_name(name, 0, 1);
    } else if (take_op(a, "-")) {
        v = -ar_unary(a);
    } else if (take_op(a, "+")) {
        v = ar_unary(a);
    } else if (take_op(a, "!")) {
        v = ar_unary(a) ? 0 : 1;
    } else if (take_op(a, "~")) {
        v = ~ar_unary(a);
    } else if (is_var_start((unsigned char)*a->p)) {
        /* A name, a value to read, and possibly `++' or `--' after it. */
        read_name(a, name, sizeof(name));
        v = (long)sh_atol(var_get(name));
        if (take_op(a, "++"))
            v = step_name(name, 1, 0);
        else if (take_op(a, "--"))
            v = step_name(name, 0, 0);
    } else {
        long num;
        if (take_op(a, "(")) {
            v = ar_comma(a);
            if (!take_op(a, ")"))
                a->bad = 1;
        } else if (read_number(a, &num)) {
            v = num;
        } else {
            a->bad = 1;
            v = 0;
        }
    }
    --a->depth;
    return v;
}

/* An assignment is `name op= value'. The compound forms work on the
 * value the variable already has, and every one of them assigns and
 * yields the new value. */
static long ar_assign(Ar* a)
{
    skip_space(a);
    if (is_var_start((unsigned char)*a->p)) {
        const char* start = a->p;
        char name[64];
        read_name(a, name, sizeof(name));
        static const char* ops[] = {"<<=", ">>=", "+=", "-=", "*=", "/=",
                                    "%=",  "&=",  "^=", "|=", "=", 0};
        for (int i = 0; ops[i]; ++i) {
            /* A `==' is a comparison, not an assignment. */
            if (ops[i][0] == '=' && ops[i][1] == 0 && a->p[1] == '=')
                continue;
            if (!take_op(a, ops[i]))
                continue;
            long cur = (long)sh_atol(var_get(name));
            long rhs = ar_assign(a);
            long v = rhs;
            const char* op = ops[i];
            if (strcmp(op, "=") != 0) {
                char kind = op[0];
                if (kind == '+') v = cur + rhs;
                else if (kind == '-') v = cur - rhs;
                else if (kind == '*') v = cur * rhs;
                else if (kind == '/') {
                    if (rhs == 0) {
                        a->bad = 1;
                        return 0;
                    }
                    v = cur / rhs;
                } else if (kind == '%') {
                    if (rhs == 0) {
                        a->bad = 1;
                        return 0;
                    }
                    v = cur % rhs;
                } else if (kind == '<') v = cur << rhs;
                else if (kind == '>') v = cur >> rhs;
                else if (kind == '&') v = cur & rhs;
                else if (kind == '^') v = cur ^ rhs;
                else if (kind == '|') v = cur | rhs;
            }
            Str num;
            str_init(&num);
            sh_itoa(v, &num);
            var_set(name, str_cstr(&num));
            str_free(&num);
            return v;
        }
        /* Not an assignment after all: it is a name to read, and the
         * expression goes on after it — `i+1' is i plus one, not just
         * i. The name is read again on the way down. */
        a->p = start;
        return ar_ternary(a);
    }
    return ar_ternary(a);
}

long arith_eval(const char* expr, int* ok)
{
    if (ok)
        *ok = 1;
    if (!expr) {
        if (ok)
            *ok = 0;
        return 0;
    }
    Ar a;
    a.p = expr;
    a.depth = 0;
    a.bad = 0;
    long v = ar_comma(&a);
    skip_space(&a);
    if (a.bad || *a.p)
        if (ok)
            *ok = 0;
    return v;
}