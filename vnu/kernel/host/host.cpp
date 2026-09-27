#include <vnu/host.h>
#include <vnu/vfs.h>
#include <vnu/install.h>
#include <vnu/tty.h>

namespace {

constexpr const char* HOSTNAME_F = "/etc/hostname";

bool is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

bool is_mid(char c)
{
    return is_alnum(c) || c == '-' || c == '_' || c == '.';
}

void trim(char* s)
{
    /* The file is a single line, but an editor or `echo x > file` can
     * leave a stray CR or a space: cut at the first character that
     * cannot be part of a name, and skip leading blanks. */
    char* p = s;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (p != s) {
        int i = 0;
        while (p[i])
            ++s[i] = p[i];
    }
    int n = 0;
    while (s[n] && is_mid(s[n]))
        ++n;
    s[n] = 0;
}

} // namespace

namespace vnu::host {

/* The file is the truth and it is tiny, so name() re-reads it on every
 * call: a program (or root at the shell) can rewrite /etc/hostname at
 * any time and the next uname(2) has to follow. The static buffer is
 * only there so the returned pointer stays valid for the caller. */
char g_name[MAX];

const char* name()
{
    char buf[MAX];
    int got = vnu::vfs::read_path(HOSTNAME_F, buf, sizeof(buf) - 1);
    buf[got > 0 ? got : 0] = 0;
    trim(buf);
    int i = 0;
    for (; buf[i] && i < MAX - 1; ++i)
        g_name[i] = buf[i];
    g_name[i] = 0;
    /* Whatever root wrote into the file is the name, exactly as on a
     * normal Unix; only an empty or missing file falls back to the
     * default. valid() guards the name this system records on a disk,
     * not the one it displays. */
    if (g_name[0] == 0) {
        int j = 0;
        for (; DEFAULT_NAME[j] && j < MAX - 1; ++j)
            g_name[j] = DEFAULT_NAME[j];
        g_name[j] = 0;
    }
    return g_name;
}

bool valid(const char* s)
{
    if (!s)
        return false;
    int n = 0;
    for (; s[n]; ++n) {
        if (n >= MAX - 1)          /* longer than any node name can be */
            return false;
        if (!is_mid(s[n]))
            return false;
        if (n == 0 && !is_alnum(s[n]))
            return false;          /* must start with a letter or digit */
    }
    if (n == 0)
        return false;
    return is_alnum(s[n - 1]);     /* and end with one */
}

bool set(const char* s)
{
    if (!valid(s))
        return false;
    char line[MAX];
    int i = 0;
    for (; s[i] && i < MAX - 2; ++i)
        line[i] = s[i];
    line[i] = '\n';
    line[i + 1] = 0;
    if (vnu::vfs::set_path(HOSTNAME_F, line,
                           static_cast<uint32_t>(i + 1)) != 0)
        return false;
    return true;
}

bool load()
{
    char buf[vnu::host::MAX];
    if (vnu::install::installed_hostname(buf, sizeof(buf)) != 0)
        return false;
    if (!vnu::host::valid(buf))
        return false;
    if (!vnu::host::set(buf)) {
        vnu::tty::write_cstr("hostname: cannot write /etc/hostname\n");
        return false;
    }
    return true;
}

} // namespace vnu::host
