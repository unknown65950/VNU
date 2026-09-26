/*
 * vcc.c - the VNU C compiler. One binary: preprocessor, i386 C subset
 * compiler producing ELF relocatable objects, and a linker that links
 * against crt0.o and /lib/libvlibc.a (both embedded in the binary).
 *
 * Constraint: this file must stay small enough to edit/compile inside
 * VNU, so it sticks to a strict C subset: no floats, no long long,
 * no bitfields, no function pointers.
 */

#include <vlibc/stdio.h>
#include <vlibc/stdlib.h>
#include <vlibc/string.h>
#include <vlibc/unistd.h>
#include <vlibc/fcntl.h>

#ifndef VCC_NOMAIN
#define MAIN main
#else
#define MAIN vcc_main
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

extern int MAIN(int argc, char** argv);

/* ------------------------------------------------------------------ */
/* arena allocator                                                    */
/* ------------------------------------------------------------------ */

#define ARENA_MAX 700000
static char arena[ARENA_MAX];
static int arena_used;

static void vmemmove(void* dst, const void* src, int n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    int i;
    if (d < s) {
        for (i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (i = n - 1; i >= 0; i--)
            d[i] = s[i];
    }
}

static void* aalloc(int n)
{
    void* p;
    n = (n + 15) & ~15;
    if (arena_used + n > ARENA_MAX) {
        printf("vcc: arena exhausted (%d)\n", arena_used + n);
        exit(1);
    }
    p = arena + arena_used;
    arena_used += n;
    return p;
}

static char* astrdup(const char* s)
{
    int n = strlen(s) + 1;
    char* p = (char*)aalloc(n);
    memcpy(p, s, n);
    return p;
}

/* fixed character buffer used for path building / small scratch */
static char scratch[1024];

/* ------------------------------------------------------------------ */
/* growable byte buffer                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    u8* data;
    int len;
    int cap;
} Buf;

static void breserve(Buf* b, int n)
{
    u8* nd;
    if (b->cap >= b->len + n)
        return;
    b->cap = b->cap ? b->cap * 2 : 256;
    while (b->cap < b->len + n)
        b->cap *= 2;
    nd = (u8*)aalloc(b->cap);
    if (b->len)
        memcpy(nd, b->data, b->len);
    b->data = nd;
}

/* Fresh Buf: all fields uninitialized otherwise, and breserve() skips the
 * first allocation when garbage cap >= garbage len + n, leaving bemit() to
 * write through a bogus data pointer. Always start a Buf with binit(). */
static void binit(Buf* b)
{
    b->data = 0;
    b->len = 0;
    b->cap = 0;
}

static void bemit(Buf* b, const void* d, int n)
{
    breserve(b, n);
    memcpy(b->data + b->len, d, n);
    b->len += n;
}

static void bemit1(Buf* b, int v)
{
    u8 c = (u8)v;
    bemit(b, &c, 1);
}
static void bemit2(Buf* b, int v)
{
    u8 d[2];
    d[0] = (u8)v;
    d[1] = (u8)(v >> 8);
    bemit(b, d, 2);
}
static void bemit4(Buf* b, u32 v)
{
    u8 d[4];
    d[0] = (u8)v;
    d[1] = (u8)(v >> 8);
    d[2] = (u8)(v >> 16);
    d[3] = (u8)(v >> 24);
    bemit(b, d, 4);
}

/* ------------------------------------------------------------------ */
/* tokens                                                              */
/* ------------------------------------------------------------------ */

enum {
    TK_EOF = 0,
    TK_ID,
    TK_NUM,
    TK_STR,
    TK_PUNCT
};

typedef struct {
    int ty;
    u32 val;         /* TK_NUM value */
    char* name;      /* TK_ID */
    int slen;        /* TK_STR byte length (no NUL stored in buf) */
    char* str;       /* TK_STR cooked bytes */
    int line;
} Tok;

static Tok* tok;        /* token vector */
static int* tok_pp;     /* per-token preprocessor group (file/if nesting), unused v1 */
static int ntok;
static int tok_cap;
static int tpos;        /* parser position */

static Tok* pcur(void)
{
    return &tok[tpos];
}

static int is_punct(const char* p)
{
    Tok* t = pcur();
    if (t->ty != TK_PUNCT)
        return 0;
    return strcmp(t->name, p) == 0;
}

static void tokskip(void)
{
    tpos++;
}

static void tskip(void)
{
    tpos++;
}

static void ensure_tok(int n)
{
    int ncap;
    Tok* nt;
    if (tpos + n >= ntok) {
        printf("vcc: unexpected end of file (line %d)\n", pcur()->line);
        exit(1);
    }
    (void)ncap;
    (void)nt;
}

/* constants for the parser: offset of syntax error reporting */
static int tline;

/* ------------------------------------------------------------------ */
/* options                                                             */
/* ------------------------------------------------------------------ */

static char* incdirs[16];
static int nincdirs;
static char outpath[256];

/* ------------------------------------------------------------------ */
/* file reader (arena)                                                 */
/* ------------------------------------------------------------------ */

static char* read_file(const char* path, int* outlen)
{
    char buf[512];
    int fd;
    int total = 0;
    int n;
    int cap = 65536;
    char* p;
    fd = open(path, 0);
    if (fd < 0)
        return 0;
    p = (char*)malloc(cap);
    for (;;) {
        n = read(fd, buf, 512);
        if (n <= 0)
            break;
        if (total + n > cap) {
            char* np;
            while (total + n > cap)
                cap *= 2;
            np = (char*)malloc(cap);
            if (total)
                memcpy(np, p, total);
            free(p);
            p = np;
        }
        memcpy(p + total, buf, n);
        total += n;
    }
    close(fd);
    p[total] = 0;
    *outlen = total;
    return p;
}

/* ------------------------------------------------------------------ */
/* preprocessor: macros, includes, conditionals                        */
/* ------------------------------------------------------------------ */

#define NHASH 257

typedef struct Macro Macro;
struct Macro {
    Macro* next;
    char* name;
    Tok* body;      /* replacement list */
    int nbody;
    char** params;
    int nparams;
    int is_func;
    int expanding;
    int undef;
};

static Macro* mtab[NHASH];

static int hashstr(const char* s)
{
    int h = 0;
    while (*s)
        h = (h * 33 + (u8)*s++) % NHASH;
    return h;
}

static Macro* mlookup(const char* name)
{
    Macro* m = mtab[hashstr(name)];
    while (m) {
        if (strcmp(m->name, name) == 0)
            return m;
        m = m->next;
    }
    return 0;
}

static void mdefine(const char* name, Tok* body, int nbody,
                    char** params, int nparams, int is_func)
{
    Macro* m = mlookup(name);
    if (!m) {
        m = (Macro*)aalloc(sizeof(Macro));
        m->name = astrdup(name);
        m->next = mtab[hashstr(name)];
        mtab[hashstr(name)] = m;
    }
    m->body = body;
    m->nbody = nbody;
    m->params = params;
    m->nparams = nparams;
    m->is_func = is_func;
    m->undef = 0;
}

static void mundef(const char* name)
{
    Macro* m = mlookup(name);
    if (m)
        m->undef = 1;
}

/* lexer source state */
static const char* src;
static int spos;
static int slen;
static int sline;


/* ---- include path resolution ---- */
static char* find_include2(const char* name, int is_angle, int fatal,
                           int* outlen, const char* dir)
{
    int i;
    char* p;
    if (!is_angle && dir && dir[0]) {
        int n = strlen(dir) + 1 + strlen(name) + 1;
        char* path = (char*)aalloc(n);
        memcpy(path, dir, strlen(dir));
        path[strlen(dir)] = '/';
        memcpy(path + strlen(dir) + 1, name, strlen(name) + 1);
        p = read_file(path, outlen);
        if (p)
            return p;
    }
    for (i = 0; i < nincdirs; i++) {
        int n = strlen(incdirs[i]) + 1 + strlen(name) + 1;
        char* path = (char*)aalloc(n);
        memcpy(path, incdirs[i], strlen(incdirs[i]));
        path[strlen(incdirs[i])] = '/';
        memcpy(path + strlen(incdirs[i]) + 1, name, strlen(name) + 1);
        p = read_file(path, outlen);
        if (p)
            return p;
    }
    p = read_file(name, outlen); /* plain current dir */
    if (p)
        return p;
    if (fatal) {
        printf("vcc: cannot open include <%s>\n", name);
        exit(1);
    }
    return 0;
}

static int pp_expr(Tok* t, int* n, int* read);

static int pp_atomic(Tok* t, int* n, int* read)
{
    if (!*n) {
        printf("vcc: #if: end of line\n");
        exit(1);
    }
    if (t[0].ty == TK_NUM) {
        *read = 1;
        return t[0].val;
    }
    if (t[0].ty == TK_PUNCT && t[0].name[0] == '(') {
        int v;
        *read = 1;
        if (*n < 2) {
            printf("vcc: #if: bad '(' \n");
            exit(1);
        }
        v = pp_expr(t + 1, &((int){*n - 1}), &((int){0}));
        /* simplistic: not used since we parse with explicit parens below */
        (void)v;
        return v;
    }
    if (t[0].ty == TK_ID) {
        Macro* m = mlookup(t[0].name);
        if (m && !m->undef && !m->is_func && m->nbody) {
            int v = 0;
            int r = 0;
            v = pp_expr(m->body, &m->nbody, &r);
            *read = 1;
            return v;
        }
        *read = 1;
        return 0;
    }
    if (t[0].ty == TK_PUNCT && strcmp(t[0].name, "defined") == 0) {
        *read = 1;
        return 1; /* handled by caller, never reached */
    }
    *read = 1;
    (void)0;
    return 0;
}

/* A proper recursive-descent pp expression parser. returns value.
 * *n = remaining count; *read = consumed count. */
static int pp_binexpr(Tok* t, int* n, int* read, int minpred);

static int pp_unary(Tok* t, int* n, int* read)
{
    int v;
    if (!*n) {
        printf("vcc: #if: premature end\n");
        exit(1);
    }
    if (t[0].ty == TK_PUNCT) {
        if (t[0].name[0] == '(' && t[0].name[1] == 0) {
            (*read)++;
            (*n)--;
            v = pp_binexpr(t + 1, n, read, 0);
            if (*n == 0 || t[0].name[0] != ')' || t[0].name[1]) {
                printf("vcc: #if: unbalanced parens\n");
                exit(1);
            }
            (*read)++;
            (*n)--;
            return v;
        }
        if (t[0].name[0] == '!' || t[0].name[0] == '~' || t[0].name[0] == '-') {
            int op = t[0].name[0];
            (*read)++;
            (*n)--;
            v = pp_unary(t + 1, n, read);
            if (op == '!')
                return !v;
            if (op == '~')
                return ~v;
            return -v;
        }
    }
    if (t[0].ty == TK_NUM) {
        (*read)++;
        (*n)--;
        return t[0].val;
    }
    if (t[0].ty == TK_ID) {
        Macro* m = 0;
        const char* nm = t[0].name;
        if (nm[0] == 'd' && nm[1] == 'e' && nm[2] == 'f' && nm[3] == 'i' &&
            nm[4] == 'n' && nm[5] == 'e' && nm[6] == 'd' && nm[7] == 0) {
            /* defined NAME or defined(NAME) */
            (*read)++;
            (*n)--;
            v = 0;
            if (*n && t[0].ty == TK_PUNCT && t[0].name[0] == '(' && !t[0].name[1]) {
                (*read)++;
                (*n)--;
                if (*n == 0 || t[0].ty != TK_ID) {
                    printf("vcc: #if: bad defined()\n");
                    exit(1);
                }
                m = mlookup(t[0].name);
                v = m && !m->undef;
                (*read)++;
                (*n)--;
                if (*n == 0 || t[0].ty != TK_PUNCT || t[0].name[0] != ')' ||
                    t[0].name[1]) {
                    printf("vcc: #if: bad defined()\n");
                    exit(1);
                }
                (*read)++;
                (*n)--;
                return v;
            }
            if (*n && t[0].ty == TK_ID) {
                m = mlookup(t[0].name);
                v = m && !m->undef;
                (*read)++;
                (*n)--;
            }
            return v;
        }
        m = mlookup(nm);
        if (m && !m->undef && !m->is_func && m->nbody) {
            int r2 = 0;
            v = pp_binexpr(m->body, &m->nbody, &r2, 0);
            (*read)++;
            (*n)--;
            return v;
        }
        (*read)++;
        (*n)--;
        return 0;
    }
    if (t[0].ty == TK_PUNCT) {
        /* char constant 'a' lexed as NUM already */
    }
    printf("vcc: #if: unexpected token\n");
    exit(1);
}

static int pp_binexpr(Tok* t, int* n, int* read, int minpred)
{
    int v = pp_unary(t, n, read);
    for (;;) {
        int pred = 0;
        int op = 0;
        if (*n && t[0].ty == TK_PUNCT) {
            const char* p = t[0].name;
            if (!p[1]) {
                int c = p[0];
                if (c == '*' || c == '/' || c == '%')
                    pred = 10, op = c;
                else if (c == '+' || c == '-')
                    pred = 9, op = c;
                else if (c == '<' || c == '>')
                    pred = 7, op = c;
                else if (c == '&')
                    pred = 6, op = c;
                else if (c == '^')
                    pred = 5, op = c;
                else if (c == '|')
                    pred = 4, op = c;
                else if (c == '!')
                    pred = 8, op = c;
                else if (c == '~')
                    pred = 8, op = c;
                else if (c == '?')
                    pred = 1, op = c;
            } else if (p[0] == '=' && p[1] == '=' && !p[2])
                pred = 7, op = '=';
            else if (p[0] == '!' && p[1] == '=' && !p[2])
                pred = 7, op = '!';
            else if (p[0] == '<' && p[1] == '=' && !p[2])
                pred = 7, op = 'L';
            else if (p[0] == '>' && p[1] == '=' && !p[2])
                pred = 7, op = 'G';
            else if (p[0] == '&' && p[1] == '&' && !p[2])
                pred = 3, op = 'A';
            else if (p[0] == '|' && p[1] == '|' && !p[2])
                pred = 2, op = 'O';
            else if (p[0] == '<' && p[1] == '<' && !p[2])
                pred = 8, op = 'S';
            else if (p[0] == '>' && p[1] == '>' && !p[2])
                pred = 8, op = 'T';
        }
        if (pred < minpred)
            break;
        if (op == 'A') {
            int rhs;
            (*read) += 2;
            (*n) -= 2;
            if (!v)
                return 0;
            rhs = pp_binexpr(t + 2, n, read, 3);
            (void)rhs;
            return v && rhs;
        }
        if (op == 'O') {
            (*read) += 2;
            (*n) -= 2;
            if (v)
                return 1;
            v = pp_binexpr(t + 2, n, read, 2);
            return v != 0;
        }
        {
            int rhs;
            (*read) += 1;
            (*n) -= 1;
            switch (op) {
            case 'S':
            case 'T':
                if (*n && t[0].ty == TK_PUNCT && t[0].name[0] == '>')
                    (*read) += 1, (*n) -= 1;
                else if (op == 'T' && *n && t[0].ty == TK_PUNCT && t[0].name[0] == '=')
                    (*read) += 1, (*n) -= 1;
                break;
            default:
                break;
            }
            rhs = pp_binexpr(t + 1, n, read, pred + 1);
            switch (op) {
            case '*': v = v * rhs; break;
            case '/': v = rhs ? v / rhs : 0; break;
            case '%': v = rhs ? v % rhs : 0; break;
            case '+': v = v + rhs; break;
            case '-': v = v - rhs; break;
            case 'S': v = v << rhs; break;
            case 'T': v = v >> rhs; break;
            case '<': v = v < rhs; break;
            case '>': v = v > rhs; break;
            case 'L': v = v <= rhs; break;
            case 'G': v = v >= rhs; break;
            case '=': v = v == rhs; break;
            case '!': v = v != rhs; break;
            case '&': v = v & rhs; break;
            case '^': v = v ^ rhs; break;
            case '|': v = v | rhs; break;
            default: break;
            }
        }
    }
    return v;
}

static int pp_expr(Tok* t, int* n, int* read)
{
    *read = 0;
    return pp_binexpr(t, n, read, 0);
}

/* ---- include / input buffer stack ---- */

#define IBMAX 16
typedef struct {
    const char* text;
    int pos;
    int len;
    int line;
    char* dir;
} InBuf;

static InBuf ibuf[IBMAX];
static int nbuf;

/* lexer source shortcuts */
#define LEXSRC(x) (x).text
#define LEXPOS(x) (x).pos
#define LEXLEN(x) (x).len
#define LEXLINE(x) (x).line

static void push_buf(const char* text, int len, char* dir)
{
    if (nbuf >= IBMAX) {
        printf("vcc: include nesting too deep\n");
        exit(1);
    }
    ibuf[nbuf].text = text;
    ibuf[nbuf].pos = 0;
    ibuf[nbuf].len = len;
    ibuf[nbuf].line = 1;
    ibuf[nbuf].dir = dir;
    nbuf++;
}

static InBuf* cb(void)
{
    return &ibuf[nbuf - 1];
}

static void push_tok(Tok* t)
{
    if (ntok >= tok_cap) {
        Tok* nt;
        int ncap = tok_cap ? tok_cap * 2 : 4096;
        nt = (Tok*)aalloc(sizeof(Tok) * ncap);
        if (ntok)
            memcpy(nt, tok, sizeof(Tok) * ntok);
        tok = nt;
        tok_cap = ncap;
    }
    tok[ntok++] = *t;
}

/* ---- macro expansion ---- */

/* expansion-queue area above the scanner is unused */
static u32 lex_num(InBuf* b)
{
    u32 v = 0;
    int base = 10;
    const char* s = b->text;
    if (b->pos + 1 < b->len && s[b->pos] == '0' &&
        (s[b->pos + 1] == 'x' || s[b->pos + 1] == 'X')) {
        base = 16;
        b->pos += 2;
    } else if (s[b->pos] == '0') {
        base = 8;
    }
    while (b->pos < b->len) {
        int c = s[b->pos];
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        v = v * base + d;
        b->pos++;
    }
    while (b->pos < b->len && (s[b->pos] == 'u' || s[b->pos] == 'U' ||
                               s[b->pos] == 'l' || s[b->pos] == 'L'))
        b->pos++;
    return v;
}

static int lex_escape_b(InBuf* b)
{
    int c;
    const char* s = b->text;
    c = s[b->pos];
    if (c >= '0' && c <= '7') {
        int v = 0, i;
        for (i = 0; i < 3 && b->pos < b->len && s[b->pos] >= '0' && s[b->pos] <= '7'; i++) {
            v = v * 8 + (s[b->pos] - '0');
            b->pos++;
        }
        return v;
    }
    if (c == 'x') {
        int v = 0;
        b->pos++;
        while (b->pos < b->len) {
            int d;
            if (s[b->pos] >= '0' && s[b->pos] <= '9')
                d = s[b->pos] - '0';
            else if (s[b->pos] >= 'a' && s[b->pos] <= 'f')
                d = s[b->pos] - 'a' + 10;
            else if (s[b->pos] >= 'A' && s[b->pos] <= 'F')
                d = s[b->pos] - 'A' + 10;
            else
                break;
            v = v * 16 + d;
            b->pos++;
        }
        return v;
    }
    b->pos++;
    switch (c) {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'r':
        return '\r';
    case 'a':
        return 7;
    case 'b':
        return 8;
    case 'e':
        return 27;
    default:
        return c;
    }
}

/* parse "..." or '...' into output buffer (cooked bytes for strings, a
 * single value for chars given back through out). */
static void lex_string_b(InBuf* b, int quote, Buf* out, int ischar)
{
    char* tmp = (char*)aalloc(1024);
    int n = 0;
    int startline = b->line;
    const char* s = b->text;
    b->pos++;
    while (b->pos < b->len && s[b->pos] != quote) {
        if (s[b->pos] == '\n') {
            printf("vcc: unterminated literal at line %d\n", startline);
            exit(1);
        }
        if (s[b->pos] == '\\') {
            b->pos++;
            tmp[n++] = (char)lex_escape_b(b);
        } else {
            tmp[n++] = s[b->pos];
            b->pos++;
        }
    }
    if (b->pos >= b->len) {
        printf("vcc: unterminated literal at line %d\n", startline);
        exit(1);
    }
    b->pos++;
    if (ischar) {
        Tok t;
        if (n != 1) {
            printf("vcc: empty character constant at line %d\n", startline);
            exit(1);
        }
        t.ty = TK_NUM;
        t.val = (u8)tmp[0];
        t.slen = 0;
        t.name = 0;
        t.line = startline;
        bemit(out, &t, sizeof(Tok));
    } else {
        Tok t;
        t.ty = TK_STR;
        t.val = 0;
        t.slen = n;
        t.name = (char*)aalloc(n + 1);
        memcpy(t.name, tmp, n);
        t.name[n] = 0;
        t.line = startline;
        bemit(out, &t, sizeof(Tok));
    }
}

static const char* puncts[] = {
    "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=",
    "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=",
    "|=", "^=",
    "+", "-", "*", "/", "%", "=", "!", "&", "|", "^", "~",
    "<", ">", "[", "]", "(", ")", "{", "}", ";", ",", ".", ":", "?", "#"
};

/* track newline: skip spaces/tabs/\r and backslash-newline continuations.
 * sets *saw_nl if a real newline (not a continuation) was crossed.
 * stops before '\n'. returns 0 when hitting newline or end of buffer. */
static int ws_nl(InBuf* b, int* saw_nl)
{
    *saw_nl = 0;
    for (;;) {
        const char* s = b->text;
        while (b->pos < b->len && (s[b->pos] == ' ' || s[b->pos] == '\t' ||
                                   s[b->pos] == '\r'))
            b->pos++;
        if (b->pos < b->len && s[b->pos] == '\n') {
            *saw_nl = 1;
            return 0;
        }
        if (b->pos + 1 < b->len && s[b->pos] == '\\' && s[b->pos + 1] == '\n') {
            b->pos += 2;
            b->line++;
            continue;
        }
        break;
    }
    return b->pos < b->len;
}

/* skip the '\n' itself and any following horizontal whitespace;
 * updates the line counter. returns 1 if more input remains. */
static void eat_newline(InBuf* b)
{
    if (b->pos < b->len && b->text[b->pos] == '\n') {
        b->pos++;
        b->line++;
    }
}

/* lex one primitive from buffer b into t. returns 1 on success.
 * sets *hash if the token is a single '#'. */
static int lex_next_b(InBuf* b, Tok* t, int* hash)
{
    int c;
    int i;
    const char* s = b->text;
    *hash = 0;
    c = s[b->pos];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        char name[128];
        int n = 0;
        while (b->pos < b->len && ((s[b->pos] >= 'a' && s[b->pos] <= 'z') ||
                                   (s[b->pos] >= 'A' && s[b->pos] <= 'Z') ||
                                   (s[b->pos] >= '0' && s[b->pos] <= '9') ||
                                   s[b->pos] == '_') && n < 126)
            name[n++] = s[b->pos++];
        name[n] = 0;
        t->ty = TK_ID;
        t->name = astrdup(name);
        t->val = 0;
        t->slen = 0;
        t->line = b->line;
        return 1;
    }
    if (c >= '0' && c <= '9') {
        t->ty = TK_NUM;
        t->val = lex_num(b);
        t->name = 0;
        t->slen = 0;
        t->line = b->line;
        return 1;
    }
    if (c == '"' || c == '\'') {
        Buf tmp;
        tmp.data = (u8*)aalloc(16);
        tmp.len = 0;
        tmp.cap = 16;
        lex_string_b(b, c, &tmp, c == '\'');
        {
            Tok save;
            memcpy(&save, &tmp, 0);
            (void)save;
        }
        *t = *(Tok*)tmp.data;
        return 1;
    }
    for (i = 0; i < (int)(sizeof(puncts) / sizeof(puncts[0])); i++) {
        const char* p = puncts[i];
        int ok = 1;
        int pc;
        for (pc = 0; p[pc] != 0; pc++) {
            if (b->pos + pc >= b->len || s[b->pos + pc] != p[pc]) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            t->ty = TK_PUNCT;
            t->name = (char*)p;
            t->val = 0;
            t->slen = 0;
            t->line = b->line;
            b->pos += pc;
            if (p[0] == '#')
                *hash = 1;
            return 1;
        }
    }
    printf("vcc: stray character '%c' (0x%02x) at line %d\n", c, c, b->line);
    exit(1);
}

