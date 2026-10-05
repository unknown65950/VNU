/*
 * px.h — self-contained image decoders for picview.
 *
 * Decodes BMP (24/32-bit BI_RGB), PNG (8-bit, non-interlaced; color
 * types 0/2/3/4/6 with tRNS) and JPEG (baseline SOF0, extended
 * sequential SOF1 and progressive SOF2, 1 or 3 components, 4:4:4 /
 * 4:2:2 / 4:2:0 subsampling) straight into an
 * RGB888 buffer.  Everything is plain static C99 so the single header
 * can be *copied* into a host test harness and compared against
 * Python/PIL output.  The decoders are freestanding (no libm, no CRT
 * beyond malloc/free), bounds-check every read and flatten alpha onto
 * an opaque white background so the caller never deals with alpha.
 */
#ifndef VNU_PX_H
#define VNU_PX_H

#include <stdint.h>
#include <stddef.h>
#if defined(VNU_IN_KERNEL)
/* Kernel build (vnu/kernel/include/vnu/px.h): freestanding, no
 * <stdlib.h>, and no heap — wallpaper.cpp maps these to its bump
 * allocator. Never defined for userspace builds. */
extern "C" void* vnu_kalloc(unsigned long n);
extern "C" void vnu_kfree(void* p);
#define malloc vnu_kalloc
#define free vnu_kfree
#elif defined(VLIBC_TARGET_VNU)
#include <vlibc/stdlib.h>
#else
#include <stdlib.h>
#endif

#define PX_BMP 1
#define PX_PNG 2
#define PX_JPG 3
#define PX_NONE 0

/* Plug the format of a file.  Returns PX_* (or PX_NONE) and reports
 * the pixel dimensions.  Never reads past n bytes, never allocates. */
static int px_probe(const uint8_t* d, unsigned n, int* w, int* h);

/* Decode d (n bytes) into rgb, which must hold 3*w*h bytes (top-down
 * rows, byte order RGB).  Returns 0 on success or a negative error. */
static inline int px_decode(int fmt, const uint8_t* d, unsigned n,
                     int w, int h, uint8_t* rgb);

/* --- bytes ------------------------------------------------------------ */

static uint32_t px_rd32be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t px_rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t px_rd16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Composite fg over bg with 0..255 alpha (rounded, integer-only). */
static uint8_t px_compose(uint8_t fg, uint8_t bg, unsigned alpha)
{
    return (uint8_t)((fg * alpha + bg * (255u - alpha) + 127u) / 255u);
}

static void px_px(uint8_t* rgb, int x, int y, int sw,
                  uint8_t r, uint8_t g, uint8_t b)
{
    rgb[(y * sw + x) * 3 + 0] = r;
    rgb[(y * sw + x) * 3 + 1] = g;
    rgb[(y * sw + x) * 3 + 2] = b;
}

static int px_sig_png(const uint8_t* d)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    for (int i = 0; i < 8; ++i)
        if (d[i] != sig[i])
            return 0;
    return 1;
}

/* --- BMP -------------------------------------------------------------- */

static int px_bmp_decode(const uint8_t* d, unsigned n,
                         int* w, int* h, uint8_t* rgb)
{
    if (n < 54 || d[0] != 'B' || d[1] != 'M')
        return -1;
    uint32_t pixoff = px_rd32(d + 10);
    uint32_t dib = px_rd32(d + 14);
    if (dib < 40 || n < pixoff)
        return -1;
    int width = (int)px_rd32(d + 18);
    int hraw = (int)px_rd32(d + 22);
    if (width <= 0 || hraw == 0 || width > 4096 || hraw < -4096 || hraw > 4096)
        return -1;
    int topdown = hraw < 0;
    int height = hraw < 0 ? -hraw : hraw;
    uint16_t planes = px_rd16(d + 26);
    uint16_t bpp = px_rd16(d + 28);
    uint32_t comp = px_rd32(d + 30);
    if (planes != 1)
        return -1;
    if (!(bpp == 24 || bpp == 32))
        return -2;
    if (!(comp == 0 || (comp == 3 && bpp == 32)))
        return -2;
    uint8_t pad = (bpp == 24) ? 3 : 4;
    uint32_t stride = ((uint32_t)width * bpp + 31u) / 32u * 4u;
    if (pixoff + stride * (uint32_t)(height - 1) + (uint32_t)width * pad > n)
        return -1;
    for (int y = 0; y < height; ++y) {
        int ry = topdown ? y : (height - 1 - y);
        const uint8_t* row = d + pixoff + (uint32_t)ry * stride;
        for (int x = 0; x < width; ++x)
            px_px(rgb, x, y, width, row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
    }
    *w = width;
    *h = height;
    return 0;
}

/* --- PNG: DEFLATE (raw, no wrapper) ----------------------------------- */

typedef struct {
    const uint8_t* d;
    unsigned n;
    unsigned pos;
    unsigned bitpos;
} PxBit;

static int pb_get(PxBit* b, unsigned nbits, unsigned long* out)
{
    unsigned long v = 0;
    for (unsigned i = 0; i < nbits; ++i) {
if (b->pos >= b->n)
                    return -1;
            if ((b->d[b->pos] >> b->bitpos) & 1)
                v |= 1ul << i;
        if (++b->bitpos == 8) {
            b->bitpos = 0;
            ++b->pos;
        }
    }
    *out = v;
    return 0;
}

/* --- Huffman decode as a prefix tree ---------------------------------- *
 * DEFLATE packs Huffman codes "most significant bit first", matching
 * how the first bit read drives the top of the tree.  A literal binary
 * tree is therefore order-independent and immune to the LSB-first bit
 * reader in pb_get(). */
#define DEF_HN_MAX 4096
#define DEF_NSYM 288

typedef struct {
    int16_t ch[2];   /* child node index, -1 if none */
    int16_t sym;     /* leaf symbol, -1 for interior */
} DHnode;

typedef struct {
    DHnode n[DEF_HN_MAX];
    int used;
} DHuff;

/* Build a tree from n symbols whose code lengths are lens[i]. */
static int def_build(const uint16_t* lens, unsigned n, DHuff* h)
{
    h->used = 0;
    unsigned cnt[16] = { 0 };
    for (unsigned i = 0; i < n; ++i)
        if (lens[i])
            ++cnt[lens[i]];
    /* next_code[]: canonical code value for the first symbol of each
     * length (RFC 1951 3.2.2). */
    unsigned next[16];
    unsigned code = 0;
    for (unsigned b = 1; b <= 15; ++b) {
        code = (code + cnt[b - 1]) << 1;
        next[b] = code;
    }
    /* Tree root lives at index 0; leaf-children of the root are fine. */
    h->n[0].ch[0] = h->n[0].ch[1] = -1;
    h->n[0].sym = -1;
    h->used = 1;
    for (unsigned i = 0; i < n; ++i) {
        unsigned l = lens[i];
        if (!l)
            continue;
        unsigned c = next[l]++;
        int nd = 0;
        for (int bit = (int)l - 1; bit >= 0; --bit) {
            int dir = (int)((c >> bit) & 1u);
            if (h->n[nd].ch[dir] < 0) {
                if (h->used >= DEF_HN_MAX)
                    return -1;
                int nl = h->used++;
                h->n[nl].ch[0] = h->n[nl].ch[1] = -1;
                h->n[nl].sym = -1;
                h->n[nd].ch[dir] = (int16_t)nl;
            }
            nd = h->n[nd].ch[dir];
        }
        if (h->n[nd].sym >= 0)
            return -1; /* duplicate code */
        h->n[nd].sym = (int16_t)i;
    }
    return 0;
}

static int def_huff(PxBit* b, const DHuff* h)
{
    int nd = 0;
    for (unsigned i = 0; i < 15; ++i) {
        unsigned long bi;
        if (pb_get(b, 1, &bi))
            return -1;
        int c = h->n[nd].ch[(int)(bi & 1)];
        if (c < 0)
            return -1;
        nd = c;
        if (h->n[nd].sym >= 0)
            return h->n[nd].sym;
    }
    return -1;
}

#define DEF_LIT 288
#define DEF_DST 32
#define DEF_MAX_CODELEN 19

static const uint16_t def_lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35,
    43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const uint8_t def_lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const uint16_t def_dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
    385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
    16385, 24577,
};
static const uint8_t def_dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};
static const uint8_t def_clorder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};
static const uint16_t def_fixed_lit[DEF_LIT] = {
    /* the fixed literal/length code lengths */
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
     7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7,
    8, 8, 8, 8, 8, 8, 8, 8,
};
static const uint16_t def_fixed_dst[DEF_DST] = {
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
};

