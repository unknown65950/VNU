#include <vnu/apps.h>
#include <vnu/vfs.h>
#include <vnu/posix.h>
#include <vnu/process.h>
#include <vnu/vga_gfx.h>

#include "../proc/embedded_hello.h"
#include "../proc/embedded_vedit.h"
#include "../proc/embedded_vash.h"
#include "../proc/embedded_calc.h"
#include "../proc/embedded_files.h"
#include "../proc/embedded_prefs.h"
#include "../proc/embedded_picview.h"
#include "../proc/embedded_clock.h"
#include "../proc/embedded_pic_flower.h"
#include "../proc/embedded_pic_sunset.h"
#include "../proc/embedded_pic_logo.h"
#include "../proc/embedded_pic_gray_alpha.h"
#include "../proc/embedded_pic_shapes_pal.h"
#include "../proc/embedded_pic_shapes_rgba.h"

namespace {

using vnu::posix::O_CREAT;
using vnu::posix::O_RDONLY;
using vnu::posix::O_TRUNC;
using vnu::posix::O_WRONLY;

int str_len(const char* s)
{
    int n = 0;
    while (s && s[n])
        ++n;
    return n;
}

void str_copy(char* d, const char* s, int cap)
{
    int i = 0;
    for (; s && s[i] && i < cap - 1; ++i)
        d[i] = s[i];
    d[i] = 0;
}

void join(char* out, int cap, const char* a, const char* b)
{
    str_copy(out, a, cap);
    int n = str_len(out);
    if (n < cap - 1) {
        out[n++] = '/';
        out[n] = 0;
    }
    str_copy(out + n, b, cap - n);
}

void install_one(const char* name, uint8_t color, uint8_t glyph,
                 const uint8_t* data, uint32_t size)
{
    char dir[64];
    join(dir, sizeof(dir), "/apps", name);
    vnu::vfs::mkdir(dir);

    char icon_path[80];
    join(icon_path, sizeof(icon_path), dir, "icon");
    int fd = vnu::vfs::open(icon_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        const uint8_t icon[2] = { color, glyph };
        vnu::vfs::write(fd, icon, 2);
        vnu::vfs::close(fd);
    }

    char bin_path[80];
    join(bin_path, sizeof(bin_path), dir, "bin");
    fd = vnu::vfs::open(bin_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        vnu::vfs::write(fd, data, size);
        vnu::vfs::close(fd);
    }
}

/* Big enough for the largest embedded binary we ship (picview, ~37 KiB
 * of ELF); matches vnu::vfs's own per-file DATA_CAP. */
constexpr uint32_t LOAD_BUF_SIZE = 65536;
uint8_t g_load_buf[LOAD_BUF_SIZE];

} // namespace

