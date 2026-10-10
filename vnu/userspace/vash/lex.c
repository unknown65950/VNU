/*
 * lex.c — vash's tokenizer.
 *
 * Words are handed to the parser as trees of parts, not as strings, and
 * nothing is expanded here: `x=hi; echo $x` must not see the `hi` before
 * the assignment has run, and a loop body must see a fresh value on
 * every turn. Reading a word therefore walks the source, remembering
 * what came from quotes (a quoted `$x` is one field and never a
 * pattern), and stops at the first metacharacter that is not part of
 * it.
 */
#include "shell.h"

void lex_init_file(Lex* lx, int fd)
{
    memset(lx, 0, sizeof(Lex));
    lx->fd = fd;
}

void lex_init_str(Lex* lx, const char* text, int len)
{
    memset(lx, 0, sizeof(Lex));
    lx->fd = -1;
    lx->text = (char*)text;
    lx->textlen = len;
}

void lex_free(Lex* lx)
{
    word_free(lx->tok.word);
    word_free(lx->peek.word);
    lx->tok.word = 0;
    lx->peek.word = 0;
}

/* --- the character source: a file line by line, or a string --- */

static int fill_line(Lex* lx)
{
    if (lx->fd < 0) {
        if (lx->pos >= lx->textlen)
            return 0;
        int n = lx->textlen - lx->pos;
        if (n > SH_LINE_MAX - 2)
            n = SH_LINE_MAX - 2;
        memcpy(lx->line, lx->text + lx->pos, (unsigned long)n);
        lx->pos += n;
        lx->len = n;
        lx->linepos = 0;
        ++lx->linenr;
        ++lx->linegen;
        return 1;
    }
    int r = (int)read(lx->fd, lx->line, SH_LINE_MAX - 2);
    if (r <= 0) {
        lx->ateof = 1;
        return 0;
    }
    if (lx->line[r - 1] != '\n')
        lx->line[r++] = '\n';
    lx->len = r;
    lx->linepos = 0;
    ++lx->linenr;
    ++lx->linegen;
    return 1;
}

static int lc_peek(Lex* lx)
{
    if (lx->linepos >= lx->len && !fill_line(lx))
        return -1;
    return (unsigned char)lx->line[lx->linepos];
}

static int lc_get(Lex* lx)
{
    int c = lc_peek(lx);
    if (c >= 0)
        ++lx->linepos;
    return c;
}

/* Look n characters ahead, crossing lines the same way the reader
 * does. Used to tell `<` from `<<' and `$((` from `$(`. */
static int lc_peek_at(Lex* lx, int n)
{
    /* The line is filled before it is copied: a fresh lexer still has an
     * empty buffer, and putting that back afterwards would hand the next
     * read NULs instead of the text. */
    if (lx->linepos >= lx->len && !fill_line(lx))
        return -1;
    int save_line = lx->linepos;
    int save_len = lx->len;
    int save_pos = lx->pos;
    int save_nr = lx->linenr;
    int save_gen = lx->linegen;
    char saved[SH_LINE_MAX];
    memcpy(saved, lx->line, sizeof(saved));
    int c = -1;
    /* The character n ahead is the one after n characters: the caller's
     * `c' is the character at the cursor, and it asks about the ones
     * that follow it. */
    for (int i = 0; i <= n; ++i)
        c = lc_get(lx);
    lx->linenr = save_nr;
    if (lx->linegen == save_gen) {
        lx->linepos = save_line;
        lx->len = save_len;
        lx->pos = save_pos;
        memcpy(lx->line, saved, sizeof(saved));
    } else {
        /* The look-ahead crossed into the next line. That line holds the
         * characters it read and must stay, so the cached line is
         * dropped instead of restored and the reader is put back where
         * the current token started. `save_pos' is past the cached
         * buffer, so the buffer's first byte is `save_pos - save_len'
         * and the cursor inside it is that plus `save_line'. */
        lx->pos = save_pos - save_len + save_line;
        lx->len = 0;
        lx->linepos = 0;
    }
    return c;
}