/* Raw DEFLATE inflate (no zlib header).  Returns 0 ok, <0 error,
 * sets *dlen to the number of bytes produced. */
static int def_inflate(const uint8_t* src, unsigned slen,
                       uint8_t* dst, unsigned dcap, unsigned* dlen)
{
    PxBit b = { src, slen, 0, 0 };
    unsigned long out = 0;
    DHuff dlit, ddist;

    for (;;) {
        if (b.pos >= slen)
            return -1;
        unsigned long fin, type;
        if (pb_get(&b, 1, &fin) || pb_get(&b, 2, &type))
            return -1;
        if (type == 0) {
            /* stored block: byte-aligned LEN/NLEN prefix */
            if (b.bitpos) {
                b.bitpos = 0;
                ++b.pos;
            }
            if (b.pos + 4 > slen)
                return -1;
            unsigned len = (unsigned)b.d[b.pos] | ((unsigned)b.d[b.pos + 1] << 8);
            unsigned nlen = (unsigned)b.d[b.pos + 2] | ((unsigned)b.d[b.pos + 3] << 8);
            if ((len ^ 0xFFFFu) != nlen || len > dcap - (unsigned)out)
                return -1;
            if (b.pos + 4 + len > slen)
                return -1;
            for (unsigned i = 0; i < len; ++i)
                dst[out + i] = b.d[b.pos + 4 + i];
            b.pos += 4 + len;
            out += len;
            goto next;
        }
        if (type == 1) {
            if (def_build(def_fixed_lit, DEF_LIT, &dlit) ||
                def_build(def_fixed_dst, DEF_DST, &ddist))
                return -1;
        } else if (type == 2) {
            unsigned long nlit, ndist, ncl;
            if (pb_get(&b, 5, &nlit) || pb_get(&b, 5, &ndist) || pb_get(&b, 4, &ncl))
                return -1;
            nlit += 257;
            ndist += 1;
            ncl += 4;
            uint16_t all[DEF_LIT + DEF_DST];
            uint16_t ccode[19] = { 0 };
            DHuff dcl;
            for (unsigned i = 0; i < ncl; ++i) {
                unsigned long v;
                if (pb_get(&b, 3, &v))
                    return -1;
                ccode[def_clorder[i]] = (uint16_t)v;
            }
            if (def_build(ccode, 19, &dcl))
                return -1;
            unsigned i = 0;
            while (i < nlit + ndist) {
                int s = def_huff(&b, &dcl);
                if (s < 0)
                    return -1;
                if (s <= 15) {
                    all[i++] = (uint16_t)s;
                } else if (s == 16) {
                    if (i == 0)
                        return -1;
                    unsigned long rep;
                    if (pb_get(&b, 2, &rep))
                        return -1;
                    uint16_t prv = all[i - 1];
                    for (unsigned k = 0; k < rep + 3; ++k) {
                        if (i >= nlit + ndist)
                            return -1;
                        all[i++] = prv;
                    }
                } else if (s == 17) {
                    unsigned long rep;
                    if (pb_get(&b, 3, &rep))
                        return -1;
                    for (unsigned k = 0; k < rep + 3; ++k) {
                        if (i >= nlit + ndist)
                            return -1;
                        all[i++] = 0;
                    }
                } else {
                    unsigned long rep;
                    if (pb_get(&b, 7, &rep))
                        return -1;
                    for (unsigned k = 0; k < rep + 11; ++k) {
                        if (i >= nlit + ndist)
                            return -1;
                        all[i++] = 0;
                    }
                }
            }
            if (def_build(all, nlit, &dlit))
                return -1;
            if (def_build(all + nlit, ndist, &ddist))
                return -1;
        } else {
            return -1;
        }

        for (;;) {
            int s = def_huff(&b, &dlit);
            if (s < 0)
                return -1;
            if (s == 256)
                break;
            if (s < 256) {
                if (out >= dcap)
                    return -2;
                dst[out++] = (uint8_t)s;
                continue;
            }
            unsigned sym = (unsigned)s - 257;
            if (sym >= 29)
                return -1;
            unsigned long ext;
            if (pb_get(&b, def_lext[sym], &ext))
                return -1;
            unsigned len = def_lbase[sym] + (unsigned)ext;
            int dsym = def_huff(&b, &ddist);
            if (dsym < 0 || dsym >= 30)
                return -1;
            unsigned long dext;
            if (pb_get(&b, def_dext[dsym], &dext))
                return -1;
            unsigned dist = def_dbase[dsym] + (unsigned)dext;
            if (dist == 0 || dist > out || len > dcap - (unsigned)out)
                return -1;
            unsigned srcp = (unsigned)out - dist;
            for (unsigned k = 0; k < len; ++k)
                dst[out + k] = dst[srcp + k];
            out += len;
        }
next:
        if (fin)
            break;
    }
    *dlen = (unsigned)out;
    return 0;
}

/* --- PNG -------------------------------------------------------------- */

static uint8_t px_paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = (int)a + b - c;
    int pa = p - a; if (pa < 0) pa = -pa;
    int pb = p - b; if (pb < 0) pb = -pb;
    int pc = p - c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

typedef struct {
    uint16_t plte[256][3];
    uint16_t trns[256];
    int nplte;
    int have_trns;
    uint16_t tr_gray;
    uint16_t tr_rgb[3];
} PngAux;

