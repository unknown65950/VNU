#include <vnu/fat16.h>

// FAT16 read/write implementation. See the header for the on-disk layout
// and the device contract. The file is freestanding (no <string.h>, no
// STL, no allocation) so it builds as kernel code and as an ordinary
// host program alike.

namespace vnu::fat16 {
namespace {

constexpr uint32_t NUM_FATS = 2;
constexpr uint32_t RESERVED = 1;
constexpr uint32_t ROOT_ENTRIES = 512;
constexpr uint16_t FAT_FREE = 0x0000;
constexpr uint16_t FAT_EOC = 0xFFFF;
constexpr uint16_t FAT_BAD = 0xFFF7;
constexpr uint32_t FAT16_MIN_CLUSTERS = 4085;
constexpr uint32_t FAT16_MAX_CLUSTER = 0xFFF4;
constexpr uint32_t MAX_LFN = 20;      /* 20 * 13 = 260 >= 255 */
constexpr uint32_t MAX_ENTRIES = MAX_LFN + 1; /* LFN entries + the 8.3 one */

uint16_t rd16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void wr16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void wr32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t slen(const char* s)
{
    uint32_t n = 0;
    while (s && s[n])
        ++n;
    return n;
}

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }

bool ci_equal(const char* a, const char* b)
{
    while (*a && *b) {
        if (upper(*a) != upper(*b))
            return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

bool sfn_equal(const uint8_t* a, const uint8_t* b)
{
    for (int i = 0; i < 11; ++i)
        if (upper(static_cast<char>(a[i])) != upper(static_cast<char>(b[i])))
            return false;
    return true;
}

/* The characters FAT allows in an 8.3 name. Anything else forces a
 * mangled short name plus an LFN entry carrying the real name. */
bool sfn_char_ok(char c)
{
    if (c < 0x20)
        return false;
    switch (c) {
    case ' ': case '"': case '*': case '+': case ',': case '/': case ':':
    case ';': case '<': case '=': case '>': case '?': case '[': case '\\':
    case ']': case '|':
        return false;
    default:
        return true;
    }
}

/* --- device I/O, always through the caller's sector buffer --- */

bool dev_read(Fs& fs, uint32_t rel, uint32_t count, void* buf)
{
    if (!fs.dev.read)
        return false;
    return fs.dev.read(fs.dev.ctx, fs.part_lba + rel, count, buf) == 0;
}

bool dev_write(Fs& fs, uint32_t rel, uint32_t count, const void* buf)
{
    if (!fs.dev.write)
        return false;
    return fs.dev.write(fs.dev.ctx, fs.part_lba + rel, count, buf) == 0;
}

uint32_t cluster_to_sector(Fs& fs, uint32_t cluster)
{
    return fs.first_data + (cluster - 2) * fs.spc;
}

bool fat_get(Fs& fs, uint32_t cluster, uint16_t* out)
{
    uint32_t off = cluster * 2;
    if (!dev_read(fs, fs.first_fat + off / SECTOR, 1, fs.sec))
        return false;
    *out = rd16(fs.sec + off % SECTOR);
    return true;
}

bool fat_set(Fs& fs, uint32_t cluster, uint16_t value)
{
    uint32_t off = cluster * 2;
    for (uint32_t copy = 0; copy < fs.num_fats; ++copy) {
        uint32_t sec = fs.first_fat + copy * fs.fatsz + off / SECTOR;
        if (!dev_read(fs, sec, 1, fs.sec))
            return false;
        wr16(fs.sec + off % SECTOR, value);
        if (!dev_write(fs, sec, 1, fs.sec))
            return false;
    }
    return true;
}

/* The cluster after `cluster`, or 0 at the end of a chain. */
bool fat_next(Fs& fs, uint32_t cluster, uint32_t* out)
{
    uint16_t v;
    if (!fat_get(fs, cluster, &v))
        return false;
    if (v == 0 || v == FAT_BAD || (v >= 0xFFF8))
        *out = 0;
    else
        *out = v;
    return true;
}

bool chain_len(Fs& fs, uint32_t first, uint32_t* out)
{
    uint32_t n = 0;
    uint32_t c = first;
    while (c >= 2 && c <= fs.max_cluster) {
        ++n;
        if (n > fs.max_cluster + 2)
            return false;
        uint32_t nx;
        if (!fat_next(fs, c, &nx))
            return false;
        if (nx == 0)
            break;
        c = nx;
    }
    *out = n;
    return true;
}

/* The n-th cluster of a chain (0 = the first). */
bool chain_at(Fs& fs, uint32_t first, uint32_t n, uint32_t* out)
{
    uint32_t c = first;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t nx;
        if (!fat_next(fs, c, &nx) || nx == 0) {
            *out = 0;
            return false;
        }
        c = nx;
    }
    *out = c;
    return c != 0;
}

bool zero_sectors(Fs& fs, uint32_t rel, uint32_t count)
{
    for (uint32_t i = 0; i < SECTOR; ++i)
        fs.sec[i] = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!dev_write(fs, rel + i, 1, fs.sec))
            return false;
    return true;
}

bool zero_cluster(Fs& fs, uint32_t cluster)
{
    return zero_sectors(fs, cluster_to_sector(fs, cluster), fs.spc);
}

bool free_chain(Fs& fs, uint32_t first)
{
    uint32_t c = first;
    uint32_t guard = 0;
    while (c >= 2 && c <= fs.max_cluster) {
        if (++guard > fs.max_cluster + 2)
            return false;
        uint32_t nx;
        if (!fat_next(fs, c, &nx))
            return false;
        if (!fat_set(fs, c, FAT_FREE))
            return false;
        c = nx;
    }
    return true;
}

/* Allocate one free cluster, link it after `prev` (when nonzero) and
 * zero it. Returns false when the volume is full. */
bool cluster_alloc(Fs& fs, uint32_t prev, uint32_t* out)
{
    uint32_t found = 0;
    for (uint32_t sec = 0; sec < fs.fatsz && !found; ++sec) {
        if (!dev_read(fs, fs.first_fat + sec, 1, fs.sec))
            return false;
        for (uint32_t i = 0; i < SECTOR / 2; ++i) {
            uint32_t cluster = sec * (SECTOR / 2) + i;
            if (cluster < 2 || cluster > fs.max_cluster)
                continue;
            if (fs.sec[i * 2] == 0 && fs.sec[i * 2 + 1] == 0) {
                found = cluster;
                break;
            }
        }
    }
    if (!found)
        return false;
    if (!fat_set(fs, found, FAT_EOC))
        return false;
    if (prev && !fat_set(fs, prev, static_cast<uint16_t>(found)))
        return false;
    if (!zero_cluster(fs, found))
        return false;
    *out = found;
    return true;
}

/* Grow a chain to `need` clusters (when `need` is 0 the file has no
 * clusters at all). New clusters come back zeroed. */
bool ensure_chain(Fs& fs, uint32_t first, uint32_t need, uint32_t* out)
{
    if (need == 0) {
        *out = first;
        return true;
    }
    uint32_t len = 0;
    if (first)
        if (!chain_len(fs, first, &len))
            return false;
    if (len >= need) {
        *out = first;
        return true;
    }
    uint32_t last = first;
    if (len == 0) {
        uint32_t nc;
        if (!cluster_alloc(fs, 0, &nc))
            return false;
        first = nc;
        last = nc;
        len = 1;
    } else if (!chain_at(fs, first, len - 1, &last)) {
        return false;
    }
    while (len < need) {
        uint32_t nc;
        if (!cluster_alloc(fs, last, &nc))
            return false;
        last = nc;
        ++len;
    }
    *out = first;
    return true;
}

/* Write `len` bytes (or zeros, when `src` is null) starting at byte
 * `off` of a file whose first cluster is `first`. */
bool range_write(Fs& fs, uint32_t first, uint32_t off, const uint8_t* src, uint32_t len)
{
    uint32_t cb = fs.spc * SECTOR;
    uint32_t clu;
    if (!chain_at(fs, first, off / cb, &clu) || clu == 0)
        return false;
    uint32_t within = off % cb;
    uint32_t done = 0;
    while (done < len) {
        uint32_t sec = within / SECTOR;
        uint32_t so = within % SECTOR;
        uint32_t chunk = SECTOR - so;
        if (chunk > len - done)
            chunk = len - done;
        uint32_t abs = cluster_to_sector(fs, clu) + sec;
        if (so == 0 && chunk == SECTOR) {
            uint8_t zeros[SECTOR];
            const uint8_t* p = src ? src + done : zeros;
            if (!src)
                for (uint32_t i = 0; i < SECTOR; ++i)
                    zeros[i] = 0;
            if (!dev_write(fs, abs, 1, p))
                return false;
        } else {
            if (!dev_read(fs, abs, 1, fs.sec))
                return false;
            for (uint32_t i = 0; i < chunk; ++i)
                fs.sec[so + i] = src ? src[done + i] : 0;
            if (!dev_write(fs, abs, 1, fs.sec))
                return false;
        }
        done += chunk;
        within += chunk;
        if (within >= cb && done < len) {
            within -= cb;
            uint32_t nx;
            if (!fat_next(fs, clu, &nx) || nx == 0)
                return false;
            clu = nx;
        }
    }
    return true;
}

/* --- directory scanning: 8.3 short names and VFAT long names --- */

uint8_t sfn_checksum(const uint8_t* sfn)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i)
        sum = static_cast<uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
    return sum;
}