/* --- word building --- */

/* How many parts a word has: a quote that closes on nothing still left
 * the word itself one, and `read -d "" x' needs that empty one. */
static int word_parts(const Word* w)
{
    int n = 0;
    for (const Word* p = w; p; p = p->next)
        ++n;
    return n;
}

static void word_add_lit(Word* w, const char* t, int n, int quoted)
{
    if (n <= 0)
        return;
    Word* last = w;
    while (last->next)
        last = last->next;
    if (last->kind == W_LIT && last->quoted == quoted) {
        str_addn(&last->text, t, n);
        return;
    }
    Word* p = word_new(W_LIT);
    if (!p)
        return;
    p->quoted = quoted;
    str_addn(&p->text, t, n);
    last->next = p;
}

static void word_add_part(Word* w, Word* part)
{
    Word* last = w;
    while (last->next)
        last = last->next;
    last->next = part;
}

static int is_meta(int c)
{
    return c == '|' || c == '&' || c == ';' || c == '(' || c == ')' ||
           c == '<' || c == '>';
}

static int is_blank(int c)
{
    return c == ' ' || c == '\t';
}

static int is_name_char(int c)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

/* --- the body of a $'...'/'...'/"..." run, copied verbatim --- */

/* The body of `$'...'', where a backslash stands for the character it
 * names: \t is a tab, \n a newline, \xHH a byte. */
static int ansi_c_escape(Lex* lx, Str* out)
{
    int c = lc_get(lx);
    if (c < 0)
        return 0;
    switch (c) {
    case 'a': str_addc(out, '\a'); return 1;
    case 'b': str_addc(out, '\b'); return 1;
    case 'e': case 'E': str_addc(out, 27); return 1;
    case 'f': str_addc(out, '\f'); return 1;
    case 'n': str_addc(out, '\n'); return 1;
    case 'r': str_addc(out, '\r'); return 1;
    case 't': str_addc(out, '\t'); return 1;
    case 'v': str_addc(out, '\v'); return 1;
    case '\\': str_addc(out, '\\'); return 1;
    case '\'': str_addc(out, '\''); return 1;
    case '"': str_addc(out, '"'); return 1;
    case '?': str_addc(out, '?'); return 1;
    case '\n': return 1; /* a line that goes on */
    case 'x': {
        int v = 0, n = 0;
        while (n < 2) {
            int h = lc_peek(lx);
            int d = (h >= '0' && h <= '9') ? h - '0'
                  : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                  : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
            if (d < 0)
                break;
            v = v * 16 + d;
            lc_get(lx);
            ++n;
        }
        if (n > 0) {
            str_addc(out, (char)v);
            return 1;
        }
        str_addc(out, 'x');
        return 1;
    }
    default:
        if (c >= '0' && c <= '7') {
            int v = c - '0', n = 1;
            while (n < 3) {
                int o = lc_peek(lx);
                if (o < '0' || o > '7')
                    break;
                v = v * 8 + (o - '0');
                lc_get(lx);
                ++n;
            }
            str_addc(out, (char)v);
            return 1;
        }
        str_addc(out, (char)c);
        return 1;
    }
}

static void read_literal_body(Lex* lx, Str* out, char end, char end2)
{
    for (;;) {
        int c = lc_get(lx);
        if (c < 0)
            break;
        if (c == '\\') {
            ansi_c_escape(lx, out);
            continue;
        }
        if (c == end)
            break;
        if (end2 && c == end2 && lc_peek(lx) == end2) {
            lc_get(lx);
            break;
        }
        if (c == '\'')
            str_addc(out, (char)c);
        else if (c == '"')
            str_addc(out, (char)c);
        else
            str_addc(out, (char)c);
    }
}