static int px_png_decode(const uint8_t* d, unsigned n,
                         int* w, int* h, uint8_t* rgb)
{
    if (n < 33 || !px_sig_png(d))
        return -1;
    if (px_rd32be(d + 8) != 13 ||
        px_rd32be(d + 12) != 0x49484452u)
        return -1;
    int width = (int)px_rd32be(d + 16);
    int height = (int)px_rd32be(d + 20);
    uint8_t bits = d[24], ct = d[25], comp = d[26], filt = d[27], inter = d[28];
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return -1;
    if (comp != 0 || filt != 0 || inter != 0)
        return -2;
    int ch;
    switch (ct) {
    case 0: ch = 1; break;
    case 2: ch = 3; break;
    case 3: ch = 1; break;
    case 4: ch = 2; break;
    case 6: ch = 4; break;
    default: return -2;
    }
    if (ct == 3) {
        if (!(bits == 1 || bits == 2 || bits == 4 || bits == 8))
            return -2;
    } else if (bits != 8) {
        return -2;
    }

    PngAux aux;
    aux.nplte = 0;
    aux.have_trns = 0;
    uint8_t* idat = NULL;
    unsigned idat_used = 0, idat_cap = 0;
    unsigned _pos = 8;
    int got_idat = 0;
    for (;;) {
        if (_pos + 12 > n)
            goto fail;
        uint32_t len = px_rd32be(d + _pos);
        if (_pos + 12 + len > n)
            goto fail;
        const uint8_t* data = d + _pos + 8;
        if (px_rd32be(d + _pos + 4) == 0x49444154u) { /* IDAT */
            got_idat = 1;
            if (idat_used + len > idat_cap) {
                unsigned ncap = idat_cap ? idat_cap * 2 : 1024;
                while (ncap < idat_used + len)
                    ncap *= 2;
                uint8_t* nx = (uint8_t*)malloc(ncap);
                if (!nx)
                    goto fail;
                for (unsigned i = 0; i < idat_used; ++i)
                    nx[i] = idat[i];
                free(idat);
                idat = nx;
                idat_cap = ncap;
            }
            for (unsigned i = 0; i < len; ++i)
                idat[idat_used + i] = data[i];
            idat_used += len;
        } else if (px_rd32be(d + _pos + 4) == 0x504C5445u) { /* PLTE */
            unsigned cnt = len / 3;
            if (cnt > 256)
                goto fail;
            for (unsigned i = 0; i < cnt; ++i) {
                aux.plte[i][0] = data[i * 3];
                aux.plte[i][1] = data[i * 3 + 1];
                aux.plte[i][2] = data[i * 3 + 2];
            }
            aux.nplte = (int)cnt;
        } else if (px_rd32be(d + _pos + 4) == 0x74524E53u) { /* tRNS */
            aux.have_trns = 1;
            if (ct == 3) {
                for (unsigned i = 0; i < len && i < 256; ++i)
                    aux.trns[i] = data[i];
            } else if (ct == 0 && len >= 2) {
                aux.tr_gray = (uint16_t)((data[0] << 8) | data[1]);
            } else if (ct == 2 && len >= 6) {
                aux.tr_rgb[0] = (uint16_t)data[0];
                aux.tr_rgb[1] = (uint16_t)data[1];
                aux.tr_rgb[2] = (uint16_t)data[2];
            }
        } else if (px_rd32be(d + _pos + 4) == 0x49454E44u) { /* IEND */
            break;
        }
        _pos += 12 + len;
        if (_pos == n && got_idat)
            break;
    }
    if (!got_idat)
        goto fail;

    unsigned bitpp = (unsigned)bits * (unsigned)ch;
    unsigned rowbytes = ((unsigned)width * bitpp + 7u) / 8u;
    unsigned expected = (unsigned)height * (1u + rowbytes);
    uint8_t* raw = (uint8_t*)malloc(expected + 1);
    if (!raw)
        goto fail;
    if (idat_used < 4)
        goto fail;
    /* zlib wrapper: CMF/FLG header, then DEFLATE. */
    if (((idat[0] << 8) | idat[1]) % 31 != 0 || (idat[1] & 0x20))
        goto fail;
    unsigned dlen = 0;
    if (def_inflate(idat + 2, idat_used - 2, raw, expected + 1, &dlen) != 0 ||
        dlen != expected)
        goto fail;
    free(idat);
    idat = NULL;

    /* Unfilter in place. Predictors reference already-reconstructed
     * bytes of the current scanline (left) and the previous scanline,
     * which remains in place at row y-1. */
    {
        int bpp = (int)((bitpp + 7u) / 8u);
        unsigned stride = 1 + rowbytes;
        for (int y = 0; y < height; ++y) {
            uint8_t* line = raw + (unsigned)y * stride + 1;
            const uint8_t* up = y ? raw + (unsigned)(y - 1) * stride + 1 : NULL;
            uint8_t ft = raw[(unsigned)y * stride];
            if (ft > 4)
                goto fail;
            for (unsigned x = 0; x < rowbytes; ++x) {
                uint8_t leftv = (x >= (unsigned)bpp) ? line[x - (unsigned)bpp] : 0;
                uint8_t upv = up ? up[x] : 0;
                uint8_t ulv = (up && x >= (unsigned)bpp) ? up[x - (unsigned)bpp] : 0;
                uint8_t f = line[x];
                switch (ft) {
                case 0: line[x] = f; break;
                case 1: line[x] = (uint8_t)(f + leftv); break;
                case 2: line[x] = (uint8_t)(f + upv); break;
                case 3: line[x] = (uint8_t)(f + ((leftv + upv) >> 1)); break;
                default: line[x] = (uint8_t)(f + px_paeth(leftv, upv, ulv)); break;
                }
            }
        }
    }

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = raw + (unsigned)y * (1 + rowbytes) + 1;
        for (int x = 0; x < width; ++x) {
            int t = (int)ct;
            if (t == 0) {
                uint8_t v;
                if ((unsigned)bits >= 8) {
                    v = row[x * (unsigned)((unsigned)bits / 8u)];
                } else {
                    unsigned ppb = 8u / (unsigned)bits;
                    unsigned sh = 8u - (unsigned)bits * ((unsigned)(x % (int)ppb) + 1u);
                    v = (uint8_t)((row[x / (int)ppb] >> sh) & ((1u << bits) - 1u));
                    if ((1u << bits) - 1u != 255)
                        v = (uint8_t)(((unsigned)v * 255u) / ((1u << bits) - 1u));
                }
                px_px(rgb, x, y, width, v, v, v);
                continue;
            }
            if (t == 2) {
                const uint8_t* p = row + x * 3;
                px_px(rgb, x, y, width, p[0], p[1], p[2]);
                continue;
            }
            if (t == 4) {
                uint8_t v = row[x * 2];
                uint8_t a = row[x * 2 + 1];
                uint8_t c = px_compose(v, 255, a);
                px_px(rgb, x, y, width, c, c, c);
                continue;
            }
            if (t == 6) {
                uint8_t r = px_compose(row[x * 4], 255, row[x * 4 + 3]);
                uint8_t g = px_compose(row[x * 4 + 1], 255, row[x * 4 + 3]);
                uint8_t b = px_compose(row[x * 4 + 2], 255, row[x * 4 + 3]);
                px_px(rgb, x, y, width, r, g, b);
                continue;
            }
            /* palette */
            {
                unsigned ppp = 8u / bits;
                unsigned shift = 8u - (unsigned)bits * ((unsigned)(x % (int)ppp) + 1u);
                uint8_t idx = (uint8_t)((row[x / (int)ppp] >> shift) & ((1u << bits) - 1));
                if (idx >= (uint8_t)aux.nplte)
                    goto fail;
                if (aux.have_trns && aux.trns[idx] != 255) {
                    uint8_t a = (uint8_t)aux.trns[idx];
                    px_px(rgb, x, y, width,
                          px_compose((uint8_t)aux.plte[idx][0], 255, a),
                          px_compose((uint8_t)aux.plte[idx][1], 255, a),
                          px_compose((uint8_t)aux.plte[idx][2], 255, a));
                } else {
                    px_px(rgb, x, y, width,
                          (uint8_t)aux.plte[idx][0],
                          (uint8_t)aux.plte[idx][1],
                          (uint8_t)aux.plte[idx][2]);
                }
            }
        }
    }
    free(raw);
    *w = width;
    *h = height;
    return 0;

fail:
    if (idat)
        free(idat);
    return -3;
}

