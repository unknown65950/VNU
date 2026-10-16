/*
 * wallpaper.cpp — decode /etc/vnu/wallpaper (any px.h format) and
 * render it stretched over the whole desktop, quantized to a palette
 * of the display's own.
 *
 * The VFS no longer caps a single file at 64 KiB (each node's content
 * grows on demand), and a full 1024x768 24-bit BMP would be ~2.4 MiB,
 * so the shipped wallpaper is a small 512x384 PNG (designed *in* the
 * palette, ~4 KiB) that the desktop nearest-neighbour-upscales onto the
 * whole screen. The /wallpaper file itself is swappable: any BMP, PNG or
 * JPEG px.h understands, up to MAX_PIX pixels, gets decoded and
 * display-fitted the same way.
 *
 * The palette a picture ends up in is the display's, and it is not the
 * desktop's sixteen: vga_gfx's first 16 slots belong to the system (the
 * console presents in them, COLOR_* indexes them) and the remaining 240
 * are the picture's, which is why the quantizer here cuts to 240 rather
 * than to 16. A photograph in sixteen colours is a postage stamp; in
 * 240 it is the photograph. Pixels of the frame are one byte each either
 * way, so a full-screen wallpaper costs exactly what the shipped PNG
 * costs.
 *
 * All of this module's buffers — the desktop frame, the quantized
 * source, the file itself and px.h's decode scratch — are taken from the
 * PMM pool for as long as they are in use, and sized for the image and
 * mode on screen rather than for the top of the ladder. What they used
 * to be (.bss arrays at MAX_MODE, plus a 2 MiB bump arena resident for
 * the kernel's whole life) is 4.7 MiB of memory a 640x480 desktop could
 * never get back; the pool hands it out to whatever needs it, and back
 * again when the desktop quits.
 *
 * The kernel has no heap of its own, but px.h's BMP/PNG/JPEG paths
 * malloc()/free(). Its <stdlib.h> facilities are #define'd (via
 * VNU_IN_KERNEL) to the bump allocator below, which carves runs out of
 * one pool allocation: 16-byte aligned bumps keep every decoder happy
 * (int16 planes, byte IDAT...) regardless of the buffer's base
 * alignment. The arena is sized for the decode that asks for it and
 * lives exactly as long as that decode, so a wallpaper nobody opens
 * costs none of it and free() within a decode is still a no-op.
 *
 * JPEG is the one decoder that needs the FPU: its IDCT is the only
 * IEEE-float code in px.h. This file is therefore the one translation
 * unit the kernel builds with -m80387 (see kernel/CMakeLists.txt), and
 * the state that costs is saved and restored around each decode by
 * fxsave()/fxrstor() — the picture is not worth handing userspace half
 * of an FPU, and a decode that clobbered the registers a program had
 * left in them would be its own class of bug.
 *
 * The wallpaper is a setting rather than a picture to browse, so it
 * sits in /etc/vnu beside the demo pack instead of inside it, and
 * picview's directory listing stays unchanged.
 *
 * Not every wallpaper is a picture: the shipped default is a flat grey,
 * written as a seven-byte marker (see vnu/wallpaper.h) that names one
 * palette slot. A user who never picks a photo therefore decodes
 * nothing and holds no frame - the desktop is painted straight from the
 * slot - which is why a fresh boot reaches the desktop in one step. The
 * same apply()/load() pair handles both, so `wallpaper` and the picker
 * need no idea which form they are installing.
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
#include <vnu/arch/cpu.h>
#include <cstdint>

#undef malloc
#undef free

namespace {

/* px.h's scratch, one allocation per decode - sized by arena_for() for
 * the image in hand. Null outside a decode, so a stray malloc() (there
 * should be none) fails loudly instead of scribbling on a buffer nobody
 * is using. */
uint8_t* g_px_pool = nullptr;
uint32_t g_px_used = 0;
uint32_t g_px_cap = 0;