/* A quoted run copied exactly as written, quotes and backslashes and
 * all: the text is going to a lexer of its own, so nothing here may
 * turn `\\n' inside "..." into `n'. */
static void read_quoted_raw(Lex* lx, Str* out, char end, char end2)
{
    for (;;) {
        int c = lc_get(lx);
        if (c < 0)
            return;
        if (c == '\\' && end == '"') {
            /* Inside double quotes a backslash escapes the quote, so
             * `"[\",/]"' is one string and not a string ending at the
             * backslash's quote. Keep both characters for the nested
             * read to interpret. */
            str_addc(out, (char)c);
            int n = lc_get(lx);
            if (n < 0)
                return;
            str_addc(out, (char)n);
            continue;
        }
        if (c == end)
            return;
        if (end2 && c == end2 && lc_peek(lx) == end2) {
            lc_get(lx);
            return;
        }
        str_addc(out, (char)c);
    }
}

/* The `$(` is already eaten. Nested $( ), backticks and quotes all
 * count, so a substitution that contains one of its own is read whole
 * and left for the nested run to interpret. */
static void read_cmdsub(Lex* lx, Str* out)
{
    int depth = 1;
    for (;;) {
        int c = lc_get(lx);
        if (c < 0)
            break;
        if (c == '\'' || c == '"') {
            Str q;
            str_init(&q);
            read_quoted_raw(lx, &q, (char)c, 0);
            str_addc(out, (char)c);
            str_addn(out, str_cstr(&q), q.len);
            str_addc(out, (char)c);
            str_free(&q);
        } else if (c == '\\') {
            str_addc(out, (char)c);
            int n = lc_get(lx);
            if (n >= 0)
                str_addc(out, (char)n);
        } else if (c == '`') {
            /* Keep the inner substitution exactly as written. */
            str_addc(out, (char)c);
            for (;;) {
                int b = lc_get(lx);
                if (b < 0)
                    break;
                str_addc(out, (char)b);
                if (b == '\\') {
                    int n = lc_get(lx);
                    if (n >= 0)
                        str_addc(out, (char)n);
                    continue;
                }
                if (b == '`')
                    break;
            }
        } else if (c == '(') {
            ++depth;
            str_addc(out, (char)c);
        } else if (c == ')') {
            if (--depth == 0)
                break;
            str_addc(out, (char)c);
        } else {
            str_addc(out, (char)c);
        }
    }
}

static void read_backtick(Lex* lx, Str* out)
{
    for (;;) {
        int c = lc_get(lx);
        if (c < 0 || c == '\n')
            break;
        if (c == '\\') {
            int n = lc_get(lx);
            if (n >= 0)
                str_addc(out, (char)n);
            continue;
        }
        if (c == '`')
            break;
        str_addc(out, (char)c);
    }
}

/* The `${` is already eaten; stop at the `}` that closes it, counting
 * quotes and nested braces so `${x:-${y}}` stays whole. */
static void read_brace(Lex* lx, Str* out)
{
    int depth = 0;
    for (;;) {
        int c = lc_get(lx);
        if (c < 0)
            break;
        if (c == '\'' || c == '"') {
            Str q;
            str_init(&q);
            read_quoted_raw(lx, &q, (char)c, 0);
            str_addc(out, (char)c);
            str_addn(out, str_cstr(&q), q.len);
            str_addc(out, (char)c);
            str_free(&q);
            continue;
        }
        if (c == '\\') {
            str_addc(out, (char)c);
            int n = lc_get(lx);
            if (n >= 0)
                str_addc(out, (char)n);
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}') {
            if (depth == 0)
                break;
            --depth;
        }
        str_addc(out, (char)c);
    }
}