namespace vnu::apps {

void install_demo_apps()
{
    vnu::vfs::mkdir("/apps");
    install_one("hello", vnu::vgfx::COLOR_LGREEN, IconGlyph::ICON_SMILE, embedded_hello_elf, embedded_hello_elf_size);
    install_one("vedit", vnu::vgfx::COLOR_YELLOW, IconGlyph::ICON_DOC, embedded_vedit_elf, embedded_vedit_elf_size);
    install_one("term", vnu::vgfx::COLOR_LCYAN, IconGlyph::ICON_TERM, embedded_vash_elf, embedded_vash_elf_size);
    install_one("calc", vnu::vgfx::COLOR_LRED, IconGlyph::ICON_KEYPAD, embedded_calc_elf, embedded_calc_elf_size);
    install_one("files", vnu::vgfx::COLOR_YELLOW, IconGlyph::ICON_FOLDER, embedded_files_elf, embedded_files_elf_size);
    install_one("picview", vnu::vgfx::COLOR_LMAGENTA, IconGlyph::ICON_PICTURE, embedded_picview_elf, embedded_picview_elf_size);
    install_one("prefs", vnu::vgfx::COLOR_LBLUE, IconGlyph::ICON_SLIDERS, embedded_prefs_elf, embedded_prefs_elf_size);
    install_one("clock", vnu::vgfx::COLOR_LGREEN, IconGlyph::ICON_CLOCK, embedded_clock_elf, embedded_clock_elf_size);
}

/* Mount the demo photograph pack under /pics so picview has something
 * to show: one VFS node per picture, filled with the embedded bytes. */
void install_demo_pics()
{
    struct Pic {
        const char* name;
        const uint8_t* data;
        uint32_t size;
    };
    static const Pic pics[] = {
        {"flower.bmp", embedded_pic_flower_elf, embedded_pic_flower_elf_size},
        {"sunset.png", embedded_pic_sunset_elf, embedded_pic_sunset_elf_size},
        {"logo.jpg", embedded_pic_logo_elf, embedded_pic_logo_elf_size},
        {"gray_alpha.png", embedded_pic_gray_alpha_elf, embedded_pic_gray_alpha_elf_size},
        {"shapes_pal.png", embedded_pic_shapes_pal_elf, embedded_pic_shapes_pal_elf_size},
        {"shapes_rgba.png", embedded_pic_shapes_rgba_elf, embedded_pic_shapes_rgba_elf_size},
    };

    vnu::vfs::mkdir("/pics");
    for (const Pic& p : pics) {
        char path[64];
        join(path, sizeof(path), "/pics", p.name);
        int fd = vnu::vfs::open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
            continue;
        vnu::vfs::write(fd, p.data, p.size);
        vnu::vfs::close(fd);
    }
}

int list(AppEntry* out, int max)
{
    int fd = vnu::vfs::open("/apps", O_RDONLY);
    if (fd < 0)
        return 0;

    static uint8_t buf[2048];
    int n = vnu::vfs::getdents(fd, buf, sizeof(buf));
    vnu::vfs::close(fd);
    if (n <= 0)
        return 0;

    int count = 0;
    uint32_t off = 0;
    while (off < static_cast<uint32_t>(n) && count < max) {
        auto* de = reinterpret_cast<vnu::vfs::Dirent*>(buf + off);
        if (de->reclen == 0)
            break;
        bool is_dot = de->name[0] == '.' &&
                      (de->name[1] == '\0' || (de->name[1] == '.' && de->name[2] == '\0'));
        if (de->type == 4 /* DT_DIR */ && !is_dot) {
            str_copy(out[count].name, de->name, NAME_CAP);

            char icon_path[80];
            char dir[64];
            join(dir, sizeof(dir), "/apps", de->name);
            join(icon_path, sizeof(icon_path), dir, "icon");
            uint8_t color = vnu::vgfx::COLOR_LGRAY;
            uint8_t glyph = IconGlyph::ICON_LETTER;
            int ifd = vnu::vfs::open(icon_path, O_RDONLY);
            if (ifd >= 0) {
                uint8_t b[2];
                int got = vnu::vfs::read(ifd, b, 2);
                if (got >= 1)
                    color = b[0];
                if (got >= 2)
                    glyph = b[1];
                vnu::vfs::close(ifd);
            }
            out[count].icon_color = color;
            out[count].icon_glyph = glyph;
            ++count;
        }
        off += de->reclen;
    }
    return count;
}

uint32_t read_bin(const char* name, uint8_t* buf, uint32_t cap)
{
    char bin_path[80];
    char dir[64];
    join(dir, sizeof(dir), "/apps", name);
    join(bin_path, sizeof(bin_path), dir, "bin");

    int fd = vnu::vfs::open(bin_path, O_RDONLY);
    if (fd < 0)
        return 0;

    uint32_t total = 0;
    for (;;) {
        int r = vnu::vfs::read(fd, buf + total, cap - total);
        if (r <= 0)
            break;
        total += static_cast<uint32_t>(r);
        if (total >= cap)
            break;
    }
    vnu::vfs::close(fd);
    return total;
}

int launch(const char* name)
{
    char bin_path[80];
    char dir[64];
    join(dir, sizeof(dir), "/apps", name);
    join(bin_path, sizeof(bin_path), dir, "bin");

    uint32_t total = read_bin(name, g_load_buf, LOAD_BUF_SIZE);
    if (total == 0)
        return -1;

    return vnu::proc::run_program_from_memory(bin_path, g_load_buf, total);
}

} // namespace vnu::apps
