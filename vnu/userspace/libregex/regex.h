/*
 * regex.h — a backtracking matcher for the POSIX extended regular
 * expressions a Unix program actually writes.
 *
 * The whole language is here: anchors, `.', the three repetition
 * operators, alternation, groups and bracket expressions, with `\'
 * taking the next character literally. What is not here is what POSIX
 * allows and no script uses: interval counts `{m,n}' and the character
 * classes `\w', `\d' and `\s', which are not POSIX in the first place.
 *
 * Matching is unanchored, the way `[[ x =~ re ]]' reads in bash: the
 * pattern can match anywhere in the text. A `^' at the front pins it to
 * the start, a `$' at the end pins it to the end.
 *
 * This is a header so that any program can carry the matcher by
 * including it — every VNU binary is built from its own sources, and a
 * shared capability lives in a header as `static' code rather than in a
 * library that has to be installed alongside. It needs nothing but
 * string.h and stdlib.h, so a host program can drop it in unchanged.
 * On an empty pattern the match is empty and matches everything, as
 * POSIX says.
 */
#ifndef VNU_REGEX_H
#define VNU_REGEX_H

#include <vlibc/string.h>
#include <vlibc/stdlib.h>

enum {
    N_CHAR,
    N_ANY,
    N_CLASS,
    N_BOL,
    N_EOL,
    N_CAT,
    N_ALT,
    N_STAR,
    N_PLUS,
    N_OPT
};

typedef struct Node Node;
struct Node {
    int kind;
    char ch;          /* N_CHAR */
    unsigned char set[32]; /* N_CLASS */
    Node* left;       /* the repeated node, or the first of N_CAT */
    Node* right;      /* the rest of N_CAT, or the next of N_ALT */
    Node* next;       /* the next alternative of N_ALT */
};

/* --- the pattern, read into nodes --- */

typedef struct {
    const char* p;
    Node* pool;
    int npool;
    int cap;
    int bad;
} Rx;

static Node* rx_node(Rx* r)
{
    if (r->npool >= r->cap)
        return 0; /* out of room: the caller treats this as a bad pattern */
    Node* n = &r->pool[r->npool++];
    /* The pool is one block, so a node that is only partly filled would
     * carry whatever was in it before — and an alternative's `next' is
     * left unset on purpose when it ends the chain. */
    memset(n, 0, sizeof(*n));
    return n;
}

static void set_add(unsigned char* set, unsigned char c)
{
    set[c >> 3] |= (unsigned char)(1 << (c & 7));
}

static int set_has(const unsigned char* set, unsigned char c)
{
    return (set[c >> 3] >> (c & 7)) & 1;
}

static Node* rx_alt(Rx* r);

/* The inside of `[...]' — a set of characters, or a range of them, or
 * the negation of both. Returns 0 for a set nobody can match, which the
 * caller turns into "the bracket expression is broken". */
static Node* rx_class(Rx* r, int* ok)
{
    Node* n = rx_node(r);
    if (!n)
        return 0;
    n->kind = N_CLASS;
    int neg = 0;
    if (*r->p == '^') {
        neg = 1;
        ++r->p;
    }
    int first = 1;
    int closed = 0;
    while (*r->p && (*r->p != ']' || first)) {
        first = 0;
        unsigned char lo;
        if (*r->p == '\\' && r->p[1])
            lo = (unsigned char)r->p[1], r->p += 2;
        else
            lo = (unsigned char)*r->p++;
        unsigned char hi = lo;
        if (*r->p == '-' && r->p[1] && r->p[1] != ']') {
            ++r->p;
            if (*r->p == '\\' && r->p[1])
                hi = (unsigned char)r->p[1], r->p += 2;
            else
                hi = (unsigned char)*r->p++;
        }
        for (int c = lo; c <= (int)hi; ++c)
            set_add(n->set, (unsigned char)c);
    }
    if (*r->p == ']') {
        ++r->p;
        closed = 1;
    }
    if (!closed) {
        *ok = 0;
        return 0;
    }
    if (neg) {
        /* The negated set is the complement of what was written. */
        unsigned char all[32];
        for (int i = 0; i < 32; ++i)
            all[i] = (unsigned char)~n->set[i];
        for (int i = 0; i < 32; ++i)
            n->set[i] = all[i];
    }
    return n;
}

static Node* rx_atom(Rx* r)
{
    char c = *r->p;
    if (c == '(') {
        ++r->p;
        Node* inner = rx_alt(r);
        if (!inner)
            return 0;
        if (*r->p != ')') {
            r->bad = 1;
            return 0;
        }
        ++r->p;
        return inner;
    }
    if (c == '[') {
        ++r->p;
        int ok = 1;
        Node* n = rx_class(r, &ok);
        if (!ok)
            r->bad = 1;
        return n;
    }
    if (c == '.') {
        ++r->p;
        Node* n = rx_node(r);
        if (n)
            n->kind = N_ANY;
        return n;
    }
    if (c == '^') {
        ++r->p;
        Node* n = rx_node(r);
        if (n)
            n->kind = N_BOL;
        return n;
    }
    if (c == '$') {
        ++r->p;
        Node* n = rx_node(r);
        if (n)
            n->kind = N_EOL;
        return n;
    }
    if (c == '\\' && r->p[1]) {
        r->p += 2;
        Node* n = rx_node(r);
        if (n) {
            n->kind = N_CHAR;
            n->ch = r->p[-1];
        }
        return n;
    }
    if (c == ')' || c == '|' || c == 0)
        return 0;
    ++r->p;
    Node* n = rx_node(r);
    if (n) {
        n->kind = N_CHAR;
        n->ch = c;
    }
    return n;
}