/* The `$((` is already eaten; two closing parens end it. */
static void read_arith(Lex* lx, Str* out)
{
    int depth = 0;
    for (;;) {
        int c = lc_get(lx);
        if (c < 0)
            break;
        if (c == '\'' || c == '"') {
            Str q;
            str_init(&q);
            read_quoted_raw(lx, &q, (char)c, 0);
            str_addn(out, str_cstr(&q), q.len);
            str_free(&q);
        } else if (c == '(') {
            ++depth;
            str_addc(out, (char)c);
        } else if (c == ')') {
            if (depth == 0 && lc_peek(lx) == ')') {
                lc_get(lx);
                break;
            }
            if (depth > 0)
                --depth;
            str_addc(out, (char)c);
        } else {
            str_addc(out, (char)c);
        }
    }
}

static void read_dollar(Lex* lx, Word* w, int quoted)
{
    Str body;
    str_init(&body);
    int c = lc_peek(lx);

    if (c == '(') {
        if (lc_peek_at(lx, 1) == '(') { /* $(( arithmetic )) */
            lc_get(lx);
            lc_get(lx);
            Word* p = word_new(W_ARITH);
            p->quoted = quoted;
            read_arith(lx, &p->text);
            word_add_part(w, p);
            str_free(&body);
            return;
        }
        lc_get(lx);
        /* `$(< file)' is a file, not a command: the shortcut bash
         * offers for `$(cat file)' that costs no process. */
        if (lc_peek(lx) == '<' && lc_peek_at(lx, 1) != '(') {
            lc_get(lx);
            Word* f = word_new(W_FILE);
            f->quoted = quoted;
            /* What stands between the `<' and the `)' is the name; the
             * `)' that closes the substitution is this word's to eat. */
            while (is_blank(lc_peek(lx)) || lc_peek(lx) == '\n')
                lc_get(lx);
            for (;;) {
                int n = lc_get(lx);
                if (n < 0 || n == ' ' || n == '\t' || n == '\n') {
                    /* the name is done: step over what is left of the
                     * line up to the `)' */
                    while (n != ')' && n >= 0 && n != '\n')
                        n = lc_get(lx);
                    if (n == ')')
                        break;
                    lc_get(lx);
                    break;
                }
                if (n == ')')
                    break;
                str_addc(&f->text, (char)n);
            }
            word_add_part(w, f);
            str_free(&body);
            return;
        }
        Word* p = word_new(W_CMDSUB);
        p->quoted = quoted;
        read_cmdsub(lx, &p->text);
        word_add_part(w, p);
        str_free(&body);
        return;
    }
    if (c == '{') {
        lc_get(lx);
        Word* p = word_new(W_BRACE);
        p->quoted = quoted;
        read_brace(lx, &p->text);
        word_add_part(w, p);
        str_free(&body);
        return;
    }
    if (c == '\'') { /* $'...' — bash's other quoting, plain here */
        lc_get(lx);
        read_literal_body(lx, &body, '\'', 0);
        word_add_lit(w, str_cstr(&body), body.len, 1);
        str_free(&body);
        return;
    }
    if (is_name_char(c) && !(c >= '0' && c <= '9')) {
        while (is_name_char(lc_peek(lx)))
            str_addc(&body, (char)lc_get(lx));
        Word* p = word_new(W_VAR);
        p->quoted = quoted;
        str_addn(&p->text, str_cstr(&body), body.len);
        word_add_part(w, p);
        str_free(&body);
        return;
    }
    if (c >= '0' && c <= '9') {
        str_addc(&body, (char)lc_get(lx));
        Word* p = word_new(W_VAR);
        p->quoted = quoted;
        str_addn(&p->text, str_cstr(&body), body.len);
        word_add_part(w, p);
        str_free(&body);
        return;
    }
    if (c == '?' || c == '#' || c == '@' || c == '*' || c == '-' || c == '$' ||
        c == '!') {
        str_addc(&body, (char)lc_get(lx));
        Word* p = word_new(W_VAR);
        p->quoted = quoted;
        str_addn(&p->text, str_cstr(&body), body.len);
        word_add_part(w, p);
        str_free(&body);
        return;
    }
    /* A '$' with nothing to name is a literal '$'. */
    word_add_lit(w, "$", 1, quoted);
    str_free(&body);
}