/* Whole-desktop frame vga_gfx blits when a wallpaper is loaded. */
bool g_ready = false;

/* True when the wallpaper in use is a flat colour rather than a picture:
 * no frame is held, and the colour lives in one palette slot (see
 * set_solid below). Cleared whenever a picture is installed. */
bool g_solid = false;

/* Largest source we'll decode: the top of the mode ladder, so any size
 * the desktop itself can be set to is a wallpaper a user may choose.
 * The memory that goes with it (the decoded RGB buffer and px.h's
 * scratch) is taken from the pool for the duration of the decode and is
 * what arena_for() sizes. */
constexpr uint32_t MAX_PIX = 1280u * 1024u;

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
        vnu::pmm::free_contig(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)), frames_for(px));
}

/* The colours a picture may have, and the slot each one takes: the
 * display's palette is split by role (see vnu/vga_gfx.h) and the
 * picture's half starts after the system's sixteen, so a frame byte of
 * 200 is a colour of this wallpaper and 3 is the desktop's. */
constexpr uint32_t PAL_SLOTS = static_cast<uint32_t>(vnu::vgfx::PICTURE_SLOTS);
constexpr uint32_t PAL_BASE = static_cast<uint32_t>(vnu::vgfx::SYSTEM_SLOTS);

/* The whole file, in a pool run: the shipped wallpaper is 4 KiB and the
 * largest picture a user may reasonably point this at is a couple of
 * hundred, so a cap in .bss would be memory every desktop pays for the
 * few that need it. Taken when a file is read and given back when the
 * decode that wanted it is over. */
constexpr uint32_t FILE_CAP = 256u * 1024u;
uint8_t* g_file = nullptr;

/* The file itself, once the decode that wanted its bytes has read the
 * last of them: nothing holds a pointer into it past that, and a
 * 256 KiB run left over is a quarter of a megabyte the desktop cannot
 * get back. */
void give_file()
{
    give(g_file, FILE_CAP);
    g_file = nullptr;
}

/* Hand every pixel back to the pool. A failed or replaced decode starts
 * from here, so no run outlives the picture that wanted it. The file
 * bytes stay only while a caller still has a use for them (apply()
 * rewrites them; load() re-reads); release_pixels() gives those back
 * too. */
void release_frame()
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

void release_pixels()
{
    give_file();
    release_frame();
    g_solid = false;
}

/* Install a flat colour as the wallpaper: the colour goes into the first
 * slot of the display's picture half, and the desktop is filled with it
 * by draw_wallpaper(). There is no frame to hold, which is the point -
 * the shipped grey costs a decode and no memory at all. */
void set_solid(uint8_t r, uint8_t g, uint8_t b)
{
    /* The DAC's colour values are 6-bit; the marker holds 8-bit ones so
     * a program can write #383838 and see #383838 (see wallpaper.h). */
    const uint8_t rgb[1][3] = {
        {static_cast<uint8_t>(r >> 2), static_cast<uint8_t>(g >> 2),
         static_cast<uint8_t>(b >> 2)},
    };
    release_frame(); /* a flat colour holds no frame from before */
    vnu::vgfx::picture_palette_set(rgb, 1);
    g_solid = true;
}



/* Read a whole file into g_file. Returns its length, 0 when it cannot
 * be opened, and never more than FILE_CAP bytes. */
