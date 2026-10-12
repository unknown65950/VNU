// Host-side self test for the FAT16 library.
//
// It mounts a RAM-backed device, exercises create/write/read/readdir/
// truncate/unlink and, when given a path on the command line, writes the
// resulting image there so tools/mtools (mdir, mcopy, fsck.fat) can check
// it independently:
//
//   g++ -std=c++17 -I vnu/kernel/include vnu/kernel/fs/fat16.cpp tools/fat16_test.cpp -o /tmp/fat16_test
//   /tmp/fat16_test /tmp/fat16.img
//   mdir -i /tmp/fat16.img ::
//
// Exits non-zero when a check fails.

#include <vnu/fat16.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vnu::fat16;

namespace {

struct Ram {
    std::vector<uint8_t> bytes;
};

int ram_read(void* ctx, uint32_t lba, uint32_t count, void* buf)
{
    Ram* r = static_cast<Ram*>(ctx);
    uint64_t off = static_cast<uint64_t>(lba) * SECTOR;
    uint64_t len = static_cast<uint64_t>(count) * SECTOR;
    if (off + len > r->bytes.size())
        return -1;
    std::memcpy(buf, r->bytes.data() + off, len);
    return 0;
}

int ram_write(void* ctx, uint32_t lba, uint32_t count, const void* buf)
{
    Ram* r = static_cast<Ram*>(ctx);
    uint64_t off = static_cast<uint64_t>(lba) * SECTOR;
    uint64_t len = static_cast<uint64_t>(count) * SECTOR;
    if (off + len > r->bytes.size())
        return -1;
    std::memcpy(r->bytes.data() + off, buf, len);
    return 0;
}

int ram_flush(void* ctx)
{
    (void)ctx;
    return 0;
}

int failures = 0;

void check(bool cond, const char* what)
{
    if (cond) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

uint32_t pattern(uint32_t i)
{
    return (i * 2654435761u) ^ (i >> 3);
}

} // namespace

int main(int argc, char** argv)
{
    Ram ram;

    /* `--list IMAGE` mounts an existing image and prints its tree, so a
     * volume produced by mtools can be read back by this library. */
    if (argc >= 3 && std::strcmp(argv[1], "--list") == 0) {
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) {
            std::printf("cannot open %s\n", argv[2]);
            return 1;
        }
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        ram.bytes.assign(static_cast<size_t>(n > 0 ? n : 0), 0);
        if (n > 0)
            (void)std::fread(ram.bytes.data(), 1, static_cast<size_t>(n), f);
        std::fclose(f);
        Device dev{&ram, ram_read, ram_write, ram_flush};
        Fs fs{};
        std::vector<uint8_t> sec(SECTOR);
        if (!mount(fs, dev, 0, sec.data())) {
            std::printf("mount failed\n");
            return 1;
        }
        struct Walk {
            Fs* fs;
            uint32_t cluster;
            int depth;
        } stack[16];
        int top = 0;
        stack[top++] = {&fs, 0, 0};
        char name[NAME_MAX];
        char content[256];
        uint32_t clu, size;
        uint8_t attr;
        while (top > 0) {
            Walk w = stack[--top];
            Iter it;
            opendir(*w.fs, w.cluster, &it);
            while (readdir(*w.fs, &it, name, sizeof(name), &clu, &size, &attr)) {
                std::printf("%*s%s%s (%u)\n", w.depth * 2, "", name,
                            (attr & ATTR_DIR) ? "/" : "", size);
                if (attr & ATTR_DIR) {
                    if (top < 16)
                        stack[top++] = {w.fs, clu, w.depth + 1};
                } else if (std::strcmp(name, "mtoollong.txt") == 0) {
                    uint32_t g = 0;
                    read(*w.fs, w.cluster, name, 0, content, sizeof(content), &g);
                    std::printf("%*s  data: %.*s\n", w.depth * 2, "", static_cast<int>(g), content);
                }
            }
        }
        return 0;
    }