/* Read one word, up to the next blank, metacharacter or newline. */
static Word* lex_word(Lex* lx)
{
    Word* w = word_new(W_LIT);
    Str lit;
    str_init(&lit);
    int quoted = 0;

    /* A `~' at the very start of a word is the home directory, and
     * `~name' that user's home — everything up to the first `/' is the
     * user, the rest of the word is an ordinary path. */
    if (lc_peek(lx) == '~' && lc_peek_at(lx, 1) != 0) {
        int c1 = lc_peek_at(lx, 1);
        int is_name = c1 == '_' || (c1 >= 'a' && c1 <= 'z') ||
                      (c1 >= 'A' && c1 <= 'Z');
        int is_slash = (c1 == '/');
        int is_end = is_blank(c1) || is_meta(c1) || c1 == '\n' || c1 < 0;
        if (is_name || is_slash || is_end) {
            lc_get(lx);
            Word* p = word_new(W_TILDE);
            Str user;
            str_init(&user);
            for (;;) {
                int c = lc_peek(lx);
                if (c < 0 || c == '/' || is_blank(c) || is_meta(c) || c == '\n')
                    break;
                str_addc(&user, (char)lc_get(lx));
            }
            str_addn(&p->text, str_cstr(&user), user.len);
            str_free(&user);
            word_add_part(w, p);
        }
    }

    for (;;) {
        int c = lc_peek(lx);
        if (c < 0)
            break;
        if (!quoted && (is_meta(c) || c == '\n'))
            break;
        if (is_blank(c) && !quoted) {
            lc_get(lx);
            break;
        }
        if (c == '\\') {
            lc_get(lx);
            int n = lc_get(lx);
            if (n >= 0)
                str_addc(&lit, (char)n);
            continue;
        }
        if (c == '\'') {
            lc_get(lx);
            /* Whatever came before the quote was not quoted, and
             * `q=a'"'"'b'"'"'' has to say so. */
            word_add_lit(w, str_cstr(&lit), lit.len, quoted);
            int before = word_parts(w);
            str_reset(&lit);
            for (;;) {
                int q = lc_get(lx);
                if (q < 0 || q == '\'')
                    break;
                str_addc(&lit, (char)q);
            }
            word_add_lit(w, str_cstr(&lit), lit.len, 1);
            /* An empty quoted run is still a word — `""' is one empty
             * argument — but `x=""' is an assignment, so only a word
             * with nothing in it at all counts. */
            if (word_parts(w) == before && w->kind == W_LIT &&
                w->text.len == 0)
                w->quoted = 1;
            str_reset(&lit);
            quoted = 0;
            continue;
        }
        if (c == '"') {
            lc_get(lx);
            /* The text in front of the quote belongs to the word, but it
             * does not belong to the quoted part: `q="a b"' is the name
             * `q' and one quoted value, not one quoted word. */
            word_add_lit(w, str_cstr(&lit), lit.len, quoted);
            int before = word_parts(w);
            str_reset(&lit);
            for (;;) {
                int q = lc_peek(lx);
                if (q < 0 || q == '"') {
                    if (q == '"')
                        lc_get(lx);
                    break;
                }
                if (q == '\\') {
                    lc_get(lx);
                    int n = lc_get(lx);
                    if (n < 0)
                        break;
                    if (n != '"' && n != '\\' && n != '$' && n != '`' &&
                        n != '\n')
                        str_addc(&lit, '\\');
                    if (n != '\n')
                        str_addc(&lit, (char)n);
                    continue;
                }
                if (q == '$') {
                    lc_get(lx);
                    word_add_lit(w, str_cstr(&lit), lit.len, 1);
                    str_reset(&lit);
                    read_dollar(lx, w, 1);
                    continue;
                }
                if (q == '`') {
                    lc_get(lx);
                    Word* p = word_new(W_CMDSUB);
                    p->quoted = 1;
                    read_backtick(lx, &p->text);
                    word_add_part(w, p);
                    continue;
                }
                str_addc(&lit, (char)lc_get(lx));
            }
            word_add_lit(w, str_cstr(&lit), lit.len, 1);
            /* An empty quoted run is still a word — `""' is one empty
             * argument — but `x=""' is an assignment, so only a word
             * with nothing in it at all counts. */
            if (word_parts(w) == before && w->kind == W_LIT &&
                w->text.len == 0)
                w->quoted = 1;
            str_reset(&lit);
            quoted = 0;
            continue;
        }
        if (c == '$') {
            lc_get(lx);
            word_add_lit(w, str_cstr(&lit), lit.len, quoted);
            str_reset(&lit);
            read_dollar(lx, w, quoted);
            continue;
        }
        if (c == '`' && !quoted) {
            lc_get(lx);
            Word* p = word_new(W_CMDSUB);
            read_backtick(lx, &p->text);
            word_add_part(w, p);
            continue;
        }
        str_addc(&lit, (char)lc_get(lx));
    }
    word_add_lit(w, str_cstr(&lit), lit.len, quoted);
    str_free(&lit);
    return w;
}

