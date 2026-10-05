/*
 * parse.c — turning tokens into commands.
 *
 * The grammar is bash's, as far as a shell needs it:
 *
 *   list     := and_or ((`;' | newline) and_or)*
 *   and_or   := pipeline ((`&&' | `||') pipeline)*
 *   pipeline := command (`|' command)*
 *   command  := simple | `(' list `)'
 *   simple   := (word | redir)+ | redir+
 *   redir    := [digits] (`<' | `>' | `>>' | `>&' | `<'&' | `&>' | `&>>') word
 *
 * Words keep their expansion steps attached (see lex.c), so nothing
 * here has to know what a variable is. A redirection's target stays a
 * word for the same reason: `> $out' is only known when the command
 * runs, not when the line was read.
 */
#include "shell.h"
#include <vlibc/string.h>

#define CUR(lx) (&(lx)->tok)
#define PEEK(lx) (lex_peek(lx))
#define PEEK2(lx) (lex_peek2(lx))

static Cmd* parse_and_or(Lex* lx);
static Cmd* parse_pipeline(Lex* lx);
static Cmd* parse_paren_group(Lex* lx);

/* `<( list )' and `>( list )' as the target of a redirection: the list
 * gets a pipe of its own and this command gets one end of it. */
static int parse_procsub_target(Lex* lx, Cmd* c, int fd, int psub_op)
{
    Cmd* list = parse_paren_group(lx);
    if (!list) {
        sh_err("vash: expected `)' to close the process substitution\n");
        return 0;
    }
    Redir* r = (Redir*)malloc(sizeof(Redir));
    if (!r)
        return 0;
    memset(r, 0, sizeof(Redir));
    r->fd = fd;
    r->mode = R_PIPE;
    r->procsub = list;
    r->procsub_out = (psub_op == OP_PSUB_OUT);
    r->next = c->redirs;
    c->redirs = r;
    return 1;
}

static Cmd* parse_paren_group(Lex* lx);
static int parse_procsub_target(Lex* lx, Cmd* c, int fd, int psub_op);
static Cmd* parse_list_inner(Lex* lx, int stop);

/* Where a list of commands ends. A top-level list runs to end of input,
 * `)' or `}'; a condition ends at the word that opens the body it
 * guards, and a body ends at the word that closes it. */
enum {
    STOP_ANY = 0,
    STOP_THEN,
    STOP_DO,
    STOP_BODY,
    STOP_DSEMI,
    STOP_IN
};

/* The reserved words that open a compound command. They arrive as
 * ordinary words, so each has to be recognised here, in the position
 * where a command is expected. */
static int is_reserved(Word* w, const char* name)
{
    if (!w || w->kind != W_LIT || w->quoted || w->next)
        return 0;
    return strcmp(str_cstr(&w->text), name) == 0;
}