uint32_t slurp(const char* path)
{
    int fd = vnu::vfs::open(path, vnu::posix::O_RDONLY);
    if (fd < 0)
        return 0;
    if (!g_file)
        g_file = take(FILE_CAP);
    if (!g_file) {
        vnu::vfs::close(fd);
        return 0;
    }
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

/* Write the bytes just read (g_file[0..n)) back to the wallpaper file,
 * so a choice made through apply() survives a reboot. 0, or -VNU_EIO
 * when the write could not complete; g_file is left for the caller to
 * give back either way. */
int write_wallpaper_file(uint32_t n)
{
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
    return wrote == n ? 0 : -VNU_EIO;
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
        g_px_used + need > g_px_cap)
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

/* --- Choosing the picture's colours ---------------------------------
 *
 * The palette is chosen from the image's own pixels, once, and every
 * pixel is then a table lookup. That is the whole design: a nearest-
 * colour search over 240 candidates for every pixel of a megapixel
 * picture is 240 million distance computations, which is seconds of
 * frozen desktop, while a 5-5-5 histogram is 32768 counters and the
 * lookup is one byte load.
 *
 * 5-5-5 rather than 6-6-6 because the DAC has six bits per channel and
 * a sixth bit nobody can see is a box nobody can split: at 5 bits a cell
 * is a colour the palette can actually name, and at 6 bits half the
 * cells are the same colour twice. */
constexpr uint32_t CELLS = 32768u;

inline uint32_t cell_of(uint8_t r, uint8_t g, uint8_t b)
{
    return (static_cast<uint32_t>(r >> 3) << 10) |
           (static_cast<uint32_t>(g >> 3) << 5) |
           static_cast<uint32_t>(b >> 3);
}

/* A box of cells, and how many pixels are in it. Median cut works by
 * cutting the whole image into one box, then repeatedly cutting the
 * fullest box in two, until there are as many boxes as the palette has
 * colours: every cut splits a colour range somewhere no pixel sits, so
 * the boxes end up holding what the picture actually uses and the rare
 * hues each keep a colour of their own instead of being averaged into a
 * common one. */
struct Box {
    uint32_t lo, hi;   /* cell indices, inclusive */
    uint32_t count;    /* pixels in the box */
    int32_t r0, r1;    /* the range its populated cells reach on each
                        * axis: what a cut is chosen from, and what the
                        * colour of the box is averaged over */
    int32_t g0, g1;
    int32_t b0, b1;
};

/* Everything quantize() asks the decode arena for: the histogram, the
 * cell-to-slot table it is cut into and the boxes, each rounded up to
 * the 16 bytes the allocator hands out. arena_for() reserves exactly
 * this much beside what the decode itself wants - worked out here rather
 * than guessed at, because a constant a few kilobytes short of the real
 * figure costs the whole quantize, and a picture that quantizes to
 * nothing is a wallpaper that never appears. */
constexpr uint32_t QUANT_BYTES =
    (CELLS * sizeof(uint32_t) + 15u) / 16u * 16u +
    (CELLS + 15u) / 16u * 16u +
    (PAL_SLOTS * sizeof(Box) + 15u) / 16u * 16u;

/* Count the pixels in a range and find how far along each axis its
 * populated cells reach. The extents are what the cut is chosen from:
 * a box whose cells all sit on one value of an axis has nothing to split
 * on that axis, however many pixels it holds. */
void measure(const uint32_t* hist, uint32_t lo, uint32_t hi, Box& box)
{
    int32_t rmin = 32, rmax = -1, gmin = 32, gmax = -1, bmin = 32, bmax = -1;
    uint32_t count = 0;
    for (uint32_t c = lo; c <= hi; ++c) {
        const uint32_t n = hist[c];
        if (!n)
            continue;
        count += n;
        const int32_t r = static_cast<int32_t>(c >> 10);
        const int32_t g = static_cast<int32_t>((c >> 5) & 31u);
        const int32_t b = static_cast<int32_t>(c & 31u);
        if (r < rmin) rmin = r;
        if (r > rmax) rmax = r;
        if (g < gmin) gmin = g;
        if (g > gmax) gmax = g;
        if (b < bmin) bmin = b;
        if (b > bmax) bmax = b;
    }
    box.lo = lo;
    box.hi = hi;
    box.count = count;
    box.r0 = rmin; box.r1 = rmax;
    box.g0 = gmin; box.g1 = gmax;
    box.b0 = bmin; box.b1 = bmax;
}

/* Cut a box at the cell where half its pixels are behind it. Returns
 * false when the box holds its pixels on single cells - nothing to cut
 * between - which is the end of the line for a box no matter how full it
 * is. */
bool cut(const uint32_t* hist, Box& box, Box& left, Box& right)
{
    if (box.r1 <= box.r0 && box.g1 <= box.g0 && box.b1 <= box.b0)
        return false;

    /* Walk the cells in order, counting pixels until half the box is
     * behind us. The cut lands on the last cell that took the count up to
     * the halfway mark, so neither side is empty and the half with fewer
     * pixels is not left with a single one-pixel sliver. A box is a range
     * of cells, and cells are indexed red, then green, then blue, so this
     * splits on red where the box still has red to spare and works down
     * to blue in the boxes that are left with one colour each. */
    const uint32_t half = box.count / 2u;
    uint32_t seen = 0, at = box.lo;
    for (uint32_t c = box.lo; c <= box.hi; ++c) {
        seen += hist[c];
        at = c;
        if (seen >= half)
            break;
    }
    if (at >= box.hi)
        return false;
    measure(hist, box.lo, at, left);
    measure(hist, at + 1, box.hi, right);
    if (!left.count || !right.count)
        return false;
    box = left;
    return true;
}

/* One box for the whole colour cube, then fullest-first cuts until
 * there are as many boxes as the palette has slots. Fewer boxes than
 * slots is not a failure - a picture with six colours in it gets six,
 * and picture_palette_set() pads the rest - so the loop stops early
 * when nothing is left to cut. */
uint32_t median_cut(const uint32_t* hist, Box* boxes, uint32_t want)
{
    uint32_t n = 0;
    measure(hist, 0, CELLS - 1, boxes[n]);
    if (!boxes[n].count)
        return 0;
    ++n;
    while (n < want) {
        /* Fullest first, longest span breaking ties: a box with a
         * million pixels in three colours deserves its cut before a box
         * with two hundred pixels spread over the whole cube. */
        uint32_t best = CELLS;
        for (uint32_t i = 0; i < n; ++i) {
            if (boxes[i].count < 2)
                continue;
            const int32_t span = (boxes[i].r1 - boxes[i].r0) +
                                 (boxes[i].g1 - boxes[i].g0) +
                                 (boxes[i].b1 - boxes[i].b0);
            if (best == CELLS)
                best = i;
            else if (boxes[i].count > boxes[best].count ||
                     (boxes[i].count == boxes[best].count &&
                      span > ((boxes[best].r1 - boxes[best].r0) +
                              (boxes[best].g1 - boxes[best].g0) +
                              (boxes[best].b1 - boxes[best].b0))))
                best = i;
        }
        if (best == CELLS)
            break;
        Box left, right;
        if (!cut(hist, boxes[best], left, right))
            break;
        boxes[best] = left;
        boxes[n++] = right;
    }
    return n;
}

/* One colour per box: the average of the cells in it, weighted by how
 * many pixels each cell holds, so a box that owns three pixels of a
 * corner shade does not decide that corner's colour. 5 bits to the DAC's
 * 6 by replication, the same expansion the VGA driver's 8-bit palette
 * reply makes in the other direction. */
void palette_from_boxes(const uint32_t* hist, const Box* boxes, uint32_t n,
                        uint8_t (*pal)[3])
{
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t count = 0, sr = 0, sg = 0, sb = 0;
        for (uint32_t c = boxes[i].lo; c <= boxes[i].hi; ++c) {
            const uint32_t k = hist[c];
            if (!k)
                continue;
            count += k;
            sr += k * (c >> 10);
            sg += k * ((c >> 5) & 31u);
            sb += k * (c & 31u);
        }
        const uint32_t r = count ? sr / count : 0;
        const uint32_t g = count ? sg / count : 0;
        const uint32_t b = count ? sb / count : 0;
        pal[i][0] = static_cast<uint8_t>((r << 1) | (r >> 4));
        pal[i][1] = static_cast<uint8_t>((g << 1) | (g >> 4));
        pal[i][2] = static_cast<uint8_t>((b << 1) | (b >> 4));
    }
}

