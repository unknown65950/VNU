/*
 * exec.c — running what the parser built.
 *
 * Every command that is not a builtin or a shell function runs in a
 * child, and the shell waits for it: that is what makes `a; b', pipes
 * and `$(...)' possible at all. Where fork() is not available — vash
 * running inside a windowed task, say, which has no second place to
 * run a process — the shell falls back to execve()ing over itself,
 * which is how it behaved before it could fork, and is enough for one
 * command with redirections.
 */
#include "shell.h"
#include "../libregex/regex.h"
#include <vlibc/sys/wait.h>
#include <vlibc/sys/stat.h>

int sh_status = 0;
int sh_flow = FLOW_NONE;

static int run_list(Cmd* c);
int sh_flow_arg = 0;
int sh_argc = 0;
char** sh_argv = 0;
char* sh_ifs = 0;
int sh_set_e = 0;
int sh_nounset = 0;
int sh_pipefail = 0;

void sh_out(const char* s)
{
    if (s && s[0])
        write(1, s, strlen(s));
}

void sh_err(const char* s)
{
    if (s && s[0])
        write(2, s, strlen(s));
}

void sh_set_status(int status)
{
    sh_status = status;
}

static void join_path(char* out, int outn, const char* dir, const char* cmd)
{
    int i = 0;
    if (dir[0] == '/' && dir[1] == 0) {
        if (i < outn - 1)
            out[i++] = '/';
    } else {
        for (int j = 0; dir[j] && i < outn - 2; ++j)
            out[i++] = dir[j];
        if (i > 0 && out[i - 1] != '/' && i < outn - 1)
            out[i++] = '/';
    }
    for (int j = 0; cmd[j] && i < outn - 1; ++j)
        out[i++] = cmd[j];
    out[i] = 0;
}

void sh_resolve(const char* cmd, char* out, int outn)
{
    if (!cmd || !cmd[0]) {
        if (outn > 0)
            out[0] = 0;
        return;
    }
    if (cmd[0] == '/') {
        strncpy(out, cmd, (unsigned long)outn - 1);
        out[outn - 1] = 0;
        return;
    }
    const char* path = var_get("PATH");
    char dirs[SH_PATH_MAX];
    strncpy(dirs, path[0] ? path : "/bin", (unsigned long)SH_PATH_MAX - 1);
    dirs[SH_PATH_MAX - 1] = 0;
    char* save = dirs;
    while (*save) {
        char* colon = save;
        while (*colon && *colon != ':')
            ++colon;
        char saved = *colon;
        *colon = 0;
        if (save[0]) {
            join_path(out, outn, save, cmd);
            int fd = open(out, O_RDONLY);
            if (fd >= 0) {
                close(fd);
                return;
            }
        }
        if (!saved)
            break;
        save = colon + 1;
    }
    /* Nothing on PATH has it: the last resort is /bin/cmd, which is
     * where the kernel's embedded table looks anyway. */
    join_path(out, outn, "/bin", cmd);
}

/* The shells a process substitution forked are the *shell's* children, so
 * they are kept here and collected when the shell next waits for one of
 * them. */
#define PSUB_MAX 32
static pid_t procsub_pids[PSUB_MAX];
static int procsub_n;

static void procsub_reap(pid_t pid)
{
    if (pid < 0)
        return;
    if (procsub_n < PSUB_MAX)
        procsub_pids[procsub_n++] = pid;
    else
        waitpid(pid, 0, 0);
}

/* Only a child that has actually finished may be forgotten. waitpid()
 * with WNOHANG answers 0 for one that is still running, and dropping it
 * anyway loses a live process — which, unreaped, is a slot out of
 * MAX_PROCS (8) held for the rest of the session.
 *
 * The compaction also has to re-examine the slot it just filled: the old
 * loop moved the last entry down to index i and then let its ++i step
 * straight over it, so that entry was skipped and could be waited for
 * twice. Keeping the index still is what makes both halves right. */
void sh_reap_procsubs(void)
{
    int i = 0;
    while (i < procsub_n) {
        int st = 0;
        if (waitpid(procsub_pids[i], &st, WNOHANG) > 0)
            procsub_pids[i] = procsub_pids[--procsub_n];
        else
            ++i;
    }
}

/* --- redirections --- */

#define SAVE_MAX 8

/* A process substitution is a pipe with a shell of its own at the far
 * end: the list writes to it for `<( )' and reads from it for `>( )'.
 *
 * The shell has to be the one that forks it. Doing this inside
 * apply_redirs() put it in the wrong process: apply_redirs() is called by
 * exec_child(), which is about to execve() a command, so the shell that
 * wrote the pid into procsub_pids[] was a process that no longer existed
 * a moment later, and the prosub's actual parent was the command about to
 * become /bin/cat. Nothing could wait for it — not the shell, whose table
 * it was in, and not the command, which never learned the pid. The pid
 * was recorded into an array that was about to be thrown away.
 *
 * So it happens here, in the shell, before the command is forked: the
 * prosub is a child of the shell, procsub_pids[] is a list of children the
 * shell really has, and the command inherits the pipe by the fork that
 * follows. The descriptor is put back afterwards by undo_redirs(), since
 * for `<( )' it was the shell's own stdin it borrowed.
 *
 * `save_fds'/`save_copies'/`nsaved' are the same saving mechanism the rest
 * of the redirections use; passing null saves nothing, which is what a
 * child wants and no longer happens here. */
static int setup_procsubs(Cmd* c, int* save_fds, int* save_copies, int* nsaved)
{
    for (Redir* r = c->redirs; r; r = r->next) {
        if (r->mode != R_PIPE || !r->procsub)
            continue;

        int to = r->fd >= 0 ? r->fd : 0;
        if (save_fds && *nsaved < SAVE_MAX) {
            save_fds[*nsaved] = to;
            save_copies[*nsaved] = dup(to);
            ++*nsaved;
        }
        int pfd[2];
        if (pipe(pfd) < 0) {
            sh_err("vash: pipe failed\n");
            return -1;
        }
        /* `<( )' leaves the reading end to the command and writes into
         * the pipe itself; `>( )' is the other way round. */
        int give = r->procsub_out ? pfd[0] : pfd[1];
        int keep = r->procsub_out ? pfd[1] : pfd[0];
        pid_t pid = fork();
        if (pid == 0) {
            close(keep);
            dup2(give, r->procsub_out ? 0 : 1);
            close(give);
            exit(run_list(r->procsub));
        }
        close(give);
        procsub_reap(pid);
        /* Before the fork, so the command child inherits the pipe end on
         * `to' and nothing else: `keep' is closed here, not in the
         * child, or the command would hold the far end open itself and
         * the prosub would never see end-of-file. */
        dup2(keep, to);
        close(keep);
    }
    return 0;
}