/* --- the token stream --- */

static void lex_read_raw(Lex* lx, Tok* t)
{
    memset(t, 0, sizeof(Tok));
    for (;;) {
        int c = lc_peek(lx);
        if (c < 0) {
            t->type = T_EOF;
            return;
        }
        if (is_blank(c)) {
            lc_get(lx);
            continue;
        }
        if (c == '\\' && lc_peek_at(lx, 1) == '\n') {
            lc_get(lx);
            lc_get(lx);
            continue;
        }
        if (c == '#') { /* a comment runs to the end of the line */
            while ((c = lc_peek(lx)) >= 0 && c != '\n')
                lc_get(lx);
            continue;
        }
        if (c == '\n') {
            lc_get(lx);
            t->type = T_NEWLINE;
            return;
        }
        break;
    }

    int c = lc_peek(lx);
    int c1 = lc_peek_at(lx, 1);
    int c2 = lc_peek_at(lx, 2);
    int op = 0;
    int n = 1; /* how many characters the operator is */
    switch (c) {
    case ';':
        op = (c1 == ';') ? OP_DUP : OP_SEMI;
        n = (c1 == ';') ? 2 : 1;
        break;
    case '&':
        if (c1 == '&') {
            op = OP_AND;
            n = 2;
        } else if (c1 == '>') {
            if (c2 == '>') {
                op = OP_AMPGTGT;
                n = 3;
            } else {
                op = OP_AMPGT;
                n = 2;
            }
        } else {
            op = OP_AMP;
            n = 1;
        }
        break;
    case '|':
        op = (c1 == '|') ? OP_OR : OP_PIPE;
        n = (c1 == '|') ? 2 : 1;
        break;
    case '<':
        if (c1 == '&')
            op = OP_LTAMP;
        else if (c1 == '<')
            op = (lc_peek_at(lx, 2) == '<') ? OP_TLT : OP_DLT;
        else if (c1 == '(')
            op = OP_PSUB_IN; /* <( list ): the list is this input */
        else
            op = OP_LT;
        n = (op == OP_LT) ? 1 : (op == OP_TLT ? 3 : (op == OP_PSUB_IN ? 2 : 2));
        break;
    case '>':
        if (c1 == '(') {
            op = OP_PSUB_OUT; /* >( list ): the list is this output */
            n = 2;
        } else {
            op = (c1 == '&') ? OP_AMPGTAMP : (c1 == '>' ? OP_DGT : OP_GT);
            n = (op == OP_GT) ? 1 : 2;
        }
        break;
    case '(':
        op = OP_LPAREN;
        break;
    case ')':
        op = OP_RPAREN;
        break;
    default:
        break;
    }
    if (op) {
        while (n-- > 0)
            lc_get(lx);
        t->type = T_OP;
        t->op = op;
        return;
    }

    /* A word — but a lone `{` or `}` is a reserved word, not text.
     * The check is for one plain piece of text: a brace that came out
     * of an expansion or out of quotes is ordinary text again. */
    Word* w = lex_word(lx);
    if (w && !w->quoted && !w->next && w->kind == W_LIT &&
        w->text.len == 1 &&
        (w->text.p[0] == '{' || w->text.p[0] == '}')) {
        char b = w->text.p[0];
        t->type = T_OP;
        t->op = (b == '{') ? OP_LBRACE : OP_RBRACE;
        word_free(w);
        return;
    }
    t->type = T_WORD;
    t->word = w;
}