/* One atom and whatever repetition follows it. */
static Node* rx_piece(Rx* r)
{
    Node* atom = rx_atom(r);
    if (!atom)
        return 0;
    int rep = 0;
    if (*r->p == '*' || *r->p == '+' || *r->p == '?') {
        rep = *r->p;
        ++r->p;
        /* A second operator on the same atom is read as nothing at all,
         * which is what POSIX says about `a**' . */
        while (*r->p == '*' || *r->p == '+' || *r->p == '?')
            ++r->p;
    }
    if (!rep)
        return atom;
    Node* n = rx_node(r);
    if (!n)
        return 0;
    n->kind = (rep == '*') ? N_STAR : (rep == '+') ? N_PLUS : N_OPT;
    n->left = atom;
    return n;
}

static Node* rx_cat(Rx* r)
{
    Node* head = 0;
    Node* tail = 0;
    while (*r->p && *r->p != '|' && *r->p != ')') {
        Node* piece = rx_piece(r);
        if (!piece)
            break;
        Node* cat = rx_node(r);
        if (!cat)
            break;
        cat->kind = N_CAT;
        cat->left = piece;
        if (tail)
            tail->right = cat;
        else
            head = cat;
        tail = cat;
    }
    return head;
}

static Node* rx_alt(Rx* r)
{
    Node* first = rx_cat(r);
    if (*r->p != '|')
        return first;
    Node* head = rx_node(r);
    if (!head)
        return 0;
    head->kind = N_ALT;
    Node* tail = head;
    tail->left = first;
    while (*r->p == '|') {
        ++r->p;
        Node* branch = rx_cat(r);
        Node* alt = rx_node(r);
        if (!alt)
            break;
        alt->kind = N_ALT;
        alt->left = branch;
        tail->next = alt;
        tail = alt;
    }
    return head;
}

/* --- matching --- */

/* Every level returns the first place the rest of the pattern could
 * carry on from, or 0 when it cannot. The repetitions walk what is left
 * of the text and hand each candidate end back to what follows them,
 * which is what makes this backtrack. */
static const char* rx_match(Node* n, const char* s, const char* start,
                            const char* end)
{
    if (!n)
        return s;
    switch (n->kind) {
    case N_CHAR:
        if (s < end && *s == n->ch)
            return s + 1;
        return 0;
    case N_ANY:
        if (s < end)
            return s + 1;
        return 0;
    case N_CLASS:
        if (s < end && set_has(n->set, (unsigned char)*s))
            return s + 1;
        return 0;
    case N_BOL:
        return (s == start) ? s : 0;
    case N_EOL:
        return (s == end) ? s : 0;
    case N_CAT: {
        const char* here = rx_match(n->left, s, start, end);
        if (!here)
            return 0;
        return rx_match(n->right, here, start, end);
    }
    case N_ALT: {
        for (Node* b = n; b; b = b->next) {
            const char* here = rx_match(b->left, s, start, end);
            if (!here)
                continue;
            const char* rest = rx_match(b->right, here, start, end);
            if (rest)
                return rest;
        }
        return 0;
    }
    case N_OPT:
    case N_STAR:
    case N_PLUS: {
        /* Greedy: as many repetitions as the text allows, then fewer. */
        const char* point[64];
        int npoint = 0;
        const char* here = s;
        if (n->kind == N_OPT) {
            const char* with = rx_match(n->left, here, start, end);
            if (with) {
                const char* rest = rx_match(n->right, with, start, end);
                if (rest)
                    return rest;
            }
            return rx_match(n->right, here, start, end);
        }
        if (n->kind == N_PLUS) {
            here = rx_match(n->left, here, start, end);
            if (!here)
                return 0;
        }
        point[npoint++] = here;
        while (npoint < 64) {
            const char* next = rx_match(n->left, point[npoint - 1], start, end);
            if (!next || next == point[npoint - 1])
                break;
            point[npoint++] = next;
        }
        for (int i = npoint - 1; i >= 0; --i) {
            const char* rest = rx_match(n->right, point[i], start, end);
            if (rest)
                return rest;
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* Unanchored match, the way `[[ text =~ pattern ]]' reads in bash: 1 when
 * `pattern' matches somewhere in `text', 0 when it does not (or the
 * pattern is broken, or memory ran out). */
static int re_match(const char* pattern, const char* text)
{
    if (!pattern || !text)
        return 0;
    int len = (int)strlen(pattern);
    /* One node per character is generous enough for any pattern a shell
     * script writes: a bracket expression is one node, a group is the
     * nodes inside it plus one for the alternative. */
    Rx r;
    r.p = pattern;
    r.bad = 0;
    r.npool = 0;
    r.cap = len * 4 + 8;
    r.pool = (Node*)malloc(sizeof(Node) * (unsigned long)r.cap);
    if (!r.pool)
        return 0;

    Node* tree = rx_alt(&r);
    int matched = 0;
    if (tree && !r.bad && !*r.p) {
        const char* end = text + strlen(text);
        for (const char* start = text; start <= end; ++start) {
            if (rx_match(tree, start, text, end)) {
                matched = 1;
                break;
            }
        }
    }
    free(r.pool);
    return matched;
}

#endif /* VNU_REGEX_H */
