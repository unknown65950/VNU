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
 * All three of this module's buffers - the desktop frame, the quantized
 * source and px.h's decode scratch - are taken from the PMM pool for
 * as long as they are in use, and sized for the mode on screen rather
 * than for the top of the ladder. What they used to be (.bss arrays at
 * MAX_MODE, plus a 2 MiB bump arena resident for the kernel's whole
 * life) is 4.7 MiB of memory a 640x480 desktop could never get back;
 * the pool hands it out to whatever needs it, and back again when the
 * desktop quits.
 *
 * The kernel has no heap of its own, but px.h's BMP/PNG paths
 * malloc()/free(). Its <stdlib.h> facilities are #define'd (via
 * VNU_IN_KERNEL) to the bump allocator below, which carves runs out of
 * one pool allocation: 16-byte aligned bumps keep every decoder happy
 * (int16 planes, byte IDAT...) regardless of the buffer's base
 * alignment, and the 2 MiB it takes comfortably holds the worst case
 * (decoded 590 KiB RGB buffer + ~590 KiB inflate work + <64 KiB of
 * compressed IDAT). The arena lives exactly as long as the decode that
 * uses it, so free() within a decode is still a no-op.
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
#include <vnu/pmm.h>
#include <vnu/vfs.h>
#include <vnu/posix.h>
#include <vnu/vga_gfx.h>
#undef malloc
#undef free

namespace {

/* px.h's scratch, one allocation per decode. Null outside a decode, so
 * a stray malloc() (there should be none) fails loudly instead of
 * scribbling on a buffer nobody is using. */
constexpr uint32_t PX_POOL_CAP = 2u * 1024u * 1024u;
uint8_t* g_px_pool = nullptr;
uint32_t g_px_used = 0;

/* Whole-desktop frame vga_gfx blits when a wallpaper is loaded. */
bool g_ready = false;

/* Largest source we'll decode: the shipped 512x384, i.e. exactly half
 * the desktop at 2x upscale. Also keeps the pixel-bound memory (decoded
 * RGB buffer + PNG inflate work) inside the decode arena above. */
constexpr uint32_t MAX_PIX = 512u * 384u;

/* The image in use, quantized to DAC indices at its own size and kept
 * across calls: a mode change only has to scale it to the new geometry,
 * and re-decoding the file would freeze the desktop for the seconds an
 * inflate of a 512x384 PNG costs on this hardware. */
uint8_t* g_small = nullptr;
int g_small_w = 0;
int g_small_h = 0;

/* Bare name of the image in use, for /proc/gfx. It is a static array on
 * purpose: releasing the pixels does not forget the choice. */
constexpr int NAME_CAP = 64;
char g_name[NAME_CAP];

/* The frame, exactly one mode big - the same discipline as the desktop
 * backbuffer it is blitted into (see vga_gfx.h). */
uint8_t* g_wall = nullptr;
uint32_t g_wall_px = 0;

/* Whole frames a run of `px` bytes takes in the pool. */
uint32_t frames_for(uint32_t px)
{
    return (px + vnu::pmm::FRAME_SIZE - 1u) / vnu::pmm::FRAME_SIZE;
}

uint8_t* take(uint32_t px)
{
    const uint32_t phys = vnu::pmm::alloc_contig(frames_for(px));
    /* Identity-mapped by paging::init(): physical is virtual here. */
    return phys ? reinterpret_cast<uint8_t*>(phys) : nullptr;
}

void give(uint8_t* p, uint32_t px)
{
    if (p)
        vnu::pmm::free_contig(reinterpret_cast<uint32_t>(p), frames_for(px));
}

/* Hand every pixel back to the pool. A failed or replaced decode starts
 * from here, so no run outlives the picture that wanted it. */
void release_pixels()
{
    give(g_wall, g_wall_px);
    g_wall = nullptr;
    g_wall_px = 0;
    give(g_small, MAX_PIX);
    g_small = nullptr;
    g_small_w = 0;
    g_small_h = 0;
    g_ready = false;
}

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
    if (!g_px_pool || need < static_cast<uint32_t>(n) ||
        g_px_used + need > PX_POOL_CAP)
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

/* Probe, decode and quantize g_file into g_small, leaving the frame
 * alone. Returns 0 on success. The scratch arena is taken and released
 * around the decode, and the quantized image - the one thing that has
 * to outlive it - is already allocated by the caller. */
int decode_quantized(uint32_t n)
{
    if (n < 4)
        return -1;

    int w = 0, h = 0;
    int fmt = px_probe(g_file, n, &w, &h);
    if (fmt == PX_NONE || w <= 0 || h <= 0)
        return -1;
    if (static_cast<uint32_t>(w) * static_cast<uint32_t>(h) > MAX_PIX)
        return -1;

    /* One arena for the decode: the RGB buffer and the inflate work it
     * needs, handed out in 16-byte bites. */
    g_px_pool = take(PX_POOL_CAP);
    if (!g_px_pool)
        return -1;
    g_px_used = 0;

    uint8_t* rgb = static_cast<uint8_t*>(vnu_kalloc(
        (static_cast<uint32_t>(w) * static_cast<uint32_t>(h)) * 3u));
    /* Dispatch the integer decoders directly instead of px_decode:
     * px_jpg_decode (with its float IDCT) must stay unreferenced so the
     * linker never pulls in soft-float calls this kernel cannot satisfy. */
    int rc;
    if (!rgb) {
        rc = -1;
    } else if (fmt == PX_BMP) {
        rc = px_bmp_decode(g_file, n, &w, &h, rgb);
    } else if (fmt == PX_PNG) {
        rc = px_png_decode(g_file, n, &w, &h, rgb);
    } else {
        rc = -1;
    }
    if (rc == 0) {
        quantize(rgb, w, h, g_small);
        g_small_w = w;
        g_small_h = h;
    }
    vnu_kfree(rgb);
    give(g_px_pool, PX_POOL_CAP);
    g_px_pool = nullptr;
    return rc;
}

} // namespace