static void appchar(char* out, int* n, int c)
{
    out[(*n)++] = (char)c;
}
/* ------------------------------------------------------------------ */
/* raw token reading, directives, macro expansion, master loop         */
/* ------------------------------------------------------------------ */

/* read up to max tokens of the current line (raw, unexpanded).
 * returns the count; leaves the newline unconsumed. */
static int read_line_toks(Tok* t, int max)
{
    int n = 0;
    for (;;) {
        InBuf* b = cb();
        int nl;
        Tok tmp;
        int hash;
        if (n >= max)
            break;
        if (!ws_nl(b, &nl))
            break;
        if (!lex_next_b(b, &tmp, &hash))
            break;
        t[n++] = tmp;
    }
    return n;
}

/* ---- conditional nesting ---- */

static int cond_skip[32];    /* this level is being skipped (incl. parents) */
static int cond_taken[32];   /* a branch at this level is active/taken */
static int ndepth = -1;

static int skipping_now(void)
{
    return ndepth >= 0 && cond_skip[ndepth];
}

/* ---- macro expansion ---- */

#define PMAX 16384
static Tok pend[PMAX];
static int pend_n;
static int pend_r;
static Tok empty_run[1];

static const char* open_macro[16];
static int open_end[16];
static int n_open;

static int pushing(const char* name)
{
    int i;
    for (i = 0; i < n_open; i++)
        if (strcmp(open_macro[i] ? open_macro[i] : "", name) == 0)
            return 1;
    return 0;
}

static int find_param(Macro* m, const char* name)
{
    int i;
    for (i = 0; i < m->nparams; i++)
        if (strcmp(m->params[i], name) == 0)
            return i;
    return -1;
}

