/*
 * expand.c — turning a lexed word into the fields a command runs with.
 *
 * Expansion is where a shell's real behaviour lives, so every byte is
 * tagged as it is produced: whether it came from quotes (never split,
 * never a pattern), from an expansion (split on $IFS), or from the
 * word's own text (a pattern when unquoted). Word splitting and globbing
 * are then two passes over that same array instead of two guesses about
 * how a word was written.
 */
#include "shell.h"
#include <vlibc/dirent.h>

/* Per-byte tags in an expansion buffer. */
#define F_QUOTE 1 /* came from inside quotes */
#define F_SPLIT 2 /* came from an expansion: IFS may break here */
#define F_GLOB  4 /* glob-active while unquoted */

typedef struct {
    char* p;
    char* q;
    int len;
    int cap;
} Exp;

static void exp_init(Exp* e)
{
    e->cap = 64;
    e->p = (char*)malloc((unsigned long)e->cap);
    e->q = (char*)malloc((unsigned long)e->cap);
    e->len = 0;
}

static void exp_free(Exp* e)
{
    free(e->p);
    free(e->q);
    e->p = 0;
    e->q = 0;
    e->len = 0;
}

static void exp_room(Exp* e, int n)
{
    if (e->len + n <= e->cap)
        return;
    int cap = e->cap ? e->cap : 64;
    while (cap < e->len + n)
        cap *= 2;
    char* np = (char*)malloc((unsigned long)cap);
    char* nq = (char*)malloc((unsigned long)cap);
    if (!np || !nq)
        return;
    if (e->p) {
        memcpy(np, e->p, (unsigned long)e->len);
        memcpy(nq, e->q, (unsigned long)e->len);
        free(e->p);
        free(e->q);
    }
    e->p = np;
    e->q = nq;
    e->cap = cap;
}

static void exp_add(Exp* e, const char* t, int n, int flags)
{
    if (n <= 0)
        return;
    exp_room(e, n);
    memcpy(e->p + e->len, t, (unsigned long)n);
    for (int i = 0; i < n; ++i)
        e->q[e->len + i] = (char)flags;
    e->len += n;
}

static void exp_addc(Exp* e, char c, int flags)
{
    exp_room(e, 1);
    e->p[e->len] = c;
    e->q[e->len] = (char)flags;
    ++e->len;
}

/* --- the pieces an expansion can produce --- */

static void add_text(Exp* e, const char* text, int quoted, int from_expansion)
{
    int flags = from_expansion ? F_SPLIT : 0;
    if (quoted)
        flags |= F_QUOTE;
    else
        flags |= F_GLOB;
    exp_add(e, text, (int)strlen(text), flags);
}

static char* home_dir(void)
{
    const char* h = var_get("HOME");
    return sh_strdup(h[0] ? h : "/root");
}

/* ~ and ~user, from /etc/passwd's sixth field. */
static void add_tilde(Exp* e, const char* rest, int quoted)
{
    if (!rest || !rest[0]) {
        char* h = home_dir();
        add_text(e, h ? h : "/root", quoted, 0);
        free(h);
        return;
    }
    char user[32];
    int i = 0;
    while (rest[i] && rest[i] != '/' && i < 31)
        user[i] = rest[i];
    user[i] = 0;
    int fd = open("/etc/passwd", O_RDONLY);
    char dir[SH_PATH_MAX];
    dir[0] = 0;
    if (fd >= 0) {
        char line[SH_LINE_MAX];
        int n;
        while ((n = (int)read(fd, line, sizeof(line) - 1)) > 0) {
            line[n] = 0;
            char* f[7];
            int nf = 0;
            char* p = line;
            while (nf < 7) {
                f[nf++] = p;
                char* c = p;
                while (*c && *c != ':' && *c != '\n')
                    ++c;
                if (!*c)
                    break;
                *c = 0;
                p = c + 1;
            }
            if (nf >= 6 && strcmp(f[0], user) == 0) {
                strncpy(dir, f[5], SH_PATH_MAX - 1);
                dir[SH_PATH_MAX - 1] = 0;
                break;
            }
        }
        close(fd);
    }
    if (dir[0]) {
        add_text(e, dir, quoted, 0);
    } else {
        Str path;
        str_init(&path);
        str_addc(&path, '/');
        str_adds(&path, user);
        add_text(e, str_cstr(&path), quoted, 0);
        str_free(&path);
    }
}

