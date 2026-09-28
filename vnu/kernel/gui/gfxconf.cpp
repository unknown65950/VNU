#include <vnu/gfxconf.h>
#include <vnu/vfs.h>
#include <vnu/vga_gfx.h>
#include <vnu/posix.h>

// The display mode the desktop boots into and switches between, as one
// line of text in /etc/vnuconfig/gfx.conf. See gfxconf.h for the shape
// of the file and why it is the single source of truth.

namespace vnu::gfxconf {

namespace {

/* The file holds one short line. Anything longer is not a setting this
 * parser understands, so the read is capped and a longer file is
 * treated as unreadable rather than half-applied. */
constexpr uint32_t CAP = 64;

int digits(const char*& s)
{
    int v = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9' && n < 5) {
        v = v * 10 + (*s - '0');
        ++s;
        ++n;
    }
    return n ? v : -1;
}

void skip_blanks(const char*& s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
}

/* Write v in decimal at p and return the one past its last digit. The
 * freestanding kernel has no printf, so this is the whole of it. */
char* put_num(char* p, int v)
{
    char rev[12];
    int m = 0;
    do {
        rev[m++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v && m < 12);
    while (m-- > 0)
        *p++ = rev[m];
    return p;
}

/* "mode 1024x768" -> 1024 x 768. Deliberately strict: the keyword and
 * the separator have to be there, and a mode that is not in the
 * driver's ladder is refused, so a hand-edited file falls back to the
 * built-in default instead of programming a mode that does not exist. */
bool parse(const char* text, int& w, int& h)
{
    const char* s = text;
    skip_blanks(s);
    if (s[0] != 'm' || s[1] != 'o' || s[2] != 'd' || s[3] != 'e')
        return false;
    s += 4;
    skip_blanks(s);
    w = digits(s);
    if (*s != 'x')
        return false;
    ++s;
    h = digits(s);
    if (w < 0 || h < 0)
        return false;
    skip_blanks(s);
    if (*s && *s != '\n')
        return false; /* trailing junk: not a setting we wrote */
    return vnu::vgfx::mode_supported(w, h);
}

} // namespace

bool load()
{
    char buf[CAP];
    int n = vnu::vfs::read_path(PATH, buf, CAP - 1);
    if (n <= 0)
        return false;
    buf[n] = 0;
    int w = 0, h = 0;
    if (!parse(buf, w, h))
        return false;
    /* Before the desktop starts this only records the choice: the card
     * is in text mode and vgfx has no registers to program until
     * enter_gfx_mode() runs. */
    return vnu::vgfx::set_resolution(w, h);
}

bool store(int w, int h)
{
    if (!vnu::vgfx::mode_supported(w, h))
        return false;
    char line[CAP];
    char* p = line;
    const char* kw = "mode ";
    while (*kw)
        *p++ = *kw++;
    p = put_num(p, w);
    *p++ = 'x';
    p = put_num(p, h);
    *p++ = '\n';
    *p = 0;

    /* O_CREAT, so a `rm`'d setting file is simply recreated: losing the
     * node is not a reason to lose the setting. */
    int fd = vnu::vfs::open(PATH, vnu::posix::O_WRONLY | vnu::posix::O_CREAT |
                                    vnu::posix::O_TRUNC);
    if (fd < 0)
        return false;
    uint32_t wrote = vnu::vfs::write(fd, line, static_cast<uint32_t>(p - line));
    vnu::vfs::close(fd);
    return static_cast<int>(wrote) == static_cast<int>(p - line);
}

} // namespace vnu::gfxconf