void build_sfn_name(const uint8_t* e, char* out)
{
    char base[9];
    int bl = 0;
    for (int i = 0; i < 8; ++i) {
        uint8_t c = e[i];
        if (c == ' ')
            break;
        if (i == 0 && c == 0x05)
            c = 0xE5;
        char ch = static_cast<char>(c);
        if (e[12] & 0x08)
            ch = static_cast<char>(is_upper(ch) ? ch - 'A' + 'a' : ch);
        base[bl++] = ch;
    }
    base[bl] = 0;
    char ext[4];
    int el = 0;
    for (int i = 0; i < 3; ++i) {
        uint8_t c = e[8 + i];
        if (c == ' ')
            break;
        char ch = static_cast<char>(c);
        if (e[12] & 0x10)
            ch = static_cast<char>(is_upper(ch) ? ch - 'A' + 'a' : ch);
        ext[el++] = ch;
    }
    ext[el] = 0;
    int o = 0;
    for (int i = 0; i < bl; ++i)
        out[o++] = base[i];
    if (el) {
        out[o++] = '.';
        for (int i = 0; i < el; ++i)
            out[o++] = ext[i];
    }
    out[o] = 0;
}

struct Entry {
    uint32_t pos;      /* byte offset of the short entry in the directory */
    uint32_t cluster;
    uint32_t size;
    uint8_t attr;
    uint8_t sfn[11];
    char name[NAME_MAX];
    uint32_t lfn_pos[MAX_LFN];
    uint32_t lfn_n;
};

uint32_t dir_total_bytes(Fs& fs, uint32_t dir_cluster)
{
    if (dir_cluster == 0)
        return fs.root_dir_sectors * SECTOR;
    uint32_t len = 0;
    if (!chain_len(fs, dir_cluster, &len))
        return 0;
    return len * fs.spc * SECTOR;
}