/* `save_fds' collects the descriptors to restore afterwards and
 * `save_copies' the duplicates that keep their old contents; with both
 * null nothing is saved, which is what a child about to execve wants. */
static int apply_redirs(Cmd* c, int* save_fds, int* save_copies, int* nsaved)
{
    for (Redir* r = c->redirs; r; r = r->next) {
        Str name;
        str_init(&name);

        if (r->mode == R_PIPE && r->procsub) {
            /* Already done by setup_procsubs(), in the shell, before this
             * process existed. The command has the pipe on the descriptor
             * it wanted by the time it gets here, and `keep' was closed
             * before the fork, so there is nothing left to open and
             * nothing to close. */
            continue;
        }

        if (r->mode == R_HEREDOC) {
            if (save_fds && *nsaved < SAVE_MAX) {
                save_fds[*nsaved] = (r->fd >= 0 ? r->fd : 0);
                save_copies[*nsaved] = dup(save_fds[*nsaved]);
                ++*nsaved;
            }
            int pfd[2];
            if (pipe(pfd) < 0) { sh_err("vash: pipe failed\n"); str_free(&name); return -1; }
            if (r->heredoc) {
                write(pfd[1], r->heredoc, strlen(r->heredoc));
            } else if (r->target) {
                /* A here-string: the word, expanded now, and a newline. */
                Str body;
                str_init(&body);
                if (r->text)
                    str_adds(&body, r->text);
                else
                    expand_word_one(r->target, &body);
                str_addc(&body, '\n');
                write(pfd[1], str_cstr(&body), body.len);
                str_free(&body);
            }
            if (r->text) {
                free(r->text);
                r->text = 0;
            }
            close(pfd[1]); dup2(pfd[0], r->fd >= 0 ? r->fd : 0); close(pfd[0]);
            continue;
        }

        /* `>&2' and `<&1' just move a descriptor. */
        if (r->mode == R_DUP_OUT || r->mode == R_DUP_IN) {
            Str num;
            str_init(&num);
            expand_word_one(r->target, &num);
            int from = atoi(str_cstr(&num));
            str_free(&num);
            if (from < 0)
                from = 0;
            int to = (r->mode == R_DUP_IN) ? 0 : 1;
            if (save_fds && *nsaved < SAVE_MAX) {
                save_fds[*nsaved] = to;
                save_copies[*nsaved] = dup(to);
                ++*nsaved;
            }
            if (from == to)
                continue;
            dup2(from, to);
            continue;
        }

        /* The target is expanded now, not when the line was read, so
         * `> $out' sees the value the loop has just set. */
        if (r->text) {
            str_adds(&name, r->text);
            free(r->text);
            r->text = 0;
        } else {
            expand_word_one(r->target, &name);
        }
        const char* path = str_cstr(&name);

        int flags = (r->mode == R_IN) ? O_RDONLY
                   : (r->mode == R_APPEND)
                       ? (O_WRONLY | O_CREAT | O_APPEND)
                       : (O_WRONLY | O_CREAT | O_TRUNC);
        if (r->mode == R_BOTH || r->mode == R_BOTH_APP) {
            flags = (r->mode == R_BOTH_APP)
                        ? (O_WRONLY | O_CREAT | O_APPEND)
                        : (O_WRONLY | O_CREAT | O_TRUNC);
        }
        /* 0666 for a hosted libc, which reads the mode of a new file
         * from the third argument; vlibc takes path and flags only. */
        int fd = open(path, flags, 0666);
        if (fd < 0) {
            sh_err("vash: cannot open ");
            sh_err(path);
            sh_err("\n");
            str_free(&name);
            return -1;
        }
        if (r->mode == R_BOTH || r->mode == R_BOTH_APP) {
            if (save_fds && *nsaved < SAVE_MAX) {
                save_fds[*nsaved] = 1;
                save_copies[(*nsaved)++] = dup(1);
            }
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
            str_free(&name);
            continue;
        }
        int to = (r->fd >= 0) ? r->fd : 1;
        if (save_fds && *nsaved < SAVE_MAX) {
            save_fds[*nsaved] = to;
            save_copies[*nsaved] = dup(to);
            ++*nsaved;
        }
        if (fd != to) {
            dup2(fd, to);
            close(fd);
        }
        str_free(&name);
    }
    return 0;
}

static void undo_redirs(int* save_fds, int* save_copies, int nsaved)
{
    /* Backwards. Several substitutions can borrow the same descriptor —
     * two `<( )' both take fd 0 — so the last copy saved is the one that
     * was in place before any of them and has to be the one restored
     * last. Forwards, the earliest copy would win and the shell would be
     * left reading from the first substitution's pipe for the rest of the
     * session. */
    for (int i = nsaved; i-- > 0;) {
        if (save_copies[i] >= 0) {
            dup2(save_copies[i], save_fds[i]);
            close(save_copies[i]);
        }
    }
}

/* --- expanding a command's words into an argv --- */

static int expand_argv(Cmd* c, int from, char** argv, int max, int one_field)
{
    int argc = 0;
    for (int i = from; i < c->argc && argc < max - 1; ++i) {
        if (one_field) {
            Str s;
            str_init(&s);
            expand_word_one(c->argv[i], &s);
            argv[argc++] = str_cstr(&s);
            continue;
        }
        Vec fields;
        vec_init(&fields);
        expand_word(c->argv[i], &fields);
        for (int f = 0; f < fields.n && argc < max - 1; ++f)
            argv[argc++] = str_cstr((Str*)fields.v[f]);
        vec_free(&fields);
    }
    argv[argc] = 0;
    return argc;
}

/* `NAME=value' as bash reads it: the name has to be plain text at the
 * front of the word, and the value is everything behind the `=' —
 * expansions included, so `x=$y', `i=$((i-1))' and `f=$(date)' are
 * assignments like `x=1' is. The value comes back as a word of its own,
 * which the caller frees with word_free(). */