/* The scratch a decode of this PNG will ask the allocator for: the IDAT
 * stream as it is copied out of the chunks it arrives in - which grows, so
 * a caller that does not reclaim what it passed sees every size it grew
 * through - and the inflated rows, a filter byte plus the row's own bytes
 * each. A caller that owns the memory - the kernel's decode arena, an
 * embedder with a fixed budget - asks here first instead of discovering
 * the cost as an allocation failure. Returns 0 for a picture this decoder
 * will refuse anyway. */
static unsigned px_png_workspace(const uint8_t* d, unsigned n)
{
    if (n < 33 || !px_sig_png(d) || px_rd32be(d + 8) != 13 ||
        px_rd32be(d + 12) != 0x49484452u)
        return 0;
    const int width = (int)px_rd32be(d + 16);
    const int height = (int)px_rd32be(d + 20);
    const uint8_t bits = d[24], ct = d[25];
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return 0;
    if (d[26] != 0 || d[27] != 0 || d[28] != 0)
        return 0;
    int ch;
    switch (ct) {
    case 0: ch = 1; break;
    case 2: ch = 3; break;
    case 3: ch = 1; break;
    case 4: ch = 2; break;
    case 6: ch = 4; break;
    default: return 0;
    }
    if (ct == 3) {
        if (!(bits == 1 || bits == 2 || bits == 4 || bits == 8))
            return 0;
    } else if (bits != 8) {
        return 0;
    }

    /* The IDAT copy, sized the way the decoder sizes it: 1 KiB to start,
     * doubling until the chunks read so far fit. */
    unsigned total = 0, used = 0, cap = 0, pos = 8;
    int got = 0;
    for (;;) {
        if (pos + 12 > n)
            return 0;
        const uint32_t len = px_rd32be(d + pos);
        if (pos + 12 + len > n)
            return 0;
        if (px_rd32be(d + pos + 4) == 0x49444154u) { /* IDAT */
            got = 1;
            if (used + len > cap) {
                unsigned ncap = cap ? cap * 2u : 1024u;
                while (ncap < used + len) {
                    if (ncap > 0x7FFFFFFFu)
                        return 0;
                    ncap *= 2u;
                }
                total += (ncap + 15u) & ~15u;
                cap = ncap;
            }
            used += len;
        }
        pos += 12 + len;
        if (pos == n && got)
            break;
    }

    const unsigned rowbytes = ((unsigned)width * (unsigned)bits *
                               (unsigned)ch + 7u) / 8u;
    const unsigned raw = (unsigned)height * (1u + rowbytes) + 1u;
    return total + ((raw + 15u) & ~15u);
}

/* --- JPEG ------------------------------------------------------------- */

#define J_MAX_C 3

/* Scans in one picture. A baseline JPEG has exactly one; a progressive
 * one has a DC scan, then a pass per band and per approximation bit -
 * libjpeg's own default ceiling is 100 and a three-component picture with
 * every successive-approximation bit set uses a couple of dozen, so this
 * is a guard against a pathological stream rather than a tight fit. */
#define J_MAX_SCANS 64

typedef struct {
    uint8_t cnt[16];
    uint8_t sym[256];
} JHuff;

typedef struct {
    uint8_t id;
    uint8_t h, v;
    uint8_t q;
} JComp;

/* One scan: which components it interleaves (and in what order), the band
 * of coefficients it codes, the approximation bit it codes them at, and
 * where its entropy-coded segment is. The DC/AC table selectors are per
 * scan, not per component - that is what lets a progressive stream
 * change its Huffman tables between bands. */
typedef struct {
    int comp[J_MAX_C];
    int ncomp;
    int dc_tbl[J_MAX_C];
    int ac_tbl[J_MAX_C];
    int ss, se;      /* spectral selection, in Zig-Zag rank */
    int ah, al;      /* successive approximation: high and low bit */
    unsigned start;  /* the entropy-coded segment */
    unsigned n;
} JScan;

typedef struct {
    const uint8_t* d;
    unsigned n;
    unsigned pos;
    unsigned byte;
    int cnt;
    int stall;
} JBit;

/* Resolve 0xFF 0x00 stuffing and restart markers in the ECS.
 * Returns a byte value, -1 on stall/EOF, or -2 on a restart marker
 * (the caller must resynchronize; the restart bytes are consumed). */
static int j_byte(JBit* b)
{
    for (;;) {
        if (b->pos >= b->n) {
            b->stall = 1;
            return -1;
        }
        int c = b->d[b->pos++];
        if (c != 0xFF)
            return c;
        if (b->pos >= b->n) {
            b->stall = 1;
            return -1;
        }
        int m = b->d[b->pos];
        if (m == 0x00) {
            ++b->pos;
            return 0xFF;
        }
        if (m == 0xFF)
            continue;        /* byte stuffing fill */
        if (m >= 0xD0 && m <= 0xD7) {
            ++b->pos;
            return -2;       /* restart marker */
        }
        ++b->pos;            /* consume the marker ourselves */
        b->stall = 1;
        return -1;
    }
}

static int j_bit(JBit* b)
{
    for (;;) {
        if (b->cnt == 0) {
            int c = j_byte(b);
            if (c == -2) {
                /* restart marker: drop any partial byte, refill aligned */
                b->cnt = 0;
                continue;
            }
            if (c < 0)
                return -1;
            b->byte = (unsigned)c;
            b->cnt = 8;
        }
        --b->cnt;
        return (int)((b->byte >> b->cnt) & 1u);
    }
}

static int j_recv(JBit* b, int n)
{
    int v = 0;
    for (int i = 0; i < n; ++i) {
        int bit = j_bit(b);
        if (bit < 0)
            return -1;
        v = (v << 1) | bit;
    }
    return v;
}

static int j_huff(JBit* b, const JHuff* t)
{
    long code = 0;
    unsigned first = 0, index = 0;
    for (int l = 1; l <= 16; ++l) {
        int bit = j_bit(b);
        if (bit < 0)
            return -1;
        code = (code << 1) | bit;
        unsigned c = t->cnt[l - 1];
        if ((unsigned long)(code - first) < c)
            return t->sym[index + (unsigned)(code - first)];
        index += c;
        first = (first + c) << 1;
    }
    return -1;
}

static int j_ext(int v, int t)
{
    return v < (1 << (t - 1)) ? (v - (1 << t) + 1) : v;
}