/* --- ${...} --- */

static void expand_sub_text(Exp* e, const char* text, int quoted);

/* Arithmetic keeps its own shape: `a[0]++' has to reach the evaluator
 * with its subscript and `x' has to stay a variable name, so the whole
 * expression is not expanded as a word. Only the `$' forms are, the way
 * bash expands them before it evaluates: `${#a[@]}', `${n:-1}',
 * `$(cmd)' and a nested `$(( ))'. */
static void expand_arith_dollars(const char* expr, Str* out)
{
    for (const char* p = expr; *p; ++p) {
        if (*p != '$') {
            str_addc(out, *p);
            continue;
        }
        if (p[1] == '{') {
            int depth = 0;
            const char* q = p + 1;
            for (; *q; ++q) {
                if (*q == '{')
                    ++depth;
                else if (*q == '}' && --depth == 0)
                    break;
            }
            if (!*q) {
                str_addc(out, *p);
                continue;
            }
            char* piece = (char*)malloc((size_t)(q - p) + 1);
            memcpy(piece, p, (size_t)(q - p) + 1);
            Exp sub;
            exp_init(&sub);
            expand_sub_text(&sub, piece, 0);
            if (sub.len)
                str_addn(out, sub.p, sub.len);
            exp_free(&sub);
            free(piece);
            p = q;
            continue;
        }
        if (p[1] != '(') {
            str_addc(out, *p);
            continue;
        }
        /* `$(( ))' nests arithmetic, `$( )' runs a command. */
        int inner = (p[2] == '(');
        int depth = 0;
        const char* q = p + 1 + (inner ? 1 : 0);
        for (; *q; ++q) {
            if (*q == '(')
                ++depth;
            else if (*q == ')' && --depth == 0)
                break;
        }
        if (!*q) {
            str_addc(out, *p);
            continue;
        }
        const char* from = p + (inner ? 3 : 2);
        int len = (int)(q - from);
        char* piece = (char*)malloc((size_t)len + 1);
        memcpy(piece, from, (size_t)len);
        piece[len] = 0;
        if (inner) {
            expand_arith_dollars(piece, out);
        } else {
            char* r = run_capture(piece);
            if (r) {
                str_adds(out, r);
                free(r);
            }
        }
        free(piece);
        p = inner ? q + 1 : q;
    }
}

static void expand_word_parts(Word* w, Exp* e, int quoted);

/* Lex one word out of a raw string and expand it — the inner half of
 * `${x:-$y}' and friends. */
static void expand_sub_text(Exp* e, const char* text, int quoted)
{
    if (!text)
        return;
    Lex lx;
    lex_init_str(&lx, text, (int)strlen(text));
    lex_next(&lx);
    if (lx.tok.type == T_WORD)
        expand_word_parts(lx.tok.word, e, quoted);
    lex_free(&lx);
}

static char* sub_text_of(const char* text, int quoted)
{
    Exp e;
    exp_init(&e);
    expand_sub_text(&e, text, quoted);
    char* out = sh_strdup(e.p ? e.p : "");
    exp_free(&e);
    return out;
}

/* Split "name", "name[index]" or "name" off the front of a ${...} body
 * and return the rest. `index' is stored in *idx when the name was a
 * subscripted one. */