/* Which palette slot every cell of the cube lands in, so mapping a pixel
 * is this table and nothing else. Built by asking which of the colours is
 * nearest to each populated cell - CELLS times a search over what is left
 * of the palette, once per image, which is why the per-pixel path can be
 * a single load. */
void build_lut(const uint32_t* hist, const uint8_t (*pal)[3], uint32_t n,
               uint8_t* lut)
{
    for (uint32_t c = 0; c < CELLS; ++c) {
        if (!hist[c]) {
            lut[c] = static_cast<uint8_t>(PAL_BASE);
            continue;
        }
        /* Cells are 5 bits per channel and the palette is 6, so the
         * distance is measured in the same 5-bit space the cells are
         * in: nothing here can see a difference the cell cannot. */
        const int32_t r = static_cast<int32_t>(c >> 10);
        const int32_t g = static_cast<int32_t>((c >> 5) & 31u);
        const int32_t b = static_cast<int32_t>(c & 31u);
        uint32_t best = 0;
        int32_t bd = 1 << 30;
        for (uint32_t i = 0; i < n; ++i) {
            const int32_t dr = r - static_cast<int32_t>(pal[i][0] >> 1);
            const int32_t dg = g - static_cast<int32_t>(pal[i][1] >> 1);
            const int32_t db = b - static_cast<int32_t>(pal[i][2] >> 1);
            const int32_t dd = dr * dr + dg * dg + db * db;
            if (dd < bd) {
                bd = dd;
                best = i;
            }
        }
        lut[c] = static_cast<uint8_t>(PAL_BASE + best);
    }
}