/* spell a token (approximate for pasted/stringized output) */
static void tok_text(Tok* t, Buf* out)
{
    int i;
    if (t->ty == TK_ID) {
        bemit(out, t->name, strlen(t->name));
    } else if (t->ty == TK_NUM) {
        u32 v = t->val;
        char nb[16];
        int n = 0;
        do {
            nb[n++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        for (i = n - 1; i >= 0; i--)
            bemit1(out, nb[i]);
    } else if (t->ty == TK_PUNCT) {
        bemit(out, t->name, strlen(t->name));
    } else {
        bemit1(out, '?');
    }
}

static int tok_is_id_or_num(Tok* t)
{
    return t->ty == TK_ID || t->ty == TK_NUM;
}

/* paste two tokens by spelling and re-lexing (single token result) */
static void paste_toks(Tok* a, Tok* b, Tok* out)
{
    char buf[128];
    Buf txt;
    const char* s;
    txt.data = (u8*)buf;
    txt.len = 0;
    txt.cap = 128;
    tok_text(a, &txt);
    tok_text(b, &txt);
    buf[txt.len] = 0;
    s = buf;
    if ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') ||
        s[0] == '_') {
        char nm[128];
        int k = 0;
        while (s[k] && ((s[k] >= 'a' && s[k] <= 'z') ||
                        (s[k] >= 'A' && s[k] <= 'Z') || s[k] == '_' ||
                        (s[k] >= '0' && s[k] <= '9')) && k < 126)
            nm[k] = s[k], k++;
        nm[k] = 0;
        out->ty = TK_ID;
        out->name = astrdup(nm);
        out->val = 0;
        out->slen = 0;
        out->line = 0;
        return;
    }
    if (s[0] >= '0' && s[0] <= '9') {
        u32 v = 0;
        int k = 0;
        while (s[k] >= '0' && s[k] <= '9')
            v = v * 10 + (s[k++] - '0');
        out->ty = TK_NUM;
        out->val = v;
        out->name = 0;
        out->slen = 0;
        out->line = 0;
        return;
    }
    out->ty = TK_PUNCT;
    out->name = astrdup(buf);
    out->val = 0;
    out->slen = 0;
    out->line = 0;
}

/* stringize a parameter's token run into a TK_STR */
static void stringize_args(Tok* args, int n, Tok* out)
{
    Buf txt;
    int i;
    char* s;
    txt.data = (u8*)aalloc(512);
    txt.len = 0;
    txt.cap = 512;
    bemit1(&txt, '"');
    for (i = 0; i < n; i++) {
        if (i > 0 && tok_is_id_or_num(&args[i - 1]) && tok_is_id_or_num(&args[i]))
            bemit1(&txt, ' ');
        tok_text(&args[i], &txt);
    }
    bemit1(&txt, '"');
    s = (char*)aalloc(txt.len + 1);
    memcpy(s, txt.data, txt.len);
    s[txt.len] = 0;
    out->ty = TK_STR;
    out->slen = txt.len - 2;
    out->name = s;
    out->val = 0;
    out->line = 0;
}

/* substitute macro parameters into a token run (body -> out) */
static void subst_macro(Macro* m, Tok* from, int nfrom,
                        Tok** args, int* argn, Buf* out)
{
    int i;
    for (i = 0; i < nfrom; i++) {
        Tok t = from[i];
        if (t.ty == TK_PUNCT && strcmp(t.name, "##") == 0) {
            if (i + 1 < nfrom) {
                Tok next = from[i + 1];
                Tok ntok;
                Tok* prev = 0;
                if (next.ty == TK_ID && m->is_func) {
                    int pj = find_param(m, next.name);
                    if (pj >= 0 && argn && argn[pj] > 0)
                        next = args[pj][0];
                }
                if (out->len >= (int)sizeof(Tok)) {
                    prev = &((Tok*)(out->data))[out->len / (int)sizeof(Tok) - 1];
                    if (prev->ty != TK_PUNCT || strcmp(prev->name, "#") != 0) {
                        paste_toks(prev, &next, &ntok);
                        out->len -= (int)sizeof(Tok);
                        bemit(out, &ntok, sizeof(Tok));
                    } else {
                        bemit(out, &next, sizeof(Tok));
                    }
                } else {
                    bemit(out, &next, sizeof(Tok));
                }
                i++;
            }
            continue;
        }
        if (t.ty == TK_PUNCT && strcmp(t.name, "#") == 0 && i + 1 < nfrom &&
            from[i + 1].ty == TK_ID && m->is_func) {
            int pj = find_param(m, from[i + 1].name);
            if (pj >= 0) {
                Tok st;
                stringize_args(args ? args[pj] : empty_run,
                               args ? argn[pj] : 0, &st);
                bemit(out, &st, sizeof(Tok));
                i++;
                continue;
            }
        }
        if (t.ty == TK_ID && m->is_func) {
            int pj = find_param(m, t.name);
            if (pj >= 0) {
                int k;
                Tok* run = (args && argn[pj]) ? args[pj] : empty_run;
                int rn = (args && argn[pj]) ? argn[pj] : 0;
                for (k = 0; k < rn; k++)
                    bemit(out, &run[k], sizeof(Tok));
                continue;
            }
        }
        bemit(out, &t, sizeof(Tok));
    }
}

/* collect macro call arguments from the live stream; the '(' was consumed */
static void collect_args(int want, Tok** argp, int* argn)
{
    int nargs = 0;
    int depth = 0;
    for (;;) {
        Tok t;
        int n = 0;
        Tok* run = (Tok*)aalloc(sizeof(Tok) * 256);
        for (;;) {
            InBuf* b = cb();
            int nl;
            int hash;
            if (b->pos >= b->len) {
                printf("vcc: unterminated macro call\n");
                exit(1);
            }
            if (!ws_nl(b, &nl)) {
                eat_newline(b);
                continue;
            }
            if (!lex_next_b(b, &t, &hash)) {
                printf("vcc: unterminated macro call\n");
                exit(1);
            }
            if (t.ty == TK_PUNCT) {
                if (strcmp(t.name, "(") == 0 || strcmp(t.name, "[") == 0 ||
                    strcmp(t.name, "{") == 0)
                    depth++;
                else if (strcmp(t.name, ")") == 0) {
                    if (depth == 0) {
                        argp[nargs] = run;
                        argn[nargs] = n;
                        if (++nargs >= want)
                            return;
                        continue;
                    }
                    depth--;
                } else if (strcmp(t.name, ",") == 0 && depth == 0) {
                    argp[nargs] = run;
                    argn[nargs] = n;
                    nargs++;
                    break;
                }
            }
            if (n < 255)
                run[n++] = t;
        }
    }
}

/* splice a run of tokens next to be emitted; registers the region end
 * of macro `cur` (for the re-expansion guard). */
static void splice_pending(Tok* run, int n, const char* cur)
{
    int live = pend_n - pend_r;
    int i;
    if (pend_r > 0 && live > 0) {
        vmemmove(pend, pend + pend_r, sizeof(Tok) * live);
        pend_r = 0;
    }
    if (pend_n + n - pend_r > PMAX) {
        printf("vcc: macro expansion overflow\n");
        exit(1);
    }
    if (live > 0)
        vmemmove(pend + n, pend, sizeof(Tok) * live);
    memcpy(pend, run, sizeof(Tok) * n);
    pend_n = n + live;
    pend_r = 0;
    if (cur && n_open < 15) {
        for (i = n_open - 1; i >= 0; i--)
            open_end[i] += n; /* shift region boundaries */
        open_macro[n_open] = cur;
        open_end[n_open] = pend_n;
        n_open++;
    }
}

static void pop_closed(void)
{
    while (n_open > 0 && open_end[n_open - 1] <= pend_r)
        n_open--;
}

/* if t is an invocable macro, collect/expand and splice; returns 1. */
static int emit_token(Tok* t)
{
    Macro* m;
    if (t->ty != TK_ID)
        return 0;
    m = mlookup(t->name);
    if (!m || m->undef || pushing(t->name))
        return 0;
    if (m->is_func) {
        InBuf* b = cb();
        int nl;
        Tok t2;
        int hash;
        if (!ws_nl(b, &nl) || nl)
            return 0;
        if (!lex_next_b(b, &t2, &hash) || t2.ty != TK_PUNCT ||
            strcmp(t2.name, "(") != 0)
            return 0;
        {
            Tok* argp[8];
            int argn[8];
            Buf outb;
            int i;
            for (i = 0; i < 8; i++)
                argp[i] = empty_run, argn[i] = 0;
            collect_args(m->nparams, argp, argn);
            outb.data = (u8*)aalloc(sizeof(Tok) * 2048);
            outb.len = 0;
            outb.cap = sizeof(Tok) * 2048;
            subst_macro(m, m->body, m->nbody, argp, argn, &outb);
            splice_pending((Tok*)outb.data, outb.len / (int)sizeof(Tok), m->name);
        }
        return 1;
    }
    {
        Buf outb;
        outb.data = (u8*)aalloc(sizeof(Tok) * 2048);
        outb.len = 0;
        outb.cap = sizeof(Tok) * 2048;
        subst_macro(m, m->body, m->nbody, 0, 0, &outb);
        splice_pending((Tok*)outb.data, outb.len / (int)sizeof(Tok), m->name);
        return 1;
    }
}

/* ---- directives ---- */

static void do_define_body(Tok* line, int n)
{
    char* name;
    Macro* m;
    int i;
    char** params;
    int np;
    Tok* body;
    if (n < 1 || line[0].ty != TK_ID) {
        printf("vcc: bad #define\n");
        exit(1);
    }
    name = line[0].name;
    params = 0;
    np = 0;
    if (n >= 2 && line[1].ty == TK_PUNCT && line[1].name[0] == '(' &&
        line[1].name[1] == 0) {
        /* function-like; params until ')' */
        params = (char**)aalloc(sizeof(char*) * 8);
        i = 2;
        while (i < n && !(line[i].ty == TK_PUNCT && line[i].name[0] == ')' &&
                          line[i].name[1] == 0)) {
            if (line[i].ty == TK_PUNCT && line[i].name[0] == '(' &&
                line[i].name[1] == 0)
                np = -100; /* mark bad, e.g. empty param list -> error */
            if (line[i].ty == TK_ID)
                params[np++] = line[i].name;
            i++;
        }
        if (np < 0)
            np = 0;
        i++; /* skip ')' */
    } else {
        i = 1;
    }
    body = (Tok*)aalloc(sizeof(Tok) * (n - i) + sizeof(Tok));
    {
        int nb = 0;
        for (; i < n; i++)
            body[nb++] = line[i];
        m = (Macro*)aalloc(sizeof(Macro));
        m->name = astrdup(name);
        m->body = body;
        m->nbody = nb;
        m->params = params;
        m->nparams = np;
        m->is_func = params != 0;
        m->undef = 0;
    }
    mdefine(name, m->body, m->nbody, m->params, m->nparams, m->is_func);
}

static void push_source(const char* text, int len, char* dir)
{
    push_buf(text, len, dir);
}

/* the include resolver (fixup for find_include's extra arg) */
static char* find_include2(const char* name, int is_angle, int fatal,
                           int* outlen, const char* dir);

/* master token collector: fills the global tok[] vector. */
static void pp_run(const char* text, int len, char* dir)
{
    int at_bol = 1;
    push_source(text, len, dir);
    for (;;) {
        Tok t;
        InBuf* b;
        int nl;
        int hash;
        if (pend_r < pend_n) {
            Tok pt = pend[pend_r++];
            pop_closed();
            if (emit_token(&pt) == 0) {
                if (pt.ty == TK_PUNCT && pt.name[0] == '#' && pt.name[1] == 0) {
                    printf("vcc: stray '#' at line %d\n", pt.line);
                    exit(1);
                }
                push_tok(&pt);
            }
            at_bol = 0;
            continue;
        }
        if (nbuf == 0)
            break;
        b = cb();
        if (b->pos >= b->len) {
            nbuf--;
            at_bol = 1;
            continue;
        }
        if (!ws_nl(b, &nl)) {
            eat_newline(b);
            at_bol = 1;
            continue;
        }
        if (!lex_next_b(b, &t, &hash)) {
            nbuf--;
            at_bol = 1;
            continue;
        }
        if (t.ty == TK_PUNCT && hash && at_bol) {
            Tok line[256];
            int n;
            n = read_line_toks(line, 256);
            at_bol = 0;
            if (n > 0 && line[0].ty == TK_ID) {
                const char* dn = line[0].name;
                if (strcmp(dn, "define") == 0) {
                    if (!skipping_now())
                        do_define_body(line + 1, n - 1);
                } else if (strcmp(dn, "undef") == 0) {
                    if (!skipping_now() && n >= 2 && line[1].ty == TK_ID)
                        mundef(line[1].name);
                } else if (strcmp(dn, "include") == 0) {
                    char* inc = 0;
                    int is_angle = 0;
                    if (n >= 2 && line[1].ty == TK_STR) {
                        inc = line[1].name;
                    } else if (n >= 3 && line[1].ty == TK_PUNCT &&
                               line[1].name[0] == '<') {
                        Buf nb;
                        int k;
                        nb.data = (u8*)aalloc(64);
                        nb.len = 0;
                        nb.cap = 64;
                        is_angle = 1;
                        for (k = 2; k < n && !(line[k].ty == TK_PUNCT &&
                                                line[k].name[0] == '>'); k++)
                            tok_text(&line[k], &nb);
                        inc = (char*)aalloc(nb.len + 1);
                        memcpy(inc, nb.data, nb.len);
                        inc[nb.len] = 0;
                    }
                    if (!skipping_now() && inc) {
                        char* data;
                        int dlen;
                        data = find_include2(inc, is_angle, 1, &dlen, b->dir);
                        if (data)
                            push_source(data, dlen, b->dir);
                    }
                } else if (strcmp(dn, "ifdef") == 0 || strcmp(dn, "ifndef") == 0) {
                    int parent = skipping_now();
                    int condv = 0;
                    if (!parent && n >= 2 && line[1].ty == TK_ID) {
                        Macro* mm = mlookup(line[1].name);
                        condv = mm && !mm->undef;
                        if (strcmp(dn, "ifndef") == 0)
                            condv = !condv;
                    } else if (parent && n >= 2 && line[1].ty == TK_ID) {
                        condv = 0;
                    } else if (!parent) {
                        condv = 1; /* ifndef of missing -> true */
                        if (strcmp(dn, "ifdef") == 0)
                            condv = 0;
                    }
                    ndepth++;
                    if (parent) {
                        cond_skip[ndepth] = 1;
                        cond_taken[ndepth] = 1;
                    } else {
                        cond_skip[ndepth] = condv ? 0 : 1;
                        cond_taken[ndepth] = condv ? 1 : 0;
                    }
                } else if (strcmp(dn, "if") == 0) {
                    int parent = skipping_now();
                    int condv = 0;
                    if (!parent && n >= 2) {
                        int rr = 0;
                        condv = pp_expr(line + 1, &((int){n - 1}), &rr);
                        condv = condv ? 1 : 0;
                    }
                    ndepth++;
                    if (parent) {
                        cond_skip[ndepth] = 1;
                        cond_taken[ndepth] = 1;
                    } else {
                        cond_skip[ndepth] = condv ? 0 : 1;
                        cond_taken[ndepth] = condv ? 1 : 0;
                    }
                } else if (strcmp(dn, "elif") == 0) {
                    if (ndepth < 0) {
                        printf("vcc: #elif without #if\n");
                        exit(1);
                    }
                    if (cond_skip[ndepth]) {
                        /* skipped by parent or taken: do nothing */
                    } else if (cond_taken[ndepth]) {
                        cond_skip[ndepth] = 1;
                    } else if (n >= 2) {
                        int rr = 0;
                        int condv = pp_expr(line + 1, &((int){n - 1}), &rr);
                        condv = condv ? 1 : 0;
                        cond_skip[ndepth] = condv ? 0 : 1;
                        cond_taken[ndepth] = condv ? 1 : 0;
                    }
                } else if (strcmp(dn, "else") == 0) {
                    if (ndepth < 0) {
                        printf("vcc: #else without #if\n");
                        exit(1);
                    }
                    if (!cond_skip[ndepth]) {
                        cond_skip[ndepth] = cond_taken[ndepth] ? 1 : 0;
                        cond_taken[ndepth] = 1;
                    }
                } else if (strcmp(dn, "endif") == 0) {
                    if (ndepth < 0) {
                        printf("vcc: stray #endif\n");
                        exit(1);
                    }
                    ndepth--;
                } else if (strcmp(dn, "error") == 0) {
                    if (!skipping_now()) {
                        printf("vcc: #error at line %d\n", line[0].line);
                        exit(1);
                    }
                } else {
                    /* unknown directives (line, pragma, ...) ignored */
                }
            }
            continue;
        }
        if (skipping_now())
            continue;
        if (emit_token(&t) == 0)
            push_tok(&t);
        at_bol = 0;
    }
    {
        Tok eof;
        eof.ty = TK_EOF;
        eof.val = 0;
        eof.slen = 0;
        eof.name = 0;
        eof.line = 0;
        push_tok(&eof);
    }
}

/* ================================================================== */
/* types                                                               */
/* ================================================================== */

enum {
    T_CHAR = 0, T_UCHAR, T_SHORT, T_USHORT, T_INT, T_UINT,
    T_PTR, T_ARRAY, T_STRUCT, T_UNION, T_VOID, T_FUNC, T_ENUM
};

typedef struct Type Type;
typedef struct Member Member;
typedef struct Var Var;

struct Member {
    char* name;
    Type* ty;
    int off;
    Member* next;
};

struct Type {
    int kind;
    int size;
    int align;
    int sign;          /* 1 for unsigned int/char/short */
    Type* ref;         /* element/return for ptr/array/func */
    Member* members;   /* struct/union */
    int n;             /* array count, enum last value, func param count */
    char* tag;
    char** pnames;     /* function parameter names */
};

static Type* ty_int;
static Type* ty_uint;
static Type* ty_char;
static Type* ty_uchar;
static Type* ty_short;
static Type* ty_ushort;
static Type* ty_void;

static Type* T(int kind, int size, int align)
{
    Type* t = (Type*)aalloc(sizeof(Type));
    t->kind = kind;
    t->size = size;
    t->align = align;
    return t;
}

static void type_init(void);

static Type* ptr_to(Type* t)
{
    Type* p = T(T_PTR, 4, 4);
    p->ref = t;
    return p;
}

/* create the primitive type descriptors (arena-recreated per file) */
static void type_init(void)
{
    ty_int = T(T_INT, 4, 4);
    ty_uint = T(T_UINT, 4, 4);
    ty_char = T(T_CHAR, 1, 1);
    ty_uchar = T(T_UCHAR, 1, 1);
    ty_short = T(T_SHORT, 2, 2);
    ty_ushort = T(T_USHORT, 2, 2);
    ty_void = T(T_VOID, 0, 1);
}

static Type* arr_of(Type* t, int n)
{
    Type* a = T(T_ARRAY, t->size * n, t->align);
    a->ref = t;
    a->n = n;
    return a;
}

static int type_size(Type* t)
{
    return t ? t->size : 4;
}

static int array_len(Type* t)
{
    if (t->kind == T_ARRAY)
        return t->n;
    return -1;
}

/* ---- struct tags ---- */

#define MAXTAG 64
static Type* tags[MAXTAG];
static char* tagnames[MAXTAG];
static int ntags;

static Type* find_tag(const char* name, int is_struct)
{
    int i;
    (void)is_struct;
    for (i = 0; i < ntags; i++)
        if (strcmp(tagnames[i], name) == 0)
            return tags[i];
    return 0;
}

static void add_tag(const char* name, Type* t)
{
    int i;
    for (i = 0; i < ntags; i++)
        if (strcmp(tagnames[i], name) == 0) {
            tags[i] = t;
            return;
        }
    if (ntags >= MAXTAG) {
        printf("vcc: too many tags\n");
        exit(1);
    }
    tagnames[ntags] = (char*)name;
    tags[ntags] = t;
    ntags++;
}

/* ---- typedef names ---- */

#define MAXTDEF 64
static Type* tdefs[MAXTDEF];
static char* tdef_names[MAXTDEF];
static int ntdefs;

static Type* find_tdef(const char* name)
{
    int i;
    for (i = 0; i < ntdefs; i++)
        if (strcmp(tdef_names[i], name) == 0)
            return tdefs[i];
    return 0;
}

static void add_tdef(const char* name, Type* t)
{
    int i;
    for (i = 0; i < ntdefs; i++)
        if (strcmp(tdef_names[i], name) == 0) {
            tdefs[i] = t;
            return;
        }
    if (ntdefs >= MAXTDEF) {
        printf("vcc: too many typedefs\n");
        exit(1);
    }
    tdef_names[ntdefs] = (char*)name;
    tdefs[ntdefs] = t;
    ntdefs++;
}

/* ---- enum constants as macros ---- */
static void add_enum_const(const char* name, int v)
{
    Tok t;
    t.ty = TK_NUM;
    t.val = v;
    t.slen = 0;
    t.name = 0;
    t.line = 0;
    mdefine(name, &t, 1, 0, 0, 0);
}

/* ================================================================== */
/* variables / symbols                                                */
/* ================================================================== */

struct Var {
    char* name;
    Type* ty;
    int storage;     /* 0 local auto, 1 static, 2 extern, 3 param, 4 global */
    int off;         /* stack offset (neg locals, pos params) */
    int sym;         /* object symbol index or -1 */
    Var* next;
};

static Var* scope;       /* active local scope */

static Var* find_local(const char* name)
{
    Var* v = scope;
    while (v) {
        if (strcmp(v->name, name) == 0)
            return v;
        v = v->next;
    }
    return 0;
}

/* ---- object symbol table (single translation unit) ---- */

#define NSYM 8192
typedef struct {
    char* name;
    u32 val;
    u32 size;
    int bind;        /* 0 local, 1 global */
    int type;        /* 0 notype, 1 object, 2 func */
    int shndx;       /* 0 und,1 text,2 rodata,3 data,4 bss,0xfff2 common */
    int defined;
} OSym;

static OSym osym[NSYM];
static int nosym;

static OSym* osym_lookup(const char* name, int create)
{
    int i;
    for (i = 0; i < nosym; i++)
        if (strcmp(osym[i].name, name) == 0)
            return &osym[i];
    if (!create)
        return 0;
    if (nosym >= NSYM) {
        printf("vcc: too many symbols\n");
        exit(1);
    }
    osym[nosym].name = astrdup(name);
    osym[nosym].shndx = 0;
    osym[nosym].defined = 0;
    osym[nosym].type = 0;
    osym[nosym].bind = 1;
    nosym++;
    return &osym[nosym - 1];
}

/* ---- relocations ---- */

typedef struct {
    int sec;         /* 1 text, 2 rodata, 3 data */
    int off;
    int sym;         /* index into osym[] */
    int type;        /* 1=R_386_32 2=R_386_PC32 */
} ORel;

static ORel orel[16384];
static int norel;

static void orel_add(int sec, int off, int sym, int type)
{
    if (norel >= 16384) {
        printf("vcc: too many relocations\n");
        exit(1);
    }
    orel[norel].sec = sec;
    orel[norel].off = off;
    orel[norel].sym = sym;
    orel[norel].type = type;
    norel++;
}

/* ---- sections (bytes) ---- */

static Buf secs[3];    /* 0 text, 1 rodata, 2 data */
static int bss_size;
static int bss_align = 1;

static void sect_align(int sec, int a)
{
    while (secs[sec].len & (a - 1))
        bemit1(&secs[sec], 0);
}

/* label counter */
static int label_count;

static char* new_label(void)
{
    char b[16];
    int n = 0;
    int v = label_count++;
    b[n++] = 'L';
    b[n++] = (char)('0' + (v / 1000) % 10);
    b[n++] = (char)('0' + (v / 100) % 10);
    b[n++] = (char)('0' + (v / 10) % 10);
    b[n++] = (char)('0' + (v % 10));
    b[n] = 0;
    return astrdup(b);
}

/* ================================================================== */
/* instruction helpers                                                 */
/* ================================================================== */

static void c1(int op)
{
    bemit1(&secs[0], op);
}
static void c2(int a, int b)
{
    bemit1(&secs[0], a);
    bemit1(&secs[0], b);
}
static void c4(int op)
{
    bemit4(&secs[0], op);
}
static void push_eax(void)
{
    c1(0x50);
}
static void pop_eax(void)
{
    c1(0x58);
}
static void push_ecx(void)
{
    c1(0x51);
}
static void pop_ecx(void)
{
    c1(0x59);
}
static void pop_edx(void)
{
    c1(0x5A);
}
static void mov_ecx_eax(void)
{
    c2(0x89, 0xC1);
}
static void mov_eax_ecx(void)
{
    c2(0x89, 0xC8);
}
static void add_eax_imm(int v)
{
    c1(0x05);
    c4(v);
}
static void add_esp(int v)
{
    c1(0x83);
    c1(0xC4);
    c1(v);
}
static void imul_eax_imm(int v)
{
    c1(0x69);
    c1(0xC0);
    c4(v);
}
static void cdq_(void)
{
    c1(0x99);
}
static void div_ecx(void)
{
    c2(0xF7, 0xF9);
}
static void idiv_ecx(void)
{
    c2(0xF7, 0xFB);
}
static void xor_eax(void)
{
    c2(0x31, 0xC0);
}
static void test_eax(void)
{
    c2(0x85, 0xC0);
}
static void op_neg(void)
{
    c2(0xF7, 0xD8);
}
static void op_not(void)
{
    c2(0xF7, 0xD0);
}
static void op_and_ecx(void)
{
    c2(0x21, 0xC8);
}
static void op_or_ecx(void)
{
    c2(0x09, 0xC8);
}
static void op_xor_ecx(void)
{
    c2(0x31, 0xC8);
}
static void op_shl_cl(void)
{
    c2(0xD3, 0xE0);
}
static void op_shr_cl(void)
{
    c2(0xD3, 0xE8);
}
static void op_sar_cl(void)
{
    c2(0xD3, 0xF8);
}
static void op_movzx_eax_al(void)
{
    c2(0x0F, 0xB6);
    c1(0xC0);
}

/* reloc helper: jcc/call/jmp into text */
static void rel_branch(int sym, int addend)
{
    c4(addend);
    orel_add(1, secs[0].len - 4, sym, 2);
}

static void op_jmp(int sym)
{
    c1(0xE9);
    rel_branch(sym, -4);
}
static void op_jz_label(int sym)
{
    c2(0x0F, 0x84);
    rel_branch(sym, -4);
}
static void op_jnz_label(int sym)
{
    c2(0x0F, 0x85);
    rel_branch(sym, -4);
}

/* mov eax,[eax] with type width / sign */
static void op_load(Type* t)
{
    int k = t->kind;
    if (t->kind == T_PTR || k == T_INT || k == T_UINT || k == T_ENUM ||
        t->size >= 4) {
        c2(0x8B, 0x00);
    } else if (t->size == 2) {
        if (t->sign) {
            c2(0x0F, 0xB7);
            c1(0x00);
        } else {
            c2(0x0F, 0xBF);
            c1(0x00);
        }
    } else {
        if (t->sign) {
            c2(0x0F, 0xB6);
            c1(0x00);
        } else {
            c2(0x0F, 0xBE);
            c1(0x00);
        }
    }
}

/* mov from eax (value) into [ecx] (address) with type width */
static void op_store(Type* t)
{
    int sz = type_size(t);
    if (sz >= 4) {
        c2(0x89, 0x01);
    } else if (sz == 2) {
        c1(0x66);
        c2(0x89, 0x01);
    } else {
        c2(0x88, 0x01);
    }
}

/* push [ebp+off] as an int value into eax (for our loads we do
 * lea ebp+off then load) -- placeholder unused */

/* forward declarations */
static int pp_expr(Tok* t, int* n, int* read);
static void tskip(void);
static int str_const(Tok* t);
static Type* expr(void);
static int const_expr(void);
static Type* decl_spec(void);
static Type* parse_decl(Type* base, char** name);
static Type* apply_suffixes(Type* t);
static Type* wrap_ptr(Type* t, int k);
static void expect_after(void);
static void define_function(const char* name, Type* ty);
static int new_label_sym(void);
static void define_label(int sym);
static void combine_bin(int op, Type* lt, Type* rt);
static Type* result_type(Type* lt, Type* rt);
static void op_load_width(Type* t);
static void op_load(Type* t);
static Member* find_member(Type* t, const char* name);
static Var* find_global(const char* name);
static void define_function(const char* name, Type* ty);
static Type* expr_unary(void);
static void incdec(Type* t, int delta, int post);
static int lv_has_val;

/* ================================================================== */
/* expression parser + codegen                                        */
/* result of expr() is always in EAX; expr() returns its type.        */
/* ================================================================== */

static Type* decl_spec(void);
static Type* parse_decl(Type* base, char** name);
static void stmt(void);
static Type* expr(void);

static int func_call_ahead(void);
static int peek_kw(const char* k);
static int peek_colon(void);
static int lookup_label(const char* name);
static void push_lvl(int kind, int brk, int con);
static void pop_lvl(void);
static void add_case(int v);
static Type* expr_bin(int minpred);
static Var* decl_var(Type* base);
static Var* push_scope_local(void);
static void pop_scope_back(Var* old);
static void branch_jz(int sym);
static void branch_jnz(int sym);
static void local_decl(void);
static void store_dx(Type* t);
static void parse_file(void);

static int const_expr(void);
static int frame_used;

/* push eax gives the value; the address helpers put address in eax */
static void lea_local(Var* v)
{
    /* lea eax, [ebp + off] */
    if (v->storage == 3) {
        int o = v->off; /* positive */
        c1(0x8D);
        c1(0x85);
        c4(o);
    } else {
        int o = v->off; /* negative */
        c1(0x8D);
        c1(0x85);
        c4(o);
    }
}

static void lea_global(Var* v)
{
    /* lea eax, [sym] */
    c1(0x8D);
    c1(0x05);
    c4(0);
    orel_add(1, secs[0].len - 4, v->sym, 1);
}

/* assign a stack offset (and size) to a local var */
static void local_alloc(Var* v)
{
    int sz = type_size(v->ty);
    int al = v->ty->align;
    if (al > 4)
        al = 4;
    frame_used += sz;
    while (frame_used & (al - 1))
        frame_used++;
    v->off = -frame_used;
}

/* forward decl for the postfix/lval helpers */
static Type* expr_assign(void);
static Type* lval_addr(void);

/* parse a primary: constants, string literals, ids, parens, cast,
 * sizeof; the postfix chain is handled in lval_addr()/call path. */
static Type* primary(void)
{
    Tok* t = pcur();
    if (t->ty == TK_NUM) {
        tskip();
        c1(0xB8);
        c4(t->val);
        return ty_int;
    }
    if (t->ty == TK_STR) {
        int sym = str_const(t);
        tskip();
        c1(0xB8);
        c4(0);
        orel_add(1, secs[0].len - 4, sym, 1);
        return ptr_to(ty_char);
    }
    if (t->ty == TK_ID) {
        Var* v;
        if (strcmp(t->name, "sizeof") == 0) {
            Type* t2;
            int sz;
            tskip();
            if (!is_punct("(")) {
                printf("vcc: sizeof needs ( at line %d\n", t->line);
                exit(1);
            }
            tskip();
            t2 = decl_spec();
            if (!t2)
                t2 = ty_int;
            {
                char* nm = 0;
                parse_decl(t2, &nm);
            }
            if (!is_punct(")")) {
                printf("vcc: sizeof: expected )\n");
                exit(1);
            }
            tskip();
            sz = t2 ? t2->size : 4;
            c1(0xB8);
            c4(sz);
            return ty_uint;
        }
        v = find_local(t->name);
        if (!v)
            v = find_global(t->name);
        if (v && v->ty->kind == T_FUNC) {
            /* function designator or direct call */
            int nargs = 0;
            int argpos[16];
            int i;
            if (func_call_ahead()) {
                int sp = 0;
                tskip(); /* name */
                tskip(); /* ( */
                if (!is_punct(")")) {
                    for (;;) {
                        argpos[nargs] = tpos;
                        expr_assign();
                        nargs++;
                        if (nargs >= 16) {
                            printf("vcc: too many args\n");
                            exit(1);
                        }
                        if (!is_punct(","))
                            break;
                        tskip();
                    }
                }
                if (!is_punct(")")) {
                    printf("vcc: expected ) in call\n");
                    exit(1);
                }
                tskip();
                {
                    int afterparen = tpos;
                    for (i = nargs - 1; i >= 0; i--) {
                        tpos = argpos[i];
                        expr_assign();
                        push_eax();
                        sp++;
                    }
                    tpos = afterparen;
                }
                lv_has_val = 1;
                c1(0xE8); /* call */
                c4(-4);
                orel_add(1, secs[0].len - 4, v->sym, 2);
                if (sp)
                    add_esp(sp * 4);
                return ty_int;
            }
            c1(0xB8);
            c4(0);
            orel_add(1, secs[0].len - 4, v->sym, 1);
            tskip();
            return v->ty;
        }
        if (!v) {
            /* external function (implicit declaration), or a defined global
               function that lives in the OSym table (no Var) */
            OSym* s;
            if (func_call_ahead()) {
                int nargs = 0;
                int argpos[16];
                int sp = 0;
                int i;
                s = osym_lookup(t->name, 0);
                if (!s) {
                    s = osym_lookup(t->name, 1);
                    s->type = 2;
                    s->shndx = 0;
                    s->defined = 0;
                }
                tskip();
                tskip();
                if (!is_punct(")")) {
                    for (;;) {
                        argpos[nargs] = tpos;
                        expr_assign();
                        nargs++;
                        if (nargs >= 16) {
                            printf("vcc: too many args\n");
                            exit(1);
                        }
                        if (!is_punct(","))
                            break;
                        tskip();
                    }
                }
                if (!is_punct(")")) {
                    printf("vcc: expected ) in call\n");
                    exit(1);
                }
                tskip();
                {
                    int afterparen = tpos;
                    for (i = nargs - 1; i >= 0; i--) {
                        tpos = argpos[i];
                        expr_assign();
                        push_eax();
                        sp++;
                    }
                    tpos = afterparen;
                }
                lv_has_val = 1;
                c1(0xE8);
                c4(-4);
                orel_add(1, secs[0].len - 4, (int)(s - osym), 2);
                if (sp)
                    add_esp(sp * 4);
                return ty_int;
            }
            printf("vcc: undefined identifier '%s' at line %d\n", t->name,
                   t->line);
            exit(1);
        }
        tskip();
        if (v->storage == 0 || v->storage == 3) {
            lea_local(v);
        } else {
            lea_global(v);
        }
        return v->ty;
    }
    if (t->ty == TK_PUNCT && t->name[0] == '(' && t->name[1] == 0) {
        Type* t2;
        tskip();
        if (is_punct(")")) {
            printf("vcc: empty ( at line %d\n", t->line);
            exit(1);
        }
        t2 = decl_spec();
        if (t2) {
            /* cast */
            char* nm = 0;
            Type* ct = parse_decl(t2, &nm);
            if (!is_punct(")")) {
                printf("vcc: cast: expected ) at line %d\n", t->line);
                exit(1);
            }
            tskip();
            t2 = (ct && ct->kind == T_ARRAY) ? ptr_to(ct->ref) : ct;
            {
                Type* v = expr_unary();
                (void)v;
            }
            return t2;
        }
        {
            Type* v = expr();
            if (!is_punct(")")) {
                printf("vcc: expected ) at line %d\n", t->line);
                exit(1);
            }
            tskip();
            return v;
        }
    }
    if (t->ty == TK_PUNCT) {
        /* unary + - ! ~ are handled in expr_unary */
        ;
    }
    printf("vcc: unexpected token \"%s\" at line %d\n", t->name, t->line);
    exit(1);
}


static int sym_name;

/* string literal -> a rodata symbol; emits the bytes; returns sym index */
static int str_const(Tok* t)
{
    char* name;
    int sym;
    OSym* s;
    name = astrdup("S0000");
    sym_name++;
    name[2] = (char)('0' + (sym_name / 1000) % 10);
    name[3] = (char)('0' + (sym_name / 100) % 10);
    name[4] = (char)('0' + (sym_name / 10) % 10);
    name[5] = (char)('0' + (sym_name % 10));
    sect_align(1, 4);
    sym = osym_lookup(name, 1) - osym;
    sect_align(1, 4);
    {
        OSym* o = osym_lookup(name, 0);
        o->shndx = 2;
        o->defined = 1;
        o->bind = 0;
        o->type = 1;
        o->val = secs[1].len;
        o->size = t->slen + 1;
    }
    bemit(&secs[1], t->name, t->slen + 1);
    return sym;
}

/* value of lvalue: parse primary+postfix; address ends up in EAX.
 * returns the lvalue's type. (name lookup inside primary puts address;
 * postfix keeps address arithmetic.) */
static Type* lval_addr(void)
{
    Type* t = primary();
    for (;;) {
        Tok* p = pcur();
        if (p->ty == TK_PUNCT) {
            const char* n = p->name;
            if (n[0] == '[' && n[1] == 0) {
                Type* elem;
                tskip();
                if (t->kind == T_PTR) {
                    elem = t->ref;
                } else if (t->kind == T_ARRAY) {
                    elem = t->ref;
                } else {
                    printf("vcc: [] on non-array at line %d\n", p->line);
                    exit(1);
                }
                push_eax();                 /* save base address */
                {
                    Type* i = expr();
                    (void)i;
                }
                imul_eax_imm(type_size(elem));  /* eax = index*size */
                mov_ecx_eax();                  /* ecx = index*size */
                pop_eax();                      /* eax = base */
                c2(0x01, 0xC8);                 /* add eax, ecx */
                if (!is_punct("]")) {
                    printf("vcc: expected ] at line %d\n", p->line);
                    exit(1);
                }
                tskip();
                /* eax = base + idx*size */
                t = elem;
                continue;
            }
            if (n[0] == '.' && n[1] == 0) {
                Member* m;
                tskip();
                if (t->kind != T_STRUCT && t->kind != T_UNION) {
                    printf("vcc: . on non-struct at line %d\n", p->line);
                    exit(1);
                }
                m = find_member(t, pcur()->name);
                if (!m) {
                    printf("vcc: no member '%s' at line %d\n", pcur()->name,
                           pcur()->line);
                    exit(1);
                }
                tskip();
                add_eax_imm(m->off);
                t = m->ty;
                continue;
            }
            if (n[0] == '-' && n[1] == '>' && n[2] == 0) {
                Member* m;
                Type* st;
                tskip();
                if (t->kind != T_PTR) {
                    printf("vcc: -> on non-pointer at line %d\n", p->line);
                    exit(1);
                }
                st = t->ref;
                if (st->kind != T_STRUCT && st->kind != T_UNION) {
                    printf("vcc: -> struct expected at line %d\n", p->line);
                    exit(1);
                }
                m = find_member(st, pcur()->name);
                if (!m) {
                    printf("vcc: no member '%s'\n", pcur()->name);
                    exit(1);
                }
                tskip();
                op_load(t);   /* deref the pointer -> struct address */
                add_eax_imm(m->off);
                t = m->ty;
                continue;
            }
            if (n[0] == '(' && n[1] == 0) {
                /* function call: t is func/ptr-to-func */
                int nargs = 0;
                int argpos[16];
                int i;
                if (t->kind == T_FUNC) {
                    /* direct call: symbol already loaded into eax; but
                     * we need the symbol name: peeks primary? We know
                     * this address came from a direct symbol lea; record
                     * the sym via a side array is complex -> instead:
                     * re-scan primary token. Simplest: do NOT emit here;
                     * handled in primary() when directly followed by '('.
                     */
                    printf("vcc: internal: call handling in postfix\n");
                    exit(1);
                }
                tskip();
                if (!is_punct(")")) {
                    for (;;) {
                        argpos[nargs] = tpos;
                        {
                            Type* a = expr_assign();
                            (void)a;
                        }
                        nargs++;
                        if (nargs >= 16) {
                            printf("vcc: too many args\n");
                            exit(1);
                        }
                        if (!is_punct(","))
                            break;
                        tskip();
                    }
                }
                if (!is_punct(")")) {
                    printf("vcc: expected ) in call at line %d\n", p->line);
                    exit(1);
                }
                tskip();
                /* push args right-to-left */
                for (i = nargs - 1; i >= 0; i--) {
                    tpos = argpos[i];
                    {
                        Type* a = expr_assign();
                        (void)a;
                    }
                    push_eax();
                }
                /* target in eax (the function pointer value) */
                if (t->kind == T_PTR) {
                    /* indirect: call eax */
                    c2(0xFF, 0xD0);
                    lv_has_val = 1;
                }
                add_esp(nargs * 4);
                t = ty_int;
                continue;
            }
            if (n[0] == '+' && n[1] == '+' && n[2] == 0) {
                tskip();
                incdec(t, 1, 1);
                lv_has_val = 1;
                continue;
            }
            if (n[0] == '-' && n[1] == '-' && n[2] == 0) {
                tskip();
                incdec(t, -1, 1);
                lv_has_val = 1;
                continue;
            }
        }
        break;
    }
    /* decay arrays to pointers: keep address, return pointer type */
    if (t->kind == T_ARRAY)
        return ptr_to(t->ref);
    return t;
}

/* load the value of the lvalue whose address is in EAX */
static void load_value(Type* t)
{
    if (t->kind == T_ARRAY || t->kind == T_FUNC)
        return;
    op_load(t);
}


/* ------------------------------------------------------------------ */
/* declarator parsing                                                  */
/* ------------------------------------------------------------------ */

static Member* find_member(Type* t, const char* name)
{
    Member* m = t->members;
    while (m) {
        if (strcmp(m->name, name) == 0)
            return m;
        m = m->next;
    }
    return 0;
}

static int is_typespec(Tok* t)
{
    if (t->ty != TK_ID)
        return 0;
    {
        const char* n = t->name;
        if (strcmp(n, "int") == 0 || strcmp(n, "char") == 0 ||
            strcmp(n, "short") == 0 || strcmp(n, "unsigned") == 0 ||
            strcmp(n, "signed") == 0 || strcmp(n, "long") == 0 ||
            strcmp(n, "void") == 0 || strcmp(n, "struct") == 0 ||
            strcmp(n, "union") == 0 || strcmp(n, "enum") == 0 ||
            strcmp(n, "const") == 0 || strcmp(n, "volatile") == 0 ||
            strcmp(n, "static") == 0 || strcmp(n, "extern") == 0 ||
            strcmp(n, "typedef") == 0 || strcmp(n, "register") == 0 ||
            strcmp(n, "auto") == 0)
            return 1;
        return find_tdef(n) != 0;
    }
}

/* base type specifier. returns null if not a type. */
static int isfollow(Tok* t);

static Type* decl_spec(void)
{
    Tok* t = pcur();
    Type* base = 0;
    int sign = 0;
    if (t->ty != TK_ID)
        return 0;
    for (;;) {
        const char* n = t->name;
        if (strcmp(n, "const") == 0 || strcmp(n, "volatile") == 0 ||
            strcmp(n, "auto") == 0 || strcmp(n, "register") == 0) {
            tskip();
        } else if (strcmp(n, "unsigned") == 0) {
            sign = 1;
            base = ty_uint;
            tskip();
        } else if (strcmp(n, "signed") == 0) {
            sign = 0;
            tskip();
        } else if (strcmp(n, "int") == 0) {
            base = sign ? ty_uint : ty_int;
            tskip();
        } else if (strcmp(n, "short") == 0) {
            base = sign ? ty_ushort : ty_short;
            tskip();
        } else if (strcmp(n, "char") == 0) {
            base = sign ? ty_uchar : ty_char;
            tskip();
        } else if (strcmp(n, "long") == 0) {
            base = ty_int;
            tskip();
            if (pcur()->ty == TK_ID && strcmp(pcur()->name, "long") == 0) {
                printf("vcc: long long unsupported at line %d\n", t->line);
                exit(1);
            }
        } else if (strcmp(n, "void") == 0) {
            base = ty_void;
            tskip();
        } else if (strcmp(n, "struct") == 0 || strcmp(n, "union") == 0) {
            int is_struct = strcmp(n, "struct") == 0;
            tskip();
            t = pcur();
            if (t->ty == TK_ID) {
                Type* tg = find_tag(t->name, is_struct);
                tskip();
                if (is_punct("{")) {
                    /* definition: build members */
                    Type* nt = T(is_struct ? T_STRUCT : T_UNION, 0, 1);
                    Member* mlist = 0;
                    int off = 0;
                    int maxalign = 1;
                    nt->tag = astrdup(pcur()->name ? "" : "");
                    nt->tag = astrdup(/* recompute below */ "");
                    if (is_struct) {
                        nt->tag = astrdup((char*)(t->name));
                    } else {
                        nt->tag = astrdup((char*)(t->name));
                    }
                    tskip();
                    while (!is_punct("}")) {
                        Type* mt = decl_spec();
                        char* mn = 0;
                        Type* ft;
                        int cnt;
                        int j;
                        if (!mt)
                            break;
                        ft = parse_decl(mt, &mn);
                        /* allow multiple declarators of same type */
                        for (cnt = 0;; cnt++) {
                            Member* m;
                            if (mn) {
                                m = (Member*)aalloc(sizeof(Member));
                                m->name = mn;
                                m->ty = ft;
                                m->off = off;
                                m->next = mlist;
                                mlist = m;
                                if (ft->align > maxalign)
                                    maxalign = ft->align;
                                off += ft->size;
                            }
                            if (!is_punct(","))
                                break;
                            tskip();
                            ft = parse_decl(mt, &mn);
                        }
                        if (!is_punct(";")) {
                            printf("vcc: expected ; in struct at line %d\n",
                                   pcur()->line);
                            exit(1);
                        }
                        tskip();
                        (void)j;
                    }
                    tskip(); /* } */
                    off = 0;
                    {
                        Member* m2 = mlist;
                        int running = 0;
                        while (m2) {
                            running += m2->ty->size;
                            m2->off = running - m2->ty->size;
                            m2 = m2->next;
                        }
                    }
                    nt->size = off;
                    nt->align = maxalign;
                    if (off & (maxalign - 1))
                        nt->size = (off + maxalign - 1) & ~(maxalign - 1);
                    nt->members = mlist;
                    add_tag(nt->tag, nt);
                    base = nt;
                    return base;
                }
                base = tg;
                if (!base) {
                    printf("vcc: unknown struct tag '%s' at line %d\n",
                           t->name, t->line);
                    exit(1);
                }
            } else if (is_punct("{")) {
                Type* nt = T(is_struct ? T_STRUCT : T_UNION, 0, 1);
                Member* mlist = 0;
                int off = 0;
                int maxalign = 1;
                tskip();
                nt->tag = astrdup("_anon");
                while (!is_punct("}")) {
                    Type* mt = decl_spec();
                    char* mn = 0;
                    Type* ft;
                    if (!mt)
                        break;
                    ft = parse_decl(mt, &mn);
                    for (;;) {
                        if (mn) {
                            Member* m = (Member*)aalloc(sizeof(Member));
                            m->name = mn;
                            m->ty = ft;
                            m->off = 0;
                            m->next = mlist;
                            mlist = m;
                            if (ft->align > maxalign)
                                maxalign = ft->align;
                            off += ft->size;
                        }
                        if (!is_punct(","))
                            break;
                        tskip();
                        ft = parse_decl(mt, &mn);
                    }
                    if (!is_punct(";")) {
                        printf("vcc: expected ; in struct\n");
                        exit(1);
                    }
                    tskip();
                }
                tskip();
                {
                    Member* m2 = mlist;
                    int running = 0;
                    while (m2) {
                        running += m2->ty->size;
                        m2->off = running - m2->ty->size;
                        m2 = m2->next;
                    }
                }
                nt->size = off;
                nt->align = maxalign;
                if (off & (maxalign - 1))
                    nt->size = (off + maxalign - 1) & ~(maxalign - 1);
                nt->members = mlist;
                nt->tag = t->name ? t->name : 0;
                if (nt->tag)
                    add_tag(nt->tag, nt);
                base = nt;
                return base;
            } else {
                printf("vcc: struct needs tag at line %d\n", t->line);
                exit(1);
            }
        } else if (strcmp(n, "enum") == 0) {
            int v = 0;
            tskip();
            t = pcur();
            if (t->ty == TK_ID) {
                tskip();
            }
            if (is_punct("{")) {
                tskip();
                while (!is_punct("}")) {
                    if (pcur()->ty == TK_ID) {
                        const char* en = pcur()->name;
                        tskip();
                        if (is_punct("=")) {
                            tskip();
                            v = const_expr();
                        }
                        add_enum_const(en, v);
                    }
                    v++;
                    if (is_punct(",")) {
                        tskip();
                        continue;
                    }
                    break;
                }
                tskip();
            }
            base = ty_int;
            return base;
        } else {
            Type* td = (t->ty == TK_ID) ? find_tdef(t->name) : 0;
            if (td) {
                base = td;
                tskip();
            }
            break;
        }
        if (base && !(isfollow(pcur())))
            break;
        t = pcur();
    }
    (void)sign;
    return base;
}

static int inner_ptr_count;
static int isfollow(Tok* t)
{
    return t->ty == TK_ID &&
           (strcmp(t->name, "int") == 0 || strcmp(t->name, "char") == 0 ||
            strcmp(t->name, "unsigned") == 0 || strcmp(t->name, "signed") == 0 ||
            strcmp(t->name, "short") == 0 || strcmp(t->name, "long") == 0 ||
            strcmp(t->name, "const") == 0 || strcmp(t->name, "volatile") == 0);
}


/* ---- declarator: base + stars + name + suffixes ---- */
/* supports: int x, int* p, char** a, int a[4], int a[4][3] (reversed
 * properly), int f(int, char*), int(*p)(int). */
static Type* parse_decl(Type* base, char** name)
{
    int stars = 0;
    while (is_punct("*"))
        tskip(), stars++;
    *name = 0;
    if (is_punct("(")) {
        /* pointer group: '(' *IDENT with possible nested group ')' then
         * suffixes. we support the common (*(*)) forms. */
        int inner_stars;
        Type* t;
        tskip();
        /* the group must be: stars + name (+ optional nested '(') */
        inner_stars = 0;
        while (is_punct("*"))
            tskip(), inner_stars++;
        if (is_punct("(")) {
            /* nested group: recurse with a temporary base */
            Type* inner = parse_decl(base, name);
            expect_after();
            if (!is_punct(")")) {
                printf("vcc: bad nested declarator\n");
                exit(1);
            }
            tskip();
            /* suffixes apply to base, then the nested group's pointers wrap */
            t = apply_suffixes(base);
            t = wrap_ptr(t, inner_ptr_count);
            return t;
            (void)stars;
            (void)inner;
        }
        if (pcur()->ty != TK_ID) {
            printf("vcc: bad declarator group at line %d\n", pcur()->line);
            exit(1);
        }
        *name = pcur()->name;
        tskip();
        {
            /* suffixes OUTSIDE the group bind to the base */
            Type* suff = apply_suffixes(base);
            while (stars--)
                suff = ptr_to(suff);
            while (inner_stars--)
                suff = ptr_to(suff);
            return suff;
        }
    }
    if (pcur()->ty != TK_ID) {
        printf("vcc: identifier expected at line %d\n", pcur()->line);
        exit(1);
    }
    *name = pcur()->name;
    tskip();
    {
        Type* t = base;
        while (stars--)
            t = ptr_to(t);
        t = apply_suffixes(t);
        return t;
    }
}


/* apply array/function suffixes that follow the declarator name */
static Type* apply_suffixes(Type* t)
{
    for (;;) {
        if (is_punct("[")) {
            int n;
            tskip();
            n = const_expr();
            if (!is_punct("]")) {
                printf("vcc: expected ]\n");
                exit(1);
            }
            tskip();
            t = arr_of(t, n);
        } else if (is_punct("(")) {
            /* function: parse params */
            Type* ret = t;
            int np = 0;
            Type* f = T(T_FUNC, 4, 4);
            f->ref = ret;
            f->pnames = (char**)aalloc(sizeof(char*) * 32);
            {
                int k;
                for (k = 0; k < 32; k++)
                    f->pnames[k] = 0;
            }
            f->n = 0;
            tskip();
            if (is_punct(")")) {
                /* no params */
            } else {
                for (;;) {
                    Type* pt = decl_spec();
                    char* pn = 0;
                    if (!pt) {
                        printf("vcc: bad function parameter at line %d\n",
                               pcur()->line);
                        exit(1);
                    }
                    if (pt->kind == T_VOID && is_punct(")")) {
                        break;
                    }
                    {
                        Type* ft = parse_decl(pt, &pn);
                        (void)ft;
                        if (f->pnames)
                            f->pnames[np] = astrdup(pn);
                    }
                    np++;
                    if (is_punct(",")) {
                        tskip();
                        if (pcur()->ty == TK_PUNCT &&
                            strcmp(pcur()->name, ".") == 0) {
                            /* ... */
                            tskip();
                            if (!is_punct("."))
                                break;
                            tskip();
                            if (!is_punct("."))
                                break;
                            tskip();
                            if (!is_punct(")"))
                                break;
                            break;
                        }
                        continue;
                    }
                    break;
                }
            }
            if (!is_punct(")")) {
                printf("vcc: expected ) in params at line %d\n", pcur()->line);
                exit(1);
            }
            tskip();
            f->n = np;
            t = f;
        } else {
            break;
        }
    }
    return t;
}

static void expect_after(void)
{
    if (!is_punct(")")) {
        printf("vcc: expected ) at line %d\n", pcur()->line);
        exit(1);
    }
    tskip();
}

static Type* wrap_ptr(Type* t, int k)
{
    while (k--)
        t = ptr_to(t);
    return t;
}


/* ---- constant integer expression (no emit) ---- */
static int cexpr_binexpr(int minpred);

static int cexpr_atom(void)
{
    Tok* t = pcur();
    if (t->ty == TK_NUM) {
        tskip();
        return t->val;
    }
    if (t->ty == TK_PUNCT && t->name[0] == '(' && t->name[1] == 0) {
        int v;
        tskip();
        v = cexpr_binexpr(0);
        if (!is_punct(")")) {
            printf("vcc: expected ) in const expr at line %d\n", pcur()->line);
            exit(1);
        }
        tskip();
        return v;
    }
    if (t->ty == TK_ID) {
        Type* td;
        const char* n = t->name;
        if (strcmp(n, "sizeof") == 0) {
            Type* t2;
            tskip();
            if (!is_punct("(")) {
                printf("vcc: sizeof needs (\n");
                exit(1);
            }
            tskip();
            t2 = decl_spec();
            if (t2) {
                char* nm = 0;
                Type* ct = parse_decl(t2, &nm);
                if (!is_punct(")")) {
                    printf("vcc: expected ) in sizeof\n");
                    exit(1);
                }
                tskip();
                return ct->size;
            }
            printf("vcc: sizeof of expression unsupported\n");
            exit(1);
        }
        td = find_tdef(n);
        if (td) {
            printf("vcc: typedef name in const expr\n");
            exit(1);
        }
        printf("vcc: non-constant at line %d\n", t->line);
        exit(1);
    }
    if (t->ty == TK_PUNCT && t->name[0] == '\'') {
        tskip();
        return t->val;
    }
    printf("vcc: bad const expr token at line %d\n", t->line);
    exit(1);
}

static int cexpr_unary(void)
{
    Tok* t = pcur();
    if (t->ty == TK_PUNCT) {
        const char* n = t->name;
        if (n[0] == '-' && n[1] == 0) {
            tskip();
            return -cexpr_unary();
        }
        if (n[0] == '+' && n[1] == 0) {
            tskip();
            return cexpr_unary();
        }
        if (n[0] == '!' && n[1] == 0) {
            tskip();
            return !cexpr_unary();
        }
        if (n[0] == '~' && n[1] == 0) {
            tskip();
            return ~cexpr_unary();
        }
    }
    return cexpr_atom();
}

static int cexpr_binexpr(int minpred)
{
    int v = cexpr_unary();
    for (;;) {
        Tok* t = pcur();
        int pred = 0;
        int op = 0;
        if (t->ty == TK_PUNCT) {
            const char* p = t->name;
            if (!p[1]) {
                int c = p[0];
                if (c == '*')
                    pred = 10, op = '*';
                else if (c == '/')
                    pred = 10, op = '/';
                else if (c == '%')
                    pred = 10, op = '%';
                else if (c == '+')
                    pred = 9, op = '+';
                else if (c == '-')
                    pred = 9, op = '-';
                else if (c == '<')
                    pred = 7, op = '<';
                else if (c == '>')
                    pred = 7, op = '>';
                else if (c == '&')
                    pred = 6, op = '&';
                else if (c == '^')
                    pred = 5, op = '^';
                else if (c == '|')
                    pred = 4, op = '|';
            } else if (!p[2]) {
                if (p[0] == '=' && p[1] == '=')
                    pred = 7, op = '=';
                else if (p[0] == '!' && p[1] == '=')
                    pred = 7, op = '!';
                else if (p[0] == '<' && p[1] == '=')
                    pred = 7, op = 'L';
                else if (p[0] == '>' && p[1] == '=')
                    pred = 7, op = 'G';
                else if (p[0] == '&' && p[1] == '&')
                    pred = 3, op = 'A';
                else if (p[0] == '|' && p[1] == '|')
                    pred = 2, op = 'O';
                else if (p[0] == '<' && p[1] == '<')
                    pred = 8, op = 'S';
                else if (p[0] == '>' && p[1] == '>')
                    pred = 8, op = 'T';
            }
        }
        if (pred < minpred)
            break;
        tskip();
        {
            int rhs;
            if (op == 'A') {
                rhs = cexpr_binexpr(3);
                return v && rhs;
            }
            if (op == 'O') {
                rhs = cexpr_binexpr(2);
                return v || rhs;
            }
            rhs = cexpr_binexpr(pred + 1);
            switch (op) {
            case '*': v = v * rhs; break;
            case '/': v = rhs ? v / rhs : 0; break;
            case '%': v = rhs ? v % rhs : 0; break;
            case '+': v = v + rhs; break;
            case '-': v = v - rhs; break;
            case 'S': v = v << rhs; break;
            case 'T': v = v >> rhs; break;
            case '<': v = v < rhs; break;
            case '>': v = v > rhs; break;
            case 'L': v = v <= rhs; break;
            case 'G': v = v >= rhs; break;
            case '=': v = v == rhs; break;
            case '!': v = v != rhs; break;
            case '&': v = v & rhs; break;
            case '^': v = v ^ rhs; break;
            case '|': v = v | rhs; break;
            default: break;
            }
        }
    }
    return v;
}

static int const_expr(void)
{
    return cexpr_binexpr(0);
}

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */

static int cur_lret;          /* label of the current function's epilogue */
static int cur_storage;      /* 0 none, 1 static, 2 extern, 4 global */
static int is_typedef;

static void sym_def_var(const char* nm, Type* ty, int bind, int sec, int val,
                        int size)
{
    OSym* s = osym_lookup(nm, 1);
    if (s->defined && s->shndx == sec) {
        /* duplicate definition */
        printf("vcc: duplicate symbol %s\n", nm);
        exit(1);
    }
    s->defined = 1;
    s->bind = bind;
    s->type = 1;
    s->shndx = sec;
    s->val = val;
    s->size = size;
}

static void emit_global_init(Type* ty, int* done);

/* global initialization; ty is the full declared type of the object */
static void ginit(Type* ty)
{
    if (ty->kind == T_ARRAY) {
        if (is_punct("{")) {
            int n = 0;
            tskip();
            while (!is_punct("}")) {
                ginit(ty->ref);
                n++;
                if (is_punct(",")) {
                    tskip();
                    continue;
                }
                break;
            }
            tskip();
            if (array_len(ty) < 0)
                ty->n = n;
            return;
        }
        if (ty->ref->kind == T_CHAR || ty->ref->kind == T_UCHAR) {
            if (pcur()->ty == TK_STR) {
                Tok* t = pcur();
                tskip();
                bemit(&secs[2], t->name, t->slen + 1);
                if (array_len(ty) < 0)
                    ty->n = t->slen + 1;
                return;
            }
        }
        /* int array from scalar init list without braces */
        if (pcur()->ty == TK_NUM || pcur()->ty == TK_ID ||
            is_punct("-") || is_punct("+")) {
            int n = 0;
            for (;;) {
                int v = const_expr();
                bemit4(&secs[2], v);
                n++;
                if (is_punct(",")) {
                    tskip();
                    continue;
                }
                break;
            }
            if (array_len(ty) < 0)
                ty->n = n;
            return;
        }
        printf("vcc: bad array init at line %d\n", pcur()->line);
        exit(1);
    }
    if (ty->kind == T_STRUCT) {
        Member* m = ty->members;
        tskip(); /* { */
        while (m) {
            ginit(m->ty);
            m = m->next;
            if (is_punct(","))
                tskip();
        }
        if (!is_punct("}")) {
            printf("vcc: expected } in struct init\n");
            exit(1);
        }
        tskip();
        return;
    }
    if (ty->kind == T_PTR) {
        /* could be &symbol or a string constant (char*) */
        if (pcur()->ty == TK_STR) {
            Tok* t = pcur();
            int sym = str_const(t);
            tskip();
            if (ty->ref->kind == T_CHAR || ty->ref->kind == T_UCHAR) {
                bemit4(&secs[2], 0);
                orel_add(3, secs[2].len - 4, sym, 1);
                return;
            }
        }
        if (is_punct("&")) {
            tskip();
            if (pcur()->ty == TK_ID) {
                const char* nm = pcur()->name;
                int sym;
                tskip();
                sym = osym_lookup(nm, 1) - osym;
                bemit4(&secs[2], 0);
                orel_add(3, secs[2].len - 4, sym, 1);
                return;
            }
        }
        printf("vcc: unsupported global pointer init at line %d\n",
               pcur()->line);
        exit(1);
    }
    {
        int v = const_expr();
        int sz = type_size(ty);
        if (sz >= 4)
            bemit4(&secs[2], v);
        else if (sz == 2)
            bemit2(&secs[2], v);
        else
            bemit1(&secs[2], v);
    }
}

static void emit_global_init(Type* ty, int* done)
{
    (void)done;
    ginit(ty);
}

static Var* gvars[512];
static int ngvars;

static Var* find_global(const char* name)
{
    int i;
    for (i = 0; i < ngvars; i++)
        if (strcmp(gvars[i]->name, name) == 0)
            return gvars[i];
    return 0;
}

/* declare a file-scope variable */
static void declare_global(Type* base)
{
    char* name = 0;
    Type* ty = parse_decl(base, &name);
    if (is_typedef) {
        add_tdef(name, ty);
        return;
    }
    if (ty->kind == T_FUNC) {
        /* function (definition or prototype) */
        if (is_punct("{")) {
            OSym* s = osym_lookup(name, 1);
            s->shndx = 1;
            s->defined = 1;
            s->bind = 1;
            s->type = 2;
            s->val = secs[0].len;
            s->size = 0;
            define_function(name, ty);
        } else {
            /* prototype */
            if (!is_punct(";")) {
                printf("vcc: bad function decl '%s' at line %d\n", name,
                       pcur()->line);
                exit(1);
            }
            /*
            OSym* s = osym_lookup(name, 1);
            s->type = 2; // UND
            */
            tskip();
            {
                OSym* s = osym_lookup(name, 1);
                s->type = 2;
            }
        }
        return;
    }
    /* data object */
    {
        int bind = 1;
        OSym* s;
        if (cur_storage == 1)
            bind = 0;
        s = osym_lookup(name, 1);
        if (s->defined) {
            printf("vcc: duplicate global %s at line %d\n", name, pcur()->line);
            exit(1);
        }
        if (is_punct("=")) {
            int start;
            tskip();
            sect_align(2, ty->align > 4 ? 4 : ty->align);
            s->defined = 1;
            s->bind = bind;
            s->type = 1;
            s->shndx = 3;
            s->val = secs[2].len;
            start = secs[2].len;
            ginit(ty);
            s->size = secs[2].len - start;
        } else if (cur_storage == 2 && !is_punct(";")) {
            printf("vcc: extern with init\n");
            exit(1);
        } else if (cur_storage == 2) {
            /* extern: just a declaration (UND unless elsewhere) */
            s->type = 1;
            tskip();
        } else {
            /* uninitialized -> BSS */
            int sz = type_size(ty);
            int al = ty->align > 4 ? 4 : ty->align;
            int pad;
            if (bss_align < al)
                bss_align = al;
            pad = (al - (bss_size & (al - 1))) & (al - 1);
            bss_size += pad;
            s->defined = 1;
            s->bind = bind;
            s->type = 1;
            s->shndx = 4;
            s->val = bss_size;
            s->size = sz;
            bss_size += sz;
            tskip();
        }
        {
            Var* v = (Var*)aalloc(sizeof(Var));
            v->name = astrdup(name);
            v->ty = ty;
            v->storage = cur_storage ? cur_storage : 4;
            v->off = 0;
            v->sym = (int)(s - osym);
            if (ngvars < 512)
                gvars[ngvars++] = v;
        }
    }
}


/* ------------------------------------------------------------------ */
/* expression core (value & lvalue)                                    */
/* ------------------------------------------------------------------ */

static void incdec(Type* t, int delta, int post)
{
    c2(0x89, 0xC3);              /* mov ebx, eax = addr */
    op_load(t);                  /* eax = old */
    if (post)
        push_eax();
    if (delta > 0) {
        c2(0x83, 0xC0);
        c1(delta);
    } else {
        c2(0x83, 0xE8);
        c1(-delta);
    }
    c2(0x89, 0xD9);              /* ecx = addr */
    op_store(t);
    if (post)
        pop_eax();
}

void* current_fn;

/* op_load that matches width but always for our needs */
static void op_load_width(Type* t)
{
    op_load(t);
}

/* ================================================================== */
/* statements, functions, driver                                      */
/* ================================================================== */

static int func_call_ahead(void)
{
    int save = tpos;
    int r = 0;
    Tok* t;
    tskip();
    t = pcur();
    if (t->ty == TK_PUNCT && t->name[0] == '(' && t->name[1] == 0)
        r = 1;
    tpos = save;
    return r;
}

/* label names for goto/statement labels: name -> sym index */
static char* label_names[128];
static int label_syms[128];
static int nlabels;

static int lookup_label(const char* name)
{
    int i;
    for (i = 0; i < nlabels; i++)
        if (strcmp(label_names[i], name) == 0)
            return label_syms[i];
    if (nlabels >= 128) {
        printf("vcc: too many labels\n");
        exit(1);
    }
    label_names[nlabels] = astrdup(name);
    label_syms[nlabels] = new_label_sym();
    nlabels++;
    return label_syms[nlabels - 1];
}

/* break/continue/switch context stack */
#define BRK 1
#define CON 2
static int brk_labels[64];
static int con_labels[64];
static int lvl_kind[64];
static int nlvl;
static int is_loop_level;

static void push_lvl(int kind, int brk, int con)
{
    if (nlvl >= 64) {
        printf("vcc: nesting too deep\n");
        exit(1);
    }
    brk_labels[nlvl] = brk;
    con_labels[nlvl] = con;
    lvl_kind[nlvl] = kind;
    nlvl++;
}
static void pop_lvl(void)
{
    nlvl--;
}

/* switch case records */
static int case_vals[512];
static int def_syms[64];
static int case_syms[512];
static int ncase_rec;
static int switch_active;

static void add_case(int v)
{
    if (ncase_rec >= 512) {
        printf("vcc: too many cases\n");
        exit(1);
    }
    case_vals[ncase_rec] = v;
    case_syms[ncase_rec] = new_label_sym();
    ncase_rec++;
}

/* helper: emit jcc-on-test for a loop/if */
static void lval_for_assign(Var* v, const char* name, Type* ty);

static void stmt(void);

/* emit epilogue and return */
static void do_ret(void)
{
    c2(0x89, 0xEC);      /* mov esp, ebp */
    c1(0x5D);            /* pop ebp */
    c1(0xC3);            /* ret */
}

static Type* expr(void)
{
    Type* t = expr_assign();
    while (is_punct(",")) {
        tskip();
        {
            Type* r = expr_assign();
            (void)r;
        }
    }
    return t;
}

static void expr_stmt(void)
{
    if (is_punct(";")) {
        tskip();
        return;
    }
    {
        Type* v = expr();
        (void)v;
    }
    if (!is_punct(";")) {
        printf("vcc: expected ; at line %d tok=\"%s\"\n", pcur()->line, pcur()->name);
        exit(1);
    }
    tskip();
}

/* parse one declaration that is local to a function block */
static void local_decl(void)
{
    int save_cur = cur_storage;
    Tok* t = pcur();
    Var* v;
    if (t->ty == TK_ID) {
        if (strcmp(t->name, "static") == 0 || strcmp(t->name, "extern") == 0) {
            if (strcmp(t->name, "static") == 0)
                cur_storage = 1;
            else
                cur_storage = 2;
            tskip();
        }
    }
    {
        Type* base = decl_spec();
        if (!base)
            base = ty_int;
        for (;;) {
            Var* var = decl_var(base);
            if (!var) {
                printf("vcc: bad local declaration at line %d\n",
                       pcur()->line);
                exit(1);
            }
            if (is_punct("=")) {
                tskip();
                {
                    Type* r = expr_assign();
                    (void)r;
                }
                push_eax();        /* save value */
                lea_local(var);    /* eax = addr */
                mov_ecx_eax();     /* ecx = addr */
                pop_eax();         /* eax = value */
                op_store(var->ty);
            }
            if (!is_punct(","))
                break;
            tskip();
        }
        if (!is_punct(";")) {
            printf("vcc: expected ; after local decl\n");
            exit(1);
        }
        tskip();
    }
    cur_storage = save_cur;
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static void stmt(void)
{
    Tok* t = pcur();
    if (t->ty == TK_PUNCT) {
        const char* n = t->name;
        if (n[0] == '{' && n[1] == 0) {
            Var* saved = push_scope_local();
            tskip();
            for (;;) {
                Tok* q = pcur();
                if (q->ty == TK_PUNCT && q->name[0] == '}' && q->name[1] == 0) {
                    tskip();
                    break;
                }
                if (q->ty == TK_EOF)
                    break;
                if (is_typespec(q) || (q->ty == TK_ID &&
                    (strcmp(q->name, "static") == 0 ||
                     strcmp(q->name, "extern") == 0)))
                    local_decl();
                else
                    stmt();
            }
            pop_scope_back(saved);
            return;
        }
        if (n[0] == ';' && n[1] == 0) {
            tskip();
            return;
        }
        if (n[0] == ':' && n[1] == 0 && n[2] == 0) {
            /* statement lable is handled via ident ':' in block loop */
            tskip();
            return;
        }
    }
    if (t->ty == TK_ID) {
        const char* nm = t->name;
        if (strcmp(nm, "if") == 0) {
            int Lend = new_label_sym();
            tskip();
            if (!is_punct("(")) {
                printf("vcc: if needs (\n");
                exit(1);
            }
            tskip();
            {
                Type* c = expr();
                (void)c;
            }
            if (!is_punct(")")) {
                printf("vcc: if needs )\n");
                exit(1);
            }
            tskip();
            test_eax();
            branch_jz(Lend);
            stmt();
            if (peek_kw("else")) {
                int Lelse = new_label_sym();
                tskip();
                op_jmp(Lelse);
                define_label(Lend);
                stmt();
                define_label(Lelse);
            } else {
                define_label(Lend);
            }
            return;
        }
        if (strcmp(nm, "while") == 0) {
            int Lcond = new_label_sym();
            int Lend = new_label_sym();
            tskip();
            if (!is_punct("(")) {
                printf("vcc: while needs (\n");
                exit(1);
            }
            tskip();
            define_label(Lcond);
            {
                Type* c = expr();
                (void)c;
            }
            if (!is_punct(")")) {
                printf("vcc: while needs )\n");
                exit(1);
            }
            tskip();
            test_eax();
            branch_jz(Lend);
            push_lvl(BRK, Lend, Lcond);
            stmt();
            pop_lvl();
            op_jmp(Lcond);
            define_label(Lend);
            return;
        }
        if (strcmp(nm, "do") == 0) {
            int Lbegin = new_label_sym();
            int Lcond = new_label_sym();
            tskip();
            define_label(Lbegin);
            push_lvl(BRK, Lbegin, Lcond);
            stmt();
            pop_lvl();
            define_label(Lcond);
            if (!is_punct("while")) {
                printf("vcc: do needs while at line %d\n", pcur()->line);
                exit(1);
            }
            tskip();
            if (!is_punct("(")) {
                printf("vcc: do-while needs (\n");
                exit(1);
            }
            tskip();
            {
                Type* c = expr();
                (void)c;
            }
            if (!is_punct(")")) {
                printf("vcc: do-while needs )\n");
                exit(1);
            }
            tskip();
            if (!is_punct(";")) {
                printf("vcc: do-while needs ;\n");
                exit(1);
            }
            tskip();
            test_eax();
            branch_jnz(Lbegin);
            return;
        }
        if (strcmp(nm, "for") == 0) {
            int Lcond = new_label_sym();
            int Linc = new_label_sym();
            int Lend = new_label_sym();
            int has_inc = 0;
            int incpos;
            int incend;
            int bodypos;
            tskip();
            if (!is_punct("(")) {
                printf("vcc: for needs (\n");
                exit(1);
            }
            tskip();
            if (!is_punct(";"))
                expr_stmt();
            else
                tskip();
            define_label(Lcond);
            if (!is_punct(";")) {
                {
                    Type* c = expr();
                    (void)c;
                }
                test_eax();
                branch_jz(Lend);
            }
            if (!is_punct(";")) {
                printf("vcc: for header expected ;\n");
                exit(1);
            }
            tskip();
            incpos = tpos;
            if (!is_punct(")")) {
                has_inc = 1;
                expr();
            }
            if (!is_punct(")")) {
                printf("vcc: for needs )\n");
                exit(1);
            }
            tskip();
            incend = tpos;
            push_lvl(BRK, Lend, Linc);
            stmt();
            pop_lvl();
            define_label(Linc);
            if (has_inc) {
                tpos = incpos;
                expr();
                tpos = incend;
            }
            op_jmp(Lcond);
            define_label(Lend);
            return;
        }
        if (strcmp(nm, "return") == 0) {
            tskip();
            if (!is_punct(";")) {
                {
                    Type* c = expr();
                    (void)c;
                }
            }
            if (!is_punct(";")) {
                printf("vcc: return needs ;\n");
                exit(1);
            }
            tskip();
            op_jmp(cur_lret);
            return;
        }
        if (strcmp(nm, "break") == 0) {
            tskip();
            if (!is_punct(";")) {
                printf("vcc: break needs ;\n");
                exit(1);
            }
            tskip();
            if (!nlvl) {
                printf("vcc: break outside loop at line %d\n", t->line);
                exit(1);
            }
            op_jmp(brk_labels[nlvl - 1]);
            return;
        }
        if (strcmp(nm, "continue") == 0) {
            tskip();
            if (!is_punct(";")) {
                printf("vcc: continue needs ;\n");
                exit(1);
            }
            tskip();
            if (!nlvl || lvl_kind[nlvl - 1] != BRK) {
                printf("vcc: continue outside loop\n");
                exit(1);
            }
            op_jmp(con_labels[nlvl - 1]);
            return;
        }
        if (strcmp(nm, "goto") == 0) {
            tskip();
            if (pcur()->ty != TK_ID) {
                printf("vcc: goto needs label\n");
                exit(1);
            }
            {
                int sym = lookup_label(pcur()->name);
                tskip();
                if (!is_punct(";")) {
                    printf("vcc: goto needs ;\n");
                    exit(1);
                }
                tskip();
                op_jmp(sym);
            }
            return;
        }
        if (strcmp(nm, "switch") == 0) {
            int Lend = new_label_sym();
            int base = ncase_rec;
            tskip();
            if (!is_punct("(")) {
                printf("vcc: switch needs (\n");
                exit(1);
            }
            tskip();
            {
                Type* c = expr();
                (void)c;
            }
            if (!is_punct(")")) {
                printf("vcc: switch needs )\n");
                exit(1);
            }
            tskip();
                        push_eax();               /* keep switch value on stack */
            def_syms[nlvl] = new_label_sym();
            push_lvl(CON, Lend, -1);
            stmt();
            pop_lvl();
            {
                int i;
                for (i = base; i < ncase_rec; i++) {
                    c1(0x8B);
                    c1(0x04);
                    c1(0x24);         /* mov eax, [esp] */
                    c1(0x3D);
                    c4(case_vals[i]); /* cmp eax, imm32 */
                    branch_jz(case_syms[i]);
                }
                op_jmp(def_syms[nlvl]);
            }
            add_esp(4);
            define_label(Lend);
            return;
        }
        if (strcmp(nm, "case") == 0) {
            int l;
            Type* ty2;
            tskip();
            if (nlvl < 1 || lvl_kind[nlvl - 1] != CON) {
                printf("vcc: case outside switch at line %d\n", t->line);
                exit(1);
            }
            l = const_expr();
            add_case(l);
            if (!is_punct(":")) {
                printf("vcc: case needs :\n");
                exit(1);
            }
            tskip();
            define_label(case_syms[ncase_rec - 1]);
            stmt();
            return;
        }
        if (strcmp(nm, "default") == 0) {
            tskip();
            if (!is_punct(":")) {
                printf("vcc: default needs :\n");
                exit(1);
            }
            tskip();
            if (nlvl < 1 || lvl_kind[nlvl - 1] != CON) {
                printf("vcc: default outside switch\n");
                exit(1);
            }
            define_label(def_syms[nlvl - 1]);
            stmt();
            return;
        }
        /* statement label:  ident ':' */
        if (peek_colon()) {
            int sym = lookup_label(nm);
            define_label(sym);
            tskip();
            tskip();
            /* following statement (if any) parsed by the block loop */
            return;
        }
    }
    if (is_typespec(t) || (t->ty == TK_ID &&
        (strcmp(t->name, "static") == 0 || strcmp(t->name, "extern") == 0))) {
        local_decl();
        return;
    }
    expr_stmt();
}

static int peek_kw(const char* k)
{
    Tok* t = pcur();
    return t->ty == TK_ID && strcmp(t->name, k) == 0;
}

static int peek_colon(void)
{
    int save = tpos;
    int r = 0;
    Tok* t;
    tskip();
    t = pcur();
    if (t->ty == TK_PUNCT && t->name[0] == ':' && t->name[1] == 0)
        r = 1;
    tpos = save;
    return r;
}

static int Lcond_cont(int inc)
{
    (void)inc;
    return inc;
}


static Var* push_scope_local(void)
{
    return scope;            /* remember innermost chain head */
}
static void pop_scope_back(Var* old)
{
    scope = old;             /* drop inner block's vars */
}

/* one local variable declaration; consumes only the declarator. */
static Var* decl_var(Type* base)
{
    char* nm = 0;
    Type* ty = parse_decl(base, &nm);
    Var* v;
    v = (Var*)aalloc(sizeof(Var));
    v->name = astrdup(nm);
    v->ty = ty;
    v->storage = 0;
    local_alloc(v);
    v->next = scope;
    scope = v;
    return v;
}

static void branch_jz(int sym)
{
    op_jz_label(sym);
}
static void branch_jnz(int sym)
{
    op_jnz_label(sym);
}

/* label helpers */
static int new_label_sym(void)
{
    char b[8];
    int n = 0;
    int v = label_count++;
    b[n++] = 'L';
    b[n++] = (char)('0' + (v / 100) % 10);
    b[n++] = (char)('0' + (v / 10) % 10);
    b[n++] = (char)('0' + (v % 10));
    b[n] = 0;
    {
        OSym* s = osym_lookup(b, 1);
        return (int)(s - osym);
    }
}

static void define_label(int sym)
{
    OSym* s = &osym[sym];
    if (s->defined) {
        printf("vcc: duplicate label at line %d\n", pcur()->line);
        exit(1);
    }
    s->defined = 1;
    s->shndx = 1;
    s->bind = 0;
    s->type = 0;
    s->val = secs[0].len;
    s->size = 0;
}

/* ------------------------------------------------------------------ */
/* unary + binary expression codegen (value in EAX)                    */
/* ------------------------------------------------------------------ */

static Type* expr_unary(void)
{
    Tok* t = pcur();
    if (t->ty == TK_PUNCT) {
        const char* n = t->name;
        if (n[0] == '-' && n[1] == 0 && n[2] == 0) {
            Type* v;
            tskip();
            v = expr_unary();
            op_neg();
            (void)v;
            return ty_int;
        }
        if (n[0] == '+' && n[1] == 0 && n[2] == 0) {
            tskip();
            return expr_unary();
        }
        if (n[0] == '!' && n[1] == 0 && n[2] == 0) {
            tskip();
            expr_unary();
            test_eax();
            c2(0x0F, 0x94);
            c1(0xC0);              /* setz al */
            op_movzx_eax_al();
            return ty_int;
        }
        if (n[0] == '~' && n[1] == 0 && n[2] == 0) {
            tskip();
            expr_unary();
            op_not();
            return ty_int;
        }
        if (n[0] == '*' && n[1] == 0 && n[2] == 0) {
            Type* t2;
            tskip();
            t2 = expr_unary();
            if (!t2 || (t2->kind != T_PTR && t2->kind != T_ARRAY)) {
                printf("vcc: deref of non-pointer at line %d\n", t->line);
                exit(1);
            }
            op_load(t2->ref);
            return t2->ref;
        }
        if (n[0] == '&' && n[1] == 0 && n[2] == 0) {
            Type* t2;
            tskip();
            t2 = lval_addr();
            if (t2->kind == T_ARRAY)
                return ptr_to(t2->ref);
            return ptr_to(t2);
        }
        if (n[0] == '+' && n[1] == '+' && n[2] == 0) {
            Type* t2;
            tskip();
            t2 = lval_addr();
            incdec(t2, 1, 0);
            return t2;
        }
        if (n[0] == '-' && n[1] == '-' && n[2] == 0) {
            Type* t2;
            tskip();
            t2 = lval_addr();
            incdec(t2, -1, 0);
            return t2;
        }
    }
    {
        Type* v;
        int lit = t->ty == TK_NUM || t->ty == TK_STR;
        v = lval_addr();
        if (!lit && !lv_has_val)
            load_value(v);
        lv_has_val = 0;
        return v;
    }
}

static Type* expr_bin_cont(Type* lt, int minpred)
{
    for (;;) {
        Tok* t = pcur();
        int pred = 0;
        int op = 0;
        if (t->ty == TK_PUNCT) {
            const char* p = t->name;
            int c = p[0] & 0xFF;
            if (!p[1]) {
                if (c == '*') pred = 10, op = '*';
                else if (c == '/') pred = 10, op = '/';
                else if (c == '%') pred = 10, op = '%';
                else if (c == '+') pred = 9, op = '+';
                else if (c == '-') pred = 9, op = '-';
                else if (c == '<') pred = 7, op = '<';
                else if (c == '>') pred = 7, op = '>';
                else if (c == '&') pred = 6, op = '&';
                else if (c == '^') pred = 5, op = '^';
                else if (c == '|') pred = 4, op = '|';
                else if (c == '?') pred = 1, op = '?';
            } else if (!p[2]) {
                if (p[0] == '=' && p[1] == '=') pred = 7, op = 'q';
                else if (p[0] == '!' && p[1] == '=') pred = 7, op = 'n';
                else if (p[0] == '<' && p[1] == '=') pred = 7, op = 'L';
                else if (p[0] == '>' && p[1] == '=') pred = 7, op = 'G';
                else if (p[0] == '&' && p[1] == '&') pred = 3, op = 'A';
                else if (p[0] == '|' && p[1] == '|') pred = 2, op = 'O';
                else if (p[0] == '<' && p[1] == '<') pred = 8, op = 'S';
                else if (p[0] == '>' && p[1] == '>') pred = 8, op = 'T';
                else if (p[0] == '+' && p[1] == '=') pred = -1;
                else if (p[0] == '-' && p[1] == '=') pred = -1;
                else if (p[0] == '*' && p[1] == '=') pred = -1;
                else if (p[0] == '/' && p[1] == '=') pred = -1;
                else if (p[0] == '%' && p[1] == '=') pred = -1;
                else if (p[0] == '&' && p[1] == '=') pred = -1;
                else if (p[0] == '|' && p[1] == '=') pred = -1;
                else if (p[0] == '^' && p[1] == '=') pred = -1;
            } else if (!p[3]) {
                if (p[0] == '<' && p[1] == '<' && p[2] == '=') pred = -1;
                else if (p[0] == '>' && p[1] == '>' && p[2] == '=') pred = -1;
            }
        }
        if (pred < minpred)
            break;
        if (op == 'q' || op == 'n' || op == 'L' || op == 'G' ||
            op == 'S' || op == 'T' || op == 'A' || op == 'O' ||
            op == '?' || op == '*' || op == '/' || op == '%' ||
            op == '+' || op == '-' || op == '&' || op == '^' || op == '|' ||
            op == '<' || op == '>') {
            if (op == 'A' || op == 'O') {
                int Lset = new_label_sym();
                int Lend = new_label_sym();
                tskip();
                test_eax();
                if (op == 'A') {
                    branch_jz(Lset);
                    {
                        Type* r = expr_bin(3);
                        (void)r;
                    }
                    test_eax();
                    branch_jz(Lset);
                    c1(0xB8);
                    c4(1);
                    op_jmp(Lend);
                    define_label(Lset);
                    c2(0x31, 0xC0);
                    define_label(Lend);
                } else {
                    branch_jnz(Lset);
                    {
                        Type* r = expr_bin(2);
                        (void)r;
                    }
                    test_eax();
                    branch_jnz(Lset);
                    c2(0x31, 0xC0);
                    op_jmp(Lend);
                    define_label(Lset);
                    c1(0xB8);
                    c4(1);
                    define_label(Lend);
                }
                return ty_int;
            }
            if (op == '?') {
                int Lfalse = new_label_sym();
                int Lend = new_label_sym();
                tskip();
                test_eax();
                branch_jz(Lfalse);
                {
                    Type* tt = expr_assign();
                    (void)tt;
                }
                if (!is_punct(":")) {
                    printf("vcc: expected : at line %d\n", t->line);
                    exit(1);
                }
                tskip();
                op_jmp(Lend);
                define_label(Lfalse);
                {
                    Type* ft = expr_assign();
                    (void)ft;
                }
                define_label(Lend);
                return ty_int;
            }
            if (op == '&' || op == '^' || op == '|') {
                /* assignment ops never reach here (pred 0) */
            }
            tskip();
            push_eax();                 /* save left */
            {
                Type* rt = expr_bin(pred + 1);
                mov_ecx_eax();          /* ecx = right */
                pop_eax();              /* eax = left */
                combine_bin(op, lt, rt);
                lt = result_type(lt, rt);
            }
        } else {
            /* single '=', compound 'op=', 0-precedence: stop */
            break;
        }
    }
    return lt;
}

static Type* expr_bin(int minpred)
{
    Type* lt = expr_unary();
    return expr_bin_cont(lt, minpred);
}

static void combine_bin(int op, Type* lt, Type* rt)
{
    int uns = 0;
    Type* pt;
    (void)lt;
    (void)rt;
    pt = (lt && (lt->kind == T_PTR || lt->kind == T_ARRAY)) ? lt : rt;
    switch (op) {
    case '*':
        c2(0x0F, 0xAF);
        c1(0xC1);                    /* imul eax, ecx */
        return;
    case '/':
    case '%':
        uns = (lt && lt->sign) || (rt && rt->sign);
        if (uns) {
            c2(0x31, 0xD2);
            div_ecx();
        } else {
            cdq_();
            idiv_ecx();
        }
        if (op == '%') {
            c2(0x89, 0xD0);          /* mov eax, edx */
        }
        return;
    case '+':
        if (pt && (pt->kind == T_PTR || pt->kind == T_ARRAY)) {
            int sz = type_size(pt->ref);
            if (lt && (lt->kind == T_PTR || lt->kind == T_ARRAY) &&
                !(rt && (rt->kind == T_PTR || rt->kind == T_ARRAY))) {
                c1(0x69); c1(0xC9); c4(sz);   /* imul ecx, ecx, sz */
                c2(0x01, 0xC8);
            } else if (rt && (rt->kind == T_PTR || rt->kind == T_ARRAY)) {
                if (!(lt && (lt->kind == T_PTR || lt->kind == T_ARRAY))) {
                    c1(0x69); c1(0xC0); c4(sz);   /* imul eax, eax, sz */
                    c2(0x01, 0xC1);
                    c2(0x89, 0xC8);
                } else {
                    c2(0x01, 0xC8);
                }
            } else {
                c2(0x01, 0xC8);
            }
        } else {
            c2(0x01, 0xC8);          /* add eax, ecx */
        }
        return;
    case '-':
        if (pt && (pt->kind == T_PTR || pt->kind == T_ARRAY) &&
            (!lt || (lt->kind != T_PTR && lt->kind != T_ARRAY))) {
            /* ptr - scalar: eax=scalar, ecx=ptr (never happens: right is
               value/ptr; here left is scalar) -> unsupported; keep simple */
            c2(0x29, 0xC8);
            return;
        }
        c2(0x29, 0xC8);              /* sub eax, ecx */
        if (pt && (pt->kind == T_PTR || pt->kind == T_ARRAY) &&
            rt && (rt->kind == T_PTR || rt->kind == T_ARRAY)) {
            /* ptr - ptr gives element count */
            int sz = type_size(pt->ref);
            c1(0xB9);
            c4(sz);
            cdq_();
            idiv_ecx();
        }
        return;
    case 'S':
        op_shl_cl();
        return;
    case 'T':
        op_shr_cl();
        return;
    case '&':
        op_and_ecx();
        return;
    case '^':
        op_xor_ecx();
        return;
    case '|':
        op_or_ecx();
        return;
    case '<':
    case '>':
    case 'L':
    case 'G':
    case 'q':
    case 'n':
        c2(0x39, 0xC8);              /* cmp eax, ecx = left - right */
        uns = (lt && lt->sign) || (rt && rt->sign);
        if (op == 'q') c2(0x0F, 0x94);
        else if (op == 'n') c2(0x0F, 0x95);
        else if (op == '<') c2(0x0F, uns ? 0x92 : 0x9C);
        else if (op == '>') c2(0x0F, uns ? 0x97 : 0x9F);
        else if (op == 'L') c2(0x0F, uns ? 0x96 : 0x9E);
        else c2(0x0F, uns ? 0x93 : 0x9D);
        c1(0xC0);
        op_movzx_eax_al();
        return;
    default:
        return;
    }
}

static Type* result_type(Type* lt, Type* rt)
{
    if (lt && (lt->kind == T_PTR || lt->kind == T_ARRAY))
        return lt;
    if (rt && (rt->kind == T_PTR || rt->kind == T_ARRAY))
        return rt;
    if (lt && rt && lt->sign)
        return ty_uint;
    return ty_int;
}

static Type* expr_assign(void)
{
    Tok* t = pcur();
    int aop = 0;
    int cop = 0;
    if (t->ty == TK_ID) {
        Type* lv;
        lv = lval_addr();
        t = pcur();
        if (t->ty == TK_PUNCT) {
            const char* p = t->name;
            if (p[0] == '=' && p[1] == 0) {
                aop = '=';
            } else if (p[1] == '=' && !p[2]) {
                char c0 = p[0];
                if (c0 == '+' || c0 == '-' || c0 == '*' || c0 == '/' ||
                    c0 == '%' || c0 == '&' || c0 == '|' || c0 == '^') {
                    aop = 1;
                    cop = c0;
                }
            } else {
                int l = 0;
                while (p[l])
                    l++;
                if (l == 3 && p[2] == '=') {
                    if (p[0] == '<' && p[1] == '<') {
                        aop = 2;
                    } else if (p[0] == '>' && p[1] == '>') {
                        aop = 3;
                    }
                }
            }
        }
        if (aop) {
            tskip();
            if (aop == '=') {
                push_eax();          /* save addr */
                expr_assign();
                pop_ecx();           /* ecx = addr */
                op_store(lv);        /* [ecx] = eax; eax = value */
            } else {
                push_eax();          /* save addr */
                expr_assign();
                mov_ecx_eax();       /* ecx = rhs */
                pop_edx();           /* edx = addr */
                if (aop == 1) {
                    c2(0x8B, 0x02);  /* eax = old */
                    combine_bin(cop, lv, lv);
                    store_dx(lv);
                } else {
                    c2(0x8B, 0x02);  /* eax = old */
                    if (aop == 2)
                        op_shl_cl();
                    else
                        op_shr_cl();
                    store_dx(lv);
                }
            }
            return lv;
        }
        if (!lv_has_val)
            load_value(lv);
        lv_has_val = 0;
        return expr_bin_cont(lv, 0);
    }
    {
        Type* r = expr_bin(0);
        lv_has_val = 0;
        return r;
    }
}

static void store_dx(Type* t)
{
    int sz = type_size(t);
    if (sz >= 4) {
        c2(0x89, 0x02);
    } else if (sz == 2) {
        c2(0x66, 0x89);
        c1(0x02);
    } else {
        c2(0x88, 0x02);
    }
}

/* ------------------------------------------------------------------ */
/* function definition                                                 */
/* ------------------------------------------------------------------ */

static int fun_depth;
static int fr_save[64];

static void define_function(const char* name, Type* ty)
{
    int fn_start = secs[0].len;
    int i;
    int fixup_pos;
    int S;
    int lret;
    OSym* s = osym_lookup(name, 1);
    Var* saved_scope = scope;
    if (fun_depth >= 64) {
        printf("vcc: nested function\n");
        exit(1);
    }
    fr_save[fun_depth] = frame_used;
    frame_used = 0;
    scope = 0;
    fun_depth++;
    lret = new_label_sym();
    cur_lret = lret;

    c1(0x55);                /* push ebp */
    c2(0x89, 0xE5);          /* mov ebp, esp */
    c2(0x81, 0xEC);          /* sub esp, S (put placeholder) */
    fixup_pos = secs[0].len;
    c4(0);
    if (!is_punct("{")) {
        printf("vcc: function body needs { at line %d\n", pcur()->line);
        exit(1);
    }
    tskip();
    for (i = 0; i < ty->n && i < 8; i++) {
        Var* v;
        int d;
        if (!ty->pnames || !ty->pnames[i])
            continue;
        v = (Var*)aalloc(sizeof(Var));
        v->name = astrdup(ty->pnames[i]);
        v->ty = ty->ref;   /* TODO: per-param types; assume int for now */
        v->storage = 3;
        local_alloc(v);
        v->next = scope;
        scope = v;
        d = v->off;
        /* cdecl: arg i arrives at [ebp + 8 + 4*i]; copy into its local */
        c1(0x8B); c1(0x85);
        c4(8 + 4 * i);               /* mov eax, [ebp+8+4i] */
        c2(0x89, 0x85); c4(d);       /* mov [ebp+d], eax */
    }
    for (;;) {
        Tok* t2 = pcur();
        if (t2->ty == TK_PUNCT && t2->name[0] == '}' && t2->name[1] == 0) {
            tskip();
            break;
        }
        if (t2->ty == TK_EOF)
            break;
        stmt();
    }
    define_label(lret);
    do_ret();
    S = (frame_used + 3) & ~3;
    {
        unsigned char* p = (unsigned char*)(secs[0].data + fixup_pos);
        p[0] = S & 0xFF;
        p[1] = (S >> 8) & 0xFF;
        p[2] = (S >> 16) & 0xFF;
        p[3] = (S >> 24) & 0xFF;
    }
    s->size = secs[0].len - fn_start;
    scope = saved_scope;
    frame_used = fr_save[fun_depth - 1];
    fun_depth--;
}

/* top-level declarations loop */
static void parse_file(void)
{
    for (;;) {
        Tok* t = pcur();
        Type* base;
        if (t->ty == TK_EOF)
            break;
        cur_storage = 0;
        is_typedef = 0;
        if (t->ty == TK_PUNCT && t->name[0] == ';' && t->name[1] == 0) {
            tskip();
            continue;
        }
        if (t->ty == TK_ID) {
            if (strcmp(t->name, "static") == 0) {
                cur_storage = 1;
                tskip();
            } else if (strcmp(t->name, "extern") == 0) {
                cur_storage = 2;
                tskip();
            } else if (strcmp(t->name, "typedef") == 0) {
                is_typedef = 1;
                tskip();
            }
        }
        base = decl_spec();
        if (!base) {
            printf("vcc: unexpected top-level token at line %d\n",
                   pcur()->line);
            break;
        }
        declare_global(base);
        cur_storage = 0;
        is_typedef = 0;
    }
}


/* ================================================================== */
/* ELF32 object file writer                                            */
/* ================================================================== */

#define ET_REL 1
#define ET_EXEC 2
#define EM_386 3
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2

static void be32(Buf* o, u32 v)
{
    bemit1(o, v & 0xFF);
    bemit1(o, (v >> 8) & 0xFF);
    bemit1(o, (v >> 16) & 0xFF);
    bemit1(o, (v >> 24) & 0xFF);
}
static void be16(Buf* o, u32 v)
{
    bemit1(o, v & 0xFF);
    bemit1(o, (v >> 8) & 0xFF);
}

static void strtab_put(Buf* t, const char* s)
{
    while (*s) {
        bemit1(t, *s);
        s++;
    }
    bemit1(t, 0);
}
static u32 strtab_off(Buf* t, const char* s)
{
    u32 off = t->len;
    strtab_put(t, s);
    return off;
}

/* write the compile result as a relocatable ELF32 object into out */
static void write_object(Buf* out)
{
    int omap[NSYM];
    int firstglobal = 1;
    int symcnt;
    int i;
    Buf strtab, shstr, symtab, relt, reld;
    u32 noff[10];
    int textlen = secs[0].len;
    int rodlen = secs[1].len;
    int datlen = secs[2].len;
    u32 ft, fro, fda, frl, frd, fsy, fstr, fshs, shoff, cur;

    strtab.len = 0; shstr.len = 0; symtab.len = 0; relt.len = 0; reld.len = 0;
    binit(&strtab); binit(&shstr); binit(&symtab); binit(&relt); binit(&reld);
    breserve(&strtab, 1024);
    breserve(&shstr, 128);
    breserve(&symtab, 4096);
    breserve(&relt, 8192);
    breserve(&reld, 4096);

    /* section name table */
    {
        const char* nm[10] = { "", ".text", ".rodata", ".data", ".bss",
                              ".rel.text", ".rel.data", ".symtab",
                              ".strtab", ".shstrtab" };
        int k;
        for (k = 0; k < 10; k++)
            noff[k] = strtab_off(&shstr, nm[k]);
    }

    /* string table: index 0 = empty string */
    bemit1(&strtab, 0);

    /* symbol table: index 0 = null entry */
    for (i = 0; i < 16; i++)
        bemit1(&symtab, 0);

    /* pass 1: locals, pass 2: globals */
    firstglobal = 1;
    for (i = 0; i < nosym; i++)
        if (osym[i].bind == 0)
            firstglobal++;
    {
        int pass;
        int symi = 1;
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < nosym; i++) {
                int islocal = osym[i].bind == 0;
                int stype;
                if (islocal != (pass == 0))
                    continue;
                omap[i] = symi;
                symi++;
                stype = osym[i].type == 2 ? STT_FUNC : STT_OBJECT;
                be32(&symtab, strtab_off(&strtab, osym[i].name));
                be32(&symtab, osym[i].val);
                be32(&symtab, osym[i].size);
                bemit1(&symtab, ((islocal ? STB_LOCAL : STB_GLOBAL) << 4) | stype);
                bemit1(&symtab, 0);
                be16(&symtab, osym[i].shndx);
            }
        }
        symcnt = symi;
    }

    /* relocations */
    for (i = 0; i < norel; i++) {
        Buf* rb = orel[i].sec == 1 ? &relt : &reld;
        be32(rb, orel[i].off);
        be32(rb, ((u32)omap[orel[i].sym] << 8) | orel[i].type);
    }

    out->len = 0;
    breserve(out, textlen + rodlen + datlen + 16384);

    /* emit sections, capturing their real file offsets from the cursor */
    for (i = 0; i < 52; i++)
        bemit1(out, 0);
    {
        u32 realoff[10];
        realoff[1] = out->len; bemit(out, secs[0].data, textlen);
        realoff[2] = out->len; bemit(out, secs[1].data, rodlen);
        realoff[3] = out->len; bemit(out, secs[2].data, datlen);
        realoff[4] = out->len;                        /* bss: no file bytes */
        realoff[5] = out->len; bemit(out, relt.data, relt.len);
        realoff[6] = out->len; bemit(out, reld.data, reld.len);
        realoff[7] = out->len; bemit(out, symtab.data, symtab.len);
        realoff[8] = out->len; bemit(out, strtab.data, strtab.len);
        realoff[9] = out->len; bemit(out, shstr.data, shstr.len);
        shoff = out->len;

        ft = realoff[1];
        fro = realoff[2];
        fda = realoff[3];
        frl = realoff[5];
        frd = realoff[6];
        fsy = realoff[7];
        fstr = realoff[8];
        fshs = realoff[9];
    }

    /* section header table (9 x 40 bytes) */
    {
        u32 sh[10][10];
        memset(sh, 0, sizeof(sh));
        sh[1][0] = noff[1]; sh[1][1] = SHT_PROGBITS;
        sh[1][2] = SHF_ALLOC | SHF_EXECINSTR;
        sh[1][4] = ft; sh[1][5] = textlen; sh[1][8] = 16;

        sh[2][0] = noff[2]; sh[2][1] = SHT_PROGBITS; sh[2][2] = SHF_ALLOC;
        sh[2][4] = fro; sh[2][5] = rodlen; sh[2][8] = 4;

        sh[3][0] = noff[3]; sh[3][1] = SHT_PROGBITS;
        sh[3][2] = SHF_ALLOC | SHF_WRITE;
        sh[3][4] = fda; sh[3][5] = datlen; sh[3][8] = 4;

        sh[4][0] = noff[4]; sh[4][1] = SHT_NOBITS;
        sh[4][2] = SHF_ALLOC | SHF_WRITE;
        sh[4][4] = frl; sh[4][5] = bss_size;
        sh[4][8] = bss_align ? bss_align : 4;

        sh[5][0] = noff[5]; sh[5][1] = SHT_REL;
        sh[5][4] = frl; sh[5][5] = relt.len;
        sh[5][6] = 7; sh[5][7] = 1; sh[5][8] = 4; sh[5][9] = 8;

        sh[6][0] = noff[6]; sh[6][1] = SHT_REL;
        sh[6][4] = frd; sh[6][5] = reld.len;
        sh[6][6] = 7; sh[6][7] = 3; sh[6][8] = 4; sh[6][9] = 8;

        sh[7][0] = noff[7]; sh[7][1] = SHT_SYMTAB;
        sh[7][4] = fsy; sh[7][5] = symtab.len;
        sh[7][6] = 8; sh[7][7] = firstglobal; sh[7][8] = 4; sh[7][9] = 16;

        sh[8][0] = noff[8]; sh[8][1] = SHT_STRTAB;
        sh[8][4] = fstr; sh[8][5] = strtab.len; sh[8][8] = 1;

        sh[9][0] = noff[9]; sh[9][1] = SHT_STRTAB;
        sh[9][4] = fshs; sh[9][5] = shstr.len; sh[9][8] = 1;

        for (i = 0; i <= 9; i++) {
            int j;
            for (j = 0; j < 10; j++)
                be32(out, sh[i][j]);
        }
    }

    /* patch ELF header */
    {
        u8* e = out->data;
        static const u8 id[16] = { 0x7F, 'E', 'L', 'F', 1, 1, 1, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0 };
        for (i = 0; i < 16; i++)
            e[i] = id[i];
        e[16] = ET_REL; e[17] = 0;
        e[18] = EM_386; e[19] = 0;
        e[20] = 1; e[21] = 0; e[22] = 0; e[23] = 0;
        e[24] = 0; e[25] = 0; e[26] = 0; e[27] = 0;
        e[28] = 0; e[29] = 0; e[30] = 0; e[31] = 0;
        e[32] = shoff & 0xFF; e[33] = (shoff >> 8) & 0xFF;
        e[34] = (shoff >> 16) & 0xFF; e[35] = (shoff >> 24) & 0xFF;
        e[36] = 0; e[37] = 0; e[38] = 0; e[39] = 0;
        e[40] = 52; e[41] = 0;
        e[42] = 0; e[43] = 0;
        e[44] = 0; e[45] = 0;
        e[46] = 40; e[47] = 0;
        e[48] = 10; e[49] = 0;
        e[50] = 9; e[51] = 0;
    }
    (void)symcnt;
    (void)cur;
}