static char* parse_name(const char* body, const char** name,
                        const char** nend, int* idx, const char** rest,
                        int* is_at)
{
    const char* p = body;
    const char* s = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_' || *p == '@' || *p == '*' ||
           *p == '#' || *p == '?' || *p == '$' || *p == '!')
        ++p;
    *name = s;
    *nend = p;
    *idx = -1;
    *is_at = 0;
    if (*p == '[') {
        const char* q = p + 1;
        const char* ks = q;
        while (*q && *q != ']')
            ++q;
        char* key = (char*)malloc((unsigned long)(q - ks) + 1);
        memcpy(key, ks, (unsigned long)(q - ks));
        key[q - ks] = 0;
        if (strcmp(key, "@") == 0 || strcmp(key, "*") == 0)
            *is_at = 1;
        *idx = atoi(key);
        free(key);
        p = (*q == ']') ? q + 1 : q;
    }
    *rest = p;
    return (char*)p;
}

static int is_name(const char* s)
{
    if (!s || !*s)
        return 0;
    /* The one-byte special forms: $? $# $@ $* $$ $! */
    if (!s[1])
        return (s[0] >= '0' && s[0] <= '9') || s[0] == '?' || s[0] == '#' ||
               s[0] == '@' || s[0] == '*' || s[0] == '$' || s[0] == '!';
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_'))
        return 0;
    const char* p = s + 1;
    while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_'))
        ++p;
    /* A subscript belongs to the name: ${#a[0]} is the element's length.
     * The scan is written out because vlibc has no strchr, which is one
     * of the POSIX gaps B26 collects. */
    if (*p == '[') {
        const char* close = p + 1;
        while (*close && *close != ']')
            ++close;
        if (!*close || close[1])
            return 0;
        p = close + 1;
    }
    return *p == 0;
}

static const char* value_of(const char* name, int idx, int is_at)
{
    if (idx >= 0 && arr_len(name) > 0) {
        char key[24];
        if (is_at)
            return arr_get(name, "0");
        Str s;
        str_init(&s);
        sh_itoa(idx, &s);
        const char* v = arr_get(name, str_cstr(&s));
        str_free(&s);
        return v;
    }
    return var_lookup_word(name);
}

