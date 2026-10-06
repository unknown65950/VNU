/* builtins.c */
#include "shell.h"
#include <vlibc/sys/wait.h>



static int b_echo(int argc, char** argv)
{
    int i = 1;
    int nflag = 0;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n' && argv[1][2] == 0) {
        nflag = 1;
        i = 2;
    }
    for (; i < argc; ++i) {
        sh_out(argv[i]);
        if (i + 1 < argc)
            sh_out(" ");
    }
    if (!nflag)
        sh_out("\n");
    return 0;
}

static int b_true(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

static int b_false(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

static int b_cd(int argc, char** argv)
{
    const char* dir = "/";
    if (argc > 1 && argv[1])
        dir = argv[1];
    if (chdir(dir) < 0) {
        sh_err("vash: cd: cannot change directory\n");
        return 1;
    }
    return 0;
}

static int b_pwd(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    char buf[SH_LINE_MAX];
    if (getcwd(buf, sizeof(buf))) {
        sh_out(buf);
        sh_out("\n");
    }
    return 0;
}

static int b_exit(int argc, char** argv)
{
    int v = sh_status;
    if (argc > 1 && argv[1])
        v = atoi(argv[1]);
    exit(v);
    return v;
}

/* export [-p] [NAME[=VALUE]...] - give shell variables to the programs
 * this shell runs. POSIX: with no operand, write the exported names in a
 * form that can be read back in; with operands, set and export them. A
 * bare NAME creates the variable empty if it did not exist, which is
 * what marks it for export - the attribute lives on the variable, so
 * there is nothing to record separately.
 *
 * The attribute only means something because var_env_array() builds the
 * child's environment out of it (vars.c) and both execve() paths hand
 * that array over; without a caller of var_export() nothing was ever
 * exported and every child inherited an empty environment. */
static int b_export(int argc, char** argv)
{
    int first = 1;
    int print = 0;
    /* Every option is read before anything is done, so an unknown one is
     * reported even when a valid one has already asked for the list. */
    while (first < argc && argv[first][0] == '-' && argv[first][1]) {
        if (strcmp(argv[first], "-p") != 0) {
            sh_err("vash: export: ");
            sh_err(argv[first]);
            sh_err(": invalid option\n");
            return 2;
        }
        print = 1;
        ++first;
    }
    if (print || first == argc) {
        /* Read back through var_find() and release only the array:
         * var_names() lends the table's own name pointers. */
        int n = 0;
        char** names = var_names(&n);
        for (int k = 0; k < n; ++k) {
            Var* v = var_find(names[k]);
            if (!v || !v->exported)
                continue;
            sh_out("export ");
            sh_out(v->name);
            sh_out("=\"");
            sh_out(v->val ? v->val : "");
            sh_out("\"\n");
        }
        free(names);
        return 0;
    }
    for (int i = first; i < argc; ++i) {
        char* a = argv[i];
        if (a[0] == '-' && a[1]) {
            sh_err("vash: export: ");
            sh_err(a);
            sh_err(": invalid option\n");
            return 2;
        }
        int j = 0;
        while (a[j] && a[j] != '=' && a[j] != '[')
            ++j;
        if (a[j] == '[') {
            /* An array cannot be in a child's environment: the kernel
             * copies NAME=value strings, so there is nowhere for
             * `a[2]' to live. Say so rather than exporting `a' and
             * leaving the caller to wonder. */
            sh_err("vash: export: ");
            sh_err(a);
            sh_err(": arrays cannot be exported\n");
            return 1;
        }
        /* The name is copied rather than cut out of the caller's word:
         * var_export() below wants the name alone, and a word that came
         * out of the expander is not ours to leave half-terminated. */
        char name[64];
        int n = j;
        if (n > (int)sizeof(name) - 1)
            n = (int)sizeof(name) - 1;
        memcpy(name, a, (unsigned long)n);
        name[n] = 0;
        if (a[j] == '=')
            var_set(name, a + j + 1);
        var_export(name, 1);
    }
    return 0;
}

static int b_unset(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        var_unset(argv[i]);
    return 0;
}

static int b_type(int argc, char** argv)
{
    if (argc <= 1)
        return 0;
    if (is_builtin_name(argv[1])) {
        sh_out(argv[1]);
        sh_out(" is a shell builtin\n");
        return 0;
    }
    if (func_find(argv[1])) {
        sh_out(argv[1]);
        sh_out(" is a function\n");
        return 0;
    }
    char r[SH_PATH_MAX];
    sh_resolve(argv[1], r, sizeof(r));
    sh_out(argv[1]);
    sh_out(" is ");
    sh_out(r);
    sh_out("\n");
    return 0;
}


/* --- small helpers ----------------------------------------------------
 * vlibc is a freestanding library: what a hosted libc hands out for free
 * is spelled out here, so a builtin reads the way a builtin should. */

static int in_set(const char* set, int c)
{
    for (; *set; ++set)
        if ((unsigned char)*set == (unsigned char)c)
            return 1;
    return 0;
}

static char* dup_str(const char* s)
{
    unsigned long n = strlen(s) + 1;
    char* out = (char*)malloc(n);
    if (!out)
        return 0;
    memcpy(out, s, n);
    return out;
}

/* `%ld' without a printf to call: the digits, a minus when the number is
 * negative, and at least `min_digits' of them. */
static unsigned long mag_long(long v)
{
    return v < 0 ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
}

static int fmt_long(char* buf, long v, int min_digits)
{
    char tmp[32];
    int n = 0;
    unsigned long u = mag_long(v);
    if (!u)
        tmp[n++] = '0';
    while (u) {
        tmp[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    int pos = 0;
    if (v < 0)
        buf[pos++] = '-';
    for (int i = n; i < min_digits; ++i)
        buf[pos++] = '0';
    for (int k = n - 1; k >= 0; --k)
        buf[pos++] = tmp[k];
    buf[pos] = 0;
    return pos;
}

static int upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

/* --- `:' — the command that does nothing, and says so ----------------- */

static int b_colon(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

/* --- printf -----------------------------------------------------------
 * `printf fmt args...' writes the format once for each argument it has
 * left, reading escapes out of the format itself and, for %b, out of the
 * argument as well. `printf -v name ...' puts the text in a variable
 * instead of on the screen. */

/* One escape sequence: `s' points at the backslash, what it stands for
 * goes into `out' and the number of characters it used comes back, so a
 * caller that walks a format can step over it. */
static int put_one_escape(const char* s, Str* out)
{
    if (*s != '\\')
        return 0;
    int c = s[1];
    if (!c)
        return 1;
    switch (c) {
    case 'a': str_addc(out, '\a'); break;
    case 'b': str_addc(out, '\b'); break;
    case 'e': str_addc(out, 27); break;
    case 'f': str_addc(out, '\f'); break;
    case 'n': str_addc(out, '\n'); break;
    case 'r': str_addc(out, '\r'); break;
    case 't': str_addc(out, '\t'); break;
    case 'v': str_addc(out, '\v'); break;
    case '\\': str_addc(out, '\\'); break;
    case '0': {
        int v = 0, n = 0;
        while (n < 3 && s[2 + n] >= '0' && s[2 + n] <= '7') {
            v = v * 8 + (s[2 + n] - '0');
            ++n;
        }
        str_addc(out, (char)v);
        return n + 2;
    }
    default:
        str_addc(out, '\\');
        str_addc(out, (char)c);
        break;
    }
    return 2;
}

static void put_escapes(const char* s, Str* out, int do_b)
{
    for (const char* p = s; *p; ++p) {
        if (*p != '\\' || !do_b) {
            str_addc(out, *p);
            continue;
        }
        p += put_one_escape(p, out) - 1;
    }
}

/* Padding around a piece of output: `zero' fills with noughts after any
 * sign, the way C does it. */
static void put_padded(Str* out, const char* text, int len, int width,
                       int left, int zero)
{
    int sign = 0;
    if (zero && len && (text[0] == '-' || text[0] == '+'))
        sign = 1;
    int pad = width > len ? width - len : 0;
    if (!left) {
        for (int i = 0; i < pad - sign; ++i)
            str_addc(out, zero ? '0' : ' ');
    }
    str_addn(out, text, len);
    if (left)
        for (int i = 0; i < pad; ++i)
            str_addc(out, ' ');
}

static void printf_conv(const char* spec, const char* arg, Str* out)
{
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
    int i = 0;
    for (; spec[i]; ++i) {
        if (spec[i] == '-') left = 1;
        else if (spec[i] == '0') zero = 1;
        else if (spec[i] == '+') plus = 1;
        else if (spec[i] == ' ') space = 1;
        else if (spec[i] == '#') alt = 1;
        else break;
    }
    int width = 0;
    if (spec[i] == '*') {
        width = atoi(arg ? arg : "0");
        ++i;
        arg = 0;
    } else
        while (spec[i] >= '0' && spec[i] <= '9')
            width = width * 10 + (spec[i++] - '0');
    int prec = -1;
    if (spec[i] == '.') {
        ++i;
        prec = 0;
        if (spec[i] == '*') {
            prec = atoi(arg ? arg : "0");
            ++i;
            arg = 0;
        } else
            while (spec[i] >= '0' && spec[i] <= '9')
                prec = prec * 10 + (spec[i++] - '0');
    }
    int conv = spec[i];
    const char* text = arg ? arg : "";
    Str body;
    str_init(&body);

    switch (conv) {
    case 'd':
    case 'i': {
        long v = atol(text);
        char digits[40];
        fmt_long(digits, v, 0);
        if (v < 0)
            str_addc(&body, '-');
        else if (plus)
            str_addc(&body, '+');
        else if (space)
            str_addc(&body, ' ');
        str_adds(&body, digits);
        put_padded(out, str_cstr(&body), body.len, width, left, zero);
        break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o': {
        unsigned long v = (unsigned long)sh_atol(text);
        int base = conv == 'o' ? 8 : 16;
        const char* digits = "0123456789abcdef";
        char tmp[72];
        int n = 0;
        if (!v)
            tmp[n++] = '0';
        while (v) {
            int d = (int)(v % (unsigned long)base);
            tmp[n++] = (conv == 'X') ? (char)upper(digits[d]) : digits[d];
            v /= (unsigned long)base;
        }
        char buf[80];
        int pos = 0;
        if (alt && n && tmp[n - 1] != '0') {
            buf[pos++] = '0';
            if (conv != 'o')
                buf[pos++] = conv == 'X' ? 'X' : 'x';
        }
        for (int k = n - 1; k >= 0; --k)
            buf[pos++] = tmp[k];
        buf[pos] = 0;
        put_padded(out, buf, pos, width, left, zero);
        break;
    }
    case 's': {
        int len = (int)strlen(text);
        if (prec >= 0 && len > prec)
            len = prec;
        put_padded(out, text, len, width, left, 0);
        break;
    }
    case 'b': {
        put_escapes(text, &body, 1);
        put_padded(out, str_cstr(&body), body.len, width, left, zero);
        break;
    }
    case 'c': {
        if (text[0])
            str_addc(&body, text[0]);
        put_padded(out, str_cstr(&body), body.len, width, left, 0);
        break;
    }
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G': {
        /* Read the number as text and lay it out by hand: the compiler
         * this shell is built with has no floating point to lean on. */
        const char* p = text;
        while (*p == ' ' || *p == '\t')
            ++p;
        int neg = 0;
        if (*p == '-' || *p == '+')
            neg = (*p++ == '-');
        long whole = 0;
        while (*p >= '0' && *p <= '9')
            whole = whole * 10 + (*p++ - '0');
        char fdig[24];
        int nf = 0;
        if (*p == '.') {
            ++p;
            while (*p >= '0' && *p <= '9' && nf < (int)sizeof(fdig) - 1)
                fdig[nf++] = *p++;
        }
        if (prec < 0)
            prec = 6;
        /* A digit past the point rounds the one before it, and the carry
         * walks back into the whole part when the tail is all noughts. */
        int k = prec - 1;
        int roundup = prec < nf && fdig[prec] >= '5';
        while (k >= 0 && roundup) {
            if (k < nf) {
                if (fdig[k] == '9')
                    fdig[k] = '0';
                else {
                    ++fdig[k];
                    roundup = 0;
                }
            } else {
                fdig[k] = '1';
                roundup = 0;
            }
            --k;
        }
        if (roundup)
            ++whole;
        if (neg || plus)
            str_addc(&body, neg ? '-' : '+');
        char wbuf[40];
        fmt_long(wbuf, whole, 0);
        str_adds(&body, wbuf);
        if (prec > 0) {
            str_addc(&body, '.');
            for (int d = 0; d < prec; ++d)
                str_addc(&body, d < nf ? fdig[d] : '0');
        }
        put_padded(out, str_cstr(&body), body.len, width, left, zero);
        break;
    }
    case '%':
        str_addc(out, '%');
        break;
    default:
        str_addc(out, '%');
        if (conv)
            str_addc(out, (char)conv);
        break;
    }
    str_free(&body);
}

/* Read one conversion: the spec runs from `%' to the letter. */
static void printf_spec(const char* fmt, int* pp, Str* out, char** args,
                        int argc, int* ai)
{
    int p = *pp;
    ++p; /* the `%' itself */
    while (fmt[p] == '-' || fmt[p] == '+' || fmt[p] == ' ' ||
           fmt[p] == '0' || fmt[p] == '#')
        ++p;
    while (fmt[p] >= '0' && fmt[p] <= '9')
        ++p;
    if (fmt[p] == '.') {
        ++p;
        while (fmt[p] >= '0' && fmt[p] <= '9')
            ++p;
    }
    char spec[40];
    int n = 0;
    for (int k = *pp + 1; k < p && n < (int)sizeof(spec) - 2; ++k)
        spec[n++] = fmt[k];
    int conv = fmt[p] ? fmt[p] : 0;
    if (conv)
        spec[n++] = (char)conv;
    spec[n] = 0;
    *pp = p;
    if (!conv)
        return;
    ++*pp;
    if (conv == '%') {
        str_addc(out, '%');
        return;
    }
    /* `*' takes its width from the arguments, one per star */
    for (const char* q = spec; *q; ++q) {
        if (*q != '*')
            continue;
        char width[24];
        fmt_long(width, *ai < argc ? atol(args[*ai]) : 0, 0);
        str_adds(out, width);
        ++*ai;
    }
    const char* arg = *ai < argc ? args[*ai] : "";
    if (*ai < argc)
        ++*ai;
    printf_conv(spec, arg, out);
}

static int b_printf(int argc, char** argv)
{
    if (argc < 2)
        return 0;
    int i = 1;
    const char* var = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] == 'v' && argv[i][2]) {
        var = argv[i] + 2;
        ++i;
    }
    if (i >= argc) {
        /* `printf' with no format is `echo' */
        for (int k = i; k < argc; ++k) {
            sh_out(argv[k]);
            if (k + 1 < argc)
                sh_out(" ");
        }
        sh_out("\n");
        return 0;
    }
    const char* fmt = argv[i++];
    Str all;
    str_init(&all);
    for (;;) {
        int before = i;
        int ai = 0;
        int p = 0;
        while (fmt[p]) {
            if (fmt[p] == '\\') {
                /* One escape at a time: the format is walked, and what
                 * follows the backslash belongs to the walk. */
                p += put_one_escape(fmt + p, &all);
                continue;
            }
            if (fmt[p] != '%') {
                str_addc(&all, fmt[p++]);
                continue;
            }
            printf_spec(fmt, &p, &all, argv + i, argc - i, &ai);
        }
        i += ai;
        /* The format is used again while arguments are left over. */
        if (i >= argc || i == before)
            break;
    }
    if (var)
        var_set(var, str_cstr(&all));
    else
        sh_out(str_cstr(&all));
    str_free(&all);
    return 0;
}

/* --- read -------------------------------------------------------------
 * One line from a descriptor, split on IFS, into variables. `read' with
 * no names puts the whole line in REPLY, which is what a script expects
 * when the line is only a test. */

static int ifs_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

static int split_ifs(char* line, const char* ifs, Str* fields, int max)
{
    int n = 0;
    if (!ifs || !*ifs)
        ifs = " \t\n";
    char* p = line;
    while (*p && in_set(ifs, *p) && ifs_space((unsigned char)*p))
        ++p;
    while (*p && n < max) {
        Str cur;
        str_init(&cur);
        while (*p && !in_set(ifs, *p))
            str_addc(&cur, *p++);
        /* A run of separators is one separator whatever it is made of,
         * and the space around a non-space one goes with it. */
        int had = *p != 0;
        int hard = 0;
        while (*p && in_set(ifs, *p)) {
            if (!ifs_space((unsigned char)*p))
                hard = 1;
            ++p;
        }
        if (had && !hard)
            while (*p && in_set(ifs, *p) && ifs_space((unsigned char)*p))
                ++p;
        fields[n++] = cur;
    }
    return n;
}

static int read_line(int fd, char delim, Str* line, long limit)
{
    line->len = 0;
    for (;;) {
        char c;
        long got = read(fd, &c, 1);
        if (got <= 0)
            return line->len ? 1 : 0;
        if (c == delim)
            return 1;
        str_addc(line, c);
        if (limit >= 0 && line->len >= limit)
            return 1;
    }
}

static int b_read(int argc, char** argv)
{
    int raw = 0, fd = 0, use_array = 0;
    const char* arrname = 0;
    char delim = '\n';
    long limit = -1;
    const char* prompt = 0;
    int i = 1;
    for (; i < argc; ++i) {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == 0)
            break;
        if (strcmp(a, "--") == 0) {
            ++i;
            break;
        }
        for (const char* f = a + 1; *f; ++f) {
            const char* value = f + 1;
            int takes = in_set("apndu", *f);
            if (takes && !*value && i + 1 < argc)
                value = argv[++i];
            switch (*f) {
            case 'r': raw = 1; break;
            case 's': case 'e': break;
            case 'a':
                arrname = value;
                use_array = 1;
                break;
            case 'p': prompt = value; break;
            case 'n': limit = atol(value); break;
            /* `-d ""' is the delimiter that never comes: the whole
             * input is the line. */
            case 'd': delim = value[0]; break;
            case 'u': fd = atoi(value); break;
            default:
                sh_err("vash: read: invalid option\n");
                return 2;
            }
            if (takes && f[1])
                f = value + strlen(value) - 1;
        }
    }
    if (prompt && isatty(2))
        sh_err(prompt);
    Str line;
    str_init(&line);
    if (!read_line(fd, delim, &line, limit)) {
        str_free(&line);
        return 1;
    }
    if (line.len && line.p[line.len - 1] == '\r')
        --line.len;
    Str text;
    str_init(&text);
    if (raw) {
        str_addn(&text, str_cstr(&line), line.len);
    } else {
        /* Without -r a backslash quotes the character after it. */
        for (int k = 0; k < line.len; ++k) {
            if (line.p[k] != '\\' || k + 1 >= line.len) {
                str_addc(&text, line.p[k]);
                continue;
            }
            ++k;
            str_addc(&text, line.p[k]);
        }
    }
    const char* ifs = var_get("IFS");
    if (!ifs || !*ifs)
        ifs = " \t\n";
    Str fields[64];
    char* copy = (char*)malloc(text.len + 1);
    if (copy) {
        memcpy(copy, str_cstr(&text), text.len);
        copy[text.len] = 0;
    }
    int n = split_ifs(copy, ifs, fields, 64);
    if (use_array) {
        const char* name = arrname ? arrname : "MAPFILE";
        for (int k = 0; k < n; ++k) {
            char idx[24];
            fmt_long(idx, k, 0);
            arr_set(name, idx, str_cstr(&fields[k]));
        }
    } else if (i >= argc) {
        var_set("REPLY", str_cstr(&text));
    } else {
        for (int k = i; k < argc; ++k) {
            const char* name = argv[k];
            if (k == argc - 1) {
                /* the last name takes the rest of the line, separators
                 * and all */
                Str rest;
                str_init(&rest);
                if (k - i < n) {
                    str_adds(&rest, str_cstr(&fields[k - i]));
                    for (int m = k - i + 1; m < n; ++m) {
                        str_addc(&rest, ' ');
                        str_adds(&rest, str_cstr(&fields[m]));
                    }
                }
                var_set(name, str_cstr(&rest));
                str_free(&rest);
            } else {
                var_set(name, (k - i) < n ? str_cstr(&fields[k - i]) : "");
            }
        }
    }
    for (int k = 0; k < n; ++k)
        str_free(&fields[k]);
    free(copy);
    str_free(&text);
    str_free(&line);
    return 0;
}

/* --- test, and `[', which is the same thing with a bracket ------------ */

static int b_test_words(int argc, char** argv)
{
    if (argc < 2)
        return 1;
    return sh_test_args(argc, argv, 1);
}

static int b_bracket(int argc, char** argv)
{
    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
        sh_err("vash: [: missing `]'\n");
        return 2;
    }
    return sh_test_args(argc, argv, 1);
}

