/*
 * wallpaper.cpp — decode /etc/vnu/wallpaper (any px.h format) and
 * render it stretched over the whole desktop, quantized to the 16
 * Catppuccin DAC colors the GUI actually displays.
 *
 * The VFS no longer caps a single file at 64 KiB (each node's content
 * grows on demand), and a full 1024x768
 * 24-bit BMP would be ~2.4 MiB, so the shipped wallpaper is a small
 * 512x384 PNG (designed *in* the palette, ~4 KiB) that the desktop
 * nearest-neighbour-upscales 2x onto the whole screen. The /wallpaper
 * file itself is swappable: any BMP or PNG px.h understands, up to
 * MAX_PIX pixels, gets decoded and display-fitted the same way. JPEG is
 * deliberately excluded: its IDCT is the only IEEE-float code in px.h,
 * and this kernel is built -mno-80387 with no soft-float library to
 * satisfy the resulting __addsf3-style calls, so the JPEG decoder is
 * never linked in.
 *
 * The kernel has no heap, but px.h's BMP/PNG paths malloc()/free().
 * Its <stdlib.h> facilities are #define'd (via VNU_IN_KERNEL) to the
 * bump allocator below: decode happens exactly once at desktop startup
 * and the memory is never needed back, so free() is a no-op and the
 * 2 MiB pool comfortably holds the worst case (decoded 590 KiB RGB
 * buffer + ~590 KiB inflate work + <64 KiB of compressed IDAT).
 *
 * The wallpaper is a setting rather than a picture to browse, so it
 * sits in /etc/vnu beside the demo pack instead of inside it, and
 * picview's directory listing stays unchanged.
 */
#define VNU_IN_KERNEL 1
#include <vnu/wallpaper.h>
#include <vnu/media.h>
#include <vnu/abi.h>
#include <vnu/px.h>
#include <vnu/vfs.h>
#include <vnu/posix.h>
#include <vnu/vga_gfx.h>
#undef malloc
#undef free

namespace {

/* Compact-bump arena backing px.h's malloc. 16-byte aligned bumps keep
 * every decoder happy (int16 planes, byte IDAT...) regardless of the
 * buffer's base alignment. Any need over the pool aborts the decode. */
constexpr uint32_t PX_POOL_CAP = 2u * 1024u * 1024u;
alignas(16) uint8_t g_px_pool[PX_POOL_CAP];
uint32_t g_px_used = 0;

/* Whole-desktop frame vga_gfx blits when a wallpaper is loaded. */
bool g_ready = false;

/* Largest source we'll decode: the shipped 512x384, i.e. exactly half
 * the desktop at 2x upscale. Also keeps the pixel-bound memory (decoded
 * RGB buffer + PNG inflate work) inside the bump pool above. */
constexpr uint32_t MAX_PIX = 512u * 384u;

/* The image in use, quantized to DAC indices at its own size and kept
 * across calls: a mode change only has to scale it to the new geometry,
 * and re-decoding the file would freeze the desktop for the seconds an
 * inflate of a 512x384 PNG costs on this hardware. */
uint8_t g_small[MAX_PIX];
int g_small_w = 0;
int g_small_h = 0;

/* Bare name of the image in use, for /proc/gfx. */
constexpr int NAME_CAP = 64;
char g_name[NAME_CAP];
/* Sized for the largest supported mode, like the desktop backbuffer. */
uint8_t g_wall[vnu::vgfx::MAX_MODE.width * vnu::vgfx::MAX_MODE.height];

/* Catppuccin Mocha DAC palette (6-bit per channel), mirrors
 * kernel/drivers/vga_gfx.cpp CATT_PAL. Quantization compares RGB>>2
 * against these, exactly like picview's map_color. */
const uint8_t PAL[16][3] = {
    {7, 7, 11}, {34, 45, 62}, {41, 56, 40}, {29, 49, 59},
    {60, 34, 42}, {50, 41, 61}, {62, 44, 33}, {12, 12, 17},
    {6, 6, 9}, {45, 47, 63}, {37, 56, 53}, {34, 55, 58},
    {58, 40, 43}, {61, 48, 57}, {62, 56, 43}, {51, 53, 61},
};

constexpr uint32_t FILE_CAP = 65536; /* VFS per-file buffer size */
uint8_t g_file[FILE_CAP];

/* Read a whole file into g_file. Returns its length, 0 when it cannot
 * be opened, and never more than FILE_CAP bytes. */
uint32_t slurp(const char* path)
{
    int fd = vnu::vfs::open(path, vnu::posix::O_RDONLY);
    if (fd < 0)
        return 0;
    uint32_t n = 0;
    for (;;) {
        int r = vnu::vfs::read(fd, g_file + n, FILE_CAP - n);
        if (r <= 0)
            break;
        n += static_cast<uint32_t>(r);
        if (n >= FILE_CAP)
            break;
    }
    vnu::vfs::close(fd);
    return n;
}

/* Bare name of a path, for /proc/gfx. */
void note_name(const char* path)
{
    const char* base = path;
    for (const char* q = path; *q; ++q)
        if (*q == '/')
            base = q + 1;
    int i = 0;
    while (base[i] && i < NAME_CAP - 1) {
        g_name[i] = base[i];
        ++i;
    }
    g_name[i] = 0;
}

} // namespace

