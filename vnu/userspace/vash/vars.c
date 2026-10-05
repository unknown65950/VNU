/*
 * vars.c — vash's variables, environment and function table.
 *
 * One list holds both scalars and arrays: an array is a variable with
 * elements, and an element is addressed by its index or, for
 * `declare -A`, by a key. Keeping them the same shape means `$a[2]` and
 * `$map[key]` are the same lookup with a different index string, and
 * `local`, `unset` and `export` have one code path each.
 */
#include "shell.h"

static Var* vars;

/* `set -a': every variable is exported, now and later. It has to be a
 * flag rather than a one-off pass over the table, because the whole point
 * is the assignments that come *after* it. */
static int allexport;

void var_init(void)
{
    Var* v;
    while ((v = vars)) {
        vars = v->next;
        free(v->name);
        free(v->val);
        free(v->el);
        free(v);
    }
}

Var* var_find(const char* name)
{
    for (Var* v = vars; v; v = v->next)
        if (strcmp(v->name, name) == 0)
            return v;
    return 0;
}

static Var* var_make(const char* name)
{
    Var* v = var_find(name);
    if (v)
        return v;
    v = (Var*)malloc(sizeof(Var));
    if (!v)
        return 0;
    memset(v, 0, sizeof(Var));
    v->name = sh_strdup(name);
    v->exported = allexport;
    /* New variables go on the front: a script reads the same handful of
     * names thousands of times, and the ones it defines later are the
     * ones it looks up most. */
    v->next = vars;
    vars = v;
    return v;
}

const char* var_get(const char* name)
{
    Var* v = var_find(name);
    return (v && v->val) ? v->val : "";
}

int var_setn(const char* name, const char* val, int n)
{
    Var* v = var_make(name);
    if (!v)
        return -1;
    free(v->val);
    v->val = (char*)malloc((unsigned long)n + 1);
    if (!v->val)
        return -1;
    memcpy(v->val, val, (unsigned long)n);
    v->val[n] = 0;
    return 0;
}

int var_set(const char* name, const char* val)
{
    return var_setn(name, val, (int)strlen(val));
}

void var_export(const char* name, int on)
{
    Var* v = var_make(name);
    if (v)
        v->exported = on;
}

int var_unset(const char* name)
{
    Var** pp = &vars;
    while (*pp) {
        Var* v = *pp;
        if (strcmp(v->name, name) == 0) {
            *pp = v->next;
            free(v->name);
            free(v->val);
            for (int i = 0; i < v->nel; ++i) {
                free(v->el[i].key);
                free(v->el[i].val);
            }
            free(v->el);
            free(v);
            return 0;
        }
        pp = &v->next;
    }
    return 1;
}

/* The environment a child process inherits: every exported variable, as
 * a fresh NULL-terminated vector. The caller frees it. */
char** var_env_array(void)
{
    int n = 0;
    for (Var* v = vars; v; v = v->next)
        if (v->exported)
            ++n;
    char** env = (char**)malloc(sizeof(char*) * (unsigned long)(n + 1));
    if (!env)
        return 0;
    int i = 0;
    for (Var* v = vars; v && i < n; v = v->next) {
        if (!v->exported)
            continue;
        Str s;
        str_init(&s);
        str_adds(&s, v->name);
        str_addc(&s, '=');
        str_adds(&s, v->val ? v->val : "");
        env[i++] = str_cstr(&s);
    }
    env[i] = 0;
    return env;
}

/* `set -a' / `set +a'. Marking the table is only half of it: the flag
 * makes every variable created from here on exported too, which is the
 * part anyone who writes `set -a' is relying on.
 *
 * Turning it off only clears the flag. It does not un-export what is
 * already exported - neither the variables `set -a' marked a moment ago
 * nor the ones `export' marked before it - because a variable's exported
 * state is its own, and `set +a' says only "stop doing it from now
 * on". Walking the table on the way out as well is the difference
 * between this and bash:
 *
 *     set -a; FOO=1; set +a; BAR=2; export -p
 *     bash  declare -x FOO="1"        FOO stays, BAR does not appear
 *     both  (nothing)                  un-marking FOO, which is wrong
 */
void var_export_all(int on)
{
    allexport = on;
    if (!on)
        return;
    for (Var* v = vars; v; v = v->next)
        v->exported = 1;
}

/* So `set -o' can report the setting instead of only setting it. */
int var_allexport(void)
{
    return allexport;
}

/* The shell's own names ($?, $#, $1, ...) are answered here rather than
 * kept as variables, so that `$?` is always the last status and `$#`
 * always the current argument count. */