/* --- shopt ------------------------------------------------------------
 * The shell options a script asks for by name. None of them changes what
 * this shell does yet, but `shopt -s x' has to succeed, and `shopt' has
 * to be able to say what is on. */

static const char* const shopt_names[] = {
    "nullglob",  "dotglob",      "extglob",   "nocasematch",
    "globstar",  "nocaseglob",   "noglob",    "failglob",
    "extquote",  "progcomp",     "verbose",   "xpg_echo",
    "pipefail",  "nounset",      "errexit",   "monitor",
    "interactive_comments",     "checkwinsize", "histexpand",
    0};

static int shopt_index(const char* name)
{
    for (int i = 0; shopt_names[i]; ++i)
        if (strcmp(shopt_names[i], name) == 0)
            return i;
    return -1;
}

static int b_shopt(int argc, char** argv)
{
    static unsigned char on[32];
    int mode = 0; /* 0 prints, 1 sets, -1 unsets, 2 asks */
    int i = 1;
    for (; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "-s") == 0) { mode = 1; continue; }
        if (strcmp(a, "-u") == 0) { mode = -1; continue; }
        if (strcmp(a, "-q") == 0) { mode = 2; continue; }
        if (strcmp(a, "-o") == 0 || strcmp(a, "+o") == 0)
            continue;
        if (a[0] == '-' && a[1]) {
            char one[2] = {a[1], 0};
            int k = shopt_index(one);
            if (k >= 0)
                on[k] = 1;
            continue;
        }
        int k = shopt_index(a);
        if (k < 0) {
            sh_err("vash: shopt: ");
            sh_err(a);
            sh_err(": invalid shell option name\n");
            return 1;
        }
        if (mode == 1) { on[k] = 1; continue; }
        if (mode == -1) { on[k] = 0; continue; }
        if (mode == 2)
            return on[k] ? 0 : 1;
        if (on[k]) {
            sh_out(a);
            sh_out("\n");
        } else {
            sh_out("set +o;");
            sh_out(a);
            sh_out("\n");
        }
    }
    if (i == 1 && !mode) {
        for (int k = 0; shopt_names[k]; ++k) {
            if (on[k]) {
                sh_out(shopt_names[k]);
                sh_out("\n");
            } else {
                sh_out("set +o;");
                sh_out(shopt_names[k]);
                sh_out("\n");
            }
        }
    }
    return 0;
}

