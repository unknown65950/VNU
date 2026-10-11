#pragma once
#ifndef VNU_USTAR_H
#define VNU_USTAR_H

/* ustar.h — a self-contained POSIX ustar (tar) reader.
 *
 * Reads an uncompressed POSIX ustar archive straight out of a memory
 * buffer: no allocation, no syscalls and no library beyond <stdint.h>
 * and <stddef.h>, so this same header drops into the freestanding
 * kernel, a VNU userspace program or a host tool and compiles as C or
 * C++. The kernel uses it to unpack the initial ramdisk the boot loader
 * hands over as a Multiboot2 module (see proc/initrd.cpp); that archive
 * is built with `tar --format=ustar`, so only the POSIX fields are read
 * - the ustar `prefix` field joins `name` for paths over 100 bytes, and
 * PAX/GNU extension headers are skipped rather than interpreted.
 */

#include <stddef.h>
#include <stdint.h>

/* One archive member. `name` is NUL-terminated and already stripped of
 * a leading "./" or "/"; for a directory the trailing slash is gone
 * too. `data` points into the archive and is valid for `size` bytes
 * only for a regular file; it is NULL for a directory. */
struct vnu_ustar_entry {
    char name[256];
    const uint8_t* data;
    uint64_t size;
    uint32_t mode;
    char type; /* POSIX typeflag: '0' regular, '5' directory */
};

/* Parse a numeric tar field: `n` bytes, leading spaces/zeros, octal,
 * NUL/space terminated. A GNU base-256 field (high bit of the first
 * byte set) is accepted as well, so a GNU-format archive still reads.
 * Returns 0 and sets *out, or -1 when the field is not numeric. */
static inline int vnu_ustar_octal(const uint8_t* p, int n, uint64_t* out)
{
    int i = 0;
    if (n <= 0)
        return -1;
    if (p[0] & 0x80) {
        uint64_t v = (uint64_t)(p[0] & 0x7F);
        for (i = 1; i < n; ++i)
            v = (v << 8) | (uint64_t)p[i];
        *out = v;
        return 0;
    }
    while (i < n && (p[i] == ' ' || p[i] == '0'))
        ++i;
    uint64_t v = 0;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i)
        v = (v << 3) | (uint64_t)(p[i] - '0');
    for (; i < n; ++i)
        if (p[i] != ' ' && p[i] != '\0')
            return -1;
    *out = v;
    return 0;
}

/* The header checksum covers all 512 bytes with the checksum field
 * itself read as eight spaces. */
static inline int vnu_ustar_checksum_ok(const uint8_t* h)
{
    uint64_t want = 0;
    if (vnu_ustar_octal(h + 148, 8, &want) != 0)
        return 0;
    uint32_t sum = 0;
    for (int i = 0; i < 512; ++i) {
        if (i >= 148 && i < 156)
            sum += (uint8_t)' ';
        else
            sum += h[i];
    }
    return sum == (uint32_t)want;
}

/* Join the prefix (offset 345) and name (offset 0) fields, then drop a
 * leading "./" and any leading '/'. Returns the length written. */
static inline size_t vnu_ustar_path(const uint8_t* h, char* out, size_t cap)
{
    char tmp[260];
    size_t n = 0;
    size_t pl = 0;
    while (pl < 155 && h[345 + pl] != 0)
        ++pl;
    if (pl) {
        for (size_t i = 0; i < pl && n + 1 < sizeof(tmp); ++i)
            tmp[n++] = (char)h[345 + i];
        if (n + 1 < sizeof(tmp))
            tmp[n++] = '/';
    }
    for (size_t i = 0; i < 100 && h[i] != 0; ++i)
        if (n + 1 < sizeof(tmp))
            tmp[n++] = (char)h[i];
    tmp[n] = 0;

    size_t b = 0;
    for (;;) {
        if (tmp[b] == '.' && tmp[b + 1] == '/') {
            b += 2;
            continue;
        }
        if (tmp[b] == '/')
            ++b;
        else
            break;
    }
    size_t m = 0;
    while (tmp[b] != 0 && m + 1 < cap) {
        out[m++] = tmp[b++];
    }
    out[m] = 0;
    return m;
}

/* Read the member starting at byte *offset. Returns 1 with *out filled
 * for a regular file or a directory, 0 at the end of the archive, or -1
 * when a header is malformed. Symlinks, devices and PAX/GNU extension
 * headers are skipped, advancing *offset past them. */
static inline int vnu_ustar_next(const void* image, uint64_t image_size,
                                 uint64_t* offset, struct vnu_ustar_entry* out)
{
    const uint8_t* p = (const uint8_t*)image;
    while (*offset + 512 <= image_size) {
        const uint8_t* h = p + *offset;
        int zero = 1;
        for (int i = 0; i < 512; ++i)
            if (h[i]) {
                zero = 0;
                break;
            }
        if (zero)
            return 0; /* the end-of-archive marker */
        if (h[257] != 'u' || h[258] != 's' || h[259] != 't' ||
            h[260] != 'a' || h[261] != 'r')
            return -1;
        if (!vnu_ustar_checksum_ok(h))
            return -1;

        uint64_t size = 0;
        if (vnu_ustar_octal(h + 124, 12, &size) != 0)
            return -1;
        uint64_t mode = 0;
        vnu_ustar_octal(h + 100, 8, &mode);
        uint8_t type = h[156];
        if (type == 0)
            type = '0';

        uint64_t data_off = *offset + 512;
        *offset = data_off + ((size + 511) & ~(uint64_t)511);
        if (data_off + size > image_size || *offset > image_size)
            return -1;

        if (type == 'x' || type == 'g' || type == 'L' || type == 'K')
            continue; /* extension header: its data belongs to nobody here */
        if (type != '0' && type != '5')
            continue; /* symlink, hardlink, device, fifo: nothing to unpack */

        size_t len = vnu_ustar_path(h, out->name, sizeof(out->name));
        if (type == '5')
            while (len > 0 && out->name[len - 1] == '/')
                out->name[--len] = 0;
        out->size = size;
        out->mode = (uint32_t)mode;
        out->type = (char)type;
        out->data = (type == '0') ? (p + data_off) : (const uint8_t*)0;
        return 1;
    }
    return 0;
}

#endif /* VNU_USTAR_H */