/* Quantize a decoded RGB image to palette indices, at the image's own
 * size (see stretch() for why not at the desktop's), and write the
 * colours those indices name into `pal`. Returns how many slots it used,
 * 0 when the image had no pixels to speak of. */
uint32_t quantize(const uint8_t* rgb, int w, int h, uint8_t* out,
                  uint8_t (*pal)[3])
{
    const uint32_t n = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);
    if (!n)
        return 0;

    /* The histogram and the table it is cut into come out of the same
     * decode arena as px.h's own scratch (see vnu_kalloc above): both
     * are per-decode working set, and neither outlives this call. */
    auto* hist = static_cast<uint32_t*>(vnu_kalloc(CELLS * sizeof(uint32_t)));
    auto* lut = static_cast<uint8_t*>(vnu_kalloc(CELLS));
    Box* boxes = static_cast<Box*>(vnu_kalloc(PAL_SLOTS * sizeof(Box)));
    if (!hist || !lut || !boxes)
        return 0;

    for (uint32_t i = 0; i < CELLS; ++i)
        hist[i] = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* p = rgb + i * 3u;
        ++hist[cell_of(p[0], p[1], p[2])];
    }

    const uint32_t used = median_cut(hist, boxes, PAL_SLOTS);
    if (!used)
        return 0;
    palette_from_boxes(hist, boxes, used, pal);
    build_lut(hist, pal, used, lut);

    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* p = rgb + i * 3u;
        out[i] = lut[cell_of(p[0], p[1], p[2])];
    }
    return used;
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

/* px.h's JPEG IDCT is IEEE single-precision float code, and this is the
 * one file the kernel builds with the FPU on (see kernel/CMakeLists.txt).
 * Everything else the kernel does with a processor state - the IDCT
 * included - happens between an arch::fpu_save() and an arch::fpu_restore()
 * of the same 512 bytes (the arch layer owns the buffer and its x86
 * 16-byte alignment), so what a program left in the registers is what it
 * finds again even though the kernel borrowed them for a megapixel of
 * JPEG. */