bool dir_read_sector(Fs& fs, uint32_t dir_cluster, uint32_t sec_idx, uint8_t* buf)
{
    if (dir_cluster == 0)
        return dev_read(fs, fs.first_root + sec_idx, 1, buf);
    uint32_t clu;
    if (!chain_at(fs, dir_cluster, sec_idx / fs.spc, &clu) || clu == 0)
        return false;
    return dev_read(fs, cluster_to_sector(fs, clu) + sec_idx % fs.spc, 1, buf);
}

bool dir_write_sector(Fs& fs, uint32_t dir_cluster, uint32_t sec_idx, const uint8_t* buf)
{
    if (dir_cluster == 0)
        return dev_write(fs, fs.first_root + sec_idx, 1, buf);
    uint32_t clu;
    if (!chain_at(fs, dir_cluster, sec_idx / fs.spc, &clu) || clu == 0)
        return false;
    return dev_write(fs, cluster_to_sector(fs, clu) + sec_idx % fs.spc, 1, buf);
}

/* Next live entry at or after `pos`, advancing `pos` past it. LFN
 * entries are folded into the name; "." / ".." / the volume label come
 * back like any other entry. */
bool scan_next(Fs& fs, uint32_t dir_cluster, uint32_t& pos, uint32_t total, Entry* ev)
{
    char lfn[NAME_MAX];
    uint32_t lfn_pos[MAX_LFN];
    uint32_t lfn_n = 0;
    uint32_t max_seq = 0;
    uint8_t lfn_chk = 0;
    for (uint32_t i = 0; i < NAME_MAX; ++i)
        lfn[i] = 0;

    while (pos + 32 <= total) {
        uint32_t sec = pos / SECTOR;
        uint32_t off = pos % SECTOR;
        if (!dir_read_sector(fs, dir_cluster, sec, fs.sec))
            return false;
        const uint8_t* e = fs.sec + off;
        uint32_t here = pos;
        pos += 32;

        if (e[0] == 0x00)
            return false;
        if (e[0] == 0xE5) {
            lfn_n = 0;
            max_seq = 0;
            continue;
        }
        if ((e[11] & 0x3F) == 0x0F) {
            uint8_t seq = static_cast<uint8_t>(e[0] & 0x1F);
            uint8_t chk = e[13];
            if (lfn_n == 0) {
                lfn_chk = chk;
                max_seq = 0;
                for (uint32_t i = 0; i < NAME_MAX; ++i)
                    lfn[i] = 0;
            }
            if (chk != lfn_chk || seq == 0 || seq > MAX_LFN || lfn_n >= MAX_LFN) {
                lfn_n = 0;
                max_seq = 0;
                continue;
            }
            static const uint8_t charoff[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            uint32_t at = (seq - 1) * 13;
            for (int k = 0; k < 13; ++k)
                if (at + k < NAME_MAX)
                    lfn[at + k] = static_cast<char>(e[charoff[k]]);
            lfn_pos[lfn_n++] = here;
            if (seq > max_seq)
                max_seq = seq;
            continue;
        }

        for (int i = 0; i < 11; ++i)
            ev->sfn[i] = e[i];
        ev->pos = here;
        ev->cluster = static_cast<uint32_t>(rd16(e + 26));
        ev->size = rd32(e + 28);
        ev->attr = static_cast<uint8_t>(e[11] & 0x3F);
        ev->lfn_n = 0;
        if (lfn_n > 0 && lfn_chk == sfn_checksum(e)) {
            uint32_t len = max_seq * 13;
            if (len > NAME_MAX - 1)
                len = NAME_MAX - 1;
            uint32_t real = 0;
            while (real < len && lfn[real])
                ++real;
            for (uint32_t i = 0; i < real; ++i)
                ev->name[i] = lfn[i];
            ev->name[real] = 0;
            for (uint32_t i = 0; i < lfn_n; ++i)
                ev->lfn_pos[i] = lfn_pos[i];
            ev->lfn_n = lfn_n;
        } else {
            build_sfn_name(e, ev->name);
        }
        return true;
    }
    return false;
}

bool find_entry(Fs& fs, uint32_t dir_cluster, const char* name, Entry* out)
{
    uint32_t total = dir_total_bytes(fs, dir_cluster);
    if (total == 0)
        return false;
    uint32_t pos = 0;
    Entry ev;
    while (scan_next(fs, dir_cluster, pos, total, &ev)) {
        if (ev.attr & ATTR_VOLUME)
            continue;
        if (ci_equal(ev.name, name)) {
            *out = ev;
            return true;
        }
    }
    return false;
}

bool dir_has_sfn(Fs& fs, uint32_t dir_cluster, const uint8_t sfn[11])
{
    uint32_t total = dir_total_bytes(fs, dir_cluster);
    uint32_t pos = 0;
    Entry ev;
    while (scan_next(fs, dir_cluster, pos, total, &ev)) {
        if (ev.attr & ATTR_VOLUME)
            continue;
        if (sfn_equal(ev.sfn, sfn))
            return true;
    }
    return false;
}

/* --- building new directory entries --- */

struct Short {
    uint8_t base[8];
    uint8_t ext[3];
    uint8_t nt;
    bool fits;     /* representable as 8.3 at all */
    bool uniform;  /* one case per part, so the NT bits are enough */
};

void build_short(const char* in, Short* s)
{
    for (int i = 0; i < 8; ++i)
        s->base[i] = ' ';
    for (int i = 0; i < 3; ++i)
        s->ext[i] = ' ';
    s->nt = 0;
    s->fits = true;
    s->uniform = true;

    int dot = -1;
    for (int i = 0; in[i]; ++i)
        if (in[i] == '.')
            dot = i;
    int blen = (dot < 0) ? static_cast<int>(slen(in)) : dot;
    const char* ext = (dot < 0) ? "" : in + dot + 1;

    bool base_lower = false, base_upper = false, base_seen = false;
    int o = 0;
    for (int i = 0; i < blen; ++i) {
        char c = in[i];
        if (is_lower(c)) { base_lower = true; base_seen = true; }
        else if (is_upper(c)) { base_upper = true; base_seen = true; }
        if (!sfn_char_ok(c)) { s->fits = false; c = '_'; }
        if (o < 8) s->base[o++] = upper(c);
        else s->fits = false;
    }
    if (blen == 0)
        s->fits = false;
    if (base_seen) {
        if (base_lower && !base_upper) s->nt |= 0x08;
        else if (base_lower && base_upper) s->uniform = false;
    }

    bool ext_lower = false, ext_upper = false, ext_seen = false;
    o = 0;
    for (int i = 0; ext[i]; ++i) {
        char c = ext[i];
        if (is_lower(c)) { ext_lower = true; ext_seen = true; }
        else if (is_upper(c)) { ext_upper = true; ext_seen = true; }
        if (!sfn_char_ok(c)) { s->fits = false; c = '_'; }
        if (o < 3) s->ext[o++] = upper(c);
        else s->fits = false;
    }
    if (ext_seen) {
        if (ext_lower && !ext_upper) s->nt |= 0x10;
        else if (ext_lower && ext_upper) s->uniform = false;
    }
}

/* A unique mangled short name for a name that does not fit 8.3. */
void make_unique_sfn(Fs& fs, uint32_t dir_cluster, const Short& s, uint8_t out[11])
{
    /* The base is trimmed so that "~N" fits before the 8th column. */
    for (uint32_t n = 1; n < 1000000u; ++n) {
        char tail[9];
        int tl = 0;
        tail[tl++] = '~';
        char digs[8];
        int dl = 0;
        uint32_t m = n;
        do {
            digs[dl++] = static_cast<char>('0' + m % 10);
            m /= 10;
        } while (m);
        for (int k = dl - 1; k >= 0; --k)
            tail[tl++] = digs[k];
        tail[tl] = 0;

        uint8_t cand[11];
        for (int i = 0; i < 11; ++i)
            cand[i] = ' ';
        int keep = 8 - tl; /* characters of the base before the tail */
        if (keep < 1)
            keep = 1;
        int co = 0;
        for (int i = 0; i < 8 && co < keep; ++i) {
            if (s.base[i] == ' ')
                break;
            cand[co++] = s.base[i];
        }
        if (co == 0)
            cand[co++] = 'F';
        for (int i = 0; i < tl && co < 8; ++i)
            cand[co++] = static_cast<uint8_t>(tail[i]);
        for (int i = 0; i < 3; ++i)
            cand[8 + i] = s.ext[i];
        if (!dir_has_sfn(fs, dir_cluster, cand)) {
            for (int i = 0; i < 11; ++i)
                out[i] = cand[i];
            return;
        }
    }
    for (int i = 0; i < 8; ++i)
        out[i] = s.base[i];
    for (int i = 0; i < 3; ++i)
        out[8 + i] = s.ext[i];
}

/* Build the on-disk entry (or entries, when an LFN is needed) for a new
 * name. `out` must hold MAX_ENTRIES * 32 bytes; `*count` gets the number
 * of entries built (LFN first, the 8.3 one last). */
bool build_entries(Fs& fs, uint32_t dir_cluster, const char* name, uint8_t attr,
                   uint32_t cluster, uint32_t size, uint8_t* out, uint32_t* count)
{
    Short s;
    build_short(name, &s);
    bool need_lfn = !s.fits || !s.uniform;
    uint8_t sfn[11];
    if (need_lfn) {
        make_unique_sfn(fs, dir_cluster, s, sfn);
    } else {
        for (int i = 0; i < 8; ++i)
            sfn[i] = s.base[i];
        for (int i = 0; i < 3; ++i)
            sfn[8 + i] = s.ext[i];
    }

    uint32_t n = 0;
    if (need_lfn) {
        uint32_t len = slen(name);
        uint32_t chunks = (len + 12) / 13;
        if (chunks == 0)
            chunks = 1;
        if (chunks > MAX_LFN)
            return false;
        uint8_t chk = sfn_checksum(sfn);
        static const uint8_t off13[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (uint32_t c = 0; c < chunks; ++c) {
            uint32_t seq = chunks - c; /* disk order: highest sequence first */
            uint8_t* e = out + n * 32;
            for (int i = 0; i < 32; ++i)
                e[i] = 0;
            e[0] = static_cast<uint8_t>(seq | (c == 0 ? 0x40 : 0));
            e[11] = 0x0F;
            e[13] = chk;
            for (uint32_t k = 0; k < 13; ++k) {
                uint32_t g = (seq - 1) * 13 + k;
                uint16_t u;
                if (g < len)
                    u = static_cast<uint8_t>(name[g]);
                else if (g == len)
                    u = 0x0000;
                else
                    u = 0xFFFF;
                e[off13[k]] = static_cast<uint8_t>(u & 0xFF);
                e[off13[k] + 1] = static_cast<uint8_t>(u >> 8);
            }
            ++n;
        }
    }

    uint8_t* e = out + n * 32;
    for (int i = 0; i < 32; ++i)
        e[i] = 0;
    for (int i = 0; i < 11; ++i)
        e[i] = sfn[i];
    e[11] = attr;
    e[12] = need_lfn ? 0 : s.nt;
    e[26] = static_cast<uint8_t>(cluster & 0xFF);
    e[27] = static_cast<uint8_t>((cluster >> 8) & 0xFF);
    wr32(e + 28, size);
    ++n;
    *count = n;
    return true;
}

bool write_entry_at(Fs& fs, uint32_t dir_cluster, uint32_t pos, const uint8_t* e)
{
    uint32_t sec = pos / SECTOR;
    uint32_t off = pos % SECTOR;
    if (!dir_read_sector(fs, dir_cluster, sec, fs.sec))
        return false;
    for (int i = 0; i < 32; ++i)
        fs.sec[off + i] = e[i];
    return dir_write_sector(fs, dir_cluster, sec, fs.sec);
}

bool mark_deleted(Fs& fs, uint32_t dir_cluster, uint32_t pos)
{
    uint32_t sec = pos / SECTOR;
    uint32_t off = pos % SECTOR;
    if (!dir_read_sector(fs, dir_cluster, sec, fs.sec))
        return false;
    fs.sec[off] = 0xE5;
    return dir_write_sector(fs, dir_cluster, sec, fs.sec);
}

/* Append `count` 32-byte entries at the end of a directory, extending a
 * cluster-backed directory by one cluster when it is full. */
bool dir_append(Fs& fs, uint32_t dir_cluster, const uint8_t* entries, uint32_t count)
{
    uint32_t total = dir_total_bytes(fs, dir_cluster);
    if (total == 0)
        return false;
    uint32_t pos = 0;
    while (pos + 32 <= total) {
        uint32_t sec = pos / SECTOR;
        uint32_t off = pos % SECTOR;
        if (!dir_read_sector(fs, dir_cluster, sec, fs.sec))
            return false;
        if (fs.sec[off] == 0x00)
            break;
        pos += 32;
    }
    if (pos + 32 > total) {
        if (dir_cluster == 0)
            return false;
        uint32_t len = 0;
        if (!chain_len(fs, dir_cluster, &len) || len == 0)
            return false;
        uint32_t last;
        if (!chain_at(fs, dir_cluster, len - 1, &last))
            return false;
        uint32_t nc;
        if (!cluster_alloc(fs, last, &nc))
            return false;
        total += fs.spc * SECTOR;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 32 > total)
            return false;
        if (!write_entry_at(fs, dir_cluster, pos, entries + i * 32))
            return false;
        pos += 32;
    }
    if (pos + 32 <= total) {
        uint32_t sec = pos / SECTOR;
        uint32_t off = pos % SECTOR;
        if (!dir_read_sector(fs, dir_cluster, sec, fs.sec))
            return false;
        fs.sec[off] = 0x00;
        if (!dir_write_sector(fs, dir_cluster, sec, fs.sec))
            return false;
    }
    return true;
}

void update_entry(Fs& fs, uint32_t dir_cluster, uint32_t pos, uint32_t cluster, uint32_t size)
{
    uint32_t sec = pos / SECTOR;
    uint32_t off = pos % SECTOR;
    if (!dir_read_sector(fs, dir_cluster, sec, fs.sec))
        return;
    fs.sec[off + 20] = 0;
    fs.sec[off + 21] = 0;
    fs.sec[off + 26] = static_cast<uint8_t>(cluster & 0xFF);
    fs.sec[off + 27] = static_cast<uint8_t>((cluster >> 8) & 0xFF);
    wr32(fs.sec + off + 28, size);
    dir_write_sector(fs, dir_cluster, sec, fs.sec);
}

bool init_subdir(Fs& fs, uint32_t clu, uint32_t parent)
{
    uint32_t sec = cluster_to_sector(fs, clu);
    if (!dev_read(fs, sec, 1, fs.sec))
        return false;
    for (int i = 0; i < 32; ++i)
        fs.sec[i] = 0;
    fs.sec[0] = '.';
    for (int i = 1; i < 11; ++i)
        fs.sec[i] = ' ';
    fs.sec[11] = ATTR_DIR;
    fs.sec[26] = static_cast<uint8_t>(clu & 0xFF);
    fs.sec[27] = static_cast<uint8_t>((clu >> 8) & 0xFF);
    for (int i = 0; i < 32; ++i)
        fs.sec[32 + i] = 0;
    fs.sec[32] = '.';
    fs.sec[33] = '.';
    for (int i = 2; i < 11; ++i)
        fs.sec[32 + i] = ' ';
    fs.sec[32 + 11] = ATTR_DIR;
    fs.sec[32 + 26] = static_cast<uint8_t>(parent & 0xFF);
    fs.sec[32 + 27] = static_cast<uint8_t>((parent >> 8) & 0xFF);
    return dev_write(fs, sec, 1, fs.sec);
}

void copy_padded(char* dst, uint32_t cap, const char* src)
{
    for (uint32_t i = 0; i < cap; ++i)
        dst[i] = ' ';
    for (uint32_t i = 0; src && src[i] && i < cap; ++i)
        dst[i] = src[i];
}

} // namespace