    ram.bytes.assign(32u * 1024 * 1024, 0);

    Device dev{&ram, ram_read, ram_write, ram_flush};
    const uint32_t total = static_cast<uint32_t>(ram.bytes.size() / SECTOR);

    check(format(dev, 0, total, "VNUFS   ", "VNU", 0x56554E55), "format 32 MiB volume");

    Fs fs{};
    std::vector<uint8_t> sec(SECTOR);
    check(mount(fs, dev, 0, sec.data()), "mount");
    check(fs.mounted, "fs marked mounted");

    uint32_t vol_total = 0, vol_free = 0;
    check(volume_stats(fs, &vol_total, &vol_free) && vol_total > 0 &&
              vol_free > 0 && vol_free <= vol_total,
          "volume_stats reports a sane capacity");

    /* A plain 8.3 file, created on demand by write(). */
    const char* hello = "hello, fat16";
    uint32_t got = 0;
    check(write(fs, 0, "notes.txt", 0, hello, 11, &got) && got == 11, "write notes.txt");
    char buf[64];
    std::memset(buf, 0, sizeof(buf));
    check(read(fs, 0, "notes.txt", 0, buf, sizeof(buf), &got) && got == 11 &&
              std::memcmp(buf, hello, 11) == 0,
          "read notes.txt back");

    /* Lowercase 8.3 must come back as written (NT case bits). */
    uint32_t clu = 0, size = 0;
    uint8_t attr = 0;
    check(lookup(fs, 0, "notes.txt", &clu, &size, &attr) && size == 11, "lookup notes.txt");

    /* A subdirectory and a file inside it. */
    check(mkdir(fs, 0, "docs"), "mkdir docs");
    uint32_t docs = 0;
    check(resolve(fs, "docs", &docs, &size, &attr) && (attr & ATTR_DIR), "resolve docs");
    check(write(fs, docs, "readme", 0, "in docs", 7, &got) && got == 7, "write docs/readme");
    check(resolve(fs, "docs/readme", &clu, &size, &attr) && size == 7, "resolve docs/readme");

    /* A file that does not fit 8.3 gets an LFN. */
    check(write(fs, 0, "long-file-name.txt", 0, "names", 5, &got) && got == 5,
          "write long-file-name.txt");
    check(lookup(fs, 0, "long-file-name.txt", &clu, &size, &attr) && size == 5,
          "lookup long name by LFN");

    /* Overwrite in place, and append through a second write at an offset. */
    check(write(fs, 0, "notes.txt", 6, " again", 6, &got) && got == 6, "overwrite notes.txt tail");
    std::memset(buf, 0, sizeof(buf));
    check(read(fs, 0, "notes.txt", 0, buf, sizeof(buf), &got) && got == 12 &&
              std::memcmp(buf, "hello, again", 12) == 0,
          "read overwritten notes.txt");

    /* A multi-megabyte file exercises the cluster chain and the FAT. */
    const uint32_t BIG = 3u * 1024 * 1024 + 123;
    std::vector<uint8_t> big(BIG);
    for (uint32_t i = 0; i < BIG; ++i)
        big[i] = static_cast<uint8_t>(pattern(i));
    uint32_t free_before = 0;
    check(volume_stats(fs, &vol_total, &free_before), "volume_stats before big.bin");
    check(write(fs, 0, "big.bin", 0, big.data(), BIG, &got) && got == BIG, "write 3 MiB big.bin");
    uint32_t free_after = 0;
    check(volume_stats(fs, &vol_total, &free_after) &&
              free_after + BIG <= free_before,
          "big.bin consumed at least its own size in free space");
    std::vector<uint8_t> back(BIG, 0);
    check(read(fs, 0, "big.bin", 0, back.data(), BIG, &got) && got == BIG &&
              std::memcmp(back.data(), big.data(), BIG) == 0,
          "read 3 MiB big.bin byte-for-byte");