/* --- local ------------------------------------------------------------
 * A name that belongs to the function and not to the shell: whatever it
 * held on entry comes back when the function returns. */

struct LocalSave {
    char name[64];
    char* value;
    int was_set;
};

static struct LocalSave* locals;
static int nlocals;
static int caplocals;

void sh_locals_begin(void)
{
    nlocals = 0;
}

void sh_locals_end(void)
{
    for (int i = nlocals - 1; i >= 0; --i) {
        if (locals[i].was_set)
            var_set(locals[i].name, locals[i].value);
        else
            var_unset(locals[i].name);
        free(locals[i].value);
    }
    nlocals = 0;
}

static int b_local(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        char* a = argv[i];
        if (strcmp(a, "-") == 0)
            continue;
        int n = 0;
        while (a[n] && a[n] != '=' && n < (int)sizeof(locals[0].name) - 1)
            ++n;
        if (nlocals >= caplocals) {
            int cap = caplocals ? caplocals * 2 : 16;
            struct LocalSave* bigger =
                (struct LocalSave*)malloc((unsigned long)cap *
                                          sizeof(struct LocalSave));
            if (!bigger)
                return 1;
            for (int k = 0; k < nlocals; ++k)
                bigger[k] = locals[k];
            free(locals);
            locals = bigger;
            caplocals = cap;
        }
        struct LocalSave* save = &locals[nlocals++];
        memset(save->name, 0, sizeof(save->name));
        memcpy(save->name, a, (unsigned long)n);
        save->was_set = var_find(save->name) != 0;
        save->value = save->was_set ? dup_str(var_get(save->name)) : 0;
        if (a[n] == '=')
            var_set(save->name, a + n + 1);
        else
            var_set(save->name, "");
    }
    return 0;
}

