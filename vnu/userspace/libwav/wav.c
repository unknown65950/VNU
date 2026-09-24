/*
 * wav — self-contained RIFF/WAVE PCM parser.
 *
 * A tiny, dependency-free library for reading uncompressed .wav files.
 * It accepts a file image in memory (as read from a file or embedded
 * blob) and extracts the PCM payload plus its parameters. Only P͟C͟M
 * (format tag 1) is supported — the format every .wav creator writes —
 * but unknown RIFF chunks between `fmt ` and `data` are skipped, so
 * files with LIST/INFO metadata still parse.
 *
 * POSIX-friendly: plain C89-ish C, no OS dependencies, works anywhere
 * a Unix program can get a buffer of bytes. The header and this
 * implementation are standalone: compile wav.c into your program.
 *
 * Returns 0 on success with *out filled; a negative code otherwise:
 *   -1  not a RIFF/WAVE file
 *   -2  not PCM (format tag != 1)
 *   -3  unsupported parameters (rate outside 8..48 kHz, channels not
 *       1/2, bits not 8/16) — the VNU audio layer cannot play those
 *   -4  no `data` chunk
 */
#include "wav.h"

static uint32_t rd16(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_id(const unsigned char* p, char a, char b, char c, char d)
{
    return p[0] == a && p[1] == b && p[2] == c && p[3] == d;
}

int wav_parse(const void* buf, size_t len, wav_info* out)
{
    const unsigned char* p = (const unsigned char*)buf;
    if (!p || !out || len < 12)
        return -1;
    if (!is_id(p, 'R', 'I', 'F', 'F') || !is_id(p + 8, 'W', 'A', 'V', 'E'))
        return -1;

    uint32_t rate = 0, dlen = 0;
    uint16_t ch = 0, bits = 0;
    const unsigned char* data = 0;
    int fmt_ok = 0, data_ok = 0;

    size_t off = 12; /* the first chunk header starts right past 'WAVE' */
    while (off + 8 <= len) {
        const unsigned char* id = p + off;
        uint32_t sz = rd32(p + off + 4);
        size_t body = off + 8;
        if (body + sz > len) /* tolerate a truncated tail chunk */
            sz = (uint32_t)(len - body);

        if (is_id(id, 'f', 'm', 't', ' ')) {
            /* fmt chunk: format tag (2), channels (2), rate (4), byte
             * rate (4), block align (2), bits (2) [+ extensible tail]. */
            if (sz >= 16) {
                uint32_t tag = rd16(p + body);
                ch = (uint16_t)rd16(p + body + 2);
                rate = rd32(p + body + 4);
                bits = (uint16_t)rd16(p + body + 14);
                fmt_ok = 1;
                if (tag != 1)
                    return -2;
            }
        } else if (is_id(id, 'd', 'a', 't', 'a')) {
            data = p + body;
            dlen = sz;
            data_ok = 1;
            /* The data chunk is (almost) always last; stopping here
             * saves a pass over the tail padding. */
            break;
        }
        /* any other chunk (LIST, fact, ...) is skipped */

        off = body + sz + (sz & 1u); /* chunks are word-aligned */
    }

    if (!fmt_ok)
        return -1;
    if (!data_ok || dlen == 0)
        return -4;
    if (rate < 8000 || rate > 48000)
        return -3;
    if (ch != 1 && ch != 2)
        return -3;
    if (bits != 8 && bits != 16)
        return -3;

    out->sample_rate = rate;
    out->channels = ch;
    out->bits_per_sample = bits;
    out->data = data;
    out->data_len = dlen;
    return 0;
}