/* Sum the free clusters by walking the FAT one sector at a time (a
 * sector holds 256 entries, so re-reading it for every cluster in it is
 * avoided by caching which sector is loaded). */
bool volume_stats(Fs& fs, uint32_t* out_total, uint32_t* out_free)
{
    if (!fs.mounted || fs.spc == 0 || fs.max_cluster < 2)
        return false;
    const uint32_t cluster_bytes = fs.spc * SECTOR;
    uint32_t free_clusters = 0;
    uint32_t loaded = 0xFFFFFFFFu;
    for (uint32_t c = 2; c <= fs.max_cluster; ++c) {
        const uint32_t off = c * 2;
        const uint32_t sec = off / SECTOR;
        if (sec != loaded) {
            if (!dev_read(fs, fs.first_fat + sec, 1, fs.sec))
                return false;
            loaded = sec;
        }
        if (rd16(fs.sec + off % SECTOR) == 0)
            ++free_clusters;
    }
    *out_total = (fs.max_cluster - 1) * cluster_bytes;
    *out_free = free_clusters * cluster_bytes;
    return true;
}

bool format(const Device& dev, uint32_t part_lba, uint32_t part_sectors,
            const char* oem, const char* label, uint32_t volume_id)
{
    Fs fs{};
    fs.dev = dev;
    fs.part_lba = part_lba;
    fs.total_sectors = part_sectors;
    fs.num_fats = NUM_FATS;
    fs.root_entries = ROOT_ENTRIES;
    fs.spc = 1;
    while (part_sectors / fs.spc > 60000 && fs.spc < 128)
        fs.spc <<= 1;
    fs.root_dir_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) / SECTOR;

    uint32_t fatsz = 1;
    for (;;) {
        if (part_sectors <= RESERVED + NUM_FATS * fatsz + fs.root_dir_sectors)
            return false;
        uint32_t data = part_sectors - RESERVED - NUM_FATS * fatsz - fs.root_dir_sectors;
        uint32_t clusters = data / fs.spc;
        uint32_t need = (clusters + 2) * 2;
        uint32_t fatsecs = (need + SECTOR - 1) / SECTOR;
        if (fatsecs <= fatsz)
            break;
        fatsz = fatsecs;
        if (fatsz > 0xFFFF)
            return false;
    }
    fs.fatsz = fatsz;
    uint32_t data_sectors = part_sectors - RESERVED - NUM_FATS * fatsz - fs.root_dir_sectors;
    uint32_t clusters = data_sectors / fs.spc;
    if (clusters < FAT16_MIN_CLUSTERS || clusters + 2 > FAT16_MAX_CLUSTER)
        return false;

    fs.first_fat = RESERVED;
    fs.first_root = RESERVED + NUM_FATS * fatsz;
    fs.first_data = fs.first_root + fs.root_dir_sectors;
    fs.max_cluster = clusters + 1;

    uint8_t buf[SECTOR];
    fs.sec = buf;
    fs.mounted = true;

    for (uint32_t i = 0; i < SECTOR; ++i)
        fs.sec[i] = 0;
    fs.sec[0] = 0xEB;
    fs.sec[1] = 0x3C;
    fs.sec[2] = 0x90;
    copy_padded(reinterpret_cast<char*>(fs.sec + 3), 8, oem);
    wr16(fs.sec + 11, SECTOR);
    fs.sec[13] = static_cast<uint8_t>(fs.spc);
    wr16(fs.sec + 14, RESERVED);
    fs.sec[16] = NUM_FATS;
    wr16(fs.sec + 17, ROOT_ENTRIES);
    if (part_sectors < 65536) {
        wr16(fs.sec + 19, static_cast<uint16_t>(part_sectors));
        wr32(fs.sec + 32, 0);
    } else {
        wr16(fs.sec + 19, 0);
        wr32(fs.sec + 32, part_sectors);
    }
    fs.sec[21] = 0xF8;
    wr16(fs.sec + 22, static_cast<uint16_t>(fatsz));
    wr16(fs.sec + 24, 63);
    wr16(fs.sec + 26, 255);
    wr32(fs.sec + 28, part_lba);
    fs.sec[36] = 0x80;
    fs.sec[38] = 0x29;
    wr32(fs.sec + 39, volume_id);
    copy_padded(reinterpret_cast<char*>(fs.sec + 43), 11, label);
    const char fstype[] = "FAT16   ";
    for (int i = 0; i < 8; ++i)
        fs.sec[54 + i] = static_cast<uint8_t>(fstype[i]);
    fs.sec[510] = 0x55;
    fs.sec[511] = 0xAA;
    if (!dev_write(fs, 0, 1, fs.sec))
        return false;

    if (!zero_sectors(fs, fs.first_fat, fatsz))
        return false;
    if (!zero_sectors(fs, fs.first_fat + fatsz, fatsz))
        return false;
    for (uint32_t copy = 0; copy < NUM_FATS; ++copy) {
        if (!dev_read(fs, fs.first_fat + copy * fatsz, 1, fs.sec))
            return false;
        wr16(fs.sec + 0, 0xFFF8);
        wr16(fs.sec + 2, 0xFFFF);
        if (!dev_write(fs, fs.first_fat + copy * fatsz, 1, fs.sec))
            return false;
    }

    if (!zero_sectors(fs, fs.first_root, fs.root_dir_sectors))
        return false;
    uint8_t e[32];
    for (int i = 0; i < 32; ++i)
        e[i] = 0;
    copy_padded(reinterpret_cast<char*>(e), 11, label);
    e[11] = ATTR_VOLUME;
    return write_entry_at(fs, 0, 0, e);
}