/* The real one: returns a malloc'd copy of `text' case-converted. */
static char* case_conv_str(const char* text, int up, const char* pat)
{
    Str out;
    str_init(&out);
    int n = (int)strlen(text);
    for (int i = 0; i < n; ++i) {
        char c = text[i];
        int take = 1;
        if (pat && pat[0])
            take = sh_pattern(pat, (char[]){c, 0});
        if (take) {
            if (up && c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            else if (!up && c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
        }
        str_addc(&out, c);
    }
    char* r = sh_strdup(str_cstr(&out));
    str_free(&out);
    return r;
}

static void add_all_args(Exp* e, int quoted)
{
    char* ifs = sh_strdup(sh_ifs ? sh_ifs : " \t\n");
    const char* sep = ifs[0] ? ifs : " ";
    if (quoted) {
        for (int i = 1; i <= sh_argc; ++i) {
            if (i > 1)
                exp_addc(e, sep[0], F_QUOTE);
            add_text(e, sh_argv[i], quoted, 1);
        }
        free(ifs);
        return;
    }
    Str out;
    str_init(&out);
    for (int i = 1; i <= sh_argc; ++i) {
        if (i > 1)
            str_addc(&out, sep[0]);
        str_adds(&out, sh_argv[i]);
    }
    add_text(e, str_cstr(&out), quoted, 1);
    str_free(&out);
    free(ifs);
}

static void add_array(Exp* e, const char* name, int quoted, int star)
{
    int n = 0;
    char** keys = arr_keys(name, &n);
    if (!keys)
        return;
    const char* ifs = sh_ifs ? sh_ifs : " \t\n";
    char sep = ifs[0] ? ifs[0] : ' ';
    for (int i = 0; i < n; ++i) {
        const char* v = arr_get(name, keys[i]);
        if (i > 0)
            exp_addc(e, sep, quoted ? F_QUOTE : (F_SPLIT | F_GLOB));
        add_text(e, v, quoted, 1);
    }
    (void)star;
    free(keys);
}

static void expand_brace_body(Exp* e, const char* body, int quoted)
{
    /* ${#name} — the length of a value, or the count of an array. */
    if (body[0] == '#' && is_name(body + 1)) {
        const char* name;
        const char* nend;
        int idx;
        int is_at;
        const char* rest;
        parse_name(body + 1, &name, &nend, &idx, &rest, &is_at);
        char* nbuf = (char*)malloc((unsigned long)(nend - name) + 1);
        memcpy(nbuf, name, (unsigned long)(nend - name));
        nbuf[nend - name] = 0;
        Str num;
        str_init(&num);
        if (is_at || (idx >= 0 && arr_len(nbuf) > 0)) {
            sh_itoa(arr_len(nbuf), &num);
        } else {
            sh_itoa((long)strlen(value_of(nbuf, idx, 0)), &num);
        }
        add_text(e, str_cstr(&num), quoted, 1);
        str_free(&num);
        free(nbuf);
        return;
    }
    /* ${!name} — the value of the variable that `name' names. Two steps,
     * and it used to take one: var_lookup_word() is the $name expander, so
     * passing it `name' printed the value of `name' and left the `!'
     * accepted but ignored — ${!P} answered exactly what ${P} does.
     * The value it returns is the *name* to look up. */
    if (body[0] == '!' && is_name(body + 1)) {
        const char* named = var_lookup_word(body + 1);
        if (!named[0]) {
            /* An unset or empty inner name has no name to indirection
             * through, and quietly expanding to nothing would be
             * indistinguishable from a variable that is merely unset. */
            sh_err("vash: ");
            sh_err(body + 1);
            sh_err(": invalid indirect expansion\n");
            return;
        }
        add_text(e, var_get(named), quoted, 1);
        return;
    }
    /* ${!prefix*} / ${!prefix@} — every name that starts with `prefix'.
     * The glob character closes the word, as in ${!P*}, so it is the last
     * byte of the body. The guard used to look for it straight after the
     * `!', as in ${!*P}, which is not a form anything writes: no body
     * could match, so the loop below was unreachable and the expression
     * fell through to expanding a variable named `!P*'. */
    int blen = (int)strlen(body);
    if (body[0] == '!' && blen > 2 && (body[blen - 1] == '*' || body[blen - 1] == '@')) {
        char prefix[64];
        int pn = blen - 2; /* one byte for the `!' and one for the glob */
        if (pn > (int)sizeof(prefix) - 1)
            pn = (int)sizeof(prefix) - 1;
        memcpy(prefix, body + 1, (unsigned long)pn);
        prefix[pn] = 0;
        int sep = sh_ifs ? sh_ifs[0] : ' ';
        int first = 1;
        int n = 0;
        char** names = var_names(&n);
        for (int i = 0; i < n; ++i) {
            if (!sh_starts(names[i], prefix))
                continue;
            if (!first)
                exp_addc(e, (char)sep, quoted ? F_QUOTE : F_SPLIT);
            first = 0;
            add_text(e, names[i], quoted, 1);
        }
        /* var_names() lends the table's own name pointers; only the array
         * is ours to release. */
        free(names);
        return;
    }

    const char* name;
    const char* nend;
    int idx;
    int is_at;
    const char* rest;
    parse_name(body, &name, &nend, &idx, &rest, &is_at);
    int nlen = (int)(nend - name);
    char* nbuf = (char*)malloc(nlen + 1);
    memcpy(nbuf, name, (unsigned long)nlen);
    nbuf[nlen] = 0;

    /* ${name[@]} / ${name[*]} — every element, joined on IFS. */
    if (is_at) {
        add_array(e, nbuf, quoted, rest[0] == '*');
        free(nbuf);
        return;
    }

    const char* op = rest;
    int colon = 0;
    char* word = 0;

    /* ${name:-default}, ${name:=default}, ${name:?message},
     * ${name:+alternate} — and the same without the colon. */
    if ((op[0] == '-' && op[1] == ':') || (op[0] == '-' && op[1] == 0) ||
        (op[0] == '=' && op[1] == ':') || (op[0] == '=' && op[1] == 0) ||
        (op[0] == '?' && op[1] == ':') || (op[0] == '?' && op[1] == 0) ||
        (op[0] == '+' && op[1] == ':') || (op[0] == '+' && op[1] == 0)) {
        char kind = op[0];
        colon = (op[1] == ':');
        word = sub_text_of(op + 1 + colon, quoted);
        const char* v = value_of(nbuf, idx, 0);
        int empty = (idx >= 0 && arr_len(nbuf) > 0) ? (v[0] == 0)
                                                     : (v[0] == 0);
        int unset = (var_find(nbuf) == 0 && !(idx >= 0 && arr_len(nbuf) > 0));
        /* Without the colon only an unset variable counts; with it an
         * empty value does too. */
        int bad = colon ? (unset || empty) : unset;
        int good = colon ? !(unset || empty) : !empty;
        if (kind == '+') {
            if (good)
                add_text(e, word, quoted, 1);
        } else if (kind == '?') {
            if (bad) {
                sh_err(nbuf);
                sh_err(colon ? (word && word[0] ? word : ": parameter null")
                             : ": parameter not set");
                sh_err("\n");
                free(word);
                free(nbuf);
                sh_flow = FLOW_EXIT;
                sh_flow_arg = 1;
                return;
            }
        } else if (bad) { /* ${x:-word} and ${x:=word} */
            if (kind == '=')
                var_set(nbuf, word ? word : "");
            add_text(e, word ? word : "", quoted, 1);
        } else {
            add_text(e, v, quoted, 1);
        }
        free(word);
        free(nbuf);
        return;
    }

    const char* v = value_of(nbuf, idx, 0);

    /* ${name:offset} and ${name:offset:length} — a piece of the value.
     * A negative offset counts back from the end. */
    if (op[0] == ':') {
        int len = (int)strlen(v);
        const char* colon = 0;
        for (const char* p = op + 1; *p; ++p) {
            if (*p == ':') {
                colon = p;
                break;
            }
        }
        char num[32];
        int n = 0;
        for (const char* p = op + 1; *p && p != colon && n < 31; ++p)
            num[n++] = *p;
        num[n] = 0;
        int start = (int)atol(num);
        if (start < 0)
            start = len + start;
        if (start < 0)
            start = 0;
        if (start > len)
            start = len;
        int stop = len;
        if (colon) {
            int m = 0;
            for (const char* p = colon + 1; *p && m < 31; ++p)
                num[m++] = *p;
            num[m] = 0;
            long count = atol(num);
            stop = (count < 0) ? len + (int)count : start + (int)count;
            if (stop < start)
                stop = start;
            if (stop > len)
                stop = len;
        }
        char* piece = (char*)malloc((unsigned long)(stop - start) + 1);
        memcpy(piece, v + start, (unsigned long)(stop - start));
        piece[stop - start] = 0;
        add_text(e, piece, quoted, 1);
        free(piece);
        free(nbuf);
        return;
    }

    /* ${name#pat}, ${name##pat}, ${name%pat}, ${name%%pat} */
    if ((op[0] == '#' && op[1] == '#') || (op[0] == '#' && op[1] == 0) ||
        (op[0] == '%' && op[1] == '%') || (op[0] == '%' && op[1] == 0)) {
        char* pat = sub_text_of(op + 1 + (op[1] ? 1 : 0), quoted);
        char out[SH_LINE_MAX];
        int stripped = 0;
        if (op[0] == '#')
            stripped = sh_strip_prefix(v, pat, op[1] == '#', out, sizeof(out));
        else
            stripped = sh_strip_suffix(v, pat, op[1] == '%', out, sizeof(out));
        add_text(e, stripped ? out : v, quoted, 1);
        free(pat);
        free(nbuf);
        return;
    }

    /* ${name/pat/rep} and its `//', `#' and `%' variants. */
    if ((op[0] == '/' || op[0] == '#' || op[0] == '%') &&
        (op[1] == '/' || (op[0] == '#' && op[1] == 0) ||
         (op[0] == '%' && op[1] == 0))) {
        char* pat = sub_text_of(op + 1, quoted);
        char* rep = sub_text_of(op + 2, quoted);
        Str out;
        str_init(&out);
        int all = (op[1] == '/');
        int anchor_front = (op[0] == '#');
        int anchor_back = (op[0] == '%');
        if (anchor_front || anchor_back) {
            char tmp[SH_LINE_MAX];
            int hit = anchor_front
                          ? sh_strip_prefix(v, pat, 0, tmp, sizeof(tmp))
                          : sh_strip_suffix(v, pat, 0, tmp, sizeof(tmp));
            if (hit) {
                str_adds(&out, rep);
                str_adds(&out, tmp);
            } else {
                str_adds(&out, v);
            }
        } else {
            int from = 0;
            int once = all ? 0 : 1;
            for (;;) {
                int at = sh_find_sub(v + from, pat, 0);
                if (at < 0)
                    break;
                str_addn(&out, v + from, at);
                str_adds(&out, rep);
                from += at + (int)strlen(pat);
                if (once)
                    break;
            }
            str_adds(&out, v + from);
        }
        add_text(e, str_cstr(&out), quoted, 1);
        str_free(&out);
        free(pat);
        free(rep);
        free(nbuf);
        return;
    }

    /* ${name^^pat}, ${name^pat}, ${name,,pat}, ${name,pat} */
    if ((op[0] == '^' && op[1] == '^') || (op[0] == '^' && op[1] == 0) ||
        (op[0] == ',' && op[1] == ',') || (op[0] == ',' && op[1] == 0)) {
        char* pat = sub_text_of(op + (op[1] ? 1 : 0), quoted);
        char* conv = case_conv_str(v, op[0] == '^', pat[0] ? pat : 0);
        add_text(e, conv, quoted, 1);
        free(conv);
        free(pat);
        free(nbuf);
        return;
    }

    /* ${name:offset:length} — a substring, counted in characters. */
    if (op[0] == ':') {
        int ok = 1;
        long off = arith_eval(op + 1, &ok);
        if (!ok)
            off = sh_atol(op + 1);
        char* rep = 0;
        if (op[1]) {
            const char* colon = op;
            while (*colon && *colon != ':')
                ++colon;
            rep = sub_text_of(colon + 1, quoted);
        }
        int len = rep ? (int)strlen(rep) : -1;
        int slen = (int)strlen(v);
        int start = (int)(off < 0 ? (slen + off < 0 ? 0 : slen + off) : off);
        if (start > slen)
            start = slen;
        int count = len < 0 ? slen - start : len;
        if (count < 0)
            count = 0;
        if (start + count > slen)
            count = slen - start;
        char tmp[SH_LINE_MAX];
        for (int i = 0; i < count && start + i < slen; ++i)
            tmp[i] = v[start + i];
        tmp[count] = 0;
        add_text(e, tmp, quoted, 1);
        free(rep);
        free(nbuf);
        return;
    }

    add_text(e, v, quoted, 1);
    free(nbuf);
}

/* --- the parts of a word --- */

char* sh_expand_arith(const char* expr)
{
    Str out;
    str_init(&out);
    if (expr)
        expand_arith_dollars(expr, &out);
    char* r = sh_strdup(out.p ? out.p : "");
    str_free(&out);
    return r;
}

static void expand_word_parts(Word* w, Exp* e, int quoted)
{
    for (Word* p = w; p; p = p->next) {
        int q = quoted || p->quoted;
        switch (p->kind) {
        case W_LIT:
            add_text(e, str_cstr(&p->text), q, 0);
            break;
        case W_VAR: {
            const char* name = str_cstr(&p->text);
            if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
                add_all_args(e, q);
                break;
            }
            const char* v = var_lookup_word(name);
            if (sh_nounset && !v[0] && var_find(name) == 0) {
                sh_err(name);
                sh_err(": unbound variable\n");
                sh_flow = FLOW_EXIT;
                sh_flow_arg = 1;
                return;
            }
            add_text(e, v, q, 1);
            break;
        }
        case W_CMDSUB: {
            char* out = run_capture(str_cstr(&p->text));
            add_text(e, out ? out : "", q, 1);
            free(out);
            break;
        }
        case W_FILE: {
            /* `$(< file)': the contents, split the way a word is. The
             * name is plain text — the lexer took it up to the blank. */
            Str name;
            str_init(&name);
            str_addn(&name, str_cstr(&p->text), p->text.len);
            char* text = read_whole_file(str_cstr(&name));
            if (text) {
                add_text(e, text, q, 1);
                free(text);
            }
            str_free(&name);
            break;
        }
        case W_ARITH: {
            int ok = 1;
            Str text;
            str_init(&text);
            expand_arith_dollars(str_cstr(&p->text), &text);
            long n = arith_eval(str_cstr(&text), &ok);
            str_free(&text);
            Str num;
            str_init(&num);
            sh_itoa(n, &num);
            add_text(e, str_cstr(&num), q, 1);
            str_free(&num);
            break;
        }
        case W_TILDE:
            add_tilde(e, str_cstr(&p->text), q);
            break;
        case W_BRACE:
            expand_brace_body(e, str_cstr(&p->text), q);
            break;
        default:
            break;
        }
    }
}

/* --- word splitting --- */

static int is_ifs(int c, const char* ifs)
{
    for (const char* p = ifs; *p; ++p)
        if (*p == c)
            return 1;
    return 0;
}

static void split_exp(Exp* e, Vec* out, int quote_all)
{
    const char* ifs = sh_ifs ? sh_ifs : " \t\n";
    if (!ifs[0]) {
        Str* s = (Str*)malloc(sizeof(Str));
        str_init(s);
        str_addn(s, e->p ? e->p : "", e->len);
        vec_push(out, s);
        return;
    }
    Str cur;
    str_init(&cur);
    int have = 0;
    for (int i = 0; i < e->len; ++i) {
        int q = e->q[i];
        int splittable = (q & F_SPLIT) && !(q & F_QUOTE) && !quote_all;
        if (splittable && is_ifs((unsigned char)e->p[i], ifs)) {
            if (have) {
                Str* s = (Str*)malloc(sizeof(Str));
                *s = cur;
                str_init(&cur);
                vec_push(out, s);
                have = 0;
            }
            continue;
        }
        str_addc(&cur, e->p[i]);
        have = 1;
    }
    if (have) {
        Str* s = (Str*)malloc(sizeof(Str));
        *s = cur;
        str_init(&cur);
        vec_push(out, s);
    }
    str_free(&cur);
}

/* --- globbing --- */

static int has_pattern(Str* s, Exp* e)
{
    if (!e->q)
        return 0;
    int n = s->len;
    for (int i = 0; i < n && i < e->len; ++i) {
        if (e->q[i] & F_QUOTE)
            continue;
        if (!(e->q[i] & F_GLOB))
            continue;
        if (s->p[i] == '*' || s->p[i] == '?' || s->p[i] == '[')
            return 1;
    }
    return 0;
}

/* The directory a pattern lives in and the pattern's tail. */
static void split_dir(const char* path, char* dir, int dirn, const char** tail)
{
    const char* slash = sh_rfind(path, "/");
    if (!slash) {
        if (dirn > 1) {
            dir[0] = '.';
            dir[1] = 0;
        }
        *tail = path;
        return;
    }
    int n = (int)(slash - path);
    if (slash == path)
        n = 1; /* "/foo" -> dir "/" */
    if (n > dirn - 1)
        n = dirn - 1;
    memcpy(dir, path, (unsigned long)n);
    dir[n] = 0;
    *tail = slash + 1;
}

static int cmp_name(const void* a, const void* b)
{
    const char* x = *(char* const*)a;
    const char* y = *(char* const*)b;
    return strcmp(x, y);
}

int sh_glob(const char* pattern, Vec* out)
{
    char dir[SH_PATH_MAX];
    const char* pat;
    split_dir(pattern, dir, sizeof(dir), &pat);
    DIR* d = opendir(dir[0] ? dir : "/");
    if (!d)
        return 0;
    Vec hits;
    vec_init(&hits);
    struct dirent* de;
    int n = 0;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        ++n;
        if (!sh_pattern(pat, de->d_name))
            continue;
        vec_push(&hits, sh_strdup(de->d_name));
    }
    closedir(d);
    if (hits.n == 0) {
        vec_free(&hits);
        return 0;
    }
    /* bash sorts glob results. */
    for (int i = 1; i < hits.n; ++i) {
        char* key = (char*)hits.v[i];
        int j = i - 1;
        while (j >= 0 && cmp_name(&hits.v[j], &key) > 0) {
            hits.v[j + 1] = hits.v[j];
            --j;
        }
        hits.v[j + 1] = key;
    }
    int prefix = (dir[0] && strcmp(dir, ".") != 0) ? (int)strlen(dir) : 0;
    for (int i = 0; i < hits.n; ++i) {
        char* name = (char*)hits.v[i];
        Str full;
        str_init(&full);
        if (prefix) {
            str_addn(&full, dir, prefix);
            str_addc(&full, '/');
        }
        str_adds(&full, name);
        vec_push(out, str_cstr(&full));
        free(name);
        str_free(&full);
    }
    vec_free(&hits);
    (void)n;
    return 1;
}

