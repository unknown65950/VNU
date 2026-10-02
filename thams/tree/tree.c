/*
 * tree — list a directory as a tree.
 *
 * The first module built through thams/: it lives in thams/tree/, and
 * nothing in vnu/ was edited to put it on the machine. It is built with
 * the same tools/vcc as the OS, against vlibc alone. See
 * thams/README.md for what a module is and what it may assume.
 *
 * tree duplicates nothing in the system tree on purpose — it is a
 * module, not a coreutils command. What it demonstrates is the path a
 * THAM takes: a directory, a manifest, a manual page, and a command.
 */
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/stdio.h>
#include <vlibc/stdlib.h>
#include <vlibc/fcntl.h>
#include <vlibc/dirent.h>
#include <vlibc/sys/stat.h>
#include <vlibc/errno.h>

#define MAX_DEPTH 24
#define MAX_PATH 256
#define MAX_ENTRIES 192
#define NAME_MAX_LEN 96

struct entry {
    char name[NAME_MAX_LEN];
    int is_dir;
    int broken;   /* stat() could not confirm it: keep it, mark it */
};

static int g_all;        /* -a: count the dot files too */
static int g_dirs_only;  /* -d: directories and nothing else */
static int g_dirs;       /* directories seen, for the last line */
static int g_failed;

/* vlibc has no strerror and no global errno: a call answers with its
 * error number negated. Only the three that a reader can act on. */
static const char* why(int rc)
{
    switch (-rc) {
    case ENOENT:  return "no such file or directory";
    case ENOTDIR: return "not a directory";
    case EACCES:  return "permission denied";
    case EMFILE:  return "too many open files";
    default:      return "cannot read it";
    }
}

static void complain(const char* what, const char* path, int rc)
{
    printf("tree: %s: %s: %s\n", path, what, rc ? why(rc) : "no such directory");
    ++g_failed;
}

/* path + "/" + name, into a fixed buffer. Every path this program
 * prints is under MAX_PATH, and a longer one is cut rather than
 * allowed to run off the end. */
static void join(char* out, const char* dir, const char* name)
{
    int at = 0;
    while (dir[at] && at < MAX_PATH - 2) {
        out[at] = dir[at];
        ++at;
    }
    if (at && out[at - 1] != '/')
        out[at++] = '/';
    for (int i = 0; name[i] && at < MAX_PATH - 1; ++i)
        out[at++] = name[i];
    out[at] = 0;
}

/* The last component, which is what a directory's own line is named
 * after. A trailing slash does not make it empty. */
static const char* base_name(const char* path)
{
    const char* last = path;
    for (const char* s = path; *s; ++s)
        if (*s == '/')
            last = s + 1;
    return last;
}

static int is_dot(const char* name)
{
    if (name[0] != '.')
        return 0;
    return name[1] == 0 || (name[1] == '.' && name[2] == 0);
}

/* Insertion sort by name: a tree that lists a directory in whatever
 * order the filesystem hands it back is one nobody can read twice, and
 * a directory is short enough that this is not worth a quicksort. */
static void sort(struct entry* e, int n)
{
    for (int i = 1; i < n; ++i) {
        struct entry key = e[i];
        int j = i - 1;
        while (j >= 0 && strcmp(e[j].name, key.name) > 0) {
            e[j + 1] = e[j];
            --j;
        }
        e[j + 1] = key;
    }
}

/* One directory, read and sorted. "." and ".." always go; the rest of
 * the dot files go unless -a asked for them. */
static int read_dir(const char* path, struct entry* out, int max)
{
    DIR* dir = opendir(path);
    if (!dir) {
        complain("cannot open directory", path, 0);
        return 0;
    }
    int n = 0;
    struct dirent* de;
    while (n < max && (de = readdir(dir)) != 0) {
        if (is_dot(de->d_name))
            continue;
        if (!g_all && de->d_name[0] == '.')
            continue;

        char full[MAX_PATH];
        join(full, path, de->d_name);

        struct stat st;
        int rc = stat(full, &st);
        int i = 0;
        for (const char* s = de->d_name; *s && i < NAME_MAX_LEN - 1; ++s)
            out[n].name[i++] = *s;
        out[n].name[i] = 0;
        /* readdir knows a directory when it sees one; everything else
         * is a file until proven otherwise, and a name stat() will not
         * confirm is still worth showing. */
        out[n].is_dir = (de->d_type == 4);
        out[n].broken = 0;
        if (rc != 0) {
            out[n].is_dir = 0;
            out[n].broken = 1;
        } else if (S_ISDIR(st.st_mode)) {
            out[n].is_dir = 1;
        }
        ++n;
    }
    closedir(dir);
    sort(out, n);
    return n;
}

static void walk(const char* path, const char* prefix, int depth)
{
    if (depth >= MAX_DEPTH) {
        printf("tree: %s: too deep, stopping here\n", path);
        return;
    }
    struct entry e[MAX_ENTRIES];
    int n = read_dir(path, e, MAX_ENTRIES);
    if (n == 0)
        return;

    /* /a /b /c /d/e — the four shapes a directory can leave its last
     * child in, so the connectors do not have to know in advance
     * whether a sibling is coming. */
    for (int i = 0; i < n; ++i) {
        if (g_dirs_only && !e[i].is_dir)
            continue;

        int last = 1;
        for (int j = i + 1; j < n; ++j) {
            if (!g_dirs_only || e[j].is_dir) {
                last = 0;
                break;
            }
        }

        printf("%s%s%s", prefix, last ? "`-- " : "|-- ", e[i].name);
        if (e[i].is_dir) {
            printf("/");
            ++g_dirs;
        } else if (e[i].broken) {
            printf("  [cannot stat it]");
        }
        printf("\n");

        if (!e[i].is_dir || last)
            continue;

        char child[MAX_PATH];
        join(child, path, e[i].name);
        char next[MAX_PATH];
        int at = 0;
        for (const char* s = prefix; *s && at < MAX_PATH - 16; ++s)
            next[at++] = *s;
        for (const char* s = last ? "    " : "|   "; *s; ++s)
            next[at++] = *s;
        next[at] = 0;
        walk(child, next, depth + 1);
    }
}

static void usage(void)
{
    printf("usage: tree [-a] [-d] [path]...\n");
    printf("  -a   list the dot files too\n");
    printf("  -d   list the directories and nothing else\n");
}

static void one(const char* path)
{
    struct stat st;
    int rc = stat(path, &st);
    if (rc != 0) {
        complain("cannot stat", path, rc);
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        /* A file is a tree of one, which is what `tree notes.txt`
         * should say rather than complain about. */
        printf("%s\n", path);
        return;
    }
    const char* label = base_name(path);
    printf("%s%s\n", label[0] ? label : "/", "/");
    ++g_dirs;
    walk(path, "", 0);
}

int main(int argc, char** argv)
{
    int i = 1;
    for (; i < argc; ++i) {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == 0)
            break;
        int bad = 0;
        for (const char* f = a + 1; *f; ++f) {
            if (*f == 'a')
                g_all = 1;
            else if (*f == 'd')
                g_dirs_only = 1;
            else
                bad = 1;
        }
        if (bad) {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        one(".");
    } else {
        for (; i < argc; ++i) {
            if (i > 1)
                printf("\n");
            one(argv[i]);
        }
    }

    /* Counted in folders, not entries: what a tree shows is its shape,
     * and a shape is measured in directories. */
    printf("\n%d director%s\n", g_dirs, g_dirs == 1 ? "y" : "ies");
    return g_failed ? 1 : 0;
}