/* --- set --------------------------------------------------------------
 * Options on both sides of the minus, positional parameters after `--',
 * and with nothing else to do a listing of what is set. */

static int b_set(int argc, char** argv)
{
    int i = 1;
    int positional = 0;
    int listed = 0;
    int opts = 0;
    for (; i < argc; ++i) {
        char* a = argv[i];
        if (strcmp(a, "--") == 0) {
            positional = 1;
            ++i;
            break;
        }
        if (a[0] != '-' && a[0] != '+') {
            positional = 1;
            break;
        }
        if (strcmp(a, "-") == 0)
            continue;
        int on = a[0] == '-';
        for (const char* f = a + 1; *f; ++f) {
            switch (*f) {
            case 'e': sh_set_e = on; break;
            case 'u': sh_nounset = on; break;
            case 'a': var_export_all(on); break;
            case 'o': {
                const char* name = f[1] ? f + 1 : (argv[i + 1] ? argv[i + 1] : "");
                if (!f[1] && name[0] && name[0] != '-' && name[0] != '+')
                    ++i;
                /* A bare -o asks for the listing, so it is decided
                 * first: the chain below ends in a diagnostic, and the
                 * empty name is not a name it knows. */
                if (!name[0])
                    opts = 1;
                else if (strcmp(name, "pipefail") == 0)
                    sh_pipefail = on;
                else if (strcmp(name, "errexit") == 0)
                    sh_set_e = on;
                else if (strcmp(name, "nounset") == 0)
                    sh_nounset = on;
                else if (strcmp(name, "allexport") == 0)
                    var_export_all(on);
                else {
                    /* Every name this shell knows was listed above, so
                     * anything reaching here is a typo. Swallowing it
                     * made `set -o allexport' (which B27 had just
                     * wired up) indistinguishable from a setting that
                     * works. */
                    sh_err("vash: set: ");
                    sh_err(name);
                    sh_err(": invalid option name\n");
                    return 2;
                }
                f += strlen(f) - 1;
                break;
            }
            default: {
                char one[2] = {*f, 0};
                sh_err("vash: set: ");
                sh_err(one);
                sh_err(": invalid option\n");
                return 2;
            }
            }
        }
    }
    if (positional) {
        char* params[SH_MAX_ARGS];
        int n = 0;
        for (; i < argc && n < SH_MAX_ARGS - 1; ++i)
            params[n++] = argv[i];
        params[n] = 0;
        sh_set_params(params, n);
        return 0;
    }
    if (opts) {
        /* A bare -o asks what the shell is currently doing, so it
         * prints the settings. It used to fall into the variable listing
         * below, which made the one form whose whole purpose is to show
         * the shell's state show the shell's variables instead, and
         * left every setting this shell has invisible in it. Only the
         * settings this shell actually has are listed - printing thirty
         * names vash does not implement would be a worse lie than
         * printing four it does. */
        static const char* const names[4] = {
            "allexport", "errexit", "nounset", "pipefail"};
        const int state[4] = {
            var_allexport(), sh_set_e, sh_nounset, sh_pipefail};
        for (int k = 0; k < 4; ++k) {
            int w = 0;
            while (names[k][w])
                ++w;
            sh_out(names[k]);
            for (int c = w; c < 15; ++c)
                sh_out(" ");
            sh_out("\t");
            sh_out(state[k] ? "on" : "off");
            sh_out("\n");
        }
        return 0;
    }
    if (listed || i == 1) {
        int n = 0;
        char** names = var_names(&n);
        for (int k = 0; k < n; ++k) {
            sh_out(names[k]);
            sh_out("=");
            sh_out(var_get(names[k]));
            sh_out("\n");
        }
        /* The array is ours; the names belong to the table (vars.c). */
        free(names);
    }
    return 0;
}