/* --- the two entry points --- */

void expand_word(Word* w, Vec* out)
{
    Exp e;
    exp_init(&e);
    expand_word_parts(w, &e, 0);

    Vec fields;
    vec_init(&fields);
    split_exp(&e, &fields, 0);

    /* `x=; echo "[$x]"' has an empty argument in it, and `""' is one
     * empty argument however empty it is: quoting is what keeps the
     * word on the list. */
    if (fields.n == 0) {
        int quoted = 0;
        for (Word* wp = w; wp; wp = wp->next)
            if (wp->quoted)
                quoted = 1;
        if (quoted) {
            Str* s = (Str*)malloc(sizeof(Str));
            str_init(s);
            vec_push(out, s);
        }
    }

    for (int i = 0; i < fields.n; ++i) {
        Str* s = (Str*)fields.v[i];
        if (has_pattern(s, &e)) {
            Vec hits;
            vec_init(&hits);
            if (sh_glob(str_cstr(s), &hits) && hits.n > 0) {
                for (int h = 0; h < hits.n; ++h) {
                    Str* g = (Str*)malloc(sizeof(Str));
                    str_init(g);
                    str_adds(g, (char*)hits.v[h]);
                    vec_push(out, g);
                }
                vec_free(&hits);
                str_free(s);
                free(s);
                continue;
            }
            vec_free(&hits);
        }
        vec_push(out, s);
    }
    vec_free(&fields);
    exp_free(&e);
}

void expand_word_one(Word* w, Str* out)
{
    Exp e;
    exp_init(&e);
    expand_word_parts(w, &e, 1);
    str_addn(out, e.p ? e.p : "", e.len);
    exp_free(&e);
}

/* Expand a whole line of text as one word (builtins that take a string
 * and want it expanded: `printf "$fmt" "$@"` and friends). */
char* expand_str(const char* s)
{
    Lex lx;
    lex_init_str(&lx, s, (int)strlen(s));
    Vec fields;
    vec_init(&fields);
    Str out;
    str_init(&out);
    lex_next(&lx);
    while (lx.tok.type == T_WORD) {
        Vec one;
        vec_init(&one);
        expand_word(lx.tok.word, &one);
        for (int i = 0; i < one.n; ++i) {
            if (i)
                str_addc(&out, ' ');
            Str* f = (Str*)one.v[i];
            str_adds(&out, str_cstr(f));
            str_free(f);
            free(f);
        }
        vec_free(&one);
        lex_next(&lx);
    }
    lex_free(&lx);
    vec_free(&fields);
    return str_cstr(&out);
}