static const float JP_COS[8][8] = {
    { 0x1.6a09e667f3bcdp-1, 0x1.6a09e667f3bcdp-1, 0x1.6a09e667f3bcdp-1, 0x1.6a09e667f3bcdp-1, 0x1.6a09e667f3bcdp-1, 0x1.6a09e667f3bcdp-1, 0x1.6a09e667f3bcdp-1, 0x1.6a09e667f3bcdp-1 },
    { 0x1.f6297cff75cb0p-1, 0x1.a9b66290ea1a3p-1, 0x1.1c73b39ae68c9p-1, 0x1.8f8b83c69a60dp-3, -0x1.8f8b83c69a608p-3, -0x1.1c73b39ae68c6p-1, -0x1.a9b66290ea1a4p-1, -0x1.f6297cff75cb0p-1 },
    { 0x1.d906bcf328d46p-1, 0x1.87de2a6aea964p-2, -0x1.87de2a6aea962p-2, -0x1.d906bcf328d46p-1, -0x1.d906bcf328d47p-1, -0x1.87de2a6aea96dp-2, 0x1.87de2a6aea967p-2, 0x1.d906bcf328d44p-1 },
    { 0x1.a9b66290ea1a3p-1, -0x1.8f8b83c69a608p-3, -0x1.f6297cff75cb0p-1, -0x1.1c73b39ae68c8p-1, 0x1.1c73b39ae68c5p-1, 0x1.f6297cff75cb0p-1, 0x1.8f8b83c69a61dp-3, -0x1.a9b66290ea1a2p-1 },
    { 0x1.6a09e667f3bcdp-1, -0x1.6a09e667f3bccp-1, -0x1.6a09e667f3bcep-1, 0x1.6a09e667f3bcbp-1, 0x1.6a09e667f3bcep-1, -0x1.6a09e667f3bc5p-1, -0x1.6a09e667f3bc9p-1, 0x1.6a09e667f3bc4p-1 },
    { 0x1.1c73b39ae68c9p-1, -0x1.f6297cff75cb0p-1, 0x1.8f8b83c69a60cp-3, 0x1.a9b66290ea1a6p-1, -0x1.a9b66290ea1a2p-1, -0x1.8f8b83c69a602p-3, 0x1.f6297cff75cb2p-1, -0x1.1c73b39ae68c2p-1 },
    { 0x1.87de2a6aea964p-2, -0x1.d906bcf328d47p-1, 0x1.d906bcf328d44p-1, -0x1.87de2a6aea965p-2, -0x1.87de2a6aea971p-2, 0x1.d906bcf328d46p-1, -0x1.d906bcf328d43p-1, 0x1.87de2a6aea95fp-2 },
    { 0x1.8f8b83c69a60dp-3, -0x1.1c73b39ae68c8p-1, 0x1.a9b66290ea1a6p-1, -0x1.f6297cff75cb2p-1, 0x1.f6297cff75cb1p-1, -0x1.a9b66290ea1a1p-1, 0x1.1c73b39ae68c2p-1, -0x1.8f8b83c69a616p-3 },
};

/* JPEG Zig-Zag reorder, as stored in the compressed scan: index is the
 * row-major lattice position (u*8+v), value is the zig-zag scan rank.
 * The scan ranks are taken under the transposed axis convention (lattice
 * slot (u,v) receives the coefficient whose standard rank is that of the
 * slot (v,u)); this yields the axis alignment used by the encoder. */
static const uint8_t JZZ[64] = {
    0, 2, 3, 9, 10, 20, 21, 35, 1, 4, 8, 11, 19, 22, 34, 36,
    5, 7, 12, 18, 23, 33, 37, 48, 6, 13, 17, 24, 32, 38, 47, 49,
    14, 16, 25, 31, 39, 46, 50, 57, 15, 26, 30, 40, 45, 51, 56, 58,
    27, 29, 41, 44, 52, 55, 59, 62, 28, 42, 43, 53, 54, 60, 61, 63
};

/* Separable float IDCT of a dequantized block; output is level-shifted
 * (+128) and clamped to 0..255. Coefficients arrive in Zig-Zag order. */
static inline void j_idct(const int* in, uint8_t* out, int stride)
{
    float t[8][8];
    for (int u = 0; u < 8; ++u)
        for (int y = 0; y < 8; ++y) {
            float s = 0.0f;
            for (int v = 0; v < 8; ++v)
                s += (float)in[JZZ[u * 8 + v]] * JP_COS[v][y];
            t[u][y] = s;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            float s = (t[0][y] * JP_COS[0][x]) * 0.25f;
            for (int u = 1; u < 8; ++u)
                s += (t[u][y] * JP_COS[u][x]) * 0.25f;
            int vv = 128 + (int)(s < 0.0f ? s - 0.5f : s + 0.5f);
            out[y * stride + x] = (uint8_t)(vv < 0 ? 0 : (vv > 255 ? 255 : vv));
        }
}

/* Scan JPEG markers for the frame header's dimensions. Every way of
 * writing a frame is accepted - SOF0 (baseline), SOF1 (extended
 * sequential) and SOF2 (progressive) - because which one it is says
 * nothing about the picture, only about how the coefficients arrive. */
static int jpg_dims(const uint8_t* d, unsigned n, int* w, int* h)
{
    unsigned pos = 2;
    while (pos + 4 <= n) {
        if (pos >= n || d[pos] != 0xFF)
            return -1;
        int m = d[pos + 1];
        pos += 2;
        if (m == 0xD9)
            return -1;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7))
            continue;
        if (pos + 2 > n)
            return -1;
        unsigned len = (unsigned)d[pos] * 256u + d[pos + 1];
        if (len < 2 || pos + len > n)
            return -1;
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            const uint8_t* b = d + pos + 2;
            if (b[0] != 8)
                return -1;
            *w = b[3] * 256 + b[4];
            *h = b[1] * 256 + b[2];
            return 0;
        }
        pos += len;
    }
    return -1;
}

/* Everything one scan needs: the coefficient planes it fills, the DC
 * predictors and end-of-band run it carries between its own blocks, and
 * the Huffman tables it was pointed at. */
typedef struct {
    const JHuff* dc_ht;
    const JHuff* ac_ht;
    int16_t* coeff[J_MAX_C];   /* Zig-Zag rank order, bstride[c] per row */
    size_t coff[J_MAX_C];       /* where its coefficients land */
    int bstride[J_MAX_C];      /* blocks per row, padded to whole MCUs */
    int bprow[J_MAX_C];        /* block rows, padded to whole MCUs */
    int bpl[J_MAX_C];          /* blocks per line the component really has */
    int brows[J_MAX_C];        /* block rows the component really has */
    int sw[J_MAX_C];           /* samples per row, padded to whole blocks */
    int sh[J_MAX_C];           /* sample rows, padded to whole blocks */
    size_t soff[J_MAX_C];      /* where the component's samples land */
    int dc_pred[J_MAX_C];      /* the last DC value, per component */
    int progressive;           /* the frame is SOF2: AC scans carry EOB runs */
    unsigned eobrun;           /* end-of-band run, AC scans only */
    unsigned mcuf;             /* MCUs into the current restart interval */
    int mcow, mcoh;            /* the MCU grid */
    int restart_interval;      /* MCUs per restart interval, 0 if none */
} JCtx;

/* One block of DC coefficients, first pass: a difference from the last
 * block's DC, placed at the low-bit position this scan codes it at. */
static int j_dc_first(JCtx* j, JBit* jb, const JScan* sc, int c, int16_t* blk)
{
    const int t = j_huff(jb, &j->dc_ht[sc->dc_tbl[c]]);
    if (t < 0)
        return -1;
    int diff = 0;
    if (t) {
        const int mv = j_recv(jb, t);
        if (mv < 0)
            return -1;
        diff = j_ext(mv, t);
    }
    j->dc_pred[c] += diff;
    blk[0] = (int16_t)(j->dc_pred[c] << sc->al);
    return 0;
}

/* DC refinement: one bit, which is the next bit of a DC value an earlier
 * scan already placed. OR rather than add, so a bit that shows up twice
 * still counts once. */
static int j_dc_refine(JCtx* j, JBit* jb, const JScan* sc, int c, int16_t* blk)
{
    (void)j;
    (void)c;
    const int bit = j_bit(jb);
    if (bit < 0)
        return -1;
    if (bit)
        blk[0] |= (int16_t)(1 << sc->al);
    return 0;
}

/* AC coefficients, first pass of a band: run-length coded pairs of "this
 * many zeroes, then a value this many bits wide", the value scaled into
 * position by this scan's low bit. An end-of-band code starts a run that
 * says how many blocks are all zeroes in this band, this one included,
 * which is why the run lives in the scan and not in the block. */
