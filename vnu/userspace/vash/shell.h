/*
 * shell.h — vash's parser/executor internals, shared by lex.c,
 * parse.c, expand.c, vars.c, exec.c, arith.c and builtins.c.
 */
#pragma once

#include <vlibc/unistd.h>
#include <vlibc/stdio.h>
#include <vlibc/stdlib.h>
#include <vlibc/string.h>
#include <vlibc/fcntl.h>

#define SH_LINE_MAX 1024   /* one input line (a `#!` line, a command) */
#define SH_MAX_ARGS 64     /* argv slots per command */
#define SH_MAX_FIELDS 128  /* split fields per expansion */
#define SH_PATH_MAX 256

/* --- growable byte string (vlibc has no realloc) ---
 * Everything the shell builds — a word, a command line, a captured
 * command substitution — is one of these. */
typedef struct {
    char* p;
    int len;
    int cap;
} Str;

void str_init(Str* s);
void str_free(Str* s);
void str_reset(Str* s);
void str_addc(Str* s, char c);
void str_addn(Str* s, const char* t, int n);
void str_adds(Str* s, const char* t);
void str_set(Str* s, const char* t);
char* str_cstr(Str* s); /* NUL-terminates and returns p */

/* --- growable pointer vector --- */
typedef struct {
    void** v;
    int n;
    int cap;
} Vec;

void vec_init(Vec* v);
void vec_push(Vec* v, void* p);
void vec_free(Vec* v);

/* --- string helpers (vlibc has no strchr/strstr/strcasecmp) --- */
int sh_starts(const char* s, const char* p);
int sh_ends(const char* s, const char* p);
const char* sh_find(const char* s, const char* p);   /* first p in s */
const char* sh_rfind(const char* s, const char* p);  /* last p in s */
int sh_eqi(const char* a, const char* b);           /* case-insensitive */
void sh_itoa(long v, Str* out);
long sh_atol(const char* s);
void sh_utoa(unsigned long v, Str* out);
char* sh_strdup(const char* s);

/* --- words -------------------------------------------------------------
 * A word is a list of parts, kept unexpanded until the command that
 * owns it runs: `x=hi; echo $x` has to see the assignment first, and a
 * loop body has to see every turn's value. */
enum {
    W_LIT = 0, /* literal text (possibly quoted: no glob, no split) */
    W_VAR,     /* $name / ${name} */
    W_CMDSUB,  /* $(...) or `...` */
    W_ARITH,   /* $((...)) */
    W_TILDE,   /* ~ at the start of a word */
    W_BRACE,   /* ${x...} — the whole body, expanded by expand.c */
    W_FILE,    /* $(< file) — the file, with no command run */
    W_ARRAY    /* name=(a b) — the elements are the parts behind it */
};

typedef struct Word Word;
struct Word {
    int kind;
    int quoted;   /* the expansion is inside quotes: no field splitting */
    Str text;     /* W_LIT text, W_VAR name, W_CMDSUB body, W_BRACE body */
    int len_only; /* ${#name} — handled in W_BRACE */
    Word* next;
    Vec* items;   /* W_ARRAY: one word per element, each with its own
                   * chain behind it */
};

Word* word_new(int kind);
Word* word_literal(const char* text);
void word_free(Word* w);

/* --- redirections --- */
enum {
    R_IN = 0,    /* < file */
    R_OUT,       /* > file */
    R_APPEND,    /* >> file */
    R_DUP_IN,    /* <& n */
    R_DUP_OUT,   /* >& n */
    R_BOTH,      /* &> file */
    R_BOTH_APP,  /* &>> file */
    R_HEREDOC,   /* << delimiter */
    R_PIPE       /* <( list ) or >( list ): a pipe, not a file */
};

typedef struct Cmd Cmd;
typedef struct Redir Redir;
struct Redir {
    int fd;     /* the descriptor being changed, -1 for &> */
    int mode;
    Word* target; /* expanded when the command runs, not when read */
    char* text;   /* the target, expanded early for one execution */
    char* heredoc; /* contents for R_HEREDOC */
    int heredoc_quoted; /* delimiter was quoted */
    Cmd* procsub;  /* the list behind <( ) or >( ) */
    int procsub_out; /* and whether it is the output side */
    Redir* next;
};