static void dbg_num(char* buf, int v)
{
    char tmp[16];
    int n = 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    if (v < 0) {
        buf[n++] = '-';
        v = -v;
    }
    while (v > 0 && n < 15)
        tmp[n++] = (char)('0' + v % 10), v /= 10;
    for (int i = 0; i < n; ++i)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

static void dbg_pos(Lex* lx, const char* what)
{
    int start = lx->linepos;
    while (start > 0 && lx->line[start - 1] != '\n')
        --start;
    int stop = start;
    while (stop < lx->len && lx->line[stop] != '\n')
        ++stop;
    sh_err("vash: [dbg] ");
    sh_err(what);
    sh_err(" | ");
    {
        const Tok* t = PEEK(lx);
        sh_err(" [tok=");
        char nb[24];
        dbg_num(nb, t->type);
        sh_err(nb);
        sh_err(",op=");
        dbg_num(nb, t->op);
        sh_err(nb);
        sh_err(",w=");
        sh_err(t->type == T_WORD && t->word ? str_cstr(&t->word->text)
                                             : "-");
        sh_err("]");
    }
    int n = stop - start;
    if (n > 120)
        n = 120;
    write(2, lx->line + start, (size_t)n);
    sh_err("\n");
}

static void add_redir(Cmd* c, int fd, int mode, Word* target)
{
    Redir* r = (Redir*)malloc(sizeof(Redir));
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    r->mode = mode;
    r->target = target;
    /* Redirections apply left to right, so a later one wins: keep the
     * order they were written in. */
    Redir** pp = &c->redirs;
    while (*pp)
        pp = &(*pp)->next;
    *pp = r;
}

static void add_argv(Cmd* c, Word* w)
{
    Word** pp =
        (Word**)malloc(sizeof(Word*) * (unsigned long)(c->argc + 1));
    if (!pp)
        return;
    for (int i = 0; i < c->argc; ++i)
        pp[i] = c->argv[i];
    pp[c->argc] = w;
    free(c->argv);
    c->argv = pp;
    ++c->argc;
}

static void add_list(Cmd* c, Cmd* item)
{
    Cmd** pp = (Cmd**)malloc(sizeof(Cmd*) * (unsigned long)(c->nlist + 1));
    if (!pp)
        return;
    for (int i = 0; i < c->nlist; ++i)
        pp[i] = c->list[i];
    pp[c->nlist] = item;
    free(c->list);
    c->list = pp;
    ++c->nlist;
}

/* Words kept for a construct that is not a command of its own: the
 * variable and the word list of `for', the patterns of a `case' item,
 * the operands and operators of a `[[ ... ]]'. */
static void add_word(Cmd* c, Word* w)
{
    if (!w)
        return;
    Word** pp =
        (Word**)malloc(sizeof(Word*) * (unsigned long)(c->nwords + 1));
    if (!pp)
        return;
    for (int i = 0; i < c->nwords; ++i)
        pp[i] = c->words[i];
    pp[c->nwords] = w;
    free(c->words);
    c->words = pp;
    ++c->nwords;
}

static void add_literal_word(Cmd* c, Str* text)
{
    Str copy;
    str_init(&copy);
    str_addn(&copy, str_cstr(text), text->len);
    add_word(c, word_literal(str_cstr(&copy)));
    str_free(&copy);
}

/* The text of a word as the arithmetic evaluator wants it: plain text
 * stays as it is, and a `$name' becomes the bare name, since the
 * evaluator looks variables up itself. */
/* The arithmetic text an `(( ... ))' body is made of: the `$' forms are
 * written back the way they were read so the expander can find them, and
 * a bare name stays bare — `x' is a variable to the evaluator and
 * `a[0]' keeps its subscript. */
static void add_word_text(Word* w, Str* out)
{
    for (; w; w = w->next) {
        switch (w->kind) {
        case W_BRACE:
            str_adds(out, "${");
            str_addn(out, str_cstr(&w->text), w->text.len);
            str_addc(out, '}');
            break;
        case W_CMDSUB:
            str_adds(out, "$(");
            str_addn(out, str_cstr(&w->text), w->text.len);
            str_addc(out, ')');
            break;
        case W_ARITH:
            str_adds(out, "$((");
            str_addn(out, str_cstr(&w->text), w->text.len);
            str_adds(out, "))");
            break;
        default:
            str_addn(out, str_cstr(&w->text), w->text.len);
            break;
        }
    }
}

/* `(( expr ))' and `for (( init; cond; step ))' read their expression
 * as text, up to the `))' that closes them. A `)' that closes a
 * subexpression inside the expression belongs to the expression, which
 * is what the depth is for. */
static void read_arith_body(Lex* lx, Cmd* c, int split)
{
    str_reset(&c->text);
    int depth = 0;
    int clause = 0;
    for (;;) {
        const Tok* p = PEEK(lx);
        if (p->type == T_EOF || p->type == T_NEWLINE)
            break;
        if (p->type == T_OP && p->op == OP_RPAREN && depth == 0 &&
            PEEK2(lx)->type == T_OP && PEEK2(lx)->op == OP_RPAREN) {
            lex_next(lx);
            lex_next(lx);
            break;
        }
        if (p->type == T_OP && p->op == OP_SEMI && depth == 0 && split &&
            clause < 2) {
            ++clause;
            str_addc(&c->text, ';');
            lex_next(lx);
            continue;
        }
        if (p->type == T_OP && (p->op == OP_LPAREN || p->op == OP_RPAREN))
            depth += (p->op == OP_LPAREN) ? 1 : -1;
        if (p->type == T_WORD) {
            Word* w = lex_take(lx);
            add_word_text(w, &c->text);
            word_free(w);
            lex_next(lx);
        } else if (p->type == T_OP) {
            str_adds(&c->text, tok_op_name(p->op));
            lex_next(lx);
        } else {
            lex_next(lx);
        }
    }
    /* `for ((x))' writes fewer clauses than a C loop has; the ones it
     * leaves out are the ones that do nothing. */
    while (split && clause < 2) {
        str_addc(&c->text, ';');
        ++clause;
    }
}

static int redir_mode_of(int op, int* fd)
{
    switch (op) {
    case OP_LT:
        *fd = 0;
        return R_IN;
    case OP_GT:
        *fd = 1;
        return R_OUT;
    case OP_DGT:
        *fd = 1;
        return R_APPEND;
    case OP_AMPGTAMP:
        *fd = 1;
        return R_DUP_OUT;
    case OP_LTAMP:
        *fd = 0;
        return R_DUP_IN;
    case OP_AMPGT:
        *fd = -1;
        return R_BOTH;
    case OP_AMPGTGT:
        *fd = -1;
        return R_BOTH_APP;
    case OP_DLT:
    case OP_TLT:
        *fd = 0;
        return R_HEREDOC;
    case OP_PSUB_IN:
        *fd = 0;
        return R_PIPE;
    case OP_PSUB_OUT:
        *fd = 1;
        return R_PIPE;
    default:
        return -1;
    }
}

/* Is this operator one that takes a descriptor as its target rather
 * than a file name (`>&2')? */
static int is_dup_op(int op)
{
    return op == OP_AMPGTAMP || op == OP_LTAMP;
}

/* Consume the target of a redirection. `>&' and `<&' may have none at
 * all, which closes off the descriptor. */
static Word* take_redir_target(Lex* lx, int op)
{
    if (is_dup_op(op)) {
        const Tok* p = PEEK(lx);
        if (p->type != T_WORD)
            return 0; /* `>&' alone: close it */
    }
    const Tok* p = PEEK(lx);
    if (p->type != T_WORD)
        return 0;
    Word* w = lex_take(lx);
    lex_next(lx);
    return w;
}

/* A bare number in front of an operator is the descriptor it applies
 * to: `2> file' is stderr, not a command called `2'. */
static int number_prefix(Word* w, char* buf, int bufn)
{
    if (!w || w->kind != W_LIT || w->quoted || w->next)
        return 0;
    const char* p = str_cstr(&w->text);
    if (!p[0])
        return 0;
    int i = 0;
    while (p[i] >= '0' && p[i] <= '9' && i < bufn - 1) {
        buf[i] = p[i];
        ++i;
    }
    if (p[i] || i == 0) {
        buf[0] = 0;
        return 0;
    }
    buf[i] = 0;
    return i;
}

/* `foo() { ... }' defines a function and `foo=(a b)' assigns an array,
 * and the two look alike until the `=' is looked for: an assignment is
 * one word carrying the sign, a definition is a bare name in front of
 * the parens. Getting this wrong eats the rest of the script, since the
 * misread body swallows everything up to some later `}'. */
static int word_is_name(Word* w)
{
    if (w->kind != W_LIT || w->quoted || w->text.len == 0)
        return 0;
    char* p = str_cstr(&w->text);
    for (int i = 0; i < w->text.len; ++i) {
        if (p[i] == '=')
            return 0;
    }
    return 1;
}

static int is_redir_op(const Tok* t)
{
    if (t->type != T_OP)
        return 0;
    switch (t->op) {
    case OP_LT:
    case OP_GT:
    case OP_DGT:
    case OP_AMPGTAMP:
    case OP_LTAMP:
    case OP_AMPGT:
    case OP_AMPGTGT:
    case OP_DLT:
    case OP_TLT:
    case OP_PSUB_IN:
    case OP_PSUB_OUT:
        return 1;
    default:
        return 0;
    }
}

static Cmd* parse_function(Lex* lx, const char* name)
{
    /* Past the name: `(' has been seen, so step over `)' next. */
    const Tok* p = PEEK(lx);
    if (p->type == T_OP && p->op == OP_LPAREN)
        lex_next(lx);
    p = PEEK(lx);
    if (p->type == T_OP && p->op == OP_RPAREN)
        lex_next(lx);
    else if (p->type == T_WORD && is_reserved(p->word, ")"))
        lex_next(lx);
    p = PEEK(lx);
    if (p->type == T_OP && p->op == OP_LBRACE)
        lex_next(lx);

    /* The body is parsed once, here, and kept as a tree; calling the
     * function runs that tree rather than reading the text again. */
    Cmd* body = parse_list_inner(lx, STOP_ANY);
    p = PEEK(lx);
    if (p->type == T_OP && p->op == OP_RBRACE)
        lex_next(lx);
    else {
        sh_err("vash: expected `}' at the end of function ");
        sh_err(name);
        sh_err("\n");
    }

    Cmd* f = cmd_new(C_FUNC);
    f->name = word_literal(name);
    f->left = body;
    func_define(name, body);
    return f;
}

static Cmd* parse_simple(Lex* lx)
{
    Cmd* c = cmd_new(C_SIMPLE);
    int saw_word = 0;

    for (;;) {
        const Tok* p = PEEK(lx);
        if (p->type == T_EOF || p->type == T_NEWLINE)
            break;
        if (p->type == T_OP &&
            (p->op == OP_SEMI || p->op == OP_AMP || p->op == OP_AND ||
             p->op == OP_OR || p->op == OP_PIPE || p->op == OP_RPAREN ||
             p->op == OP_RBRACE))
            break;

        /* `select' is the one compound command the parser does not read. */
        if (p->type == T_WORD && !saw_word && c->argc == 0 &&
            is_reserved(p->word, "select")) {
            sh_err("vash: select: not supported yet\n");
            cmd_free(c);
            return 0;
        }
        if (p->type == T_WORD && !saw_word && c->argc == 0 &&
            is_reserved(p->word, "function")) {
            lex_next(lx);
            const Tok* n = PEEK(lx);
            if (n->type == T_WORD) {
                Str nm; str_init(&nm); expand_word_one(n->word, &nm);
                lex_next(lx);
                Cmd* f = parse_function(lx, str_cstr(&nm));
                str_free(&nm);
                cmd_free(c);
                return f;
            }
            cmd_free(c); return 0;
        }

        /* `2> file' — a descriptor number glued to the operator. */
        char num[8];
        int n = p->type == T_WORD ? number_prefix(p->word, num, sizeof(num)) : 0;
        if (n > 0) {
            Word* w = lex_take(lx);
            lex_next(lx);
            const Tok* op_tok = PEEK(lx);
            if (is_redir_op(op_tok)) {
                int op = op_tok->op;
                int fd = atoi(num);
                lex_next(lx);
                Word* target = 0;
                int psub_op = 0;
                {
                    const Tok* pt = PEEK(lx);
                    if (pt->type == T_OP &&
                        (pt->op == OP_PSUB_IN || pt->op == OP_PSUB_OUT)) {
                        psub_op = pt->op;
                        lex_next(lx);
                    } else {
                        target = take_redir_target(lx, op);
                    }
                }
                int mode = redir_mode_of(op, &fd);
                if (mode < 0) {
                    /* should not happen */
                }
                if (psub_op) {
                    if (!parse_procsub_target(lx, c, fd, psub_op)) {
                        cmd_free(c);
                        return 0;
                    }
                    continue;
                }
                if (!is_dup_op(op) || target) {
                    if (!target) {
                        dbg_pos(lx, "syntax error after");
                        sh_err("vash: syntax error after ");
                        sh_err(tok_op_name(op));
                        sh_err("\n");
                        cmd_free(c);
                        return 0;
                    }
                    if (mode == R_HEREDOC && op != OP_TLT) {
                        Str delim;
                        str_init(&delim);
                        expand_word_one(target, &delim);
                        Redir* r = (Redir*)malloc(sizeof(Redir));
                        memset(r, 0, sizeof(Redir));
                        r->fd = fd;
                        r->mode = R_HEREDOC;
                        r->next = c->redirs;
                        c->redirs = r;
                        lex_push_hdoc(lx, r, str_cstr(&delim), 0);
                        str_free(&delim);
                    } else {
                        /* The redirection owns the word from here: it is
                         * expanded when the command runs. */
                        add_redir(c, fd, mode, target);
                        target = 0;
                    }
                    word_free(target);
                }
                continue;
            }
            /* Not a redirection: the number was a word after all. */
            add_argv(c, w);
            saw_word = 1;
            continue;
        }

        if (is_redir_op(p)) {
            int op = p->op;
            lex_next(lx);
            int fd = 1;
            int mode = redir_mode_of(op, &fd);
            if (mode < 0) {
                cmd_free(c);
                return 0;
            }

            /* `<<- word' drops the tabs off the front of the body */
            int strip_tabs = 0;
            Word* target = 0;
            if (mode == R_HEREDOC && op == OP_DLT) {
                const Tok* d = PEEK(lx);
                if (d->type == T_WORD && d->word && d->word->kind == W_LIT &&
                    !d->word->quoted && d->word->text.len > 0 &&
                    d->word->text.p[0] == '-') {
                    strip_tabs = 1;
                    Word* dw = lex_take(lx);
                    lex_next(lx);
                    Word* rest = word_new(W_LIT);
                    if (rest) {
                        str_addn(&rest->text, str_cstr(&dw->text) + 1,
                                 dw->text.len - 1);
                        target = rest;
                    }
                    word_free(dw);
                }
            }
            /* `<( list )' feeds the command from the list's output and
             * `>( list )' drains the command into its input, either on
             * its own or as what a `<' or `>' points at. */
            int psub_op = 0;
            if (op == OP_PSUB_IN || op == OP_PSUB_OUT) {
                psub_op = op;
            } else {
                const Tok* pt = PEEK(lx);
                if (pt->type == T_OP &&
                    (pt->op == OP_PSUB_IN || pt->op == OP_PSUB_OUT)) {
                    psub_op = pt->op;
                    lex_next(lx);
                }
            }
            if (psub_op) {
                if (!parse_procsub_target(lx, c, fd, psub_op)) {
                    cmd_free(c);
                    return 0;
                }
                continue;
            }
            if (!target)
                target = take_redir_target(lx, op);
            if (!target && !is_dup_op(op)) {
                dbg_pos(lx, "syntax error after");
                sh_err("vash: syntax error after ");
                sh_err(tok_op_name(op));
                sh_err("\n");
                cmd_free(c);
                return 0;
            }

            Str body;
            str_init(&body);
            if (op == OP_TLT) {
                /* `<<< word' is the word itself and a newline, expanded
                 * when the command runs, so the word stays on the
                 * redirection rather than being read into the body now. */
                add_redir(c, fd, R_HEREDOC, target);
                target = 0;
                str_free(&body);
                continue;
            } else if (mode == R_HEREDOC) {
                Str delim;
                str_init(&delim);
                if (target)
                    expand_word_one(target, &delim);
                word_free(target);
                Redir* r = (Redir*)malloc(sizeof(Redir));
                memset(r, 0, sizeof(Redir));
                r->fd = fd;
                r->mode = R_HEREDOC;
                r->next = c->redirs;
                c->redirs = r;
                lex_push_hdoc(lx, r, str_cstr(&delim), strip_tabs);
                str_free(&delim);
                str_free(&body);
                continue;
            } else if (target) {
                add_redir(c, fd, mode, target);
                str_free(&body);
                continue;
            }
            str_free(&body);
        }

        if (p->type == T_WORD) {
            /* `name() { ... }' is a function definition and `arr=(one
             * two)' is an array assignment, and both start where a
             * command would otherwise start. */
            int assign_start = !saw_word && c->argc == 0 &&
                               p->word->kind == W_LIT && !p->word->quoted &&
                               p->word->text.len > 0 &&
                               str_cstr(&p->word->text)[p->word->text.len - 1] == '=';
            if (assign_start || (!saw_word && c->argc == 0 &&
                                 word_is_name(p->word))) {
                Word* w = lex_take(lx);
                lex_next(lx);
                const Tok* q = PEEK(lx);
                int is_def = (q->type == T_OP && q->op == OP_LPAREN) ||
                             (q->type == T_WORD && is_reserved(q->word, "()"));
                /* The word ends in `=', so the `(' opens an element
                 * instead of a parameter list. */
                const char* head = str_cstr(&w->text);
                int arrayish = assign_start &&
                               q->type == T_OP && q->op == OP_LPAREN;
                if (is_def && !arrayish) {
                    Str nm;
                    str_init(&nm);
                    expand_word_one(w, &nm);
                    Cmd* f = parse_function(lx, str_cstr(&nm));
                    str_free(&nm);
                    word_free(w);
                    cmd_free(c);
                    return f;
                }
                if (arrayish) {
                    Word* arr = word_new(W_ARRAY);
                    str_addn(&arr->text, head, w->text.len);
                    word_free(w);
                    arr->items = (Vec*)malloc(sizeof(Vec));
                    vec_init(arr->items);
                    lex_next(lx); /* the `(' */
                    int depth = 1;
                    while (depth > 0) {
                        const Tok* el = PEEK(lx);
                        if (el->type == T_EOF || el->type == T_NEWLINE)
                            break;
                        if (el->type == T_OP && el->op == OP_LPAREN) {
                            ++depth;
                            lex_next(lx);
                            vec_push(arr->items, word_literal("("));
                            continue;
                        }
                        if (el->type == T_OP && el->op == OP_RPAREN) {
                            --depth;
                            lex_next(lx);
                            if (!depth)
                                break;
                            vec_push(arr->items, word_literal(")"));
                            continue;
                        }
                        if (el->type == T_OP && el->op == OP_SEMI) {
                            lex_next(lx);
                            continue;
                        }
                        Word* part = lex_take(lx);
                        lex_next(lx);
                        if (!part)
                            continue;
                        /* One element is one word with its own chain
                         * behind it — `$x', `a$x' — and the next element
                         * is a word of its own in the list. */
                        vec_push(arr->items, part);
                    }
                    if (depth > 0)
                        sh_err("vash: expected `)' to close the array\n");
                    add_argv(c, arr);
                    saw_word = 1;
                    continue;
                }
                add_argv(c, w);
                saw_word = 1;
                continue;
            }
            Word* w = lex_take(lx);
            lex_next(lx);
            add_argv(c, w);
            saw_word = 1;
            continue;
        }
        break;
    }

    if (!saw_word && !c->redirs && c->argc == 0) {
        cmd_free(c);
        return 0;
    }
    return c;
}

/* A closing keyword, stepped over. Anything else is reported and left
 * where it is, so one missing `fi' does not swallow the rest of the
 * script. */
/* `&&', `||' and `|' reach over a line break: whatever they introduce
 * may sit on the next line. */
static void skip_nl(Lex* lx)
{
    while (PEEK(lx)->type == T_NEWLINE)
        lex_next(lx);
}

static int expect_reserved(Lex* lx, const char* name)
{
    /* The closing words may sit on their own line, and a `;' may come
     * before them: `if { echo x; }; then' says it that way. */
    while (PEEK(lx)->type == T_NEWLINE ||
           (PEEK(lx)->type == T_OP && PEEK(lx)->op == OP_SEMI))
        lex_next(lx);
    const Tok* p = PEEK(lx);
    if (p->type == T_WORD && is_reserved(p->word, name)) {
        lex_next(lx);
        return 1;
    }
    sh_err("vash: expected `");
    sh_err(name);
    sh_err("'\n");
    return 0;
}

/* --- if/elif/else --- */

static Cmd* parse_if_tail(Lex* lx)
{
    Cmd* c = cmd_new(C_IF);
    c->left = parse_list_inner(lx, STOP_THEN);
    expect_reserved(lx, "then");
    c->right = parse_list_inner(lx, STOP_BODY);

    const Tok* p = PEEK(lx);
    if (p->type == T_WORD && is_reserved(p->word, "elif")) {
        lex_next(lx);
        /* An `elif' is an `if' with no keyword in front of it, and it
         * nests in the branch it guards. */
        c->init = parse_if_tail(lx);
        return c;
    }
    if (p->type == T_WORD && is_reserved(p->word, "else")) {
        lex_next(lx);
        c->init = parse_list_inner(lx, STOP_BODY);
    }
    expect_reserved(lx, "fi");
    return c;
}

/* --- while/until --- */

static Cmd* parse_while(Lex* lx, int until)
{
    Cmd* c = cmd_new(until ? C_UNTIL : C_WHILE);
    c->left = parse_list_inner(lx, STOP_DO);
    expect_reserved(lx, "do");
    c->right = parse_list_inner(lx, STOP_BODY);
    expect_reserved(lx, "done");
    return c;
}

/* --- for --- */

static Cmd* parse_for(Lex* lx)
{
    const Tok* p = PEEK(lx);
    if (p->type == T_OP && p->op == OP_LPAREN) {
        /* `for ((init; cond; step))' — the C-style loop keeps its
         * expression as text and walks it when the loop runs. */
        lex_next(lx);
        if (PEEK(lx)->type == T_OP && PEEK(lx)->op == OP_LPAREN) {
            lex_next(lx);
            Cmd* c = cmd_new(C_ARITHFOR);
            read_arith_body(lx, c, 1);
            if (PEEK(lx)->type == T_OP && PEEK(lx)->op == OP_SEMI)
                lex_next(lx);
            expect_reserved(lx, "do");
            c->right = parse_list_inner(lx, STOP_BODY);
            expect_reserved(lx, "done");
            return c;
        }
        sh_err("vash: `(' after `for'\n");
        return 0;
    }

    Cmd* c = cmd_new(C_FOR);
    if (p->type != T_WORD || !word_is_name(p->word)) {
        sh_err("vash: `for' wants a variable name\n");
        return 0;
    }
    c->name = lex_take(lx);
    lex_next(lx);

    /* The word list is optional: `for f do' walks "$@" . */
    if (PEEK(lx)->type == T_WORD && is_reserved(PEEK(lx)->word, "in")) {
        lex_next(lx);
        for (;;) {
            const Tok* q = PEEK(lx);
            if (q->type != T_WORD || is_reserved(q->word, "do"))
                break;
            add_word(c, lex_take(lx));
            lex_next(lx);
        }
    }
    if (PEEK(lx)->type == T_OP && PEEK(lx)->op == OP_SEMI)
        lex_next(lx);
    expect_reserved(lx, "do");
    c->right = parse_list_inner(lx, STOP_BODY);
    expect_reserved(lx, "done");
    return c;
}

/* --- case --- */

static Cmd* parse_case(Lex* lx)
{
    dbg_pos(lx, "parse_case");
    Cmd* c = cmd_new(C_CASE);
    const Tok* p = PEEK(lx);
    if (p->type != T_WORD) {
        sh_err("vash: `case' wants a word to test\n");
        return 0;
    }
    c->name = lex_take(lx);
    lex_next(lx);
    expect_reserved(lx, "in");
    dbg_pos(lx, "after in");

    for (;;) {
        p = PEEK(lx);
        while (p->type == T_NEWLINE ||
               (p->type == T_OP && p->op == OP_SEMI)) {
            lex_next(lx);
            p = PEEK(lx);
        }
        if (p->type == T_EOF)
            break;
        if (p->type == T_WORD && is_reserved(p->word, "esac")) {
            lex_next(lx);
            break;
        }
        dbg_pos(lx, "case item");
        Cmd* item = cmd_new(C_CASE);
        int patterns = 0;
        for (;;) {
            /* The patterns run up to the `)' that closes them: the
             * `a*|b*)' form. */
            p = PEEK(lx);
            if (p->type == T_OP && p->op == OP_RPAREN) {
                lex_next(lx);
                break;
            }
            if (p->type != T_WORD)
                break;
            add_word(item, lex_take(lx));
            lex_next(lx);
            patterns = 1;
            p = PEEK(lx);
            if (p->type == T_OP && p->op == OP_PIPE) {
                lex_next(lx);
                continue;
            }
        }
        if (!patterns) {
            dbg_pos(lx, "no patterns");
            sh_err("vash: [dbg] patterns=0\n");
            /* No `)' and no pattern: the list is malformed. Step over one
             * token so the parser cannot sit on it forever. */
            lex_next(lx);
            cmd_free(item);
            break;
        }
        item->right = parse_list_inner(lx, STOP_DSEMI);
        dbg_pos(lx, "case body");
        add_list(c, item);

        p = PEEK(lx);
        if (p->type == T_OP && (p->op == OP_DUP || p->op == OP_SEMI))
            lex_next(lx);
    }
    return c;
}

/* --- [[ ... ]] --- */

static Cmd* parse_dblbracket(Lex* lx)
{
    Cmd* c = cmd_new(C_TEST);
    for (;;) {
        const Tok* p = PEEK(lx);
        if (p->type == T_EOF || p->type == T_NEWLINE)
            break;
        if (p->type == T_WORD && is_reserved(p->word, "]]")) {
            lex_next(lx);
            break;
        }
        if (p->type == T_WORD) {
            add_word(c, lex_take(lx));
            lex_next(lx);
            continue;
        }
        if (p->type == T_OP) {
            /* An operator keeps its place inside the brackets: there it
             * is the test's own operator, not a redirection. */
            Str op;
            str_init(&op);
            str_adds(&op, tok_op_name(p->op));
            add_literal_word(c, &op);
            str_free(&op);
            lex_next(lx);
            continue;
        }
        lex_next(lx);
    }
    return c;
}

/* --- (( ... )) --- */

static Cmd* parse_arithcmd(Lex* lx)
{
    Cmd* c = cmd_new(C_ARITHCMD);
    read_arith_body(lx, c, 0);
    return c;
}

static Cmd* parse_command(Lex* lx)
{
    const Tok* p = PEEK(lx);
    if (p->type == T_WORD) {
        if (is_reserved(p->word, "if")) {
            lex_next(lx);
            return parse_if_tail(lx);
        }
        if (is_reserved(p->word, "while")) {
            lex_next(lx);
            return parse_while(lx, 0);
        }
        if (is_reserved(p->word, "until")) {
            lex_next(lx);
            return parse_while(lx, 1);
        }
        if (is_reserved(p->word, "for")) {
            lex_next(lx);
            return parse_for(lx);
        }
        if (is_reserved(p->word, "case")) {
            lex_next(lx);
            return parse_case(lx);
        }
        if (is_reserved(p->word, "[[")) {
            lex_next(lx);
            return parse_dblbracket(lx);
        }
    }
    if (p->type == T_OP && p->op == OP_LBRACE) {
        /* `{ list }' is a command of its own, and only its exit status
         * says anything: no new scope, no new redirections. */
        lex_next(lx);
        Cmd* c = cmd_new(C_GROUP);
        c->left = parse_list_inner(lx, STOP_ANY);
        p = PEEK(lx);
        if (p->type == T_OP && p->op == OP_RBRACE)
            lex_next(lx);
        else
            sh_err("vash: expected `}'\n");
        return c;
    }
    if (p->type == T_OP && p->op == OP_LPAREN) {
        lex_next(lx);
        p = PEEK(lx);
        if (p->type == T_OP && p->op == OP_LPAREN) {
            /* `(( expr ))' is an arithmetic command; `( list )' is a
             * subshell around a list of commands. */
            lex_next(lx);
            return parse_arithcmd(lx);
        }
        Cmd* c = cmd_new(C_GROUP);
        c->left = parse_list_inner(lx, STOP_ANY);
        p = PEEK(lx);
        if (p->type == T_OP && p->op == OP_RPAREN)
            lex_next(lx);
        else
            sh_err("vash: expected `)\'\n");
        return c;
    }
    return parse_simple(lx);
}

/* `(' list `)': a subshell around a list of commands, without a scope
 * of its own - the same thing inside `<( )'. */
static Cmd* parse_paren_group(Lex* lx)
{
    /* The `(' came in with the operator: `<(' and `>(' are one token. */
    Cmd* c = cmd_new(C_GROUP);
    c->left = parse_list_inner(lx, STOP_ANY);
    const Tok* p = PEEK(lx);
    if (p->type == T_OP && p->op == OP_RPAREN)
        lex_next(lx);
    else
        sh_err("vash: expected `)' to close the group\n");
    return c->left ? c : 0;
}

static Cmd* parse_pipeline(Lex* lx)
{
    Cmd* first = parse_command(lx);
    if (!first)
        return 0;
    const Tok* p = PEEK(lx);
    if (!(p->type == T_OP && p->op == OP_PIPE))
        return first;

    Cmd* pipe = cmd_new(C_PIPE);
    pipe->left = first;
    while (p->type == T_OP && p->op == OP_PIPE) {
        lex_next(lx);
        skip_nl(lx);
        Cmd* stage = parse_command(lx);
        if (!stage) {
            sh_err("vash: syntax error after `|'\n");
            break;
        }
        add_list(pipe, stage);
        p = PEEK(lx);
    }
    return pipe;
}

static Cmd* parse_and_or(Lex* lx)
{
    Cmd* left = parse_pipeline(lx);
    /* Every here-document on this line is read now that the line has
     * been read: the body of one is what follows the command. */
    lex_collect_hdocs(lx);
    if (!left)
        return 0;
    for (;;) {
        const Tok* p = PEEK(lx);
        if (p->type != T_OP || (p->op != OP_AND && p->op != OP_OR))
            return left;
        int type = (p->op == OP_AND) ? C_AND : C_OR;
        int failed_op = p->op;
        lex_next(lx);
        skip_nl(lx);
        Cmd* right = parse_pipeline(lx);
        if (!right) {
            dbg_pos(lx, "and/or");
            sh_err("vash: syntax error after `");
            sh_err(tok_op_name(failed_op));
            sh_err("'\n");
            return left;
        }
        Cmd* c = cmd_new(type);
        c->left = left;
        c->right = right;
        left = c;
    }
}

/* One or more commands joined by `;' and newlines, stopping at
 * end of input, `)' or `}' — whatever ends the construct it is part
 * of. */
/* The closing word of the list being read, or 0 for none. A list is also
 * cut short by `)' , `}' and, inside `case', by `;;'. */
static int stop_word(int stop, const Tok* p)
{
    if (p->type != T_WORD || !p->word || p->word->quoted)
        return 0;
    const char* w = str_cstr(&p->word->text);
    switch (stop) {
    case STOP_THEN:
        return strcmp(w, "then") == 0 || strcmp(w, "fi") == 0 ||
               strcmp(w, "elif") == 0 || strcmp(w, "else") == 0;
    case STOP_DO:
        return strcmp(w, "do") == 0 || strcmp(w, "done") == 0;
    case STOP_BODY:
        return strcmp(w, "then") == 0 || strcmp(w, "fi") == 0 ||
               strcmp(w, "elif") == 0 || strcmp(w, "else") == 0 ||
               strcmp(w, "done") == 0 || strcmp(w, "esac") == 0;
    case STOP_DSEMI:
        /* The last arm of a `case' may leave out its `;;'. */
        return strcmp(w, "esac") == 0;
    case STOP_IN:
        return strcmp(w, "in") == 0;
    default:
        return 0;
    }
}

static Cmd* parse_list_inner(Lex* lx, int stop)
{
    Cmd* head = 0;

    for (;;) {
        const Tok* p = PEEK(lx);
        while (p->type == T_NEWLINE ||
               (p->type == T_OP && (p->op == OP_SEMI || p->op == OP_AMP))) {
            lex_next(lx);
            p = PEEK(lx);
        }
        if (p->type == T_EOF)
            break;
        if (p->type == T_OP && (p->op == OP_RPAREN || p->op == OP_RBRACE))
            break;
        if (stop == STOP_DSEMI && p->type == T_OP && p->op == OP_DUP)
            break;
        if (stop_word(stop, p))
            break;

        Cmd* c = parse_and_or(lx);
        if (!c) {
            /* Something could not be parsed. Step over one token so the
             * parser cannot sit on it forever. */
            const Tok* q = PEEK(lx);
            if (q->type != T_EOF)
                lex_next(lx);
            continue;
        }

        /* `cmd &' sends what was just read off to run on its own. */
        const Tok* q = PEEK(lx);
        if (q->type == T_OP && q->op == OP_AMP) {
            lex_next(lx);
            Cmd* bg = cmd_new(C_BG);
            bg->left = c;
            c = bg;
        }

        /* The commands of a sequence hang off one node in the order they
         * were read, rather than each one becoming the left arm of the
         * node before it: a script is not shy about being two thousand
         * top-level commands long, and a chain that deep is a chain that
         * has to be walked, run and freed without recursing down it. */
        if (!head)
            head = cmd_new(C_SEQ);
        if (!head)
            return 0;
        add_list(head, c);

        p = PEEK(lx);
        if (p->type == T_OP && (p->op == OP_SEMI || p->op == OP_AMP))
            continue;
        if (p->type == T_NEWLINE)
            continue;
        break;
    }
    return head;
}

Cmd* parse_list(Lex* lx, int to_eof)
{
    (void)to_eof;
    return parse_list_inner(lx, STOP_ANY);
}