bool mount(Fs& fs, const Device& dev, uint32_t part_lba, uint8_t* sector_buf)
{
    if (!sector_buf)
        return false;
    fs = Fs{};
    fs.dev = dev;
    fs.part_lba = part_lba;
    fs.sec = sector_buf;
    if (!dev_read(fs, 0, 1, fs.sec))
        return false;
    const uint8_t* b = fs.sec;
    if (b[510] != 0x55 || b[511] != 0xAA)
        return false;
    uint32_t bps = static_cast<uint32_t>(b[11] | (b[12] << 8));
    if (bps != SECTOR)
        return false;
    fs.spc = b[13];
    if (fs.spc == 0 || (fs.spc & (fs.spc - 1)) != 0)
        return false;
    uint32_t reserved = static_cast<uint32_t>(b[14] | (b[15] << 8));
    fs.num_fats = b[16];
    fs.root_entries = static_cast<uint32_t>(b[17] | (b[18] << 8));
    uint32_t tot = static_cast<uint32_t>(b[19] | (b[20] << 8));
    if (tot == 0)
        tot = rd32(b + 32);
    fs.total_sectors = tot;
    fs.fatsz = static_cast<uint32_t>(b[22] | (b[23] << 8));
    if (reserved == 0 || fs.num_fats == 0 || fs.fatsz == 0)
        return false;
    fs.first_fat = reserved;
    fs.root_dir_sectors = (fs.root_entries * 32 + SECTOR - 1) / SECTOR;
    fs.first_root = reserved + fs.num_fats * fs.fatsz;
    fs.first_data = fs.first_root + fs.root_dir_sectors;
    if (fs.total_sectors <= fs.first_data)
        return false;
    uint32_t clusters = (fs.total_sectors - fs.first_data) / fs.spc;
    if (clusters < FAT16_MIN_CLUSTERS || clusters + 2 > FAT16_MAX_CLUSTER)
        return false;
    fs.max_cluster = clusters + 1;
    fs.mounted = true;
    return true;
}