    /* Read from the middle of the big file. */
    std::memset(back.data(), 0, 512);
    check(read(fs, 0, "big.bin", 1000000, back.data(), 512, &got) && got == 512 &&
              std::memcmp(back.data(), big.data() + 1000000, 512) == 0,
          "read big.bin at an offset");

    /* Directory listing. */
    {
        Iter it;
        opendir(fs, 0, &it);
        std::string names;
        while (readdir(fs, &it, buf, sizeof(buf), &clu, &size, &attr)) {
            names += buf;
            names += " ";
        }
        check(names == "notes.txt docs long-file-name.txt big.bin ",
              "readdir root lists every entry");
    }

    /* A directory grown past its first cluster: forty small files in
     * one subdirectory, every one found again with its own content.
     * (A chain walk used to load FAT content into the shared sector
     * scratch, clobbering the staged entry for anything past the first
     * cluster, so the 29th file came back as garbage.) Half the names
     * need LFN entries, so multi-slot batches also cross the growth
     * point: a batch either lands complete or not at all. */
    {
        check(mkdir(fs, 0, "many"), "mkdir many");
        uint32_t many = 0;
        check(resolve(fs, "many", &many, &size, &attr) && (attr & ATTR_DIR),
              "resolve many");
        bool stored = true;
        for (int i = 0; i < 40; ++i) {
            char name[32];
            if (i % 2)
                std::snprintf(name, sizeof(name), "f%02d", i);
            else
                std::snprintf(name, sizeof(name), "long-file-%02d.txt", i);
            char data[64];
            std::snprintf(data, sizeof(data), "payload-%d", i);
            uint32_t len = static_cast<uint32_t>(std::strlen(data));
            stored = stored && write(fs, many, name, 0, data, len, &got) &&
                     got == len;
        }
        check(stored, "write 40 files into many/");
        bool back_ok = true;
        for (int i = 0; i < 40; ++i) {
            char name[32];
            if (i % 2)
                std::snprintf(name, sizeof(name), "f%02d", i);
            else
                std::snprintf(name, sizeof(name), "long-file-%02d.txt", i);
            char data[64];
            std::snprintf(data, sizeof(data), "payload-%d", i);
            uint32_t len = static_cast<uint32_t>(std::strlen(data));
            char in[64];
            std::memset(in, 0, sizeof(in));
            back_ok = back_ok &&
                      lookup(fs, many, name, &clu, &size, &attr) &&
                      size == len &&
                      read(fs, many, name, 0, in, sizeof(in), &got) &&
                      got == len && std::memcmp(in, data, len) == 0;
        }
        check(back_ok, "all 40 files resolve with their own content");
    }

    /* truncate() past and below the current end. */
    check(truncate(fs, 0, "big.bin", 0), "truncate big.bin to 0");
    check(lookup(fs, 0, "big.bin", &clu, &size, &attr) && size == 0, "big.bin is empty");
    check(read(fs, 0, "big.bin", 0, buf, sizeof(buf), &got) && got == 0, "reading empty file yields 0");

    /* unlink() and a now-missing lookup. */
    check(unlink(fs, 0, "long-file-name.txt"), "unlink long-file-name.txt");
    check(!lookup(fs, 0, "long-file-name.txt", &clu, &size, &attr), "gone after unlink");
    check(!unlink(fs, 0, "docs"), "unlink refuses a directory");
    check(rmdir(fs, 0, "docs") == false, "rmdir refuses a non-empty directory");
    check(unlink(fs, docs, "readme"), "unlink docs/readme");
    check(rmdir(fs, 0, "docs"), "rmdir empty docs");

    check(sync(fs), "sync");

    if (argc > 1) {
        FILE* f = std::fopen(argv[1], "wb");
        if (!f) {
            std::printf("FAIL cannot open %s\n", argv[1]);
            ++failures;
        } else {
            std::fwrite(ram.bytes.data(), 1, ram.bytes.size(), f);
            std::fclose(f);
            std::printf("wrote %s (%zu bytes) for mtools\n", argv[1], ram.bytes.size());
        }
    }

    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