static int j_ac_first(JCtx* j, JBit* jb, const JScan* sc, int c, int16_t* blk)
{
    if (j->eobrun > 0) {
        --j->eobrun;
        return 0;
    }
    const JHuff* t = &j->ac_ht[sc->ac_tbl[c]];
    int k = sc->ss > 0 ? sc->ss : 1;
    while (k <= sc->se) {
        const int rs = j_huff(jb, t);
        if (rs < 0)
            return -1;
        const int r = rs >> 4, s = rs & 15;
        if (s) {
            k += r;
            if (k > sc->se)
                break;            /* past the band: the stream is wrong */
            const int mv = j_recv(jb, s);
            if (mv < 0)
                return -1;
            blk[k] = (int16_t)(j_ext(mv, s) << sc->al);
            ++k;
            continue;
        }
        if (r == 15) {
            k += 16;              /* sixteen zeroes, and no more bits */
            continue;
        }
        /* End of band: in a progressive frame the code says how many blocks
         * are empty in this band, the one being decoded now included, so the
         * run counts this block away right here; in a sequential frame it
         * only ends this block. */
        if (j->progressive) {
            j->eobrun = 1u << r;
            if (r) {
                const int mv = j_recv(jb, r);
                if (mv < 0)
                    return -1;
                j->eobrun += (unsigned)mv;
            }
            --j->eobrun;
        }
        break;
    }
    return 0;
}

/* AC coefficients, a refinement pass: no new values, only the next bit of
 * the ones earlier scans placed. A coefficient that was still zero turns
 * into +/- 1 in the coded bit position - a size-1 code says which sign -
 * while one that was non-zero gets a correction bit that can only make
 * its magnitude grow, so a zero bit leaves it exactly as it was. */
static int j_ac_refine(JCtx* j, JBit* jb, const JScan* sc, int c, int16_t* blk)
{
    const int p1 = 1 << sc->al;
    const int m1 = -(1 << sc->al);
    const JHuff* t = &j->ac_ht[sc->ac_tbl[c]];
    int k = sc->ss > 0 ? sc->ss : 1;
    int s = 0;

    if (j->eobrun == 0) {
        for (; k <= sc->se; ++k) {
            const int sym = j_huff(jb, t);
            if (sym < 0)
                return -1;
            int r = sym >> 4;
            s = sym & 15;
            if (s) {
                const int bit = j_bit(jb);
                if (bit < 0)
                    return -1;
                s = bit ? p1 : m1;  /* newly non-zero: this sign */
            } else if (r != 15) {
                if (j->progressive) {
                    j->eobrun = 1u << r;
                    if (r) {
                        const int mv = j_recv(jb, r);
                        if (mv < 0)
                            return -1;
                        j->eobrun += (unsigned)mv;
                    }
                }
                break;            /* the rest of the band is EOB logic */
            }
            /* Step over what this pair refers to: a correction bit for
             * every coefficient that is already non-zero, nothing at all
             * for every one that is still zero, until r of them are
             * behind us. */
            int step = r;
            while (k <= sc->se) {
                if (blk[k] != 0) {
                    const int bit = j_bit(jb);
                    if (bit < 0)
                        return -1;
                    if (bit && (blk[k] & p1) == 0)
                        blk[k] = (int16_t)(blk[k] +
                                           (blk[k] >= 0 ? p1 : m1));
                } else if (--step < 0) {
                    break;        /* reached the new non-zero coefficient */
                }
                ++k;
            }
            if (s && k <= sc->se)
                blk[k] = (int16_t)s;
        }
    }
    if (j->eobrun > 0) {
        /* What is left of the band is a correction bit for every
         * coefficient that is non-zero. */
        for (; k <= sc->se; ++k) {
            if (blk[k] == 0)
                continue;
            const int bit = j_bit(jb);
            if (bit < 0)
                return -1;
            if (bit && (blk[k] & p1) == 0)
                blk[k] = (int16_t)(blk[k] + (blk[k] >= 0 ? p1 : m1));
        }
        --j->eobrun;
    }
    return 0;
}

/* Decode one whole scan into the coefficient planes. A scan with several
 * components walks the picture in MCUs, taking each component's block
 * inside the MCU; a scan with one component walks that component's own
 * block grid, which is what the standard calls a non-interleaved scan. */
static int j_scan(JCtx* j, const JScan* sc, const JComp* comps,
                  const uint8_t* data)
{
    JBit jb;
    jb.d = data + sc->start;
    jb.n = sc->n;
    jb.pos = 0;
    jb.byte = 0;
    jb.cnt = 0;
    jb.stall = 0;

    j->eobrun = 0;
    j->mcuf = 0;
    /* A scan's band may be the DC only (a progressive DC pass), a band of
     * AC only (every progressive AC pass), or DC plus everything from 1
     * up - which is what the single scan of a baseline or extended
     * sequential picture carries. */
    const int want_dc = (sc->ss == 0);
    const int want_ac = (sc->se > 0);
    const int first = (sc->ah == 0);
    const int inter = sc->ncomp > 1;

    for (int mcu = 0;; ++mcu) {
        if (inter) {
            if (mcu >= j->mcow * j->mcoh)
                break;
        } else if (mcu >= j->brows[sc->comp[0]] * j->bpl[sc->comp[0]]) {
            break;
        }
        for (int i = 0; i < sc->ncomp; ++i) {
            const int c = sc->comp[i];
            const int nb = inter ? comps[c].h * comps[c].v : 1;
            for (int b = 0; b < nb; ++b) {
                int bx, by;
                if (inter) {
                    bx = (mcu % j->mcow) * comps[c].h + (b % comps[c].h);
                    by = (mcu / j->mcow) * comps[c].v + (b / comps[c].h);
                } else {
                    bx = mcu % j->bpl[c];
                    by = mcu / j->bpl[c];
                }
                /* Block addressing counts blocks of 64 coefficients, not
                 * coefficients: the plane's row stride is in blocks. */
                int16_t* blk = j->coeff[c] +
                               (size_t)by * (size_t)j->bstride[c] * 64u +
                               (size_t)bx * 64u;
                int rc = 0;
                if (want_dc)
                    rc = first ? j_dc_first(j, &jb, sc, c, blk)
                               : j_dc_refine(j, &jb, sc, c, blk);
                if (rc == 0 && want_ac)
                    rc = first ? j_ac_first(j, &jb, sc, c, blk)
                               : j_ac_refine(j, &jb, sc, c, blk);
                if (rc < 0) {
                    /* A progressive scan may simply stop short: the blocks
                     * left over are empty in this band, and an encoder is
                     * free not to write them out. Running out of bits there
                     * ends the scan; in a sequential picture, where every
                     * block of every scan is coded, it means a truncated
                     * file. */
                    if (j->progressive && jb.stall)
                        return 0;
                    return -1;
                }
            }
        }
        if (j->restart_interval && ++j->mcuf ==
                (unsigned)j->restart_interval) {
            /* A restart interval starts a new DC prediction and a new
             * end-of-band run. */
            for (int c = 0; c < J_MAX_C; ++c)
                j->dc_pred[c] = 0;
            j->eobrun = 0;
            j->mcuf = 0;
        }
    }
    return 0;
}

/* Where a scan's entropy-coded segment ends: the next marker that is not
 * a stuffed 0xFF00 or a restart interval's RSTn. */