/* A here-document whose body has not been read yet: the text is the
 * lines that follow the whole command, not the line it was written on,
 * so `cat <<EOF > file' keeps both the redirection and the body. */
typedef struct HDoc HDoc;
struct HDoc {
    struct HDoc* next;
    char delim[64];
    int strip_tabs;
    Redir* redir;
};

void sh_reap_procsubs(void);

/* --- commands --- */
enum { C_SIMPLE = 0, C_PIPE, C_AND, C_OR, C_SEQ, C_BG, C_FUNC, C_SUB, C_IF,
       C_WHILE, C_UNTIL, C_FOR, C_CASE, C_GROUP, C_TEST, C_ARITHCMD,
       C_ARITHFOR };

struct Cmd {
    int type;
    Word** argv;      /* C_SIMPLE */
    int argc;
    Redir* redirs;
    Cmd* left;        /* && || ; & */
    Cmd* right;       /* pipelines, the body of a compound, the else */
    Cmd** list;       /* the branches of a compound (if/elif chain, case) */
    int nlist;
    Str text;         /* C_FUNC body, C_SUB/C_GROUP raw text */
    Word* name;       /* C_FUNC name, C_FOR/C_WHILE condition */
    Cmd* next;        /* the next element of a list */
    Cmd* init;        /* C_FOR init clause (the `((...))` or a pipeline) */
    Word** words;     /* C_FOR word list after `in` */
    int nwords;
};

Cmd* cmd_new(int type);
void cmd_free(Cmd* c);

/* --- the lexer -------------------------------------------------------- */
enum {
    T_EOF = 0,
    T_WORD,
    T_NEWLINE,
    T_OP
};

enum {
    OP_SEMI = 1,   /* ; */
    OP_AMP,        /* & */
    OP_AND,        /* && */
    OP_OR,         /* || */
    OP_PIPE,       /* | */
    OP_DUP,        /* ;; inside case */
    OP_LT,         /* < */
    OP_GT,         /* > */
    OP_DLT,        /* << */
    OP_TLT,        /* <<< */
    OP_DGT,        /* >> */
    OP_AMPGT,      /* &> */
    OP_AMPGTGT,    /* &>> */
    OP_LTAMP,      /* <& */
    OP_AMPGTAMP,   /* >& */
    OP_LPAREN,     /* ( */
    OP_RPAREN,     /* ) */
    OP_PSUB_IN,    /* <( list ) — the list's output is the input */
    OP_PSUB_OUT,   /* >( list ) — the list's input is the output */
    OP_LBRACE,     /* { as a reserved word */
    OP_RBRACE      /* } as a reserved word */
};

typedef struct Tok Tok;
struct Tok {
    int type;
    int op;
    Word* word;     /* T_WORD */
};

typedef struct Lex Lex;
struct Lex {
    int fd;             /* script source, -1 when reading a string */
    char* text;         /* string source */
    int textlen;        /* how long that string is */
    int len;            /* bytes in the current line buffer */
    int pos;            /* how much of the string they came from */
    char line[SH_LINE_MAX];
    int linepos;
    int linenr;
    int linegen;        /* which line buffer generation is loaded */
    int ateof;
    Tok tok;            /* the token most recently read */
    int have_peek;
    Tok peek;
    Tok peek2;         /* the token after the peeking one */
    int have_peek2;
    struct HDoc* hd;   /* here-documents still waiting for their text */
    char err[192];
};

void lex_init_file(Lex* lx, int fd);
void lex_init_str(Lex* lx, const char* text, int len);
void lex_next(Lex* lx);           /* read the next token into lx->tok */
const Tok* lex_peek(Lex* lx);     /* one token of lookahead */
const Tok* lex_peek2(Lex* lx);    /* two, to spot the `))' of `(( ... ))' */
Word* lex_take(Lex* lx);          /* the peeking token's word, now ours */
void lex_heredoc(Lex* lx, const char* delim, int strip_tabs,
                 int at_line_start, Str* body);
void lex_push_hdoc(Lex* lx, Redir* r, const char* delim, int strip_tabs);
void lex_collect_hdocs(Lex* lx);
void lex_free(Lex* lx);
const char* tok_op_name(int op);