extern "C" void* vnu_kalloc(unsigned long n)
{
    const uint32_t need = static_cast<uint32_t>((n + 15u) & ~15u);
    if (need < static_cast<uint32_t>(n) || g_px_used + need > PX_POOL_CAP)
        return nullptr;
    void* p = g_px_pool + g_px_used;
    g_px_used += need;
    return p;
}

extern "C" void vnu_kfree(void* p)
{
    (void)p; /* the pool is reset once per decode, never reclaimed */
}

namespace vnu::wallpaper {

namespace {

/* Quantize a decoded RGB image to DAC indices, at the image's own size
 * (see stretch() for why not at the desktop's). */
void quantize(const uint8_t* rgb, int w, int h, uint8_t* out)
{
    const uint32_t n = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* p = rgb + i * 3u;
        int r = p[0] >> 2, g = p[1] >> 2, b = p[2] >> 2;
        int best = 0, bd = 1 << 30;
        for (int c = 0; c < 16; ++c) {
            int dr = r - PAL[c][0];
            int dg = g - PAL[c][1];
            int db = b - PAL[c][2];
            int dd = dr * dr + dg * dg + db * db;
            if (dd < bd) {
                bd = dd;
                best = c;
            }
        }
        out[i] = static_cast<uint8_t>(best);
    }
}

/* Scale the quantized image over every desktop pixel (nearest neighbour
 * per axis, like picview's zoom-out).
 *
 * The palette search is the expensive half of a frame: 16 candidates per
 * pixel, a million pixels at the top mode, and it only has to run once
 * per image. Scaling a mode change re-quantizes are identical either way
 * - quantize-then-scale picks the same source pixels, and the entry a
 * source pixel quantizes to does not depend on where it lands - so the
 * indices are computed at the image's own size and the desktop frame is
 * a per-pixel copy from then on. */
void stretch()
{
    const int W = vnu::vgfx::width();
    const int H = vnu::vgfx::height();
    const int w = g_small_w, h = g_small_h;
    for (int y = 0; y < H; ++y) {
        int sy = (y * h) / H;
        if (sy >= h)
            sy = h - 1;
        const uint8_t* row = g_small + sy * w;
        for (int x = 0; x < W; ++x) {
            int sx = (x * w) / W;
            if (sx >= w)
                sx = w - 1;
            g_wall[y * W + x] = row[sx];
        }
    }
}

/* Decode whatever is in g_file into g_wall. Returns 0 on success; a
 * failure leaves g_wall exactly as it was, because the quantization
 * only runs once a decoder has returned a clean picture. */
int decode_into_frame(uint32_t n)
{
    if (n < 4)
        return -1;

    /* The bump pool is never given back, so a second decode would run
     * into the first one's leftovers. Nothing in it is live any more
     * (the quantized image is a plain static array) so starting it over
     * is safe. */
    g_px_used = 0;
    g_small_w = 0;
    g_small_h = 0;

    int w = 0, h = 0;
    int fmt = px_probe(g_file, n, &w, &h);
    if (fmt == PX_NONE || w <= 0 || h <= 0)
        return -1;
    if (static_cast<uint32_t>(w) * static_cast<uint32_t>(h) > MAX_PIX)
        return -1;

    uint8_t* rgb = static_cast<uint8_t*>(vnu_kalloc(
        (static_cast<uint32_t>(w) * static_cast<uint32_t>(h)) * 3u));
    if (!rgb)
        return -1;
    /* Dispatch the integer decoders directly instead of px_decode:
     * px_jpg_decode (with its float IDCT) must stay unreferenced so the
     * linker never pulls in soft-float calls this kernel cannot satisfy. */
    int rc;
    if (fmt == PX_BMP)
        rc = px_bmp_decode(g_file, n, &w, &h, rgb);
    else if (fmt == PX_PNG)
        rc = px_png_decode(g_file, n, &w, &h, rgb);
    else
        rc = -1;
    if (rc != 0) {
        vnu_kfree(rgb);
        return -1;
    }

    quantize(rgb, w, h, g_small);
    vnu_kfree(rgb);
    g_small_w = w;
    g_small_h = h;
    stretch();
    return 0;
}

} // namespace

