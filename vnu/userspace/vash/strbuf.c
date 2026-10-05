/* strbuf.c */
#include "shell.h"
void str_init(Str* s)
{
    s->p = (char*)malloc(32);
    s->len = 0;
    s->cap = 32;
    if (s->p)
        s->p[0] = 0;
}

void str_free(Str* s)
{
    free(s->p);
    s->p = 0;
    s->len = 0;
    s->cap = 0;
}

void str_reset(Str* s)
{
    s->len = 0;
    if (s->p)
        s->p[0] = 0;
}

static void str_grow(Str* s, int need)
{
    if (s->len + need + 1 <= s->cap)
        return;
    int cap = s->cap ? s->cap : 32;
    while (cap < s->len + need + 1)
        cap *= 2;
    char* np = (char*)malloc((unsigned long)cap);
    if (!np)
        return;
    if (s->p) {
        memcpy(np, s->p, (unsigned long)s->len);
        free(s->p);
    }
    np[s->len] = 0;
    s->p = np;
    s->cap = cap;
}

void str_addc(Str* s, char c)
{
    str_grow(s, 1);
    if (s->len < s->cap - 1) {
        s->p[s->len++] = c;
        s->p[s->len] = 0;
    }
}

void str_addn(Str* s, const char* t, int n)
{
    if (n <= 0 || !t)
        return;
    str_grow(s, n);
    for (int i = 0; i < n && s->len < s->cap - 1; ++i)
        s->p[s->len++] = t[i];
    s->p[s->len] = 0;
}

void str_adds(Str* s, const char* t)
{
    if (!t)
        return;
    int n = (int)strlen(t);
    str_addn(s, t, n);
}

void str_set(Str* s, const char* t)
{
    str_reset(s);
    str_adds(s, t);
}

char* str_cstr(Str* s)
{
    if (!s->p)
        return 0;
    if (s->len >= s->cap)
        str_grow(s, 1);
    s->p[s->len] = 0;
    return s->p;
}

char* sh_strdup(const char* s)
{
    if (!s)
        return 0;
    int n = (int)strlen(s);
    char* p = (char*)malloc((unsigned long)n + 1);
    if (!p)
        return 0;
    memcpy(p, s, (unsigned long)n);
    p[n] = 0;
    return p;
}

long sh_atol(const char* s)
{
    if (!s)
        return 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t')
        ++s;
    if (*s == '-') {
        sign = -1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        ++s;
    }
    return sign * v;
}

void sh_utoa(unsigned long v, Str* out)
{
    if (!out)
        return;
    str_reset(out);
    if (v == 0) {
        str_addc(out, '0');
        return;
    }
    char buf[32];
    int i = 0;
    while (v > 0 && i < 31) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (i--)
        str_addc(out, buf[i]);
}

void sh_itoa(long v, Str* out)
{
    if (!out)
        return;
    /* The magnitude goes in first and the sign is moved in front of it:
     * sh_utoa() starts by resetting the string, so a `-' written before
     * the call would be wiped. */
    int neg = v < 0;
    unsigned long m = neg ? (unsigned long)(-(v + 1)) + 1ul : (unsigned long)v;
    sh_utoa(m, out);
    if (!neg)
        return;
    str_addc(out, ' ');
    int i = out->len - 1;
    while (i > 0) {
        out->p[i] = out->p[i - 1];
        --i;
    }
    out->p[0] = '-';
}

void vec_init(Vec* v)
{
    v->v = 0;
    v->n = 0;
    v->cap = 0;
}

void vec_push(Vec* v, void* p)
{
    if (v->n >= v->cap) {
        int cap = v->cap ? v->cap : 8;
        while (cap <= v->n)
            cap *= 2;
        void** np = (void**)malloc((unsigned long)cap * sizeof(void*));
        if (!np)
            return;
        if (v->v) {
            for (int i = 0; i < v->n; ++i)
                np[i] = v->v[i];
            free(v->v);
        }
        v->v = np;
        v->cap = cap;
    }
    v->v[v->n++] = p;
}

void vec_free(Vec* v)
{
    free(v->v);
    v->v = 0;
    v->n = 0;
    v->cap = 0;
}

int sh_starts(const char* s, const char* p)
{
    if (!s || !p)
        return 0;
    int ls = (int)strlen(s);
    int lp = (int)strlen(p);
    if (lp > ls)
        return 0;
    for (int i = 0; i < lp; ++i)
        if (s[i] != p[i])
            return 0;
    return 1;
}

int sh_ends(const char* s, const char* p)
{
    if (!s || !p)
        return 0;
    int ls = (int)strlen(s);
    int lp = (int)strlen(p);
    if (lp > ls)
        return 0;
    for (int i = 0; i < lp; ++i)
        if (s[ls - lp + i] != p[i])
            return 0;
    return 1;
}

const char* sh_find(const char* s, const char* p)
{
    if (!s || !p || !p[0])
        return 0;
    for (const char* t = s; *t; ++t) {
        int ok = 1;
        for (int i = 0; p[i]; ++i) {
            if (!t[i] || t[i] != p[i]) {
                ok = 0;
                break;
            }
        }
        if (ok)
            return t;
    }
    return 0;
}

const char* sh_rfind(const char* s, const char* p)
{
    if (!s || !p || !p[0])
        return 0;
    int ls = (int)strlen(s);
    int lp = (int)strlen(p);
    if (lp > ls)
        return 0;
    for (int i = ls - lp; i >= 0; --i) {
        int ok = 1;
        for (int j = 0; j < lp; ++j) {
            if (s[i + j] != p[j]) {
                ok = 0;
                break;
            }
        }
        if (ok)
            return s + i;
    }
    return 0;
}