/* The lines of a here-document: every line up to the one that reads
 * exactly `delim'. The parser asks for them here because the text is
 * still in the lexer's hands — a file is read in blocks, so the body
 * may be sitting in the buffer with the rest of the script behind it. */
void lex_heredoc(Lex* lx, const char* delim, int strip_tabs, int at_line_start,
                 Str* body)
{
    /* The newline after the delimiter has usually been read already —
     * it is what ended the command — so the body starts here. When it
     * has not, the rest of that line is thrown away first. */
    if (!at_line_start) {
        int c = lc_peek(lx);
        while (c >= 0 && c != '\n')
            c = lc_get(lx);
        if (c == '\n')
            lc_get(lx);
    }

    unsigned long dlen = (unsigned long)strlen(delim);
    for (;;) {
        Str line;
        str_init(&line);
        while (lx->linepos < lx->len) {
            int ch = lc_get(lx);
            if (ch < 0)
                break;
            if (ch == '\n')
                break;
            str_addc(&line, (char)ch);
        }
        /* `<<-' eats the tabs that line the body up in the script, in
         * the delimiter line and in every line of the body */
        int start = 0;
        const char* text = str_cstr(&line);
        int len = line.len;
        if (strip_tabs) {
            while (start < len && text[start] == '\t')
                ++start;
        }
        if ((unsigned long)(len - start) == dlen &&
            strncmp(text + start, delim, dlen) == 0) {
            /* The newline that ended the delimiter line belongs to the
             * command: the parser is still waiting for it to close the
             * line, so it goes back into the buffer. */
            if (lx->linepos > 0 && lx->line[lx->linepos - 1] == '\n')
                --lx->linepos;
            str_free(&line);
            return;
        }
        str_addn(body, str_cstr(&line) + start, line.len - start);
        str_free(&line);
        str_addc(body, '\n');
        if (lx->linepos >= lx->len && !fill_line(lx))
            return; /* no delimiter: the rest of the file is the body */
    }
}

/* Remember a here-document and read it when the command it belongs to is
 * finished: everything on the line is still to be parsed, and the body
 * is what comes after all of it. */
void lex_push_hdoc(Lex* lx, Redir* r, const char* delim, int strip_tabs)
{
    HDoc* h = (HDoc*)malloc(sizeof(HDoc));
    if (!h)
        return;
    memset(h, 0, sizeof(*h));
    int n = 0;
    while (delim[n] && n < (int)sizeof(h->delim) - 1) {
        h->delim[n] = delim[n];
        ++n;
    }
    h->delim[n] = 0;
    h->strip_tabs = strip_tabs;
    h->redir = r;
    h->next = lx->hd;
    lx->hd = h;
}