class FpuBorrow {
public:
    FpuBorrow() { vnu::arch::fpu_save(); }
    ~FpuBorrow() { vnu::arch::fpu_restore(); }
    FpuBorrow(const FpuBorrow&) = delete;
    FpuBorrow& operator=(const FpuBorrow&) = delete;
};

/* How much scratch a decode of this format at this size can ask for, so
 * the arena is a decode's actual cost rather than a fixed 2 MiB every
 * desktop would be holding. The three formats want different things:
 * BMP reads straight into the caller's buffer, PNG needs the inflated
 * filtered rows (4 bytes per pixel) plus the compressed stream, and JPEG
 * needs the coefficients of every component plus a sample plane for each
 * of them, whose size px.h works out from the frame header itself. */
uint32_t arena_for(int fmt, uint32_t w, uint32_t h, uint32_t n)
{
    /* Every allocation the arena hands out is rounded up to the
     * allocator's 16-byte bump, so the RGB buffer is asked for as it
     * will be given and not as it is written. */
    const uint32_t px = w * h;
    const uint32_t rgb = (px * 3u + 15u) & ~15u;
    uint32_t want;
    if (fmt == PX_JPG) {
        const uint32_t scratch = px_jpg_workspace(g_file, n);
        /* A header px.h cannot size is a header it will refuse; fall back
         * to the largest picture we accept rather than to no arena at
         * all, and let the decode report the error itself. */
        want = rgb + (scratch ? scratch : 1280u * 1024u * 8u);
    } else if (fmt == PX_PNG) {
        /* The same trade for a PNG: what the decode asks for is the IDAT
         * stream it grows through plus the rows it inflates into, and a
         * header it cannot size is one it will refuse anyway. The rows
         * are at most five bytes a pixel, the stream four times the file. */
        const uint32_t scratch = px_png_workspace(g_file, n);
        want = rgb + (scratch ? scratch : n * 4u + px * 5u + 4096u);
    } else {
        want = rgb;              /* the RGB buffer is all a BMP needs */
    }
    /* The quantizer's own working set rides in the same arena: a 5-5-5
     * histogram, the cell-to-slot table it is cut into and the boxes. */
    return want + QUANT_BYTES;
}

/* Probe, decode and quantize g_file into g_small, leaving the frame
 * alone, and hand back the colours the frame's indices name. Returns 0
 * on success. The scratch arena is taken and released around the decode,
 * and the quantized image - the one thing that has to outlive it - is
 * already allocated by the caller.
 *
 * The palette comes back rather than going straight into the display,
 * so a decode that fails leaves the wallpaper on screen drawing in the
 * colours it was already using. */