static unsigned j_ecs_end(const uint8_t* d, unsigned n, unsigned start)
{
    unsigned q = start;
    while (q < n) {
        if (d[q] != 0xFF) {
            ++q;
            continue;
        }
        if (q + 1 >= n)
            break;
        const uint8_t m = d[q + 1];
        if (m == 0x00 || m == 0xFF || (m >= 0xD0 && m <= 0xD7)) {
            q += 2;
            continue;
        }
        break;
    }
    return q;
}

/* The peak scratch a decode of this JPEG will ask the allocator for: the
 * coefficients of every component at once (a progressive picture cannot
 * be transformed until its last scan has been read), plus one sample plane
 * per component - the decode allocates them together, before it
 * transforms anything. A caller that owns the memory - the kernel's
 * decode arena, an embedder with a fixed budget - asks here first instead
 * of discovering the cost as an allocation failure. Returns 0 for a
 * picture this decoder will refuse anyway. */
static unsigned px_jpg_workspace(const uint8_t* d, unsigned n)
{
    int w = 0, h = 0;
    if (jpg_dims(d, n, &w, &h) != 0 || w <= 0 || h <= 0)
        return 0;
    unsigned pos = 2, hmax = 1, vmax = 1;
    JComp comps[J_MAX_C];
    int ncomp = 0;
    while (pos + 4 <= n && d[pos] == 0xFF) {
        const int m = d[pos + 1];
        pos += 2;
        if (m == 0xD9)
            break;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7))
            continue;
        if (pos + 2 > n)
            return 0;
        const unsigned len = (unsigned)d[pos] * 256u + d[pos + 1];
        if (len < 2 || pos + len > n)
            return 0;
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            const uint8_t* b = d + pos + 2;
            ncomp = b[5];
            if (ncomp != 1 && ncomp != 3)
                return 0;
            for (int c = 0; c < ncomp; ++c) {
                const uint8_t* p = b + 6 + c * 3;
                comps[c].id = p[0];
                comps[c].h = p[1] >> 4;
                comps[c].v = p[1] & 15;
                comps[c].q = p[2];
                if (comps[c].h < 1 || comps[c].h > 2 ||
                    comps[c].v < 1 || comps[c].v > 2)
                    return 0;
                if (comps[c].h > hmax) hmax = comps[c].h;
                if (comps[c].v > vmax) vmax = comps[c].v;
            }
        }
        pos += len;
    }
    if (!ncomp)
        return 0;
    const unsigned mcow = ((unsigned)w + 8u * hmax - 1u) / (8u * hmax);
    const unsigned mcoh = ((unsigned)h + 8u * vmax - 1u) / (8u * vmax);
    unsigned long coeff = 0, planes = 0;
    for (int c = 0; c < ncomp; ++c) {
        const unsigned bcw = mcow * comps[c].h, bch = mcoh * comps[c].v;
        coeff += (unsigned long)bcw * bch * 128ul;      /* 64 * int16 */
        const unsigned sw = bcw * 8u, sh = bch * 8u;
        planes += (unsigned long)sw * sh;
    }
    /* One allocation holds every coefficient array and a second holds
     * every sample plane, both taken before anything is transformed, so
     * the peak is the sum of the two and not the larger of them. Both
     * are what an allocator is asked for, rounded up to the 16 bytes it
     * hands out - the picture a caller with a fixed budget has to have. */
    return (unsigned)(((coeff + 15ul) & ~15ul) + ((planes + 15ul) & ~15ul));
}

/* --- JPEG ------------------------------------------------------------- */

/* --- dispatcher ------------------------------------------------------- */

/* Decode a baseline or progressive JPEG into RGB888. Baseline is the
 * degenerate case of the same code: one scan, DC band plus all of AC,
 * everything at approximation bit zero. The coefficients of every
 * component are collected first - a progressive picture cannot be
 * transformed until its last scan has been read - then dequantized and
 * inverse-transformed one component at a time, then upsampled.
 * 0 on success, -1 malformed, -2 unsupported, -3 out of entropy data,
 * -4 out of memory. */