static int split_assignment(Word* w, char* name, int namen, Word** value)
{
    if (value)
        *value = 0;
    if (!w || w->kind != W_LIT || w->quoted)
        return 0;
    const char* p = str_cstr(&w->text);
    int j = 0;
    if (!(*p == '_' || (*p >= 'a' && *p <= 'z') ||
          (*p >= 'A' && *p <= 'Z')))
        return 0;
    /* `a[2]=x' names an element, so the brackets are part of the name
     * here; assign_value() is the one that splits them apart. */
    while (p[j] && p[j] != '=' &&
           (p[j] == '_' || (p[j] >= 'a' && p[j] <= 'z') ||
            (p[j] >= 'A' && p[j] <= 'Z') || (p[j] >= '0' && p[j] <= '9') ||
            p[j] == '[' || p[j] == ']' ||
            (p[j] == '@' && p[j + 1] == ']') || p[j] == '-'))
        ++j;
    if (!p[j] || p[j] != '=')
        return 0;
    for (int k = 0; k < j && k < namen - 1; ++k)
        name[k] = p[k];
    name[j < namen - 1 ? j : namen - 1] = 0;

    int skip = j + 1;
    Word* tail = 0;
    for (Word* q = w; q; q = q->next) {
        Word* n = word_new(q->kind);
        if (!n)
            break;
        n->quoted = q->quoted;
        const char* t = str_cstr(&q->text);
        int len = q->text.len;
        if (skip >= len) {
            skip -= len; /* none of this part is part of the value */
        } else {
            str_addn(&n->text, t + skip, len - skip);
            skip = 0;
        }
        if (!tail) {
            if (value)
                *value = n;
        } else {
            tail->next = n;
        }
        tail = n;
    }
    return 1;
}

/* `a=1' sets a name and `a[2]=1' sets an element; the subscript is text,
 * so `a[$i]=1' is not something this door can open. */
static int assign_value(const char* name, const char* value)
{
    const char* br = name;
    while (*br && *br != '[')
        ++br;
    if (!*br)
        return var_set(name, value);
    int n = (int)(br - name);
    char base[64];
    if (n > (int)sizeof(base) - 1)
        n = (int)sizeof(base) - 1;
    memcpy(base, name, (unsigned long)n);
    base[n] = 0;
    const char* key = br + 1;
    int klen = 0;
    while (key[klen] && key[klen] != ']')
        ++klen;
    if (klen == 1 && key[0] == '@')
        return -1; /* `a[@]=...' is a whole-array assignment */
    char kbuf[64];
    if (klen > (int)sizeof(kbuf) - 1)
        klen = (int)sizeof(kbuf) - 1;
    memcpy(kbuf, key, (unsigned long)klen);
    kbuf[klen] = 0;
    /* An associative array keeps the key as written; an indexed one has
     * a number for a key, so `[ 02 ]' is element 2 and not element 02. */
    int digits = 0;
    for (int k = 0; kbuf[k]; ++k)
        if (kbuf[k] >= '0' && kbuf[k] <= '9')
            ++digits;
    if (!arr_is_assoc(base) && digits == klen && klen > 0) {
        Str num;
        str_init(&num);
        sh_itoa(atol(kbuf), &num);
        arr_set(base, str_cstr(&num), value);
        str_free(&num);
    } else
        arr_set(base, kbuf, value);
    return 0;
}

/* `arr=(one two three)' and `arr+=(four)' — every element is expanded
 * into a field of its own and put into the array under the next index. */
static void array_name(Word* w, char* out, int max, int* append);

static void run_array_assign(Word* w)
{
    char name[64];
    int append = 0;
    array_name(w, name, sizeof(name), &append);
    if (!append)
        arr_clear(name);
    int index = arr_len(name);
    if (!w->items)
        return;
    for (int i = 0; i < w->items->n; ++i) {
        Word* part = (Word*)w->items->v[i];
        Str val;
        str_init(&val);
        expand_word_one(part, &val);
        Str num;
        str_init(&num);
        sh_itoa(index, &num);
        arr_set(name, str_cstr(&num), str_cstr(&val));
        str_free(&num);
        ++index;
        str_free(&val);
    }
}

/* The array an `arr=(...)' word is about to fill, `arr' with the `=' and
 * a `+=' taken off. */
static void array_name(Word* w, char* out, int max, int* append)
{
    const char* text = str_cstr(&w->text);
    int n = 0;
    while (text[n] && text[n] != '=')
        ++n;
    int plus = n > 0 && text[n - 1] == '+';
    if (append)
        *append = plus;
    int len = n - (plus ? 1 : 0);
    if (len > max - 1)
        len = max - 1;
    memcpy(out, text, (unsigned long)len);
    out[len] = 0;
}

/* The words a command redirects are expanded before the assignments in
 * front of it take effect, the way bash does it: in `IFS=$'\n' read -a x
 * <<< "$y"' the here-string is built with the IFS the shell had, not the
 * one the builtin is about to run with. */
static void cache_redirs(Cmd* c)
{
    for (Redir* r = c->redirs; r; r = r->next) {
        if (!r->target || r->text)
            continue;
        Str t;
        str_init(&t);
        expand_word_one(r->target, &t);
        r->text = sh_strdup(str_cstr(&t));
        str_free(&t);
    }
}

/* An array that a prefix assignment is about to overwrite, kept so the
 * command it belongs to can hand it back untouched. */
struct ArrSave {
    char name[64];
    char** keys;
    char** vals;
    int n;
};

static void arr_save(struct ArrSave* s, const char* name)
{
    memset(s, 0, sizeof(*s));
    int k = 0;
    for (const char* p = name; *p && k < (int)sizeof(s->name) - 1; ++p)
        s->name[k++] = *p;
    s->name[k] = 0;
    s->n = arr_len(name);
    if (!s->n)
        return;
    int nkeys = 0;
    char** keys = arr_keys(name, &nkeys);
    if (!keys)
        return;
    s->keys = (char**)malloc((unsigned long)nkeys * sizeof(char*));
    s->vals = (char**)malloc((unsigned long)nkeys * sizeof(char*));
    for (int i = 0; i < nkeys; ++i) {
        s->keys[i] = sh_strdup(keys[i]);
        s->vals[i] = sh_strdup(arr_get(name, keys[i]));
    }
    free(keys);
}

static void arr_restore(struct ArrSave* s)
{
    if (!s->keys) {
        var_unset(s->name);
        return;
    }
    arr_clear(s->name);
    for (int i = 0; i < s->n; ++i)
        arr_set(s->name, s->keys[i], s->vals[i]);
    for (int i = 0; i < s->n; ++i) {
        free(s->keys[i]);
        free(s->vals[i]);
    }
    free(s->keys);
    free(s->vals);
    s->keys = 0;
}

/* How many `NAME=value' words a command starts with. */
static int leading_assignments(Cmd* c)
{
    int n = 0;
    for (int i = 0; i < c->argc; ++i) {
        if (c->argv[i]->kind == W_ARRAY) {
            ++n;
            continue;
        }
        char name[64];
        Word* value = 0;
        if (!split_assignment(c->argv[i], name, sizeof(name), &value))
            break;
        word_free(value);
        ++n;
    }
    return n;
}

/* --- a builtin, run inside the shell itself --- */

/* `declare', `local', `export' and friends take their arguments as
 * assignments: `local x=$y' must not have $y split into fields. */