bool umount(Fs& fs)
{
    fs.mounted = false;
    return true;
}

bool sync(Fs& fs)
{
    if (!fs.mounted)
        return false;
    if (fs.dev.flush)
        fs.dev.flush(fs.dev.ctx);
    return true;
}

bool lookup(Fs& fs, uint32_t dir_cluster, const char* name, uint32_t* out_cluster,
            uint32_t* out_size, uint8_t* out_attr)
{
    Entry ev;
    if (!find_entry(fs, dir_cluster, name, &ev))
        return false;
    if (out_cluster)
        *out_cluster = ev.cluster;
    if (out_size)
        *out_size = ev.size;
    if (out_attr)
        *out_attr = ev.attr;
    return true;
}

bool resolve(Fs& fs, const char* path, uint32_t* out_cluster, uint32_t* out_size,
             uint8_t* out_attr)
{
    uint32_t cur = 0;
    uint32_t size = 0;
    uint8_t attr = ATTR_DIR;
    const char* p = path ? path : "";
    while (*p) {
        while (*p == '/')
            ++p;
        if (!*p)
            break;
        if (p[0] == '.' && (p[1] == 0 || p[1] == '/')) {
            while (*p && *p != '/')
                ++p;
            continue;
        }
        if (p[0] == '.' && p[1] == '.' && (p[2] == 0 || p[2] == '/')) {
            while (*p && *p != '/')
                ++p;
            if (cur != 0) {
                Entry up;
                if (find_entry(fs, cur, "..", &up))
                    cur = up.cluster;
                else
                    cur = 0;
            }
            continue;
        }
        char comp[NAME_MAX];
        uint32_t n = 0;
        while (*p && *p != '/') {
            if (n + 1 < NAME_MAX)
                comp[n++] = *p;
            ++p;
        }
        comp[n] = 0;
        Entry ev;
        if (!find_entry(fs, cur, comp, &ev))
            return false;
        cur = ev.cluster;
        size = ev.size;
        attr = ev.attr;
        if (!(attr & ATTR_DIR) && *p)
            return false;
    }
    if (out_cluster)
        *out_cluster = cur;
    if (out_size)
        *out_size = size;
    if (out_attr)
        *out_attr = attr;
    return true;
}