int decode_quantized(uint32_t n, uint8_t (*pal)[3], uint32_t* pal_count)
{
    if (n < 4)
        return -1;

    int w = 0, h = 0;
    int fmt = px_probe(g_file, n, &w, &h);
    if (fmt == PX_NONE || w <= 0 || h <= 0)
        return -1;
    if (static_cast<uint32_t>(w) * static_cast<uint32_t>(h) > MAX_PIX)
        return -1;

    /* One arena for the decode, sized for this image: the RGB buffer
     * and whatever the format needs to produce it, handed out in 16-byte
     * bites. */
    const uint32_t cap = arena_for(fmt, static_cast<uint32_t>(w),
                                  static_cast<uint32_t>(h), n);
    g_px_pool = take(cap);
    if (!g_px_pool)
        return -1;
    g_px_used = 0;
    g_px_cap = cap;

    uint8_t* rgb = static_cast<uint8_t*>(vnu_kalloc(
        (static_cast<uint32_t>(w) * static_cast<uint32_t>(h)) * 3u));
    int rc;
    if (!rgb) {
        rc = -1;
    } else if (fmt == PX_BMP) {
        rc = px_bmp_decode(g_file, n, &w, &h, rgb);
    } else if (fmt == PX_PNG) {
        rc = px_png_decode(g_file, n, &w, &h, rgb);
    } else if (fmt == PX_JPG) {
        /* The only decoder that touches the FPU, so the only one that
         * borrows it. */
        FpuBorrow fpu;
        rc = px_jpg_decode(g_file, n, &w, &h, rgb);
    } else {
        rc = -1;
    }
    *pal_count = 0;
    if (rc == 0) {
        *pal_count = quantize(rgb, w, h, g_small, pal);
        if (*pal_count == 0)
            rc = -1;
        else {
            g_small_w = w;
            g_small_h = h;
        }
    }
    vnu_kfree(rgb);
    give(g_px_pool, cap);
    g_px_pool = nullptr;
    g_px_cap = 0;
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
    if (is_solid(g_file, n)) {
        /* A flat colour: nothing to decode and no frame to hold, so this
         * is the whole job - install the colour and report the name. It
         * is what a fresh boot takes, and the reason the shipped default
         * costs no decode and no memory. */
        set_solid(g_file[4], g_file[5], g_file[6]);
        give_file();
        note_name(VNU_WALLPAPER);
        return true;
    }
    if (n < 4)
        return false;
    g_small = take(MAX_PIX);
    if (!g_small)
        return false;
    if (!take_frame(vnu::vgfx::width(), vnu::vgfx::height())) {
        release_pixels();
        return false;
    }
    uint8_t pal[PAL_SLOTS][3];
    uint32_t used = 0;
    if (decode_quantized(n, pal, &used) != 0) {
        release_pixels();
        return false;
    }
    /* The frame is full of indices this decode has just chosen, so the
     * display has to learn them before it is shown - and on an 8bpp card
     * picture_palette_set() puts them in the DAC, which is what makes
     * them reach the monitor. */
    vnu::vgfx::picture_palette_set(pal, used);
    give_file();
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
    if (is_solid(g_file, n)) {
        /* A flat colour cannot fail to decode, so it is installed first
         * and then written back as the setting, the same two steps a
         * picture takes on success. set_solid() also drops any frame the
         * previous wallpaper held, so switching from a photo to a grey
         * gives the photo's memory straight back. */
        set_solid(g_file[4], g_file[5], g_file[6]);
        int rc = write_wallpaper_file(n);
        give_file();
        if (rc != 0)
            return rc;
        note_name(path);
        return 0;
    }
    /* The candidate is decoded before the frame in use is touched, so
     * a picture that turns out to be a format px.h does not know, too
     * big for the pool or undecodable leaves the desktop exactly as it
     * was - the reason apply() is not a no-op on failure is the file,
     * not the pixels. */
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
    /* The candidate's colours, held back until the decode has worked:
     * a picture that fails leaves the display drawing in the colours the
     * wallpaper already had, which is the whole promise apply() makes. */
    uint8_t pal[PAL_SLOTS][3];
    uint32_t used = 0;
    int rc = decode_quantized(n, pal, &used);
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
    vnu::vgfx::picture_palette_set(pal, used);
    give(old_wall, old_wall_px);
    give(old_small, MAX_PIX);

    /* The picture is good, so it becomes the wallpaper file too. The
     * frame is already on screen by now: overwriting a VFS node of this
     * size cannot really fail, and a failure here (-VNU_EIO) is the only
     * case where the screen and the file can disagree. */
    int wr = write_wallpaper_file(n);
    give_file();   /* the write is the last reader of these bytes */
    if (wr != 0)
        return wr;

    g_solid = false; /* a picture replaces a flat colour */
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

bool solid()
{
    return g_solid;
}

uint8_t solid_color()
{
    /* The first slot of the picture half: set_solid() put the colour
     * there, and nothing else on the desktop draws from that half. */
    return static_cast<uint8_t>(vnu::vgfx::SYSTEM_SLOTS);
}

const char* current_name()
{
    return g_name;
}

} // namespace vnu::wallpaper