/* --- variables -------------------------------------------------------- */
typedef struct AEnt AEnt;
struct AEnt {
    char* key;   /* element index, or an associative array's key */
    char* val;
};

typedef struct Var Var;
struct Var {
    char* name;
    char* val;
    int exported;
    int is_array;
    int is_assoc;
    AEnt* el;
    int nel;
    int cap;
    Var* next;
};

void var_init(void);
Var* var_find(const char* name);
const char* var_get(const char* name);
int var_set(const char* name, const char* val);
int var_setn(const char* name, const char* val, int n);
void var_export(const char* name, int on);
int var_unset(const char* name);
void var_export_all(int on);
int var_allexport(void);
const char* var_lookup_word(const char* name); /* $0, $?, $1 ... */
char** var_env_array(void);                    /* the exported environment */

const char* arr_get(const char* name, const char* key);
int arr_set(const char* name, const char* key, const char* val);
int arr_append(const char* name, const char* val);
int arr_len(const char* name);
void arr_clear(const char* name);
int arr_is_assoc(const char* name);
void arr_make_assoc(const char* name);
char** arr_keys(const char* name, int* count);
char** var_names(int* count);  /* every name; borrowed, free the array only */

/* --- functions --------------------------------------------------------
 * A function keeps the command tree of its body: defining it parses the
 * body once, and every call runs that tree. */
typedef struct Func Func;
struct Func {
    char* name;
    Cmd* body;
    Func* next;
};

Func* func_find(const char* name);
void func_define(const char* name, Cmd* body);
int func_undef(const char* name);

/* --- pattern matching (fnmatch.c) --- */
int sh_pattern(const char* pat, const char* str);
int sh_strip_prefix(const char* str, const char* pat, int longest, char* out, int outn);
int sh_strip_suffix(const char* str, const char* pat, int longest, char* out, int outn);
int sh_find_sub(const char* str, const char* pat, int from);

/* --- expansion -------------------------------------------------------- */
void expand_word(Word* w, Vec* out);           /* split + glob */
void expand_word_one(Word* w, Str* out);        /* no split, no glob */
int sh_glob(const char* pattern, Vec* out);     /* 1 if it matched */

/* --- arithmetic ------------------------------------------------------- */
long arith_eval(const char* expr, int* ok);
char* sh_expand_arith(const char* expr);

/* --- parsing (parse.c) ------------------------------------------------ */
Cmd* parse_list(Lex* lx, int to_eof);

/* --- execution -------------------------------------------------------- */
enum { FLOW_NONE = 0, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN, FLOW_EXIT };

extern int sh_status;      /* $? */
extern int sh_flow;        /* FLOW_*, set by break/continue/return/exit */
extern int sh_flow_arg;    /* what return/exit asked for */
extern int sh_argc;        /* $# */
extern char** sh_argv;     /* $0, $1 ... */
extern char* sh_ifs;       /* $IFS */
extern int sh_set_e;       /* set -e */
extern int sh_nounset;     /* set -u */
extern int sh_pipefail;    /* set -o pipefail */

int run_command_list(Cmd* c);
int run_string(const char* text, int capture);
char* run_capture(const char* text);  /* $(...) — the output as a string */
char* read_whole_file(const char* path); /* $(< file) — its contents */
int run_subshell(Cmd* c);
int sh_exec(Cmd* c);
int call_function(const char* name, Cmd* call);

/* --- builtins --------------------------------------------------------- */
int builtin_run(int argc, char** argv);
int sh_test_args(int argc, char** argv, int start);
void sh_set_params(char** argv, int argc);
void sh_locals_begin(void);
void sh_locals_end(void);
int is_builtin_name(const char* name);
void builtin_set_shell(char* argv0, char** argv, int argc);
int builtin_source_file(const char* path);
void builtin_set_ifs(const char* ifs);

/* --- process helpers (exec.c) ------------------------------------------ */
void sh_resolve(const char* cmd, char* out, int outn);
void sh_set_status(int status);

/* --- the shell's own stdout helpers (vash.c) --- */
void sh_out(const char* s);
void sh_err(const char* s);