static int takes_assignments(const char* name)
{
    return strcmp(name, "local") == 0 || strcmp(name, "declare") == 0 ||
           strcmp(name, "typeset") == 0 || strcmp(name, "export") == 0 ||
           strcmp(name, "readonly") == 0 || strcmp(name, "unset") == 0;
}

static int run_builtin_for(Cmd* c, const char* name)
{
    char* argv[SH_MAX_ARGS];
    int argc = expand_argv(c, 0, argv, SH_MAX_ARGS, takes_assignments(name));

    /* A redirection on a builtin is this shell's own, so the old
     * descriptors have to go back afterwards. The substitution is made
     * here too: a builtin runs in the shell, so `read x <(echo hi)' is
     * one of the cases where the shell is the parent of the prosub. */
    int save_fds[SAVE_MAX];
    int save_copies[SAVE_MAX];
    int nsaved = 0;
    if (setup_procsubs(c, save_fds, save_copies, &nsaved) < 0) {
        undo_redirs(save_fds, save_copies, nsaved);
        return 1;
    }
    if (apply_redirs(c, save_fds, save_copies, &nsaved) < 0)
        return 1;

    /* `local' inside a function makes a variable that vanishes when the
     * function returns; the scope stack lives in builtins.c. */
    int rc = builtin_run(argc, argv);
    undo_redirs(save_fds, save_copies, nsaved);
    return rc;
}

/* `set -- a b c' puts a new list in place of the positional parameters.
 * The shell keeps its own copy: the caller hands over pointers into its
 * own argv, which is gone by the time anyone reads `$1'. */
void sh_set_params(char** argv, int argc)
{
    static char* store[SH_MAX_ARGS];
    int n = argc < SH_MAX_ARGS - 1 ? argc : SH_MAX_ARGS - 1;
    for (int i = 0; i < n; ++i) {
        free(store[i + 1]);
        store[i + 1] = (char*)malloc(strlen(argv[i]) + 1);
        if (store[i + 1])
            memcpy(store[i + 1], argv[i], strlen(argv[i]) + 1);
    }
    for (int i = n + 1; i < SH_MAX_ARGS; ++i) {
        free(store[i]);
        store[i] = 0;
    }
    if (!store[0])
        store[0] = (char*)malloc(SH_PATH_MAX);
    if (store[0])
        strcpy(store[0], sh_argv && sh_argv[0] ? sh_argv[0] : "vash");
    sh_argv = store;
    sh_argc = n;
}

/* What a wait status word says as an exit code: what the program exited
 * with, or 128 plus the signal that killed it, as bash counts it. */
static int wait_status(int raw)
{
    if (WIFEXITED(raw))
        return WEXITSTATUS(raw);
    if (WIFSIGNALED(raw))
        return 128 + WTERMSIG(raw);
    return raw;
}

/* --- a command in a child --- */

static void exec_child(Cmd* c, int pipe_in, int pipe_out, int err_out)
{
    if (pipe_in >= 0) {
        dup2(pipe_in, 0);
        close(pipe_in);
    }
    if (pipe_out >= 0) {
        dup2(pipe_out, 1);
        close(pipe_out);
    }
    if (err_out >= 0) {
        dup2(err_out, 2);
        close(err_out);
    }
    /* A compound stage — a group, an if, a loop — runs as itself, not as
     * a command word: `echo a | { read x; ... }' is two commands. */
    if (c->type != C_SIMPLE)
        exit(run_list(c));
    /* The command's own redirections are applied on top of the pipe, so
     * `cmd 2> file | other' still files cmd's errors. */
    if (apply_redirs(c, 0, 0, 0) < 0)
        exit(1);

    char* argv[SH_MAX_ARGS];
    int argc = expand_argv(c, 0, argv, SH_MAX_ARGS, 0);
    if (argc == 0)
        exit(0);
    if (is_builtin_name(argv[0])) {
        /* A builtin in a pipeline stage still runs in the child, so it
         * sees only its own output. */
        exit(builtin_run(argc, argv));
    }
    char resolved[SH_PATH_MAX];
    sh_resolve(argv[0], resolved, sizeof(resolved));
    char** env = var_env_array();
    execve(resolved, argv, env);
    sh_err("vash: ");
    sh_err(argv[0]);
    sh_err(": not found\n");
    exit(127);
}