static int px_jpg_decode(const uint8_t* d, unsigned n, int* out_w, int* out_h,
                         uint8_t* rgb)
{
    uint16_t qtab[4][64];
    JHuff dc_ht[4], ac_ht[4];
    JComp comps[J_MAX_C];
    JCtx jc;
    int nscans = 0, ncomp = 0, w = 0, h = 0, dri = 0, framed = 0;
    int hmax = 1, vmax = 1, progressive = 0;
    int16_t* coeff = NULL;
    uint8_t* planes = NULL;
    size_t coff = 0, soff = 0;

    jc.dc_ht = dc_ht;
    jc.ac_ht = ac_ht;
    jc.eobrun = 0;
    for (int c = 0; c < J_MAX_C; ++c) {
        jc.dc_pred[c] = 0;
        jc.coeff[c] = NULL;
    }

    unsigned pos = 2;
    while (pos + 4 <= n && d[pos] == 0xFF) {
        const int m = d[pos + 1];
        pos += 2;
        if (m == 0xD9)
            break;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7))
            continue;
        if (pos + 2 > n)
            return -1;
        const unsigned len = (unsigned)d[pos] * 256u + d[pos + 1];
        if (len < 2 || pos + len > n)
            return -1;
        const uint8_t* seg = d + pos + 2;   /* the length field is behind us */
        const unsigned plen = len - 2;      /* and so is its size */
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {         /* SOF0/1/2 */
            if (framed || seg[0] != 8 || plen < 6)
                return -2;
            framed = 1;
            progressive = (m == 0xC2);
            h = seg[1] * 256 + seg[2];
            w = seg[3] * 256 + seg[4];
            ncomp = seg[5];
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
                return -2;
            if (ncomp != 1 && ncomp != 3)
                return -2;
            if (plen < 6u + (unsigned)ncomp * 3u)
                return -1;
            for (int c = 0; c < ncomp; ++c) {
                const uint8_t* p = seg + 6 + c * 3;
                comps[c].id = p[0];
                comps[c].h = p[1] >> 4;
                comps[c].v = p[1] & 15;
                comps[c].q = p[2];
                if (comps[c].h < 1 || comps[c].h > 2 ||
                    comps[c].v < 1 || comps[c].v > 2 || comps[c].q > 3)
                    return -2;
                if (comps[c].h > hmax) hmax = comps[c].h;
                if (comps[c].v > vmax) vmax = comps[c].v;
            }
        } else if (m == 0xDB) {                             /* DQT */
            unsigned p = 0;
            while (p < plen) {
                const int pq = seg[p] >> 4, tq = seg[p] & 15;
                ++p;
                if (tq > 3 || pq > 1)
                    return -1;
                for (int i = 0; i < 64; ++i) {
                    if (pq) {
                        if (p + 2 > plen)
                            return -1;
                        qtab[tq][i] = (uint16_t)(seg[p] * 256 + seg[p + 1]);
                        p += 2;
                    } else {
                        if (p >= plen)
                            return -1;
                        qtab[tq][i] = seg[p++];
                    }
                }
            }
        } else if (m == 0xC4) {                             /* DHT */
            unsigned p = 0;
            while (p + 17 <= plen) {
                const int tc = seg[p] >> 4, th = seg[p] & 15;
                ++p;
                if (tc > 1 || th > 3)
                    return -1;
                JHuff* t = tc ? ac_ht : dc_ht;
                unsigned total = 0;
                for (int i = 0; i < 16; ++i) {
                    t[th].cnt[i] = seg[p + i];
                    total += t[th].cnt[i];
                }
                p += 16;
                if (total > 256 || p + total > plen)
                    return -1;
                for (unsigned i = 0; i < total; ++i)
                    t[th].sym[i] = seg[p + i];
                p += total;
            }
        } else if (m == 0xDD) {                             /* DRI */
            if (plen < 2)
                return -1;
            dri = seg[0] * 256 + seg[1];
        } else if (m == 0xDA) {                             /* SOS */
            if (!framed || nscans >= J_MAX_SCANS)
                return -2;
            const unsigned ns = seg[0];
            if (ns == 0 || ns > (unsigned)ncomp || plen < ns * 2u + 3u)
                return -1;
            JScan sc;
            sc.ncomp = (int)ns;
            for (unsigned i = 0; i < ns; ++i) {
                const uint8_t* p = seg + 1 + i * 2;
                int c = -1;
                for (int k = 0; k < ncomp; ++k)
                    if (comps[k].id == p[0])
                        c = k;
                if (c < 0)
                    return -1;
                sc.comp[i] = c;
                sc.dc_tbl[c] = p[1] >> 4;
                sc.ac_tbl[c] = p[1] & 15;
            }
            const uint8_t* p = seg + 1 + ns * 2;
            sc.ss = p[0];
            sc.se = p[1];
            sc.ah = p[2] >> 4;
            sc.al = p[2] & 15;
            if (sc.ss > 63 || sc.se > 63 || sc.ss > sc.se ||
                sc.ah > 13 || sc.al > 13)
                return -2;
            sc.start = pos + len;
            sc.n = j_ecs_end(d, n, sc.start) - sc.start;
            if (!nscans) {
                /* The first scan sizes the working set, which then lives
                 * until the picture is finished: a progressive picture
                 * cannot be transformed before its last scan. */
                jc.progressive = progressive;
                jc.mcow = (w + 8 * hmax - 1) / (8 * hmax);
                jc.mcoh = (h + 8 * vmax - 1) / (8 * vmax);
                for (int c = 0; c < ncomp; ++c) {
                    const int cw = (w * comps[c].h + hmax - 1) / hmax;
                    const int ch = (h * comps[c].v + vmax - 1) / vmax;
                    jc.bstride[c] = jc.mcow * comps[c].h;
                    jc.bprow[c] = jc.mcoh * comps[c].v;
                    jc.bpl[c] = (cw + 7) / 8;
                    jc.brows[c] = (ch + 7) / 8;
                    jc.sw[c] = jc.bstride[c] * 8;
                    jc.sh[c] = jc.bprow[c] * 8;
                    jc.coff[c] = coff;
                    jc.soff[c] = soff;
                    coff += (size_t)jc.bstride[c] * (size_t)jc.bprow[c] * 128u;
                    soff += (size_t)jc.sw[c] * (size_t)jc.sh[c];
                }
                coeff = (int16_t*)malloc(coff);
                planes = (uint8_t*)malloc(soff);
                if (!coeff || !planes) {
                    free(coeff);
                    free(planes);
                    return -4;
                }
                for (size_t i = 0; i < coff; i += 2u)
                    coeff[i / 2u] = 0;
                for (int c = 0; c < ncomp; ++c)
                    jc.coeff[c] = coeff + jc.coff[c] / 2u;
            }
            /* A scan is decoded the moment its header has been read: the
             * Huffman tables a progressive file redefines between scans
             * would be lost if the whole marker stream were read first. */
            jc.restart_interval = dri;
            if (sc.ss == 0 && sc.ah == 0)
                for (int c = 0; c < J_MAX_C; ++c)
                    jc.dc_pred[c] = 0;
            if (j_scan(&jc, &sc, comps, d) < 0) {
                free(coeff);
                free(planes);
                return -3;
            }
            ++nscans;
            pos = sc.start + sc.n;
            continue;           /* the next bytes are a marker, not a seg */
        }
        pos += len;
    }
    if (!framed || !nscans)
        return -1;

    /* Dequantize, inverse-transform and level-shift each component into
     * its own sample plane. */
    for (int c = 0; c < ncomp; ++c) {
        uint8_t* out = planes + jc.soff[c];
        const int16_t* src = jc.coeff[c];
        for (int by = 0; by < jc.bprow[c]; ++by) {
            for (int bx = 0; bx < jc.bstride[c]; ++bx) {
                int blk[64];
                for (int i = 0; i < 64; ++i)
                    blk[i] = src[i] * qtab[comps[c].q][i];
                j_idct(blk, out + (size_t)by * 8u * jc.sw[c] + (size_t)bx * 8u,
                       jc.sw[c]);
                src += 64;
            }
        }
    }
    free(coeff);

    /* Upsample each component to the picture (nearest, hmax/hc) and
     * turn the result into RGB: one component is gray, three are YCbCr. */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int s[3] = { 0, 0, 0 };
            for (int c = 0; c < ncomp; ++c) {
                const int sx = x * comps[c].h / hmax;
                const int sy = y * comps[c].v / vmax;
                s[c] = planes[jc.soff[c] + (size_t)sy * jc.sw[c] + sx];
            }
            if (ncomp == 1) {
                const int v = s[0] < 0 ? 0 : (s[0] > 255 ? 255 : s[0]);
                px_px(rgb, x, y, w, (uint8_t)v, (uint8_t)v, (uint8_t)v);
                continue;
            }
            const int yy = s[0] - 128, cb = s[1] - 128, cr = s[2] - 128;
            int r = yy + (int)(1.402f * cr) + 128;
            int g = yy - (int)(0.344136f * cb) - (int)(0.714136f * cr) + 128;
            int b = yy + (int)(1.772f * cb) + 128;
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            px_px(rgb, x, y, w, (uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
    free(planes);
    *out_w = w;
    *out_h = h;
    return 0;
}

static int px_probe(const uint8_t* d, unsigned n, int* w, int* h)
{
    *w = 0;
    *h = 0;
    if (n < 4)
        return PX_NONE;
    if (d[0] == 'B' && d[1] == 'M') {
        if (n >= 26) {
            int width = (int)px_rd32(d + 18);
            int hraw = (int)px_rd32(d + 22);
            if (hraw < 0)
                hraw = -hraw;
            if (width > 0 && hraw > 0 && width < 4096 && hraw < 4096) {
                *w = width;
                *h = hraw;
                return PX_BMP;
            }
        }
        return PX_NONE;
    }
    if (px_sig_png(d)) {
        if (n >= 29) {
            int width = (int)px_rd32be(d + 16);
            int height = (int)px_rd32be(d + 20);
            if (width > 0 && height > 0 && width < 4096 && height < 4096) {
                *w = width;
                *h = height;
                return PX_PNG;
            }
        }
        return PX_NONE;
    }
    if (d[0] == 0xFF && d[1] == 0xD8) {
        if (jpg_dims(d, n, w, h) == 0 && *w > 0 && *h > 0)
            return PX_JPG;
        return PX_NONE;
    }
    return PX_NONE;
}

static inline int px_decode(int fmt, const uint8_t* d, unsigned n,
                     int w, int h, uint8_t* rgb)
{
    int dw = 0, dh = 0;
    int rc;
    switch (fmt) {
    case PX_BMP: rc = px_bmp_decode(d, n, &dw, &dh, rgb); break;
    case PX_PNG: rc = px_png_decode(d, n, &dw, &dh, rgb); break;
    case PX_JPG: rc = px_jpg_decode(d, n, &dw, &dh, rgb); break;
    default: return -1;
    }
    if (rc != 0 || dw != w || dh != h)
        return rc < 0 ? rc : -1;
    return 0;
}

#endif /* VNU_PX_H */