void resize()
{
    /* The frame is scaled to the desktop when it is decoded, so a
     * resolution change leaves it the wrong size: scale the quantized
     * image we still have. Decoding the file again would work too, but
     * an inflate of a 512x384 PNG takes seconds here, and the mode
     * switch would freeze the desktop for all of it. A decode with
     * nothing to scale (no file, a format it does not like) drops back
     * to the procedural scene, which fits any mode. */
    if (!g_ready)
        return; /* the procedural scene has nothing to rescale */
    if (g_small_w <= 0 || g_small_h <= 0)
        g_ready = false;
    else
        stretch();
}

bool load()
{
    uint32_t n = slurp(VNU_WALLPAPER);
    if (decode_into_frame(n) != 0)
        return false;
    g_ready = true;
    note_name(VNU_WALLPAPER);
    return true;
}

int apply(const char* path)
{
    uint32_t n = slurp(path);
    if (n == 0)
        return -VNU_ENOENT;
    if (decode_into_frame(n) != 0)
        return -VNU_EINVAL;

    /* The picture is good, so it becomes the wallpaper file too. The
     * frame is already on screen by now: overwriting a VFS node of this
     * size cannot really fail, and a failure here (-VNU_EIO) is the only
     * case where the screen and the file can disagree. */
    int fd = vnu::vfs::open(VNU_WALLPAPER,
                            vnu::posix::O_WRONLY | vnu::posix::O_CREAT |
                            vnu::posix::O_TRUNC);
    if (fd < 0)
        return -VNU_EIO;
    uint32_t wrote = 0;
    while (wrote < n) {
        int r = vnu::vfs::write(fd, g_file + wrote, n - wrote);
        if (r <= 0)
            break;
        wrote += static_cast<uint32_t>(r);
    }
    vnu::vfs::close(fd);
    if (wrote != n)
        return -VNU_EIO;

    g_ready = true;
    note_name(path);
    return 0;
}

const uint8_t* frame()
{
    return g_wall;
}

bool ready()
{
    return g_ready;
}

const char* current_name()
{
    return g_name;
}

} // namespace vnu::wallpaper