static int spawn_wait(int pipe_in, int pipe_out, int err_out, Cmd* c)
{
    /* The shell is about to fork, so this is the moment to collect the
     * substitutions that have finished since the last time — otherwise
     * procsub_pids[] grows with every one of them for the whole session.
     * And the substitutions themselves are forked here, in the shell, so
     * that the shell is their parent and can wait for them at all. */
    sh_reap_procsubs();
    int save_fds[SAVE_MAX];
    int save_copies[SAVE_MAX];
    int nsaved = 0;
    if (setup_procsubs(c, save_fds, save_copies, &nsaved) < 0) {
        undo_redirs(save_fds, save_copies, nsaved);
        return 1;
    }

    int pid = fork();
    if (pid == 0)
        exec_child(c, pipe_in, pipe_out, err_out);
    if (pipe_in >= 0)
        close(pipe_in);
    if (pipe_out >= 0)
        close(pipe_out);
    if (err_out >= 0)
        close(err_out);
    if (pid < 0) {
        undo_redirs(save_fds, save_copies, nsaved);
        return -1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    /* The command is done, so nothing is reading or writing the pipe any
     * more and the shell's own descriptor can go back. */
    undo_redirs(save_fds, save_copies, nsaved);
    return wait_status(status);
}

/* --- one simple command --- */

static void dbg_num3(char* buf, int v)
{
    char tmp[16];
    int n = 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    while (v > 0 && n < 15)
        tmp[n++] = (char)('0' + v % 10), v /= 10;
    for (int i = 0; i < n; ++i)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

static int run_simple(Cmd* c)
{
    int prefix = leading_assignments(c);
    if (c->argc == 0)
        return sh_status;

    if (prefix == 0) {
        /* A shell function or a builtin runs here; anything else is a
         * program to exec. */
        Str name;
        str_init(&name);
        expand_word_one(c->argv[0], &name);

        /* `break', `continue', `return' and `exit' are not programs to
         * look for on the filesystem: they are the shell talking to
         * itself, so they are answered here. */
        const char* cmd = str_cstr(&name);
        if (strcmp(cmd, "break") == 0 || strcmp(cmd, "continue") == 0 ||
            strcmp(cmd, "return") == 0) {
            int n = 0;
            if (c->argc > 1) {
                Str arg;
                str_init(&arg);
                expand_word_one(c->argv[1], &arg);
                n = (int)sh_atol(str_cstr(&arg));
                str_free(&arg);
            }
            sh_flow = strcmp(cmd, "break") == 0 ? FLOW_BREAK
                     : strcmp(cmd, "continue") == 0 ? FLOW_CONTINUE
                     : FLOW_RETURN;
            sh_flow_arg = n;
            str_free(&name);
            sh_status = 0;
            return 0;
        }
        if (strcmp(cmd, "exit") == 0) {
            int n = sh_status;
            if (c->argc > 1) {
                Str arg;
                str_init(&arg);
                expand_word_one(c->argv[1], &arg);
                n = (int)sh_atol(str_cstr(&arg));
                str_free(&arg);
            }
            str_free(&name);
            exit(n);
        }

        Func* f = func_find(str_cstr(&name));
        if (f) {
            int r = call_function(f->name, c);
            str_free(&name);
            return r;
        }
        if (is_builtin_name(str_cstr(&name))) {
            int r = run_builtin_for(c, str_cstr(&name));
            str_free(&name);
            return r;
        }
        str_free(&name);
        return spawn_wait(-1, -1, -1, c);
    }

    /* `x=1 cmd' — the assignment belongs to that one command, so the
     * shell's own value is put back the moment it finishes. On its own,
     * `x=1' is a command that only assigns, and the assignment stays. */
    cache_redirs(c);
    struct ArrSave* saves = 0;
    if (prefix > 0) {
        saves = (struct ArrSave*)malloc((unsigned long)prefix *
                                       sizeof(struct ArrSave));
        memset(saves, 0, (unsigned long)prefix * sizeof(struct ArrSave));
    }
    int n = prefix;
    while (n > 0) {
        if (c->argv[n - 1]->kind == W_ARRAY) {
            Word* w = c->argv[n - 1];
            char base[64];
            array_name(w, base, sizeof(base), 0);
            if (saves)
                arr_save(&saves[n - 1], base);
            run_array_assign(w);
            --n;
            continue;
        }
        char name[64];
        Word* value = 0;
        split_assignment(c->argv[n - 1], name, sizeof(name), &value);
        Str val;
        str_init(&val);
        expand_word_one(value, &val);
        assign_value(name, str_cstr(&val));
        str_free(&val);
        word_free(value);
        --n;
    }
    if (prefix == c->argc) {
        free(saves);
        sh_status = 0;
        return 0;
    }

    Cmd rest;
    memset(&rest, 0, sizeof(rest));
    rest.type = C_SIMPLE;
    rest.argv = c->argv + prefix;
    rest.argc = c->argc - prefix;
    rest.redirs = c->redirs;

    /* A builtin and a shell function belong to this shell, prefix
     * assignments and all: `IFS= read -a x < f' still leaves `x' set,
     * because the command never left the shell. */
    int rc;
    Str cname;
    str_init(&cname);
    expand_word_one(rest.argv[0], &cname);
    const char* ccmd = str_cstr(&cname);
    Func* f = func_find(ccmd);
    if (f) {
        rc = call_function(f->name, &rest);
    } else if (is_builtin_name(ccmd)) {
        rc = run_builtin_for(&rest, ccmd);
    } else {
        rc = spawn_wait(-1, -1, -1, &rest);
    }
    str_free(&cname);

    /* Put back what each name was before, remembering that it was unset
     * at all (bash leaves `x=1 env' out of the shell entirely). */
    for (int i = 0; i < prefix; ++i) {
        if (c->argv[i]->kind == W_ARRAY) {
            arr_restore(&saves[i]);
            continue;
        }
        char name[64];
        Word* value = 0;
        split_assignment(c->argv[i], name, sizeof(name), &value);
        word_free(value);
        var_unset(name);
    }
    free(saves);
    return rc < 0 ? 127 : rc;
}

/* --- a pipeline --- */

static int run_pipeline(Cmd* c)
{
    int n = 1 + (c->list ? c->nlist : 0);
    if (n == 1)
        return run_simple(c->left ? c->left : c);

    /* Each stage's substitutions are made here, in the shell, before any
     * stage is forked - for the reason exec_child() cannot do it: the
     * stage that ran them would be gone the moment it execve()d, taking
     * the pid of the substitution with it. */
    sh_reap_procsubs();
    int psave[16][SAVE_MAX];
    int pcopy[16][SAVE_MAX];
    int pnsaved[16];
    int stages = n > 16 ? 16 : n;
    for (int i = 0; i < stages; ++i) {
        Cmd* stage = (i == 0) ? c->left : c->list[i - 1];
        pnsaved[i] = 0;
        if (setup_procsubs(stage, psave[i], pcopy[i], &pnsaved[i]) < 0) {
            for (int j = 0; j <= i; ++j)
                undo_redirs(psave[j], pcopy[j], pnsaved[j]);
            return 127;
        }
    }

    int fds[16][2];
    for (int i = 0; i < stages - 1; ++i) {
        if (pipe(fds[i]) < 0) {
            /* The substitutions are already running and their pipes are
             * already on the shell's descriptors, so the shell has to put
             * its own back even on this path - otherwise `a | b' with a
             * full pipe table leaves the shell reading from whatever the
             * last substitution wrote. */
            for (int j = 0; j < i; ++j) {
                close(fds[j][0]);
                close(fds[j][1]);
            }
            for (int j = 0; j < stages; ++j)
                undo_redirs(psave[j], pcopy[j], pnsaved[j]);
            return 127;
        }
    }

    int pids[16];
    int status = 0;
    for (int i = 0; i < stages; ++i) {
        Cmd* stage = (i == 0) ? c->left : c->list[i - 1];
        int in = (i > 0) ? fds[i - 1][0] : -1;
        int out = (i < stages - 1) ? fds[i][1] : -1;
        int pid = fork();
        if (pid == 0)
            exec_child(stage, in, out, -1);
        pids[i] = pid;
        /* The parent keeps no copy of any pipe end: what a stage needs
         * was dup2()d into place in that stage's child. */
        if (in >= 0)
            close(in);
        if (out >= 0)
            close(out);
    }
    for (int i = 0; i < stages - 1; ++i) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    int worst = 0;
    for (int i = 0; i < stages; ++i) {
        int st = 0;
        waitpid(pids[i], &st, 0);
        st = wait_status(st);
        if (i == stages - 1)
            status = st;
        if (sh_pipefail && st != 0)
            worst = st;
    }
    /* Every stage is finished, so the shell's own descriptors are the
     * shell's again. */
    for (int i = 0; i < stages; ++i)
        undo_redirs(psave[i], pcopy[i], pnsaved[i]);
    return sh_pipefail ? (worst ? worst : status) : status;
}

/* --- `&&', `||', `;' and `&' --- */

/* --- compound commands --- */

/* What a loop body asked the loop that holds it to do. */
enum { BODY_NEXT = 0, BODY_AGAIN, BODY_STOP };

static int loop_body(Cmd* body, int* again)
{
    *again = BODY_NEXT;
    if (!body)
        return sh_status;
    int r = run_list(body);
    if (sh_flow == FLOW_BREAK) {
        sh_flow = FLOW_NONE;
        return -1;
    }
    if (sh_flow == FLOW_CONTINUE) {
        sh_flow = FLOW_NONE;
        *again = BODY_AGAIN;
        return r;
    }
    return sh_flow == FLOW_NONE ? r : -1;
}

/* --- if/elif/else --- */

static int run_if(Cmd* c)
{
    int r = run_list(c->left);
    if (sh_flow != FLOW_NONE)
        return r;
    if (r == 0)
        r = run_list(c->right);
    else if (c->init)
        r = run_list(c->init);
    else
        r = 0; /* nothing to do, and nothing went wrong */
    if (sh_flow == FLOW_NONE)
        sh_status = r;
    return r;
}

/* --- while/until --- */

static int run_loop(Cmd* c)
{
    int r = sh_status;
    for (;;) {
        int cond = run_list(c->left);
        if (sh_flow != FLOW_NONE)
            return r;
        int go = (c->type == C_WHILE) ? (cond == 0) : (cond != 0);
        if (!go)
            break;
        int again = 0;
        int rc = loop_body(c->right, &again);
        if (rc < 0)
            break;
        r = rc;
    }
    sh_status = r;
    return r;
}

/* --- for --- */

/* `for f; do' and `for f in "$@"; do' both walk the positional
 * parameters, one per turn. */
static int word_is_at(Word* w)
{
    for (Word* p = w; p; p = p->next)
        if (p->kind == W_VAR && strcmp(str_cstr(&p->text), "@") == 0)
            return p->quoted;
    return 0;
}

static void word_name(Word* w, char* out, int outn)
{
    out[0] = 0;
    for (Word* p = w; p && outn > 1; p = p->next) {
        int n = p->text.len;
        if (n > outn - 2)
            n = outn - 2;
        for (int i = 0; i < n; ++i)
            out[i] = str_cstr(&p->text)[i];
        outn -= n;
        out += n;
    }
    *out = 0;
}

static int run_for(Cmd* c)
{
    char name[64];
    word_name(c->name, name, sizeof(name));
    int over_args = (c->nwords == 0) ||
                    (c->nwords == 1 && word_is_at(c->words[0]));
    int r = sh_status;

    /* The word list is expanded once, into the fields it splits into:
     * `for s in $list' walks every word the expansion produced. */
    char* fields[SH_MAX_ARGS];
    int nfields = 0;
    if (!over_args) {
        Vec v;
        vec_init(&v);
        for (int n = 0; n < c->nwords && nfields < SH_MAX_ARGS - 1; ++n)
            expand_word(c->words[n], &v);
        for (int n = 0; n < v.n && nfields < SH_MAX_ARGS - 1; ++n)
            fields[nfields++] = str_cstr((Str*)v.v[n]);
        vec_free(&v);
    }

    for (int n = 0;; ++n) {
        if (over_args) {
            if (n >= sh_argc)
                break;
            var_set(name, sh_argv[n + 1]);
        } else {
            if (n >= nfields)
                break;
            var_set(name, fields[n]);
        }
        int again = 0;
        int rc = loop_body(c->right, &again);
        if (rc < 0) {
            r = sh_status;
            break;
        }
        r = rc;
    }
    sh_status = r;
    return r;
}

/* --- for ((init; cond; step)) --- */

static int run_arithfor(Cmd* c)
{
    /* The three clauses were read once, in order, separated by `;'. They
     * are cut apart here; a clause that was left out is the one a C loop
     * would read as doing nothing. */
    Str copy;
    str_init(&copy);
    str_adds(&copy, str_cstr(&c->text));
    char* text = str_cstr(&copy);
    char* part[3] = { text, 0, 0 };
    int np = 1;
    for (char* q = text; *q && np < 3; ++q)
        if (*q == ';') {
            *q = 0;
            part[np++] = q + 1;
        }
    for (int i = 0; i < 3; ++i)
        if (!part[i])
            part[i] = part[0] + strlen(part[0]);

    /* The three clauses are expanded as they are reached, so a `$' form
     * in the step sees what the body left behind. */
    int ok = 1;
    if (*part[0]) {
        char* t0 = sh_expand_arith(part[0]);
        (void)arith_eval(t0, &ok);
        free(t0);
    }
    int r = sh_status;
    for (;;) {
        int cok = 1;
        char* t1 = sh_expand_arith(*part[1] ? part[1] : "1");
        long v = arith_eval(t1, &cok);
        free(t1);
        if (!cok || v == 0)
            break;
        int again = 0;
        int rc = loop_body(c->right, &again);
        if (rc < 0) {
            r = sh_status;
            break;
        }
        r = rc;
        if (*part[2]) {
            char* t2 = sh_expand_arith(part[2]);
            (void)arith_eval(t2, &ok);
            free(t2);
        }
    }
    str_free(&copy);
    sh_status = r;
    return r;
}

/* --- case --- */

static int run_case(Cmd* c)
{
    Str subject;
    str_init(&subject);
    expand_word_one(c->name, &subject);
    int r = sh_status;
    Str pat;
    str_init(&pat);
    for (int i = 0; i < c->nlist; ++i) {
        Cmd* item = c->list[i];
        int hit = 0;
        for (int k = 0; k < item->nwords; ++k) {
            str_reset(&pat);
            expand_word_one(item->words[k], &pat);
            /* A pattern written in quotes is the text itself, as in
             * `case x in "a*")`, where the quotes take the stars away. */
            hit = item->words[k]->quoted
                      ? strcmp(str_cstr(&pat), str_cstr(&subject)) == 0
                      : sh_pattern(str_cstr(&pat), str_cstr(&subject));
            if (hit)
                break;
        }
        if (hit) {
            r = run_list(item->right);
            break;
        }
    }
    str_free(&pat);
    str_free(&subject);
    if (sh_flow == FLOW_NONE)
        sh_status = r;
    return r;
}

/* --- [[ ... ]] --- */

typedef struct {
    Cmd* c;
    int i;
    int literal; /* `test a = b': every word is text, patterns included */
} Test;

static void test_text(Word* w, Str* out)
{
    str_reset(out);
    for (; w; w = w->next) {
        if (w->kind == W_VAR) {
            const char* v = var_get(str_cstr(&w->text));
            if (v)
                str_adds(out, v);
        } else if (w->kind == W_CMDSUB) {
            char* r = run_capture(str_cstr(&w->text));
            str_adds(out, r);
            free(r);
        } else if (w->kind == W_FILE) {
            char* r = read_whole_file(str_cstr(&w->text));
            if (r) {
                str_adds(out, r);
                free(r);
            }
        } else {
            str_addn(out, str_cstr(&w->text), w->text.len);
        }
    }
}

static Word* test_word(Test* t)
{
    if (t->i >= t->c->nwords)
        return 0;
    return t->c->words[t->i++];
}

/* An operator is a plain unquoted word that happens to read like one,
 * so `[[ $x == "&&" ]]' keeps its `&&' as text. */
static int test_op(Test* t, const char* op)
{
    if (t->i >= t->c->nwords)
        return 0;
    Word* w = t->c->words[t->i];
    if (!w || w->kind != W_LIT || w->quoted || w->next)
        return 0;
    return strcmp(str_cstr(&w->text), op) == 0;
}

/* The same, one word further on, so an operator can be recognised
 * without eating the operand before it. */
static int test_op_at(Test* t, int at, const char* op)
{
    if (at >= t->c->nwords)
        return 0;
    Word* w = t->c->words[at];
    if (!w || w->kind != W_LIT || w->quoted || w->next)
        return 0;
    return strcmp(str_cstr(&w->text), op) == 0;
}

static int test_or(Test* t);

static int test_cmp(Test* t)
{
    Str left, right;
    str_init(&left);
    str_init(&right);
    int r = 0;

    /* -z, -n and the file tests take one operand. */
    if (test_op(t, "-z")) {
        ++t->i; /* step over the operator itself */
        Word* w = test_word(t);
        test_text(w, &left);
        r = left.len == 0;
        goto done;
    }
    if (test_op(t, "-n")) {
        ++t->i; /* step over the operator itself */
        Word* w = test_word(t);
        test_text(w, &left);
        r = left.len != 0;
        goto done;
    }
    if (test_op(t, "-v")) {
        ++t->i; /* step over the operator itself */
        Word* w = test_word(t);
        Str name;
        str_init(&name);
        test_text(w, &name);
        r = var_find(str_cstr(&name)) != 0;
        str_free(&name);
        goto done;
    }
    static const struct {
        const char* op;
        int kind;
    } filetests[] = {{"-e", 0}, {"-f", 1}, {"-d", 2}, {"-s", 3},
                     {"-r", 4}, {"-w", 5}, {"-x", 6}, {"-h", 7}, {"-L", 7},
                     {"-p", 8}, {"-c", 9}, {"-b", 10}};
    if (test_op(t, "-t")) {
        ++t->i; /* step over the operator itself */
        Word* w = test_word(t);
        test_text(w, &left);
        r = isatty((int)sh_atol(str_cstr(&left)));
        goto done;
    }
    for (unsigned i = 0; i < sizeof(filetests) / sizeof(filetests[0]); ++i) {
        if (!test_op(t, filetests[i].op))
            continue;
        ++t->i; /* step over the operator itself */
        Word* w = test_word(t);
        test_text(w, &left);
        struct stat st;
        if (stat(str_cstr(&left), &st) != 0) {
            r = 0;
            goto done;
        }
        switch (filetests[i].kind) {
        case 0: r = 1; break;
        case 1: r = S_ISREG(st.st_mode); break;
        case 2: r = S_ISDIR(st.st_mode); break;
        case 3: r = st.st_size > 0; break;
        case 4: r = (st.st_mode & 0444) != 0; break;
        case 5: r = (st.st_mode & 0222) != 0; break;
        case 6: r = (st.st_mode & 0111) != 0; break;
        case 7: r = S_ISLNK(st.st_mode); break;
        case 8: r = S_ISFIFO(st.st_mode); break;
        case 9: r = S_ISCHR(st.st_mode); break;
        case 10: r = S_ISBLK(st.st_mode); break;
        default: r = 0; break;
        }
        goto done;
    }

    /* Otherwise it is a pair of operands and a comparison. */
    Word* lw = test_word(t);
    test_text(lw, &left);
    if (!lw) {
        r = 0;
        goto done;
    }
    int at = t->i;
    static const struct {
        const char* op;
        int kind;
    } cmps[] = {{"==", 0}, {"=", 0}, {"!=", 1}, {"=~", 2}, {"<", 3},
                {">", 4}, {"-eq", 5}, {"-ne", 6}, {"-lt", 7}, {"-le", 8},
                {"-gt", 9}, {"-ge", 10}};
    for (unsigned i = 0; i < sizeof(cmps) / sizeof(cmps[0]); ++i) {
        if (!test_op_at(t, at, cmps[i].op))
            continue;
        t->i = at + 1;
        Word* rw = test_word(t);
        test_text(rw, &right);
        /* `[[ a = b* ]]' asks for a pattern; `[ a = b* ]' is asking
         * whether two words are the same word. */
        int quoted = (rw && rw->quoted) || t->literal;
        switch (cmps[i].kind) {
        case 0:
            r = quoted ? strcmp(str_cstr(&left), str_cstr(&right)) == 0
                        : sh_pattern(str_cstr(&right), str_cstr(&left));
            break;
        case 1:
            r = quoted ? strcmp(str_cstr(&left), str_cstr(&right)) != 0
                        : !sh_pattern(str_cstr(&right), str_cstr(&left));
            break;
        case 2:
            r = re_match(str_cstr(&right), str_cstr(&left)) == 1;
            break;
        case 3:
            r = strcmp(str_cstr(&left), str_cstr(&right)) < 0;
            break;
        case 4:
            r = strcmp(str_cstr(&left), str_cstr(&right)) > 0;
            break;
        default: {
            long a = sh_atol(str_cstr(&left));
            long b = sh_atol(str_cstr(&right));
            switch (cmps[i].kind) {
            case 5: r = a == b; break;
            case 6: r = a != b; break;
            case 7: r = a < b; break;
            case 8: r = a <= b; break;
            case 9: r = a > b; break;
            case 10: r = a >= b; break;
            default: r = 0; break;
            }
            break;
        }
        }
        goto done;
    }

    /* A lone word is true when it is not empty. */
    r = left.len != 0;

done:
    str_free(&left);
    str_free(&right);
    return r;
}

static int test_primary(Test* t)
{
    if (test_op(t, "!")) {
        ++t->i;
        return !test_primary(t);
    }
    if (test_op(t, "(")) {
        ++t->i;
        int r = test_or(t);
        if (test_op(t, ")"))
            ++t->i;
        return r;
    }
    return test_cmp(t);
}

static int test_and(Test* t)
{
    int r = test_primary(t);
    while (test_op(t, "&&")) {
        ++t->i;
        int right = test_primary(t);
        r = r && right;
    }
    return r;
}

static int test_or(Test* t)
{
    int r = test_and(t);
    while (test_op(t, "||")) {
        ++t->i;
        int right = test_and(t);
        r = r || right;
    }
    return r;
}

static int run_test(Cmd* c, int literal);

/* `test a = b' and `[ a = b ]' are the same question `[[ ]]' asks, asked
 * about arguments instead of a parsed command line, so they go through
 * the same code: the words are built the way the shell would build them
 * out of quotes, which means no pattern in `[ x = a* ]'. */
int sh_test_args(int argc, char** argv, int start)
{
    Cmd c;
    memset(&c, 0, sizeof(c));
    int n = 0;
    c.words = (Word**)malloc((unsigned long)(argc - start + 1) * sizeof(Word*));
    for (int i = start; i < argc; ++i) {
        c.words[n++] = word_literal(argv[i]);
    }
    c.nwords = n;
    int r = run_test(&c, 1);
    for (int i = 0; i < n; ++i)
        word_free(c.words[i]);
    free(c.words);
    return r;
}

static int run_test(Cmd* c, int literal)
{
    Test t;
    t.c = c;
    t.i = 0;
    t.literal = literal;
    int r = test_or(&t) ? 0 : 1;
    if (sh_flow == FLOW_NONE)
        sh_status = r;
    return r;
}

static int run_list(Cmd* c)
{
    if (!c)
        return sh_status;
    switch (c->type) {
    case C_SIMPLE: {
        /* `$?' is whatever the last command of its own was, and a
         * builtin or a function is as much a command as a program. */
        int r = run_simple(c);
        if (sh_flow == FLOW_NONE)
            sh_status = r;
        return r;
    }
    case C_PIPE:
        return run_pipeline(c);
    case C_SUB:
        return run_subshell(c);
    case C_IF:
        return run_if(c);
    case C_WHILE:
    case C_UNTIL:
        return run_loop(c);
    case C_FOR:
        return run_for(c);
    case C_ARITHFOR:
        return run_arithfor(c);
    case C_CASE:
        return run_case(c);
    case C_TEST:
        return run_test(c, 0);
    case C_ARITHCMD: {
        /* `(( expr ))' fails when the expression comes out zero, which is
         * what makes it usable as a condition. */
        int ok = 1;
        char* text = sh_expand_arith(str_cstr(&c->text));
        long v = arith_eval(text, &ok);
        free(text);
        int r = (ok && v != 0) ? 0 : 1;
        sh_status = r;
        return r;
    }
    case C_SEQ: {
        /* The node holds its commands in the order they were read, so
         * running a sequence is a walk of that list: `a; b; c' is three
         * commands side by side, not three nodes deep. */
        int r = sh_status;
        for (int i = 0; i < c->nlist; ++i) {
            r = run_list(c->list[i]);
            if (sh_flow != FLOW_NONE)
                return r;
        }
        return r;
    }
    case C_AND: {
        int r = run_list(c->left);
        if (sh_flow != FLOW_NONE)
            return r;
        if (r == 0)
            return run_list(c->right);
        return r;
    }
    case C_OR: {
        int r = run_list(c->left);
        if (sh_flow != FLOW_NONE)
            return r;
        if (r != 0)
            return run_list(c->right);
        return r;
    }
    case C_BG: {
        int pid = fork();
        if (pid == 0) {
            int r = run_list(c->left);
            exit(r);
        }
        if (pid < 0)
            return run_list(c->left);
        return 0;
    }
    case C_GROUP:
        return run_list(c->left);
    default:
        return sh_status;
    }
}

int run_command_list(Cmd* c)
{
    int r = run_list(c);
    if (r >= 0)
        sh_status = r;
    return r;
}

int sh_exec(Cmd* c)
{
    return run_list(c);
}

/* --- running text --- */

int run_string(const char* text, int capture)
{
    (void)capture;
    Lex lx;
    lex_init_str(&lx, text, (int)strlen(text));
    Cmd* c = parse_list(&lx, 0);
    int r = run_command_list(c);
    cmd_free(c);
    lex_free(&lx);
    return r;
}

int run_subshell(Cmd* c)
{
    /* As in spawn_wait(): the shell forks the substitution itself, so that
     * it is the shell's child, and collects the ones already finished. */
    sh_reap_procsubs();
    int save_fds[SAVE_MAX];
    int save_copies[SAVE_MAX];
    int nsaved = 0;
    if (setup_procsubs(c, save_fds, save_copies, &nsaved) < 0) {
        undo_redirs(save_fds, save_copies, nsaved);
        return 1;
    }
    int pid = fork();
    if (pid == 0) {
    if (apply_redirs(c, 0, 0, 0) < 0)
        exit(1);
    int r = 0;
    if (c->left)
        r = run_command_list(c->left);
    else
        r = run_string(c->text.p ? str_cstr(&c->text) : "", 1);
    exit(r);
    }
    if (pid < 0)
        return c->left ? run_command_list(c->left)
                       : run_string(c->text.p ? str_cstr(&c->text) : "", 1);
    int status = 0;
    waitpid(pid, &status, 0);
    undo_redirs(save_fds, save_copies, nsaved);
    return wait_status(status);
}

/* `$(...)' — run the text and hand back what it printed, without the
 * trailing newlines bash would strip. */
/* The whole of a file as a string, for `$(< file)'. */
char* read_whole_file(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    Str all;
    str_init(&all);
    char buf[512];
    for (;;) {
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        str_addn(&all, buf, n);
    }
    close(fd);
    /* A command substitution drops the newlines that end it, and so does
     * `$(< file)': it is the same shortcut. */
    while (all.len > 0 && all.p[all.len - 1] == '\n')
        --all.len;
    return sh_strdup(str_cstr(&all));
}

char* run_capture(const char* text)
{
    int fds[2];
    if (pipe(fds) < 0)
        return sh_strdup("");
    int pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return sh_strdup("");
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        int r = run_string(text, 1);
        exit(r);
    }
    close(fds[1]);
    Str out;
    str_init(&out);
    char buf[512];
    for (;;) {
        int n = (int)read(fds[0], buf, sizeof(buf));
        if (n <= 0)
            break;
        str_addn(&out, buf, n);
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    while (out.len > 0 && out.p[out.len - 1] == '\n')
        out.p[--out.len] = 0;
    char* result = sh_strdup(out.p ? out.p : "");
    str_free(&out);
    return result;
}
int call_function(const char* name, Cmd* call)
{
    (void)call;
    Func* f = func_find(name);
    if (!f || !f->body)
        return 127;
    sh_locals_begin();
    int r = run_command_list(f->body);
    /* What `local' shadowed comes back when the function returns. */
    sh_locals_end();
    /* `return n' comes back as the value of the call, and the loop the
     * function ran in — if any — is none of its business. */
    if (sh_flow == FLOW_RETURN) {
        sh_flow = FLOW_NONE;
        r = sh_flow_arg;
        sh_flow_arg = 0;
        sh_status = r;
    }
    return r;
}