void lex_collect_hdocs(Lex* lx)
{
    while (lx->hd) {
        HDoc* h = lx->hd;
        lx->hd = h->next;
        /* When the line did not end with a newline — `cat <<EOF; echo',
         * where the body belongs to the whole line — the rest of it is
         * still ahead and the body starts after that. */
        int at_start = lx->have_peek &&
                       (lx->peek.type == T_NEWLINE || lx->peek.type == T_EOF);
        Str body;
        str_init(&body);
        lex_heredoc(lx, h->delim, h->strip_tabs, at_start, &body);
        h->redir->heredoc = sh_strdup(str_cstr(&body));
        h->redir->heredoc_quoted = h->strip_tabs;
        str_free(&body);
        free(h);
    }
}

void lex_next(Lex* lx)
{
    word_free(lx->tok.word);
    if (lx->have_peek) {
        lx->tok = lx->peek;
        lx->have_peek = 0;
        memset(&lx->peek, 0, sizeof(Tok));
    } else {
        memset(&lx->tok, 0, sizeof(Tok));
        lex_read_raw(lx, &lx->tok);
    }
    /* What was two tokens ahead is one token ahead now. */
    lx->have_peek2 = 0;
    memset(&lx->peek2, 0, sizeof(Tok));
}

const Tok* lex_peek(Lex* lx)
{
    if (!lx->have_peek) {
        memset(&lx->peek, 0, sizeof(Tok));
        lex_read_raw(lx, &lx->peek);
        lx->have_peek = 1;
        lx->have_peek2 = 0;
        memset(&lx->peek2, 0, sizeof(Tok));
    }
    return &lx->peek;
}

const Tok* lex_peek2(Lex* lx)
{
    if (lx->have_peek2)
        return &lx->peek2;

    /* Reading a token and then putting the lexer back where it was is
     * the whole of it: `(( expr ))' is closed by two brackets in a row,
     * and one token of lookahead cannot see that without eating the
     * first of them. The token read is thrown away — it is freed here
     * and the parser only ever looks at what kind it is. */
    (void)lex_peek(lx);
    Lex saved = *lx;
    Word* between = saved.peek.word;

    lx->tok.word = 0; /* the current token is the parser's to do with */
    memset(&lx->tok, 0, sizeof(Tok));
    memset(&lx->peek, 0, sizeof(Tok));
    lx->have_peek = 0;
    lx->have_peek2 = 0;

    lex_next(lx); /* the token after the peeking one — that is the second */
    Tok second = lx->tok;
    word_free(lx->tok.word);

    *lx = saved;
    lx->peek.word = between;
    lx->peek2 = second;
    lx->peek2.word = 0;
    lx->have_peek2 = 1;
    return &lx->peek2;
}

Word* lex_take(Lex* lx)
{
    /* A token's word belongs to whoever takes it. The parser builds its
     * tree out of these words, so taking one clears it from the token:
     * the lexer frees what it still owns and nothing else does. */
    Word* w = 0;
    if (lx->have_peek && lx->peek.word) {
        w = lx->peek.word;
        lx->peek.word = 0;
    } else if (lx->tok.word) {
        w = lx->tok.word;
        lx->tok.word = 0;
    }
    return w;
}

const char* tok_op_name(int op)
{
    switch (op) {
    case OP_SEMI: return ";";
    case OP_AMP: return "&";
    case OP_AND: return "&&";
    case OP_OR: return "||";
    case OP_PIPE: return "|";
    case OP_DUP: return ";;";
    case OP_LT: return "<";
    case OP_GT: return ">";
    case OP_DLT: return "<<";
    case OP_DGT: return ">>";
    case OP_AMPGT: return "&>";
    case OP_AMPGTGT: return "&>>";
    case OP_LTAMP: return "<&";
    case OP_AMPGTAMP: return ">&";
    case OP_PSUB_IN: return "<(";
    case OP_PSUB_OUT: return ">(";
    case OP_TLT: return "<<<";
    case OP_LPAREN: return "(";
    case OP_RPAREN: return ")";
    case OP_LBRACE: return "{";
    case OP_RBRACE: return "}";
    default: return "?";
    }
}