/* A frame for the mode now programmed, the old one released. The mode
 * only ever grows through resize(), but taking a fresh run when it does
 * is simpler than tracking a capacity, and a frame that cannot be had
 * is not a reason to keep the wrong-sized one. */
bool take_frame(int w, int h)
{
    const uint32_t px = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);
    uint8_t* next = take(px);
    if (!next)
        return false;
    give(g_wall, g_wall_px);
    g_wall = next;
    g_wall_px = px;
    return true;
}

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
    if (g_small_w <= 0 || g_small_h <= 0) {
        g_ready = false;
        return;
    }
    if (!take_frame(vnu::vgfx::width(), vnu::vgfx::height())) {
        /* Out of pool for the bigger mode: the procedural scene is
         * always available and always fits. */
        g_ready = false;
        return;
    }
    stretch();
}

void unload()
{
    /* The desktop is over, so the pixels it was drawing are worth their
     * frames back. The name stays: /etc/vnu/wallpaper is still the
     * setting, and the next session re-decodes it. */
    release_pixels();
}

bool load()
{
    /* A fresh start every time: the pool gets the last session's runs
     * back before the new ones are asked for, so a machine that has run
     * a few desktops in a row is in exactly the state of a fresh boot
     * plus one desktop. */
    release_pixels();
    uint32_t n = slurp(VNU_WALLPAPER);
    if (n < 4)
        return false;
    g_small = take(MAX_PIX);
    if (!g_small)
        return false;
    if (!take_frame(vnu::vgfx::width(), vnu::vgfx::height())) {
        release_pixels();
        return false;
    }
    if (decode_quantized(n) != 0) {
        release_pixels();
        return false;
    }
    g_ready = true;
    stretch();
    note_name(VNU_WALLPAPER);
    return true;
}

int apply(const char* path)
{
    uint32_t n = slurp(path);
    if (n == 0)
        return -VNU_ENOENT;
    /* The candidate is decoded before the frame in use is touched, so
     * a picture that turns out to be JPEG, oversized or out of pool
     * leaves the desktop exactly as it was - the reason apply() is not
     * a no-op on failure is the file, not the pixels. */
    if (n < 4)
        return -VNU_EINVAL;
    uint8_t* small = take(MAX_PIX);
    if (!small)
        return -VNU_EINVAL;
    uint32_t frame_px = static_cast<uint32_t>(vnu::vgfx::width()) *
                        static_cast<uint32_t>(vnu::vgfx::height());
    uint8_t* wall = take(frame_px);
    if (!wall) {
        give(small, MAX_PIX);
        return -VNU_EINVAL;
    }
    uint8_t* old_small = g_small;
    uint32_t old_small_w = g_small_w, old_small_h = g_small_h;
    uint8_t* old_wall = g_wall;
    uint32_t old_wall_px = g_wall_px;
    bool old_ready = g_ready;
    g_small = small;
    g_wall = wall;
    g_wall_px = frame_px;
    g_small_w = 0;
    g_small_h = 0;
    int rc = decode_quantized(n);
    if (rc != 0) {
        give(wall, frame_px);
        give(small, MAX_PIX);
        g_small = old_small;
        g_small_w = old_small_w;
        g_small_h = old_small_h;
        g_wall = old_wall;
        g_wall_px = old_wall_px;
        g_ready = old_ready;
        return -VNU_EINVAL;
    }
    give(old_wall, old_wall_px);
    give(old_small, MAX_PIX);

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
    stretch();
    note_name(path);
    return 0;
}

const uint8_t* frame()
{
    return g_wall;
}

/* True only when there is a frame, and it is a frame for the mode on
 * screen: the blit copies a whole mode's worth of pixels, so a frame
 * left over from another geometry must read as absent (and the
 * procedural scene, which is drawn straight into the backbuffer, takes
 * its place). */
bool ready()
{
    if (!g_ready || !g_wall)
        return false;
    const uint32_t px = static_cast<uint32_t>(vnu::vgfx::width()) *
                        static_cast<uint32_t>(vnu::vgfx::height());
    return px == g_wall_px;
}

const char* current_name()
{
    return g_name;
}

} // namespace vnu::wallpaper