/* --- mapfile / readarray ---------------------------------------------- */

static int b_mapfile(int argc, char** argv)
{
    int strip = 0, fd = 0;
    const char* origin = 0;
    const char* name = 0;
    long count = -1;
    char delim = '\n';
    int i = 1;
    for (; i < argc; ++i) {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == 0)
            break;
        if (strcmp(a, "--") == 0) {
            ++i;
            break;
        }
        for (const char* f = a + 1; *f; ++f) {
            const char* value = f + 1;
            int takes = in_set("dnOu", *f);
            if (takes && !*value && i + 1 < argc)
                value = argv[++i];
            switch (*f) {
            case 't': strip = 1; break;
            case 'd': delim = value[0]; break;
            case 'n': count = atol(value); break;
            case 'O': origin = value; break;
            case 'u': fd = atoi(value); break;
            case 's': break;
            default:
                sh_err("vash: mapfile: invalid option\n");
                return 2;
            }
            if (takes && f[1])
                f = value + strlen(value) - 1;
        }
    }
    if (i < argc)
        name = argv[i];
    if (!name)
        name = "MAPFILE";
    long at = origin ? atol(origin) : 0;
    long got = 0;
    for (;;) {
        if (count >= 0 && got >= count)
            break;
        Str line;
        str_init(&line);
        if (!read_line(fd, delim, &line, -1)) {
            str_free(&line);
            break;
        }
        if (strip && line.len && line.p[line.len - 1] == '\r')
            --line.len;
        char idx[24];
        fmt_long(idx, at + got, 0);
        arr_set(name, idx, str_cstr(&line));
        ++got;
        str_free(&line);
    }
    return 0;
}

struct Builtin {
    const char* name;
    int (*fn)(int, char**);
};

static struct Builtin builtins[] = {
    {"echo", b_echo},
    {":", b_colon},
    {"printf", b_printf},
    {"read", b_read},
    {"shopt", b_shopt},
    {"local", b_local},
    {"set", b_set},
    {"mapfile", b_mapfile},
    {"readarray", b_mapfile},
    {"test", b_test_words},
    {"[", b_bracket},
    {"true", b_true},
    {"false", b_false},
    {"cd", b_cd},
    {"pwd", b_pwd},
    {"exit", b_exit},
    {"export", b_export},
    {"unset", b_unset},
    {"type", b_type},
    {0, 0},
};

int is_builtin_name(const char* name)
{
    for (int i = 0; builtins[i].name; ++i)
        if (strcmp(builtins[i].name, name) == 0)
            return 1;
    return 0;
}

int builtin_run(int argc, char** argv)
{
    if (argc <= 0 || !argv[0])
        return sh_status;
    for (int i = 0; builtins[i].name; ++i) {
        if (strcmp(builtins[i].name, argv[0]) == 0)
            return builtins[i].fn(argc, argv);
    }
    return 127;
}