bool read(Fs& fs, uint32_t dir_cluster, const char* name, uint32_t off, void* buf,
          uint32_t len, uint32_t* out)
{
    *out = 0;
    Entry ev;
    if (!find_entry(fs, dir_cluster, name, &ev))
        return false;
    if (ev.attr & ATTR_DIR)
        return false;
    if (off >= ev.size)
        return true;
    uint32_t avail = ev.size - off;
    if (len > avail)
        len = avail;
    uint32_t cb = fs.spc * SECTOR;
    uint32_t clu;
    if (!chain_at(fs, ev.cluster, off / cb, &clu) || clu == 0)
        return true;
    uint32_t within = off % cb;
    uint8_t* dst = static_cast<uint8_t*>(buf);
    uint32_t done = 0;
    while (done < len) {
        uint32_t sec = within / SECTOR;
        uint32_t so = within % SECTOR;
        if (!dev_read(fs, cluster_to_sector(fs, clu) + sec, 1, fs.sec))
            return false;
        uint32_t chunk = SECTOR - so;
        if (chunk > len - done)
            chunk = len - done;
        for (uint32_t i = 0; i < chunk; ++i)
            dst[done + i] = fs.sec[so + i];
        done += chunk;
        within += chunk;
        if (within >= cb && done < len) {
            within -= cb;
            uint32_t nx;
            if (!fat_next(fs, clu, &nx) || nx == 0)
                break;
            clu = nx;
        }
    }
    *out = done;
    return true;
}

bool write(Fs& fs, uint32_t dir_cluster, const char* name, uint32_t off,
           const void* buf, uint32_t len, uint32_t* out)
{
    *out = 0;
    Entry ev;
    if (!find_entry(fs, dir_cluster, name, &ev)) {
        if (!create(fs, dir_cluster, name))
            return false;
        if (!find_entry(fs, dir_cluster, name, &ev))
            return false;
    }
    if (ev.attr & ATTR_DIR)
        return false;
    if (len == 0)
        return true;
    if (off > 0xFFFFFFFFu - len)
        return false;
    uint32_t end = off + len;
    uint32_t cb = fs.spc * SECTOR;
    uint32_t need = (end + cb - 1) / cb;
    uint32_t first = ev.cluster;
    uint32_t nf;
    if (!ensure_chain(fs, first, need, &nf))
        return false;
    if (off > ev.size) {
        if (!range_write(fs, nf, ev.size, nullptr, off - ev.size))
            return false;
    }
    const uint8_t* src = static_cast<const uint8_t*>(buf);
    if (!range_write(fs, nf, off, src, len))
        return false;
    uint32_t nsize = end > ev.size ? end : ev.size;
    update_entry(fs, dir_cluster, ev.pos, nf, nsize);
    *out = len;
    return true;
}

