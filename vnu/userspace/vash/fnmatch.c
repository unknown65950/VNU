/*
 * fnmatch.c — shell pattern matching: `*`, `?`, `[...]` and `\`.
 * The same matcher serves globs (where a pattern is matched against
 * directory entries) and the pattern side of `${x#pat}`, `case` and
 * `[[ x == pat ]]`.
 */
#include "shell.h"

static int match_here(const char* pat, const char* str);

/* `pat` may be null: unset variables expand to nothing, and an empty
 * pattern only ever matches an empty string. */
static int match_bracket(const char** pat, char c)
{
    const char* p = *pat; /* points at '[' */
    ++p;
    int neg = 0;
    if (*p == '!' || *p == '^') {
        neg = 1;
        ++p;
    }
    int hit = 0;
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;
        if (p[1] == '-' && p[2] && p[2] != ']') {
            if (c >= p[0] && c <= p[2])
                hit = 1;
            p += 3;
        } else if (*p == '\\' && p[1]) {
            if (p[1] == c)
                hit = 1;
            p += 2;
        } else {
            if (*p == c)
                hit = 1;
            ++p;
        }
    }
    if (*p == ']')
        ++p;
    *pat = p;
    return neg ? !hit : hit;
}

static int match_here(const char* pat, const char* str)
{
    while (*pat) {
        if (*pat == '*') {
            ++pat;
            if (!*pat)
                return 1;
            /* `*' alone is the whole string; otherwise try every
             * length, longest suffix last. */
            for (const char* s = str;; ++s) {
                if (match_here(pat, s))
                    return 1;
                if (!*s)
                    return 0;
            }
        }
        if (!*str)
            return 0;
        if (*pat == '?') {
            ++pat;
            ++str;
            continue;
        }
        if (*pat == '[') {
            const char* p = pat;
            if (!match_bracket(&p, *str))
                return 0;
            pat = p;
            ++str;
            continue;
        }
        if (*pat == '\\' && pat[1]) {
            if (pat[1] != *str)
                return 0;
            pat += 2;
            ++str;
            continue;
        }
        if (*pat != *str)
            return 0;
        ++pat;
        ++str;
    }
    return *str == 0;
}

int sh_pattern(const char* pat, const char* str)
{
    return match_here(pat, str);
}

/* The longest or shortest prefix/suffix a pattern strips, for
 * ${x#pat} and friends: `#` takes the shortest match, `##' the longest,
 * `%` the shortest suffix, `%%' the longest. */
int sh_strip_prefix(const char* str, const char* pat, int longest, char* out,
                    int outn)
{
    int slen = (int)strlen(str);
    /* `take' is how much of the front is cut away: the longest match
     * first for `##', the shortest for `#'. */
    for (int take = longest ? slen : 0;; take += longest ? -1 : 1) {
        if (take < 0 || take > slen)
            break;
        char head[SH_LINE_MAX];
        if (take >= SH_LINE_MAX)
            continue;
        for (int i = 0; i < take; ++i)
            head[i] = str[i];
        head[take] = 0;
        if (match_here(pat, head)) {
            int n = slen - take;
            if (n > outn - 1)
                n = outn - 1;
            for (int i = 0; i < n; ++i)
                out[i] = str[take + i];
            out[n] = 0;
            return 1;
        }
    }
    return 0;
}

int sh_strip_suffix(const char* str, const char* pat, int longest, char* out,
                    int outn)
{
    int slen = (int)strlen(str);
    /* `take' is how much of the front is kept, so the matched tail is
     * slen - take: the longest tail first for `%%', the shortest for
     * `%'. */
    for (int take = longest ? 0 : slen;; take += longest ? 1 : -1) {
        if (take < 0 || take > slen)
            break;
        char tail[SH_LINE_MAX];
        int n = slen - take;
        if (n > SH_LINE_MAX - 1)
            n = SH_LINE_MAX - 1;
        for (int i = 0; i < n; ++i)
            tail[i] = str[take + i];
        tail[n] = 0;
        if (match_here(pat, tail)) {
            int keep = take;
            if (keep > outn - 1)
                keep = outn - 1;
            for (int i = 0; i < keep; ++i)
                out[i] = str[i];
            out[keep] = 0;
            return 1;
        }
    }
    return 0;
}

/* The first match of `pat' in `str' at or after `from', for ${x/pat/y}
 * and `[[ x == pat ]]'. Returns the offset, or -1. */
/* Does a pattern contain a character that makes it more than a literal
 * string? `\\' quotes the one after it. */
static int has_glob(const char* p)
{
    for (; *p; ++p) {
        if (*p == '\\' && p[1]) {
            ++p;
            continue;
        }
        if (*p == '*' || *p == '?' || *p == '[')
            return 1;
    }
    return 0;
}

int sh_find_sub(const char* str, const char* pat, int from, int* mlen)
{
    int slen = (int)strlen(str);
    int plen = (int)strlen(pat);
    *mlen = plen;
    if (plen == 0) {
        *mlen = 0;
        return from <= slen ? from : -1;
    }
    /* A pattern with no glob characters matches exactly plen bytes, so
     * a plain scan finds the leftmost one. */
    if (!has_glob(pat)) {
        for (int i = from; i <= slen - plen; ++i)
            if (sh_starts(str + i, pat))
                return i;
        return -1;
    }
    /* Otherwise try every length of substring at each position,
     * shortest first, so the leftmost smallest match wins. */
    for (int i = from; i <= slen; ++i) {
        for (int L = 0; i + L <= slen && L < SH_LINE_MAX; ++L) {
            char buf[SH_LINE_MAX];
            memcpy(buf, str + i, (size_t)L);
            buf[L] = 0;
            if (match_here(pat, buf)) {
                *mlen = L;
                return i;
            }
        }
    }
    return -1;
}