/* helper: clear a Buf (length 0) */
static void strtab_reset(Buf* b)
{
    b->len = 0;
}

/* ================================================================== */
/* object file reader (ELF32 relocatable)                              */
/* ================================================================== */

typedef struct {
    u8* buf;
    int len;
    u32 shoff;
    int shnum;
    u32 shstrndx;
} ElfF;

typedef struct {
    u32 name, type, flags, addr, offset, size, link, info, addralign, entsize;
} ElfSH;

typedef struct {
    char** sn;      /* symbol names */
    u32* sv;        /* values */
    u32* ss;        /* sizes */
    u8* ssh;        /* section index */
    u8* sb;         /* bind */
    u8* st;         /* type */
    int nsym;
    int* rsec;      /* output sec for each reloc: 0 text,1 ro,2 data,-1 sk */
    u32* roff;
    int* rsym;
    int* rtyp;
    int nrel;
    u8* txt; int tlen;
    u8* rod; int rlen;
    u8* dat; int dlen;
    int bss; int bssal;
    u8 secmap[64];   /* shndx -> 0 text,1 ro,2 data,4 bss, or 0xFF ignore */
    u8* mangled;
} Obj;

static u32 u32at(const u8* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}
static u16 u16at(const u8* p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static void obj_free(Obj* o)
{
    int i;
    for (i = 0; i < o->nsym; i++)
        if (o->sn[i])
            free(o->sn[i]);
    free(o->sn);
    free(o->sv);
    free(o->ss);
    free(o->ssh);
    free(o->sb);
    free(o->st);
    free(o->rsec);
    free(o->roff);
    free(o->rsym);
    free(o->rtyp);
    free(o->txt);
    free(o->rod);
    free(o->dat);
}

static char* mstrdup(const char* s)
{
    int n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    memcpy(p, s, n);
    return p;
}

static void obj_read(const u8* f, int len, Obj* o)
{
    ElfSH sh[64];
    int n;
    int texti = -1, roi = -1, dati = -1;
    u32 shoff, shnum, shstr;
    u32 stoff, stofflen;
    int i;
    memset(o, 0, sizeof(*o));
    {
        int k;
        for (k = 0; k < 64; k++)
            o->secmap[k] = 0xFF;
    }
    if (len < 52 || f[0] != 0x7F || f[1] != 'E' || f[2] != 'L' || f[3] != 'F') {
        printf("vcc: not an ELF object\n");
        exit(1);
    }
    shoff = u32at(f + 32);
    shnum = u16at(f + 48);
    shstr = u16at(f + 50);
    if (shnum > 64) {
        printf("vcc: object has too many sections\n");
        exit(1);
    }
    for (i = 0; i < shnum; i++) {
        const u8* p = f + shoff + i * 40;
        sh[i].name = u32at(p);
        sh[i].type = u32at(p + 4);
        sh[i].flags = u32at(p + 8);
        sh[i].addr = u32at(p + 12);
        sh[i].offset = u32at(p + 16);
        sh[i].size = u32at(p + 20);
        sh[i].link = u32at(p + 24);
        sh[i].info = u32at(p + 28);
        sh[i].addralign = u32at(p + 32);
        sh[i].entsize = u32at(p + 36);
    }
    /* section names */
    {
        u32 s = sh[shstr].offset;
        int k;
        for (k = 0; k < shnum; k++) {
            const char* nm = (const char*)(f + s + sh[k].name);
            if (strcmp(nm, ".text") == 0)
                texti = k;
            else if (strcmp(nm, ".data") == 0)
                dati = k;
else if (roi < 0 &&
                 (strcmp(nm, ".rodata") == 0 || strcmp(nm, ".eh_frame") == 0 ||
                  strcmp(nm, ".note.gnu.property") == 0))
                roi = k;
        }
    }
    if (texti < 0 && dati < 0 && roi < 0) {
        printf("vcc: object has no code\n");
        exit(1);
    }
    if (shstr >= shnum) {
        printf("vcc: bad string table index\n");
        exit(1);
    }
    if (texti >= 0) o->secmap[texti] = 0;
    if (roi >= 0) o->secmap[roi] = 1;
    if (dati >= 0) o->secmap[dati] = 2;
    {
        int k;
        for (k = 1; k < shnum; k++) {
            if (sh[k].type == SHT_NOBITS && o->secmap[k] == 0xFF) {
                if ((sh[k].flags & 3) == 3) {   /* WA: write|alloc */
                    o->secmap[k] = 4;          /* bss */
                }
            }
        }
    }
    /* copy content sections */
    if (texti >= 0 && sh[texti].type == SHT_PROGBITS) {
        int z = sh[texti].size;
        o->txt = (u8*)malloc(z ? z : 1);
        memcpy(o->txt, f + sh[texti].offset, z);
        o->tlen = z;
    }
    if (roi >= 0 && sh[roi].type == SHT_PROGBITS) {
        int z = sh[roi].size;
        o->rod = (u8*)malloc(z ? z : 1);
        memcpy(o->rod, f + sh[roi].offset, z);
        o->rlen = z;
    }
    if (dati >= 0 && sh[dati].type == SHT_PROGBITS) {
        int z = sh[dati].size;
        o->dat = (u8*)malloc(z ? z : 1);
        memcpy(o->dat, f + sh[dati].offset, z);
        o->dlen = z;
    }
    if (texti >= 0 && sh[texti].type == SHT_NOBITS) {
        o->bss = sh[texti].size;
        o->bssal = sh[texti].addralign;
    } else if (dati >= 0 && sh[dati].type == SHT_NOBITS) {
        o->bss = sh[dati].size;
        o->bssal = sh[dati].addralign;
    }
    /* symbol table */
    stoff = 0; stofflen = 0;
    for (i = 0; i < shnum; i++) {
        int cap = 8;
        if (sh[i].type == SHT_SYMTAB) {
            stoff = sh[i].offset;
            stofflen = sh[i].size;
            /* link -> strtab index */
            o->nsym = sh[i].size / 16;
            o->sn = (char**)malloc(sizeof(char*) * (o->nsym ? o->nsym : 1));
            o->sv = (u32*)malloc(sizeof(u32) * (o->nsym ? o->nsym : 1));
            o->ss = (u32*)malloc(sizeof(u32) * (o->nsym ? o->nsym : 1));
            o->ssh = (u8*)malloc(o->nsym ? o->nsym : 1);
            o->sb = (u8*)malloc(o->nsym ? o->nsym : 1);
            o->st = (u8*)malloc(o->nsym ? o->nsym : 1);
            n = sh[i].link;
            {
                const u8* str = f + sh[n].offset;
                int k;
                for (k = 0; k < o->nsym; k++) {
                    const u8* e = f + stoff + k * 16;
                    u32 no = u32at(e);
                    o->sn[k] = mstrdup((const char*)(str + no));
                    o->sv[k] = u32at(e + 4);
                    o->ss[k] = u32at(e + 8);
                    o->ssh[k] = (u8)u16at(e + 14);
                    o->sb[k] = (u8)(e[12] >> 4);
                    o->st[k] = (u8)e[13];
                }
            }
            stofflen = 0;
            (void)cap;
            break;
        }
    }
    /* relocations (two passes: count, then fill) */
    {
        int j;
        int total = 0;
        for (i = 0; i < shnum; i++)
            if (sh[i].type == SHT_REL)
                total += sh[i].size / 8;
        o->rsec = (int*)malloc(sizeof(int) * (total ? total : 1));
        o->roff = (u32*)malloc(sizeof(u32) * (total ? total : 1));
        o->rsym = (int*)malloc(sizeof(int) * (total ? total : 1));
        o->rtyp = (int*)malloc(sizeof(int) * (total ? total : 1));
        o->nrel = 0;
        for (i = 0; i < shnum; i++) {
            int tgt;
            int osec;
            int z;
            int base;
            const u8* p;
            if (sh[i].type != SHT_REL)
                continue;
            tgt = (int)sh[i].info;
            osec = -1;
            if (tgt == texti)
                osec = 0;
            else if (tgt == dati)
                osec = 2;
            else if (tgt == roi)
                osec = 1;
            z = sh[i].size / 8;
            base = o->nrel;
            p = f + sh[i].offset;
            for (j = 0; j < z; j++) {
                o->roff[base + j] = u32at(p + 8 * j);
                o->rtyp[base + j] = (int)(u32at(p + 8 * j + 4) & 0xFF);
                o->rsym[base + j] = (int)(u32at(p + 8 * j + 4) >> 8);
                o->rsec[base + j] = osec;
            }
            o->nrel += z;
        }
    }
    (void)stofflen;
    (void)n;
}

/* ================================================================== */
/* ar archive reader                                                  */
/* ================================================================== */

static u8* read_big(const char* path, int* len)
{
    u8* p;
    int fd;
    int cap = 65536;
    int n = 0;
    fd = open(path, 0);
    if (fd < 0)
        return 0;
    p = (u8*)malloc(cap);
    for (;;) {
        int r;
        if (n + 512 > cap) {
            u8* np;
            cap *= 2;
            np = (u8*)malloc(cap);
            if (n)
                memcpy(np, p, n);
            free(p);
            p = np;
        }
        r = read(fd, p + n, 512);
        if (r <= 0)
            break;
        n += r;
    }
    close(fd);
    *len = n;
    return p;
}

typedef struct {
    u8* data;
    int len;
} AMem;

/* collect object-member (u8*,len) pairs from an archive image */
static int arch_members(const u8* a, int alen, AMem* out, int maxout)
{
    int pos = 0;
    int cnt = 0;
    if (alen < 8) return 0;
    {
        const char* magic = "!<arch>\n";
        int k;
        for (k = 0; k < 8; k++)
            if (a[k] != (u8)magic[k])
                return 0;
    }
    pos = 8;
    while (pos + 60 <= alen) {
        const u8* h = a + pos;
        u32 size;
        char name[17];
        memcpy(name, h, 16);
        name[16] = 0;
        {
            int k;
            for (k = 15; k >= 0; k--)
                if (k > 0 && name[k] == ' ')
                    name[k] = 0;
        }
        size = (u32)atoi((const char*)(h + 48));
        if (h[58] != '`' || h[59] != '\n') {
            printf("vcc: bad archive member header at pos=%d\n", pos);
            exit(1);
        }
        if (strcmp(name, "/") == 0 || strcmp(name, "//") == 0 ||
            name[0] == 0) {
            /* symbol table / long names / padding: skip */
        } else {
            if (cnt < maxout) {
                out[cnt].data = (u8*)(a + pos + 60);
                out[cnt].len = (int)size;
                cnt++;
            }
        }
        pos += 60 + size + (size & 1);
    }
    return cnt;
}

/* ================================================================== */
/* linker + executable writer                                          */
/* ================================================================== */

typedef struct {
    char* name;
    u32 val;       /* absolute virtual address (allocation sections) */
} GSym;

static GSym gsyms[1024];
static int ngsyms;

static void gsym_def(const char* name, u32 val)
{
    int i;
    for (i = 0; i < ngsyms; i++)
        if (strcmp(gsyms[i].name, name) == 0)
            return;   /* first definition wins */
    if (ngsyms >= 1024) {
        printf("vcc: too many global symbols\n");
        exit(1);
    }
    gsyms[ngsyms].name = mstrdup(name);
    gsyms[ngsyms].val = val;
    ngsyms++;
}

static int gsym_lookup(const char* name)
{
    int i;
    for (i = 0; i < ngsyms; i++)
        if (strcmp(gsyms[i].name, name) == 0)
            return 1;
    return 0;
}
static u32 gsym_val(const char* name)
{
    int i;
    for (i = 0; i < ngsyms; i++)
        if (strcmp(gsyms[i].name, name) == 0)
            return gsyms[i].val;
    return 0;
}

#define LOAD_BASE 0x400000

/* place objects, resolve relocations, emit an ET_EXEC image */
static int link_all(Obj** objs, int nobjs, Buf* out, u32* entry_out)
{
    u32* obtext;
    u32* obro;
    u32* obdat;
    u32* obbss;
    u32 cur = LOAD_BASE;
    u32 text_end = 0, dat_end = 0, bss_end = 0;
    int i, j;
    u32 entry = 0;

    obtext = (u32*)malloc(sizeof(u32) * (nobjs ? nobjs : 1));
    obro = (u32*)malloc(sizeof(u32) * (nobjs ? nobjs : 1));
    obdat = (u32*)malloc(sizeof(u32) * (nobjs ? nobjs : 1));
    obbss = (u32*)malloc(sizeof(u32) * (nobjs ? nobjs : 1));

    /* layout */
    for (i = 0; i < nobjs; i++) {
        int a = 16;
        cur = (cur + a - 1) & ~(a - 1);
        obtext[i] = cur;
        cur += objs[i]->tlen;
        cur = (cur + a - 1) & ~(a - 1);
        obro[i] = cur;
        cur += objs[i]->rlen;
        cur = (cur + a - 1) & ~(a - 1);
        obdat[i] = cur;
        cur += objs[i]->dlen;
        cur = (cur + a - 1) & ~(a - 1);
        obbss[i] = cur;
        cur += objs[i]->bss;
    }
    text_end = obro[0];                  /* not used heavily */
    dat_end = cur - objs[nobjs - 1]->bss;

    /* pass 1: register global symbols on first definition */
    ngsyms = 0;
    for (i = 0; i < nobjs; i++) {
        Obj* o = objs[i];
        for (j = 0; j < o->nsym; j++) {
            u32 secbase;
            int sh = o->ssh[j];
            if (o->sb[j] != STB_GLOBAL)
                continue;
            if (o->ssh[j] == 0)
                continue;
            if (sh >= 64)
                continue;
            switch (o->secmap[sh]) {
            case 0: secbase = obtext[i]; break;
            case 1: secbase = obro[i]; break;
            case 2: secbase = obdat[i]; break;
            case 4: secbase = obbss[i]; break;
            default: secbase = 0;
            }
            if (o->secmap[sh] == 0xFF) {
                printf("vcc: global in ignored section\n");
                free(obtext);
                free(obro);
                free(obdat);
                free(obbss);
                return -1;
            }
            if (strcmp(o->sn[j], "_start") == 0 && entry == 0)
                entry = secbase + o->sv[j];
            gsym_def(o->sn[j], secbase + o->sv[j]);
        }
    }

    /* pass 2: resolve relocations */
    for (i = 0; i < nobjs; i++) {
        Obj* o = objs[i];
        for (j = 0; j < o->nrel; j++) {
            u32 base;
            u32 S, A, P;
            u8* sec;
            int secsz;
            u32 off;
            int symidx = o->rsym[j];
            if (o->rsec[j] < 0)
                continue;
            switch (o->rsec[j]) {
            case 0: base = obtext[i]; sec = o->txt; secsz = o->tlen; break;
            case 1: base = obro[i]; sec = o->rod; secsz = o->rlen; break;
            default: base = obdat[i]; sec = o->dat; secsz = o->dlen; break;
            }
            off = o->roff[j];
            if (symidx < 0 || symidx >= o->nsym) {
                printf("vcc: bad relocation\n");
                free(obtext);
                free(obro);
                free(obdat);
                free(obbss);
                return -1;
            }
            if (o->ssh[symidx] != 0) {
                u32 sbase;
                int sh = o->ssh[symidx];
                if (sh >= 64 || o->secmap[sh] == 0xFF) {
                    printf("vcc: relocation into ignored section: sym='%s'\n", o->sn[symidx]);
                    free(obtext);
                    free(obro);
                    free(obdat);
                    free(obbss);
                    return -1;
                }
                switch (o->secmap[sh]) {
                case 0: sbase = obtext[i]; break;
                case 1: sbase = obro[i]; break;
                case 2: sbase = obdat[i]; break;
                case 4: sbase = obbss[i]; break;
                default: sbase = 0;
                }
                if (o->st[symidx] == 3)
                    S = sbase;          /* STT_SECTION: offset is in addend */
                else
                    S = sbase + o->sv[symidx];
            } else {
                if (!gsym_lookup(o->sn[symidx])) {
                    printf("vcc: undefined symbol '%s'\n", o->sn[symidx]);
                    free(obtext);
                    free(obro);
                    free(obdat);
                    free(obbss);
                    return -1;
                }
                S = gsym_val(o->sn[symidx]);
            }
            if ((int)off + 4 > secsz) {
                printf("vcc: relocation out of range\n");
                return -1;
            }
            A = u32at(sec + off);
            P = base + off;
            if (o->rtyp[j] == 2) {
                u32 v = S + A - P;
                sec[off] = v & 0xFF;
                sec[off + 1] = (v >> 8) & 0xFF;
                sec[off + 2] = (v >> 16) & 0xFF;
                sec[off + 3] = (v >> 24) & 0xFF;
            } else if (o->rtyp[j] == 1) {
                u32 v = S + A;
                sec[off] = v & 0xFF;
                sec[off + 1] = (v >> 8) & 0xFF;
                sec[off + 2] = (v >> 16) & 0xFF;
                sec[off + 3] = (v >> 24) & 0xFF;
            }
        }
    }

    /* emit executable image */
    {
        int i2;
        u32 content = 0;
        u32 p_offset = 84;
        u32 p_filesz, p_memsz;
        u32 lastend = 0;
        out->len = 0;
        breserve(out, 84);
        for (i2 = 0; i2 < 84; i2++)
            bemit1(out, 0);
        for (i2 = 0; i2 < nobjs; i2++) {
            /* pad to obtext alignment inside file (16-byte aligned) */
            while (out->len < (int)(p_offset + (obtext[i2] - LOAD_BASE)))
                bemit1(out, 0);
            if (objs[i2]->tlen)
                bemit(out, objs[i2]->txt, objs[i2]->tlen);
            if (objs[i2]->rlen)
                bemit(out, objs[i2]->rod, objs[i2]->rlen);
            if (objs[i2]->dlen)
                bemit(out, objs[i2]->dat, objs[i2]->dlen);
        }
        p_filesz = out->len - p_offset;
        /* memsz includes bss tail */
        {
            u32 b = obbss[nobjs - 1] + objs[nobjs - 1]->bss;
            p_memsz = p_filesz + (b - (LOAD_BASE + p_filesz));
        }
        lastend = LOAD_BASE + p_filesz;
        (void)lastend;
        (void)text_end;
        (void)dat_end;
        (void)content;

        /* elf header */
        {
            u8* e = out->data;
            static const u8 id[16] = { 0x7F, 'E', 'L', 'F', 1, 1, 1, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0 };
            for (i2 = 0; i2 < 16; i2++)
                e[i2] = id[i2];
            e[16] = ET_EXEC; e[17] = 0;
            e[18] = EM_386; e[19] = 0;
            e[20] = 1; e[21] = 0; e[22] = 0; e[23] = 0;
            e[24] = entry & 0xFF; e[25] = (entry >> 8) & 0xFF;
            e[26] = (entry >> 16) & 0xFF; e[27] = (entry >> 24) & 0xFF;
            e[28] = 52; e[29] = 0; e[30] = 0; e[31] = 0;   /* e_phoff */
            e[32] = 0; e[33] = 0; e[34] = 0; e[35] = 0;
            e[36] = 0; e[37] = 0; e[38] = 0; e[39] = 0;
            e[40] = 52; e[41] = 0;
            e[42] = 32; e[43] = 0;                          /* phentsize */
            e[44] = 1; e[45] = 0;                           /* phnum */
            e[46] = 0; e[47] = 0;
            e[48] = 0; e[49] = 0;
            e[50] = 0; e[51] = 0;
            /* program header at file offset 52, size 32 */
            e[52] = 1; e[53] = 0; e[54] = 0; e[55] = 0;     /* p_type */
            e[56] = p_offset & 0xFF; e[57] = (p_offset >> 8) & 0xFF;
            e[58] = (p_offset >> 16) & 0xFF; e[59] = (p_offset >> 24) & 0xFF;
            {
                u32 va = LOAD_BASE;
                e[60] = va & 0xFF; e[61] = (va >> 8) & 0xFF;
                e[62] = (va >> 16) & 0xFF; e[63] = (va >> 24) & 0xFF;
                e[64] = va & 0xFF; e[65] = (va >> 8) & 0xFF;
                e[66] = (va >> 16) & 0xFF; e[67] = (va >> 24) & 0xFF;
            }
            e[68] = p_filesz & 0xFF; e[69] = (p_filesz >> 8) & 0xFF;
            e[70] = (p_filesz >> 16) & 0xFF; e[71] = (p_filesz >> 24) & 0xFF;
            e[72] = p_memsz & 0xFF; e[73] = (p_memsz >> 8) & 0xFF;
            e[74] = (p_memsz >> 16) & 0xFF; e[75] = (p_memsz >> 24) & 0xFF;
            e[76] = 7; e[77] = 0; e[78] = 0; e[79] = 0;     /* flags RWX */
            e[80] = 0x00; e[81] = 0x10; e[82] = 0x00; e[83] = 0x00; /* align */
        }
        *entry_out = entry;
    }
    free(obtext);
    free(obro);
    free(obdat);
    free(obbss);
    return 0;
}

/* embedded library access (crt0.o + libvlibc.a) */
#define LIBCRT0_SYM crt0_lib
#define LIBVLIBC_SYM libvlibc_lib
#ifdef VCC_LIB_EMBEDDED
extern const unsigned char LIBCRT0_SYM[];
extern const unsigned int LIBCRT0_SYM##_len;
extern const unsigned char LIBVLIBC_SYM[];
extern const unsigned int LIBVLIBC_SYM##_len;
#define libcrt0_data() LIBCRT0_SYM
#define libcrt0_len() LIBCRT0_SYM##_len
#define libvlibc_data() LIBVLIBC_SYM
#define libvlibc_len() LIBVLIBC_SYM##_len
#else
static u8* hcrt0;
static int hcrt0len;
static u8* hlibc;
static int hlibclen;
/* Host builds run from the repo root and read ./sysroot/lib; the guest
 * build reads the same bytes from the mounted /lib. Try both. */
static u8* read_lib(const char* rel, const char* abs, int* len)
{
    u8* p = read_big(rel, len);
    if (p)
        return p;
    return read_big(abs, len);
}
#define libcrt0_data() (hcrt0 ? hcrt0 : (hcrt0 = read_lib("./sysroot/lib/crt0.o", "/lib/crt0.o", &hcrt0len), hcrt0))
#define libcrt0_len() hcrt0len
#define libvlibc_data() (hlibc ? hlibc : (hlibc = read_lib("./sysroot/lib/libvlibc.a", "/lib/libvlibc.a", &hlibclen), hlibc))
#define libvlibc_len() hlibclen
#endif

/* ================================================================== */
/* driver                                                              */
/* ================================================================== */

#define NHASH_MACRO NHASH

static void compile_reset(void)
{
    int i;
    arena_used = 0;
    type_init();
    ntok = 0;
    tok_cap = 0;
    tpos = 0;
    tline = 0;
    nbuf = 0;
    pend_n = 0;
    pend_r = 0;
    n_open = 0;
    ndepth = -1;
    nosym = 0;
    norel = 0;
    for (i = 0; i < NHASH_MACRO; i++)
        mtab[i] = 0;
    for (i = 0; i < 3; i++) {
        secs[i].data = 0;
        secs[i].len = 0;
        secs[i].cap = 0;
    }
    bss_size = 0;
    bss_align = 1;
    label_count = 0;
    frame_used = 0;
    sym_name = 0;
    inner_ptr_count = 0;
    scope = 0;
    ntags = 0;
    ntdefs = 0;
    ngvars = 0;
    nlabels = 0;
    ncase_rec = 0;
    nlvl = 0;
    cur_storage = 0;
    is_typedef = 0;
}

/* compile one .c file to a relocatable object file */
static int compile_file(const char* srcpath, const char* objpath)
{
    char* text;
    int len;
    Buf obj;
    int fd;
    int i;
    binit(&obj);
    compile_reset();
    text = read_file(srcpath, &len);
    if (!text) {
        printf("vcc: cannot open '%s'\n", srcpath);
        return 1;
    }
    pp_run(text, len, ".");
    tpos = 0;
    parse_file();
    write_object(&obj);
    fd = open(objpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("vcc: cannot write '%s'\n", objpath);
        return 1;
    }
    i = write(fd, obj.data, obj.len);
    close(fd);
    if (i != obj.len) {
        printf("vcc: short write to '%s'\n", objpath);
        return 1;
    }
    return 0;
}


static int ends_with(const char* s, const char* suf)
{
    int sl = strlen(s);
    int fl = strlen(suf);
    return sl >= fl && strcmp(s + sl - fl, suf) == 0;
}

int MAIN(int argc, char** argv)
{
    const char* outpath2 = 0;
    int compile_only = 0;
    int i;
    Obj* objs[64];
    int nobjs = 0;
    Buf finalout;
    u32 entry = 0;
    binit(&finalout);

    if (argc < 2) {
        printf("usage: vcc [-c] [-o out] [-I dir] file.c [file.o] [lib.a]\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] == '-' && a[1] == 'o' && a[2] == 0) {
            if (i + 1 < argc)
                outpath2 = argv[++i];
        } else if (a[0] == '-' && a[1] == 'c' && a[2] == 0) {
            compile_only = 1;
        } else if (a[0] == '-' && a[1] == 'I' && a[2] == 0) {
            if (i + 1 < argc && nincdirs < 16)
                incdirs[nincdirs++] = argv[++i];
        }
    }
    if (!outpath2)
        outpath2 = compile_only ? "a.o" : "a.out";

    if (compile_only) {
        for (i = 1; i < argc; i++) {
            const char* a = argv[i];
            if (a[0] == '-')
                continue;
            if (outpath2 && strcmp(a, outpath2) == 0)
                continue;
            if (ends_with(a, ".c"))
                return compile_file(a, outpath2);
        }
        printf("vcc: no input file\n");
        return 1;
    }

    /* full link: crt0.o + *.o + *.c + every object in libvlibc.a */
    /* first compile each .c in argv to an in-memory object */
    {
        char tmpname[64];
        int oi = 0;
        for (i = 1; i < argc; i++) {
            const char* a = argv[i];
            if (a[0] == '-' || !ends_with(a, ".c"))
                continue;
            (void)oi;
            memcpy(tmpname, "vcc_tmp.o", 10);
            if (compile_file(a, tmpname) != 0)
                return 1;
            {
                int tl;
                u8* tb = read_big(tmpname, &tl);
                Obj* o = (Obj*)malloc(sizeof(Obj));
                obj_read(tb, tl, o);
                objs[nobjs++] = o;
                free(tb);
            }
            break;
        }
    }
    /* add precompiled .o / .a args in order */
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        int tl;
        u8* tb;
        if (a[0] == '-')
            continue;
        if (ends_with(a, ".c"))
            continue;
        if (outpath2 && strcmp(a, outpath2) == 0)
            continue;
        tb = read_big(a, &tl);
        if (!tb) {
            printf("vcc: cannot read '%s'\n", a);
            return 1;
        }
        if (ends_with(a, ".a")) {
            AMem mems[128];
            int n = arch_members(tb, tl, mems, 128);
            int k;
            for (k = 0; k < n && nobjs < 64; k++) {
                Obj* o = (Obj*)malloc(sizeof(Obj));
                obj_read(mems[k].data, mems[k].len, o);
                objs[nobjs++] = o;
            }
            free(tb);
        } else {
            Obj* o = (Obj*)malloc(sizeof(Obj));
            obj_read(tb, tl, o);
            objs[nobjs++] = o;
            free(tb);
        }
    }
    /* append crt0.o then all of libvlibc.a */
    {
        AMem mems[128];
        int n;
        int k;
        u8* cd = libcrt0_data();
        Obj* o;
        if (!cd) {
            printf("vcc: crt0.o unavailable\n");
            return 1;
        }
        o = (Obj*)malloc(sizeof(Obj));
        obj_read(cd, libcrt0_len(), o);
        objs[nobjs++] = o;
        {
            /* sequence the data()/len() calls: arg order is unspecified */
            u8* libd = libvlibc_data();
            int liblen = libvlibc_len();
            n = arch_members(libd, liblen, mems, 128);
        }
        for (k = 0; k < n && nobjs < 64; k++) {
            Obj* o2 = (Obj*)malloc(sizeof(Obj));
            obj_read(mems[k].data, mems[k].len, o2);
            objs[nobjs++] = o2;
        }
    }
    if (nobjs == 0) {
        printf("vcc: nothing to link\n");
        return 1;
    }
    if (link_all(objs, nobjs, &finalout, &entry) != 0)
        return 1;
    {
        int fd = open(outpath2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int w;
        if (fd < 0) {
            printf("vcc: cannot write '%s'\n", outpath2);
            return 1;
        }
        w = write(fd, finalout.data, finalout.len);
        close(fd);
        if (w != finalout.len) {
            printf("vcc: short write to '%s'\n", outpath2);
            return 1;
        }
    }
    return 0;
}
