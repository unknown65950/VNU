/*
 * wallpaper.cpp — decode /wallpaper (any px.h format) and render it
 * stretched over the whole desktop, quantized to the 16 Catppuccin DAC
 * colors the GUI actually displays.
 *
 * The VFS caps a single file at 64 KiB (DATA_CAP), and a full 1024x768
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
 * Hiding /wallpaper from picview: it lives at the VFS root, not under
 * /pics, so the viewer's directory listing is unchanged.
 */
#define VNU_IN_KERNEL 1
#include <vnu/wallpaper.h>
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
uint8_t g_wall[vnu::vgfx::WIDTH * vnu::vgfx::HEIGHT];

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

/* Largest source we'll decode: the shipped 512x384, i.e. exactly half
 * the desktop at 2x upscale. Also keeps the pixel-bound memory (decoded
 * RGB buffer + PNG inflate work) inside the bump pool above. */
constexpr uint32_t MAX_PIX = 512u * 384u;

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
    (void)p; /* one-shot decode: the pool is never reclaimed */
}

namespace vnu::wallpaper {

bool load()
{
    int fd = vnu::vfs::open("/wallpaper", vnu::posix::O_RDONLY);
    if (fd < 0)
        return false;
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
    if (n < 4)
        return false;

    int w = 0, h = 0;
    int fmt = px_probe(g_file, n, &w, &h);
    if (fmt == PX_NONE || w <= 0 || h <= 0)
        return false;
    if (static_cast<uint32_t>(w) * static_cast<uint32_t>(h) > MAX_PIX)
        return false;

    uint8_t* rgb = static_cast<uint8_t*>(vnu_kalloc(
        (static_cast<uint32_t>(w) * static_cast<uint32_t>(h)) * 3u));
    if (!rgb)
        return false;
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
        return false;
    }

    /* Stretch to every desktop pixel (nearest neighbour per axis, like
     * picview's zoom-out) and quantize straight to a DAC index. */
    const int W = vnu::vgfx::WIDTH;
    const int H = vnu::vgfx::HEIGHT;
    for (int y = 0; y < H; ++y) {
        int sy = (y * h) / H;
        if (sy >= h)
            sy = h - 1;
        const uint8_t* row = rgb + sy * w * 3;
        for (int x = 0; x < W; ++x) {
            int sx = (x * w) / W;
            if (sx >= w)
                sx = w - 1;
            const uint8_t* p = row + sx * 3;
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
            g_wall[y * W + x] = static_cast<uint8_t>(best);
        }
    }
    vnu_kfree(rgb);

    g_ready = true;
    return true;
}

const uint8_t* frame()
{
    return g_wall;
}

bool ready()
{
    return g_ready;
}

} // namespace vnu::wallpaper