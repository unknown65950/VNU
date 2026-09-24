/*
 * px.h — self-contained image decoders for picview.
 *
 * Decodes BMP (24/32-bit BI_RGB), PNG (8-bit, non-interlaced; color
 * types 0/2/3/4/6 with tRNS) and baseline JPEG (SOF0, 1 or 3
 * components, 4:4:4 / 4:2:2 / 4:2:0 subsampling) straight into an
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

/* --- JPEG ------------------------------------------------------------- */

#define J_MAX_C 3

typedef struct {
    uint8_t cnt[16];
    uint8_t sym[256];
} JHuff;

typedef struct {
    uint8_t id;
    uint8_t h, v;
    uint8_t q;
    uint8_t dc_sel, ac_sel;
} JComp;

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
static inline void j_idct(const int* in, uint8_t* out)
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
            out[y * 8 + x] = (uint8_t)(vv < 0 ? 0 : (vv > 255 ? 255 : vv));
        }
}

/* Scan JPEG markers looking for SOF0 dimensions. */
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
        if (m == 0xC0) {
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

static inline int px_jpg_decode(const uint8_t* d, unsigned n,
                         int* w, int* h, uint8_t* rgb)
{
    int qtab[4][64];
    JHuff ht[8];
    int have_ht[8] = { 0 };
    JComp comps[J_MAX_C];
    int comp_order[J_MAX_C];
    int ncomp = 0;
    int width = 0, height = 0;
    int hmax = 1, vmax = 1;
    unsigned ecs_start = 0, ecs_n = 0;
    int restart_interval = 0;
    int have_dri = 0;

    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return -1;

    unsigned pos = 2;
    int phase = 0; /* 0=segments, 1=after SOS */
    for (;;) {
        if (pos >= n || d[pos] != 0xFF)
            return -1;
        int m = d[pos + 1];
        pos += 2;
        if (m == 0xD9)
            break;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7))
            continue;
        if (pos + 2 > n)
            return -1;
        unsigned len = (unsigned)d[pos] * 256u + d[pos + 1];
        const uint8_t* body = d + pos + 2;
        if (len < 2 || pos + len > n)
            return -1;
        if (m == 0xC0) {
            if (body[0] != 8)
                return -2;
            height = body[1] * 256 + body[2];
            width = body[3] * 256 + body[4];
            ncomp = body[5];
            if (width <= 0 || height <= 0 || (ncomp != 1 && ncomp != 3))
                return -2;
            for (int c = 0; c < ncomp; ++c) {
                const uint8_t* p = body + 6 + c * 3;
                comps[c].id = p[0];
                comps[c].h = p[1] >> 4;
                comps[c].v = p[1] & 15;
                comps[c].q = p[2];
                if (comps[c].h < 1 || comps[c].h > 2 ||
                    comps[c].v < 1 || comps[c].v > 2)
                    return -2;
                if (comps[c].h > hmax) hmax = comps[c].h;
                if (comps[c].v > vmax) vmax = comps[c].v;
            }
        } else if (m == 0xC4) {
            unsigned p = 0;
            while (p + 1 < len - 2) {
                uint8_t ci = body[p];
                int cls = ci >> 4, id = ci & 15;
                int slot = cls * 4 + id;
                if (slot < 0 || slot >= 8)
                    return -2;
                unsigned csum = 0;
                for (int i = 0; i < 16; ++i) {
                    ht[slot].cnt[i] = body[p + 1 + i];
                    csum += ht[slot].cnt[i];
                }
                p += 17;
                if (p + csum > len - 2)
                    return -1;
                unsigned idx = 0;
                for (int i = 0; i < 16; ++i)
                    for (unsigned k = 0; k < ht[slot].cnt[i]; ++k) {
                        ht[slot].sym[idx] = body[p + idx];
                        ++idx;
                    }
                p += csum;
                have_ht[slot] = 1;
            }
        } else if (m == 0xDB) {
            unsigned p = 0;
            while (p + 1 < len - 2) {
                int prec = body[p] >> 4;
                int id = body[p] & 15;
                if (id >= 4)
                    return -2;
                p += 1;
                if (p + 64 + (prec ? 64 : 0) > len - 2)
                    return -1;
                if (prec == 0) {
                    for (int i = 0; i < 64; ++i)
                        qtab[id][i] = body[p + i];
                    p += 64;
                } else {
                    for (int i = 0; i < 64; ++i)
                        qtab[id][i] = body[p + i * 2] * 256 + body[p + i * 2 + 1];
                    p += 128;
                }
            }
        } else if (m == 0xDD) {
            have_dri = 1;
            restart_interval = body[0] * 256 + body[1];
        } else if (m == 0xDA) {
            /* scan: interleaved baseline only */
            unsigned ns = body[0];
            unsigned p = 1;
            if (ns != (unsigned)ncomp)
                return -2;
            for (unsigned c = 0; c < ns; ++c) {
                if (p + 2 > len - 2)
                    return -1;
                uint8_t cid = body[p];
                uint8_t sels = body[p + 1];
                p += 2;
                int found = -1;
                for (int k = 0; k < ncomp; ++k)
                    if (comps[k].id == cid)
                        found = k;
                if (found < 0)
                    return -2;
                comps[found].dc_sel = sels >> 4;
                comps[found].ac_sel = sels & 15;
                comp_order[c] = found;
            }
            if (p + 2 > len - 2)
                return -1;
            if (body[p] != 0 || body[p + 1] != 63 || body[p + 2] != 0)
                return -2; /* progressive / spectral selection unsupported */
            ecs_start = pos + len;
            /* find end of the entropy-coded segment */
            unsigned q = ecs_start;
            while (q < n) {
                if (d[q] == 0xFF) {
                    if (q + 1 >= n)
                        break;
                    uint8_t m2 = d[q + 1];
                    if (m2 == 0x00 || m2 == 0xFF) {
                        q += 2;
                        continue;
                    }
                    if (m2 >= 0xD0 && m2 <= 0xD7) {
                        q += 2;
                        continue;
                    }
                    break;
                }
                ++q;
            }
            ecs_n = q - ecs_start;
            phase = 1;
            break;
        }
        pos += len;
    }
    (void)phase;
    if (!(ncomp == 1 || ncomp == 3) || ecs_n == 0)
        return -1;
    if (ncomp == 3 && (comps[0].h != hmax || comps[0].v != vmax))
        return -2; /* Y must be max-sampled */

    for (int c = 0; c < ncomp; ++c) {
        int dc = comps[c].dc_sel, ac = comps[c].ac_sel;
        if (!have_ht[dc] || !have_ht[4 + ac])
            return -1;
    }

    /* Planes: MCU-grid aligned per component. */
    unsigned mcow = ((unsigned)width + 8u * hmax - 1) / (8u * hmax);
    unsigned mcoh = ((unsigned)height + 8u * vmax - 1) / (8u * vmax);
    int16_t* planes[J_MAX_C];
    int stride[J_MAX_C], prow[J_MAX_C];
    unsigned long total = 0;
    for (int c = 0; c < ncomp; ++c) {
        unsigned sw = mcow * 8u * comps[c].h;
        unsigned sh = mcoh * 8u * comps[c].v;
        stride[c] = (int)sw;
        prow[c] = (int)sh;
        total += (unsigned long)sw * sh * 2ul;
    }
    if (total > 900000ul)
        return -2;
    for (int c = 0; c < ncomp; ++c) {
        planes[c] = (int16_t*)malloc((unsigned)stride[c] * (unsigned)prow[c] * 2u);
        if (!planes[c]) {
            for (int k = 0; k < c; ++k)
                free(planes[k]);
            return -4;
        }
    }

    JBit jb;
    jb.d = d + ecs_start;
    jb.n = ecs_n;
    jb.pos = 0;
    jb.byte = 0;
    jb.cnt = 0;
    jb.stall = 0;

    int dc_pred[J_MAX_C] = { 0 };
    int mcuf = 0;

    for (unsigned my = 0; my < mcoh; ++my) {
        for (unsigned mx = 0; mx < mcow; ++mx) {
            if (have_dri && restart_interval &&
                (int)(my * mcow + mx) > 0 &&
                (my * mcow + mx) % (unsigned)restart_interval == 0) {
                for (int c = 0; c < ncomp; ++c)
                    dc_pred[c] = 0;
            }
            (void)mcuf;
            ++mcuf;
            for (int ci = 0; ci < ncomp; ++ci) {
                int c = ci;
                for (unsigned byc = 0; byc < comps[c].v; ++byc) {
                    for (unsigned bxc = 0; bxc < comps[c].h; ++bxc) {
                        int block[64] = { 0 };
                        int dcsel = comps[c].dc_sel, acsel = comps[c].ac_sel;
                        int t = j_huff(&jb, &ht[dcsel]);
                        if (t < 0)
                            goto fail;
                        if (t > 0) {
                            int mv = j_recv(&jb, t);
                            if (mv < 0)
                                goto fail;
                            int diff = j_ext(mv, t);
                            dc_pred[c] += diff;
                        }
                        block[0] = dc_pred[c] * qtab[comps[c].q][0];
                        int k = 1;
                        for (;;) {
                            if (k >= 64)
                                break;   /* block complete; no EOB needed */
                            int rs = j_huff(&jb, &ht[4 + acsel]);
                            if (rs < 0)
                                goto fail;
                            int r = rs >> 4, s = rs & 15;
                            if (s == 0) {
                                if (r == 15) {
                                    k += 16;
                                    if (k > 64)
                                        goto fail;
                                    continue;
                                }
                                for (; k < 64; ++k)
                                    block[k] = 0;
                                break;
                            }
                            k += r;
                            if (k > 64)
                                goto fail;
                            int mv = j_recv(&jb, s);
                            if (mv < 0)
                                goto fail;
                            block[k] = j_ext(mv, s) * qtab[comps[c].q][k];
                            ++k;
                        }
                        uint8_t o[64];
                        j_idct(block, o);
                        unsigned bx = mx * comps[c].h + bxc;
                        unsigned by = my * comps[c].v + byc;
                        for (int yy2 = 0; yy2 < 8; ++yy2) {
                            int16_t* line = planes[c] + (by * 8u + yy2) * (unsigned)stride[c];
                            for (int xx2 = 0; xx2 < 8; ++xx2)
                                line[bx * 8u + xx2] = (int16_t)o[yy2 * 8 + xx2];
                        }
                    }
                }
            }
        }
    }

    /* Upsample (nearest ×hmax/hc) and convert to RGB. */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (ncomp == 1) {
                int sx = x / hmax, sy = y / vmax;
                int16_t v0 = planes[0][sy * stride[0] + sx];
                uint8_t v = (uint8_t)(v0 < 0 ? 0 : (v0 > 255 ? 255 : v0));
                px_px(rgb, x, y, width, v, v, v);
                continue;
            }
            int sx0 = x / (hmax / comps[0].h), sy0 = y / (vmax / comps[0].v);
            int sx1 = x / (hmax / comps[1].h), sy1 = y / (vmax / comps[1].v);
            int sx2 = x / (hmax / comps[2].h), sy2 = y / (vmax / comps[2].v);
            int Y0 = planes[0][sy0 * stride[0] + sx0];
            int Cb0 = planes[1][sy1 * stride[1] + sx1];
            int Cr0 = planes[2][sy2 * stride[2] + sx2];
            int yy = Y0 - 128, cb = Cb0 - 128, cr = Cr0 - 128;
            int r = yy + (int)(1.402f * cr) + 128;
            int g = yy - (int)(0.344136f * cb) - (int)(0.714136f * cr) + 128;
            int b = yy + (int)(1.772f * cb) + 128;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            px_px(rgb, x, y, width, (uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
    for (int c = 0; c < ncomp; ++c)
        free(planes[c]);
    *w = width;
    *h = height;
    return 0;

fail:
    for (int c = 0; c < ncomp; ++c)
        free(planes[c]);
    return -3;
}

/* --- dispatcher ------------------------------------------------------- */

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