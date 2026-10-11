#include <vnu/initrd.h>
#include <vnu/mboot.h>
#include <vnu/ustar.h>
#include <vnu/vfs.h>

// Unpack the boot initrd (a POSIX ustar archive passed as a Multiboot2
// module) into /initrd. See initrd.h for the contract. The archive is
// located by its ustar magic rather than by module order, so the kernel
// module the installer also loads (module 0) can never be mistaken for
// it and a boot with no initrd simply unpacks nothing.

namespace {

constexpr uint32_t PATH_CAP = 64;      // == vfs.cpp PATH_CAP
constexpr uint32_t MAX_UNPACK = 8u << 20; // refuse to mirror more than this

const void* g_image = nullptr;
uint32_t g_size = 0;
uint32_t g_files = 0;
uint32_t g_skipped = 0;

bool looks_like_ustar(const void* m, uint32_t size)
{
    if (!m || size < 512)
        return false;
    const uint8_t* b = static_cast<const uint8_t*>(m);
    return b[257] == 'u' && b[258] == 's' && b[259] == 't' &&
           b[260] == 'a' && b[261] == 'r';
}

// Reject a name that would escape /initrd once vfs::normalize() has run
// ("a/../.."), and any name the 64-byte path field cannot hold.
bool safe_relative(const char* name)
{
    if (!name[0])
        return false;
    uint32_t seg = 0;
    for (uint32_t i = 0;; ++i) {
        char c = name[i];
        if (c == '/' || c == 0) {
            if (seg == 2 && name[i - 2] == '.' && name[i - 1] == '.')
                return false;
            if (c == 0)
                break;
            seg = 0;
            continue;
        }
        ++seg;
    }
    // "/initrd/" + name + NUL must fit in PATH_CAP.
    uint32_t len = 0;
    while (name[len])
        ++len;
    return len + 8 < PATH_CAP;
}

// Create every directory on the way to `path` (which must be a writable
// buffer), leaving the last component alone. "mkdir" failing with EEXIST
// is the normal case and is ignored.
void make_parents(char* path)
{
    for (uint32_t i = 1; path[i]; ++i) {
        if (path[i] != '/')
            continue;
        path[i] = 0;
        vnu::vfs::mkdir(path);
        path[i] = '/';
    }
}

} // namespace

namespace vnu::initrd {

const void* image() { return g_image; }
uint32_t image_size() { return g_size; }
bool present() { return g_image != nullptr; }
uint32_t files() { return g_files; }
uint32_t skipped() { return g_skipped; }

void unpack()
{
    g_image = nullptr;
    g_size = 0;
    g_files = 0;
    g_skipped = 0;

    int n = vnu::mboot::module_count();
    for (int i = 0; i < n; ++i) {
        uint32_t size = 0;
        const void* m = vnu::mboot::module_at(i, size);
        if (looks_like_ustar(m, size)) {
            g_image = m;
            g_size = size;
            break;
        }
    }
    if (!g_image)
        return;

    uint64_t off = 0;
    struct vnu_ustar_entry e;
    uint32_t total = 0;
    while (vnu_ustar_next(g_image, g_size, &off, &e) == 1) {
        if (!e.name[0])
            continue; /* the archive's own "./" root entry */
        if (!safe_relative(e.name)) {
            ++g_skipped;
            continue;
        }
        char path[PATH_CAP];
        uint32_t k = 0;
        const char* pre = "/initrd/";
        for (; pre[k]; ++k)
            path[k] = pre[k];
        for (uint32_t i = 0; e.name[i]; ++i)
            path[k++] = e.name[i];
        path[k] = 0;

        make_parents(path);
        if (e.type == '5') {
            vnu::vfs::mkdir(path);
            continue;
        }

        if (total + (uint32_t)e.size > MAX_UNPACK) {
            ++g_skipped;
            continue;
        }
        if (vnu::vfs::touch(path) != 0) {
            ++g_skipped;
            continue;
        }
        if (vnu::vfs::set_path(path, reinterpret_cast<const char*>(e.data),
                               static_cast<uint32_t>(e.size)) != 0) {
            ++g_skipped;
            continue;
        }
        // Carry the archive's permission bits over (root is writing, so
        // this always succeeds); an executable script stays runnable.
        vnu::vfs::chmod(path, e.mode & 0777u);
        total += static_cast<uint32_t>(e.size);
        ++g_files;
    }
}

} // namespace vnu::initrd