bool create(Fs& fs, uint32_t dir_cluster, const char* name)
{
    Entry ev;
    if (find_entry(fs, dir_cluster, name, &ev))
        return false;
    uint8_t entries[MAX_ENTRIES * 32];
    uint32_t n = 0;
    if (!build_entries(fs, dir_cluster, name, ATTR_ARCH, 0, 0, entries, &n))
        return false;
    return dir_append(fs, dir_cluster, entries, n);
}

bool truncate(Fs& fs, uint32_t dir_cluster, const char* name, uint32_t size)
{
    Entry ev;
    if (!find_entry(fs, dir_cluster, name, &ev))
        return false;
    if (ev.attr & ATTR_DIR)
        return false;
    if (size == ev.size)
        return true;
    uint32_t cb = fs.spc * SECTOR;
    uint32_t first = ev.cluster;
    if (size > ev.size) {
        uint32_t nf;
        if (!ensure_chain(fs, first, (size + cb - 1) / cb, &nf))
            return false;
        first = nf;
    } else {
        uint32_t keep = (size + cb - 1) / cb;
        if (keep == 0) {
            if (first && !free_chain(fs, first))
                return false;
            first = 0;
        } else {
            uint32_t last;
            if (!chain_at(fs, first, keep - 1, &last))
                return false;
            uint32_t rest;
            if (!fat_next(fs, last, &rest))
                return false;
            if (rest) {
                if (!free_chain(fs, rest))
                    return false;
                if (!fat_set(fs, last, FAT_EOC))
                    return false;
            }
            uint32_t tail = size - (keep - 1) * cb;
            if (tail < cb && !range_write(fs, first, size, nullptr, cb - tail))
                return false;
        }
    }
    update_entry(fs, dir_cluster, ev.pos, first, size);
    return true;
}

bool unlink(Fs& fs, uint32_t dir_cluster, const char* name)
{
    Entry ev;
    if (!find_entry(fs, dir_cluster, name, &ev))
        return false;
    if (ev.attr & ATTR_DIR)
        return false;
    if (ev.cluster && !free_chain(fs, ev.cluster))
        return false;
    for (uint32_t i = 0; i < ev.lfn_n; ++i)
        if (!mark_deleted(fs, dir_cluster, ev.lfn_pos[i]))
            return false;
    return mark_deleted(fs, dir_cluster, ev.pos);
}

bool mkdir(Fs& fs, uint32_t dir_cluster, const char* name)
{
    Entry ev;
    if (find_entry(fs, dir_cluster, name, &ev))
        return false;
    uint32_t clu;
    if (!cluster_alloc(fs, 0, &clu))
        return false;
    if (!init_subdir(fs, clu, dir_cluster)) {
        free_chain(fs, clu);
        return false;
    }
    uint8_t out[MAX_ENTRIES * 32];
    uint32_t n = 0;
    if (!build_entries(fs, dir_cluster, name, ATTR_DIR, clu, 0, out, &n)) {
        free_chain(fs, clu);
        return false;
    }
    if (!dir_append(fs, dir_cluster, out, n)) {
        free_chain(fs, clu);
        return false;
    }
    return true;
}

bool rmdir(Fs& fs, uint32_t dir_cluster, const char* name)
{
    Entry ev;
    if (!find_entry(fs, dir_cluster, name, &ev))
        return false;
    if (!(ev.attr & ATTR_DIR))
        return false;
    uint32_t total = dir_total_bytes(fs, ev.cluster);
    uint32_t pos = 0;
    Entry child;
    while (scan_next(fs, ev.cluster, pos, total, &child)) {
        if (child.name[0] == '.' &&
            (child.name[1] == 0 || (child.name[1] == '.' && child.name[2] == 0)))
            continue;
        return false; /* not empty */
    }
    if (!free_chain(fs, ev.cluster))
        return false;
    for (uint32_t i = 0; i < ev.lfn_n; ++i)
        if (!mark_deleted(fs, dir_cluster, ev.lfn_pos[i]))
            return false;
    return mark_deleted(fs, dir_cluster, ev.pos);
}

void opendir(Fs& fs, uint32_t dir_cluster, Iter* it)
{
    it->dir_cluster = dir_cluster;
    it->pos = 0;
    it->total = dir_total_bytes(fs, dir_cluster);
    it->seen = 0;
}

bool readdir(Fs& fs, Iter* it, char* name, uint32_t name_cap, uint32_t* out_cluster,
             uint32_t* out_size, uint8_t* out_attr)
{
    Entry ev;
    while (scan_next(fs, it->dir_cluster, it->pos, it->total, &ev)) {
        if (ev.attr & ATTR_VOLUME)
            continue;
        if (ev.name[0] == '.' &&
            (ev.name[1] == 0 || (ev.name[1] == '.' && ev.name[2] == 0)))
            continue;
        uint32_t n = 0;
        while (ev.name[n] && n + 1 < name_cap) {
            name[n] = ev.name[n];
            ++n;
        }
        name[n] = 0;
        if (out_cluster)
            *out_cluster = ev.cluster;
        if (out_size)
            *out_size = ev.size;
        if (out_attr)
            *out_attr = ev.attr;
        ++it->seen;
        return true;
    }
    return false;
}

} // namespace vnu::fat16