const char* var_lookup_word(const char* name)
{
    static char buf[SH_LINE_MAX];
    if (strcmp(name, "?") == 0) {
        {
            Str t; str_init(&t); sh_itoa(sh_status, &t); strncpy(buf, str_cstr(&t), sizeof(buf)-1); buf[sizeof(buf)-1]=0; str_free(&t); return buf;
        }
    }
    if (strcmp(name, "#") == 0) {
        {
            Str t; str_init(&t); sh_itoa(sh_argc, &t); strncpy(buf, str_cstr(&t), sizeof(buf)-1); buf[sizeof(buf)-1]=0; str_free(&t); return buf;
        }
    }
    if (strcmp(name, "$") == 0) {
        {
            Str t; str_init(&t); sh_itoa((long)getpid(), &t); strncpy(buf, str_cstr(&t), sizeof(buf)-1); buf[sizeof(buf)-1]=0; str_free(&t); return buf;
        }
    }
    if (strcmp(name, "-") == 0) {
        {
            Str t;
            str_init(&t);
            sh_utoa((unsigned long)(sh_set_e ? 1u : 0u), &t);
            strncpy(buf, str_cstr(&t), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            str_free(&t);
            return buf;
        }
        return buf;
    }
    if (name[0] >= '0' && name[0] <= '9') {
        int i = name[0] - '0';
        if (i == 0)
            return sh_argv && sh_argv[0] ? sh_argv[0] : "vash";
        if (i <= sh_argc && sh_argv)
            return sh_argv[i];
        return "";
    }
    return var_get(name);
}

/* --- array elements --- */

static AEnt* arr_find(Var* v, const char* key)
{
    for (int i = 0; i < v->nel; ++i)
        if (strcmp(v->el[i].key, key) == 0)
            return &v->el[i];
    return 0;
}

const char* arr_get(const char* name, const char* key)
{
    Var* v = var_find(name);
    if (!v || !v->is_array)
        return "";
    AEnt* e = arr_find(v, key);
    return e ? e->val : "";
}

int arr_len(const char* name)
{
    Var* v = var_find(name);
    return v ? v->nel : 0;
}

int arr_is_assoc(const char* name)
{
    Var* v = var_find(name);
    return v ? v->is_assoc : 0;
}

/* `arr=(x y)' replaces what the array held; the array itself stays. */
void arr_clear(const char* name)
{
    Var* v = var_find(name);
    if (!v)
        return;
    for (int i = 0; i < v->nel; ++i) {
        free(v->el[i].key);
        free(v->el[i].val);
    }
    v->nel = 0;
}

void arr_make_assoc(const char* name)
{
    Var* v = var_make(name);
    if (v)
        v->is_assoc = 1;
}

static int arr_put(Var* v, const char* key, const char* val)
{
    AEnt* e = arr_find(v, key);
    if (e) {
        free(e->val);
        e->val = sh_strdup(val);
        return 0;
    }
    if (v->nel + 1 > v->cap) {
        int cap = v->cap ? v->cap * 2 : 8;
        AEnt* ne = (AEnt*)malloc(sizeof(AEnt) * (unsigned long)cap);
        if (!ne)
            return -1;
        if (v->el) {
            memcpy(ne, v->el, sizeof(AEnt) * (unsigned long)v->nel);
            free(v->el);
        }
        v->el = ne;
        v->cap = cap;
    }
    v->el[v->nel].key = sh_strdup(key);
    v->el[v->nel].val = sh_strdup(val);
    ++v->nel;
    return 0;
}

int arr_set(const char* name, const char* key, const char* val)
{
    Var* v = var_make(name);
    if (!v)
        return -1;
    v->is_array = 1;
    return arr_put(v, key, val);
}

int arr_append(const char* name, const char* val)
{
    Var* v = var_make(name);
    if (!v)
        return -1;
    v->is_array = 1;
    /* The next free index is one past the highest numeric key already
     * in use, which is what bash does when a list grows with `+='. */
    int max = -1;
    for (int i = 0; i < v->nel; ++i) {
        const char* k = v->el[i].key;
        int n = 0;
        int digits = 0;
        while (k[digits] && k[digits] >= '0' && k[digits] <= '9') {
            n = n * 10 + (k[digits] - '0');
            ++digits;
        }
        if (!k[digits] && n > max)
            max = n;
    }
    Str key;
    str_init(&key);
    sh_itoa(max + 1, &key);
    int r = arr_put(v, str_cstr(&key), val);
    str_free(&key);
    return r;
}

/* The element keys, in order — what `${a[@]}` walks. The caller frees
 * the vector, not the keys (they belong to the variable). */
char** arr_keys(const char* name, int* count)
{
    Var* v = var_find(name);
    *count = 0;
    if (!v || !v->is_array)
        return 0;
    char** keys = (char**)malloc(sizeof(char*) * (unsigned long)(v->nel + 1));
    if (!keys)
        return 0;
    for (int i = 0; i < v->nel; ++i)
        keys[i] = v->el[i].key;
    keys[v->nel] = 0;
    *count = v->nel;
    return keys;
}

/* Every variable name, in the order the list holds them — what
 * `${!prefix*}' walks. The pointers are the table's own, borrowed for
 * the length of the call: freeing one frees the Var's name while the Var
 * still points at it, and every later lookup of that name then reads
 * freed memory. The array is the caller's to free; the names are not. */
char** var_names(int* count)
{
    int n = 0;
    for (Var* v = vars; v; v = v->next)
        ++n;
    char** out = (char**)malloc(sizeof(char*) * (unsigned long)(n + 1));
    if (!out) {
        *count = 0;
        return 0;
    }
    int i = 0;
    for (Var* v = vars; v; v = v->next)
        out[i++] = v->name;
    out[i] = 0;
    *count = n;
    return out;
}

/* --- functions --- */

static Func* funcs;

Func* func_find(const char* name)
{
    for (Func* f = funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0)
            return f;
    return 0;
}

void func_define(const char* name, Cmd* body)
{
    Func* f = func_find(name);
    if (!f) {
        f = (Func*)malloc(sizeof(Func));
        if (!f)
            return;
        memset(f, 0, sizeof(Func));
        f->name = sh_strdup(name);
        f->body = body;
        f->next = funcs;
        funcs = f;
        return;
    }
    if (f->body)
        cmd_free(f->body);
    f->body = body;
}

int func_undef(const char* name)
{
    Func** pp = &funcs;
    while (*pp) {
        Func* f = *pp;
        if (strcmp(f->name, name) == 0) {
            *pp = f->next;
            free(f->name);
            if (f->body)
                cmd_free(f->body);
            free(f);
            return 0;
        }
        pp = &f->next;
    }
    return 1;
}