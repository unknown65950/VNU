#include <vnu/vfs.h>
#include <vnu/abi.h>
#include <vnu/pmm.h>
#include <vnu/process.h>

namespace {

constexpr int MAX_FD = 64;
/* Every Node carries a full DATA_CAP buffer, so this is the dominant
 * consumer of kernel .bss (MAX_N * ~20 KiB). Raised from 64 to fit
 * /dev and /proc without displacing the existing /bin + /apps entries;
 * the resulting .bss still ends well below the 0x400000 app-image
 * region (checked after building — see VIBEGRAPHICS_CHANGES.md). */
constexpr int MAX_N = 96;
constexpr int DATA_CAP = 20480; /* big enough to hold a small embedded ELF binary */
constexpr int PATH_CAP = 64;

/* Which /proc file a node synthesizes, if any. Content is regenerated
 * on every open() rather than stored, so a reader always sees current
 * values; the generated text lands in the node's normal data buffer so
 * read()/lseek() work on it unchanged. */
enum class SynthKind : uint8_t { None = 0, Version, CpuInfo, MemInfo, SelfStatus, Mounts, Uptime };

struct Node {
    bool used;
    bool dir;
    vnu::vfs::DevKind dev;
    SynthKind synth;
    char path[PATH_CAP];
    char data[DATA_CAP];
    uint32_t size;
};

Node nodes[MAX_N];
vnu::vfs::File files[MAX_FD];
char cwd[PATH_CAP] = "/";

bool same(const char* a, const char* b)
{
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return !*a && !*b;
}

void copy(char* d, const char* s, int n)
{
    int i = 0;
    for (; s[i] && i < n - 1; ++i)
        d[i] = s[i];
    d[i] = 0;
}

int strlen_local(const char* s)
{
    int n = 0;
    while (s[n])
        ++n;
    return n;
}

/* Resolve relative path against cwd into out (absolute-ish). Also
 * resolves "." and ".." path components, so `cd ..` etc. work no
 * matter how deep you've navigated. */
void normalize(const char* path, char* out, int outn)
{
    char raw[PATH_CAP];

    if (!path || !path[0]) {
        copy(raw, cwd, PATH_CAP);
    } else if (path[0] == '/') {
        copy(raw, path, PATH_CAP);
    } else {
        int i = 0;
        for (; cwd[i] && i < PATH_CAP - 2; ++i)
            raw[i] = cwd[i];
        if (i > 1 && raw[i - 1] != '/')
            raw[i++] = '/';
        for (int j = 0; path[j] && i < PATH_CAP - 1; ++j)
            raw[i++] = path[j];
        raw[i] = 0;
    }

    /* Segment-by-segment resolve: push normal components, pop on "..",
     * skip "." and empty (double-slash) components. Segments are kept
     * as (offset, length) pairs into `raw` so we never need to mutate
     * the buffer while still scanning it. */
    int seg_start[16];
    int seg_len[16];
    int depth = 0;

    int i = 0;
    while (raw[i]) {
        while (raw[i] == '/')
            ++i;
        if (!raw[i])
            break;
        int start = i;
        while (raw[i] && raw[i] != '/')
            ++i;
        int len = i - start;

        if (len == 1 && raw[start] == '.') {
            continue; /* "." — no-op */
        }
        if (len == 2 && raw[start] == '.' && raw[start + 1] == '.') {
            if (depth > 0)
                --depth; /* ".." — pop, but never above root */
            continue;
        }
        if (depth < 16) {
            seg_start[depth] = start;
            seg_len[depth] = len;
            ++depth;
        }
    }

    /* Rebuild "/seg0/seg1/.../segN" (or "/" if depth == 0). */
    int o = 0;
    if (outn > 0)
        out[o++] = '/';
    for (int k = 0; k < depth; ++k) {
        if (k > 0 && o < outn - 1)
            out[o++] = '/';
        for (int c = 0; c < seg_len[k] && o < outn - 1; ++c)
            out[o++] = raw[seg_start[k] + c];
    }
    if (o < outn)
        out[o] = 0;
    else if (outn > 0)
        out[outn - 1] = 0;
}

Node* find(const char* p)
{
    for (int i = 0; i < MAX_N; ++i)
        if (nodes[i].used && same(nodes[i].path, p))
            return &nodes[i];
    return nullptr;
}

int find_index(const char* p)
{
    for (int i = 0; i < MAX_N; ++i)
        if (nodes[i].used && same(nodes[i].path, p))
            return i;
    return -1;
}

Node* add(const char* p, bool d)
{
    for (int i = 0; i < MAX_N; ++i) {
        if (!nodes[i].used) {
            nodes[i] = {};
            nodes[i].used = true;
            nodes[i].dir = d;
            copy(nodes[i].path, p, PATH_CAP);
            return &nodes[i];
        }
    }
    return nullptr;
}

bool is_prefix_child(const char* parent, const char* child)
{
    /* True if child is a direct child path of parent directory. */
    if (same(parent, child))
        return false;
    int pl = strlen_local(parent);
    if (parent[0] == '/' && parent[1] == 0) {
        if (child[0] != '/')
            return false;
        for (int i = 1; child[i]; ++i)
            if (child[i] == '/')
                return false;
        return child[1] != 0;
    }
    for (int i = 0; i < pl; ++i)
        if (child[i] != parent[i])
            return false;
    if (child[pl] != '/')
        return false;
    for (int i = pl + 1; child[i]; ++i)
        if (child[i] == '/')
            return false;
    return child[pl + 1] != 0;
}

const char* basename(const char* path)
{
    const char* s = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/')
            s = p + 1;
    return s;
}

/* --- small text-building helpers for the /proc generators --- */

struct Appender {
    char* buf;
    uint32_t cap;
    uint32_t len;

    void str(const char* s)
    {
        while (s && *s && len < cap - 1)
            buf[len++] = *s++;
        buf[len] = 0;
    }

    void num(uint32_t v)
    {
        char tmp[12];
        int n = 0;
        if (v == 0) {
            str("0");
            return;
        }
        while (v && n < 12) {
            tmp[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        while (n-- > 0 && len < cap - 1)
            buf[len++] = tmp[n];
        buf[len] = 0;
    }
};

inline uint8_t cmos_read(uint8_t reg)
{
    asm volatile("outb %0, $0x70" : : "a"(reg));
    uint8_t v;
    asm volatile("inb $0x71, %0" : "=a"(v));
    return v;
}

/* Seconds since midnight from the RTC. Used only for /proc/uptime,
 * as a delta against a boot-time snapshot — this kernel has no timer
 * interrupt, so the RTC is the only real monotonic-ish clock
 * available. BCD-encoded unless the CMOS status register says
 * otherwise, which is the usual default on PC hardware and QEMU. */
uint32_t rtc_seconds_of_day()
{
    while (cmos_read(0x0A) & 0x80) {
    } /* wait out an update-in-progress */
    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t status_b = cmos_read(0x0B);
    if (!(status_b & 0x04)) { /* BCD mode */
        auto unbcd = [](uint8_t v) -> uint8_t {
            return static_cast<uint8_t>((v & 0x0F) + ((v >> 4) * 10));
        };
        sec = unbcd(sec);
        min = unbcd(min);
        hour = unbcd(static_cast<uint8_t>(hour & 0x7F));
    }
    return static_cast<uint32_t>(hour) * 3600u + static_cast<uint32_t>(min) * 60u +
           static_cast<uint32_t>(sec);
}

uint32_t g_boot_seconds = 0;

/* Simple xorshift, for /dev/random and /dev/urandom. Not a CSPRNG —
 * there's no entropy source in this kernel — seeded from the RTC so
 * it at least differs between boots. */
uint32_t g_rand_state = 0x12345678;

uint8_t next_random_byte()
{
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 17;
    g_rand_state ^= g_rand_state << 5;
    return static_cast<uint8_t>(g_rand_state & 0xFF);
}

void cpu_vendor(char out[13])
{
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    auto put4 = [&](int off, uint32_t v) {
        out[off + 0] = static_cast<char>(v & 0xFF);
        out[off + 1] = static_cast<char>((v >> 8) & 0xFF);
        out[off + 2] = static_cast<char>((v >> 16) & 0xFF);
        out[off + 3] = static_cast<char>((v >> 24) & 0xFF);
    };
    put4(0, ebx);
    put4(4, edx);
    put4(8, ecx);
    out[12] = 0;
}

void regen_synth(Node& n)
{
    Appender a{n.data, DATA_CAP, 0};
    switch (n.synth) {
    case SynthKind::Version:
        a.str("VNU version 0.2 (vibe) i386\n");
        break;
    case SynthKind::CpuInfo: {
        char vendor[13];
        cpu_vendor(vendor);
        uint32_t eax, ebx, ecx, edx;
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        a.str("processor\t: 0\n");
        a.str("vendor_id\t: ");
        a.str(vendor);
        a.str("\ncpu family\t: ");
        a.num((eax >> 8) & 0xF);
        a.str("\nmodel\t\t: ");
        a.num((eax >> 4) & 0xF);
        a.str("\nstepping\t: ");
        a.num(eax & 0xF);
        a.str("\nfpu\t\t: ");
        a.str((edx & 1) ? "yes" : "no");
        a.str("\n");
        break;
    }
    case SynthKind::MemInfo: {
        uint32_t total = vnu::pmm::total_frames();
        uint32_t freef = vnu::pmm::free_frames();
        a.str("MemTotal:       ");
        a.num(total * 4);
        a.str(" kB\nMemFree:        ");
        a.num(freef * 4);
        a.str(" kB\nMemUsed:        ");
        a.num((total - freef) * 4);
        a.str(" kB\n");
        break;
    }
    case SynthKind::SelfStatus: {
        auto* p = vnu::proc::current();
        a.str("Name:\tvnu-process\nPid:\t");
        a.num(p ? static_cast<uint32_t>(p->pid) : 0);
        a.str("\nPPid:\t");
        a.num(p ? static_cast<uint32_t>(p->ppid) : 0);
        a.str("\nUid:\t0\nGid:\t0\n");
        break;
    }
    case SynthKind::Mounts:
        a.str("vfs / vfs rw 0 0\ndev /dev devfs rw 0 0\nproc /proc procfs rw 0 0\n");
        break;
    case SynthKind::Uptime: {
        uint32_t now = rtc_seconds_of_day();
        uint32_t delta = now >= g_boot_seconds ? now - g_boot_seconds
                                               : (86400u - g_boot_seconds) + now;
        a.num(delta);
        a.str(".00 ");
        a.num(delta);
        a.str(".00\n");
        break;
    }
    case SynthKind::None:
        break;
    }
    n.size = a.len;
}

} // namespace

namespace vnu::vfs {

void init()
{
    for (int i = 0; i < MAX_FD; ++i)
        files[i] = {false, 0, 0, -1};
    for (int i = 0; i < 3; ++i)
        files[i] = {true, 0, 0, -1};
    for (int i = 0; i < MAX_N; ++i)
        nodes[i] = {};
    copy(cwd, "/", PATH_CAP);

    add("/", true);
    add("/etc", true);
    auto* m = add("/etc/motd", false);
    copy(m->data, "Welcome to VNU\n", DATA_CAP);
    m->size = 15;
    add("/etc/initialD", true);
    add("/sbin", true);
    add("/bin", true);
    add("/bin/vash", false);
    add("/bin/sh", false);
    add("/bin/hello", false);
    add("/bin/echo", false);
    add("/bin/true", false);
    add("/bin/false", false);
    add("/bin/pwd", false);
    add("/bin/cat", false);
    add("/bin/ls", false);
    add("/bin/mkdir", false);
    add("/bin/rm", false);
    add("/bin/touch", false);
    add("/bin/uname", false);
    add("/bin/clear", false);
    add("/bin/coreutils", false);
    add("/bin/vedit", false);
    add("/bin/ttytest", false);
    add("/bin/wc", false);
    add("/bin/head", false);
    add("/bin/tail", false);
    add("/bin/grep", false);
    add("/bin/sort", false);
    add("/bin/cp", false);
    add("/bin/mv", false);
    add("/bin/basename", false);
    add("/bin/dirname", false);
    add("/bin/seq", false);
    add("/tmp", true);
    auto* notes = add("/tmp/notes.txt", false);
    if (notes) {
        const char* msg = "Hello from VEDIT\nEdit me!\n";
        int i = 0;
        for (; msg[i]; ++i) notes->data[i] = msg[i];
        notes->size = (uint32_t)i;
    }

    /* --- FHS: /dev character devices --- */
    add("/dev", true);
    auto add_dev = [](const char* path, DevKind kind) {
        Node* n = add(path, false);
        if (n)
            n->dev = kind;
    };
    add_dev("/dev/null", DevKind::Null);
    add_dev("/dev/zero", DevKind::Zero);
    add_dev("/dev/full", DevKind::Full);
    add_dev("/dev/random", DevKind::Random);
    add_dev("/dev/urandom", DevKind::Random);
    add_dev("/dev/tty", DevKind::Tty);
    add_dev("/dev/console", DevKind::Tty);

    /* --- FHS: /proc, regenerated per open() (see regen_synth) --- */
    g_boot_seconds = rtc_seconds_of_day();
    g_rand_state ^= g_boot_seconds * 2654435761u;
    if (g_rand_state == 0)
        g_rand_state = 0x12345678;

    add("/proc", true);
    auto add_synth = [](const char* path, SynthKind kind) {
        Node* n = add(path, false);
        if (n)
            n->synth = kind;
    };
    add_synth("/proc/version", SynthKind::Version);
    add_synth("/proc/cpuinfo", SynthKind::CpuInfo);
    add_synth("/proc/meminfo", SynthKind::MemInfo);
    add_synth("/proc/mounts", SynthKind::Mounts);
    add_synth("/proc/uptime", SynthKind::Uptime);
    add("/proc/self", true);
    add_synth("/proc/self/status", SynthKind::SelfStatus);

    /* --- FHS: remaining conventional directories --- */
    add("/usr", true);
    add("/usr/bin", true);
    add("/var", true);
    add("/var/log", true);
    add("/home", true);
    add("/root", true);
    add("/mnt", true);
}

int open(const char* path, uint32_t flags)
{
    char abs[PATH_CAP];
    normalize(path, abs, PATH_CAP);
    int ni = find_index(abs);
    if (ni < 0) {
        if (flags & vnu::posix::O_CREAT) {
            Node* n = add(abs, false);
            if (!n)
                return -VNU_ENOSPC;
            ni = find_index(abs);
            if (flags & vnu::posix::O_TRUNC) {
                n->size = 0;
            }
        } else {
            return -VNU_ENOENT;
        }
    }
    if (nodes[ni].dir && (flags & vnu::posix::O_WRONLY))
        return -VNU_EACCES;
    /* /proc files are generated fresh per open, so each reader sees
     * current values rather than whatever the last reader saw. */
    if (nodes[ni].synth != SynthKind::None)
        regen_synth(nodes[ni]);
    /* fd 3 is reserved as the gfx surface for windowed tasks (see
     * syscall.cpp's write/lseek intercept), so real VFS descriptors
     * start at 4 — otherwise any file a gfx app opens (the file
     * manager's directories, vash's history, ...) would silently
     * steal the surface descriptor and break its framebuffer. */
    for (int i = 4; i < MAX_FD; ++i) {
        if (!files[i].used) {
            files[i] = {true, 0, flags, ni};
            /* Truncating a character device is meaningless (and would
             * corrupt the "size" we report for it), so only regular
             * files honor O_TRUNC. */
            if ((flags & vnu::posix::O_TRUNC) && !nodes[ni].dir &&
                nodes[ni].dev == DevKind::None && nodes[ni].synth == SynthKind::None)
                nodes[ni].size = 0;
            return i;
        }
    }
    return -VNU_ENOSPC;
}

int close(int fd)
{
    if (fd < 3 || fd >= MAX_FD || !files[fd].used)
        return -VNU_EBADF;
    files[fd] = {false, 0, 0, -1};
    return 0;
}

int read(int fd, void* buf, uint32_t count)
{
    if (fd < 0 || fd >= MAX_FD || !files[fd].used)
        return -VNU_EBADF;
    /* fd 0/1/2 normally mean the console and are handled in the
     * syscall layer, but once redirected they point at a real node and
     * must be served here like any other fd. */
    if (fd < 3 && files[fd].node < 0)
        return -VNU_EBADF;
    if (files[fd].node < 0)
        return -VNU_EBADF;
    Node& n = nodes[files[fd].node];
    if (n.dir)
        return -VNU_EISDIR;
    auto* out = static_cast<char*>(buf);

    if (n.dev != DevKind::None) {
        switch (n.dev) {
        case DevKind::Null:
            return 0; /* immediate EOF */
        case DevKind::Zero:
        case DevKind::Full:
            for (uint32_t i = 0; i < count; ++i)
                out[i] = 0;
            return static_cast<int>(count);
        case DevKind::Random:
            for (uint32_t i = 0; i < count; ++i)
                out[i] = static_cast<char>(next_random_byte());
            return static_cast<int>(count);
        case DevKind::Tty:
            /* Routed by the syscall layer (see fd_dev_kind) so it picks
             * up the same console/windowed-task handling as fd 0. */
            return -VNU_EIO;
        case DevKind::None:
            break;
        }
    }

    uint32_t avail = n.size > files[fd].position ? n.size - files[fd].position : 0;
    uint32_t take = avail < count ? avail : count;
    for (uint32_t i = 0; i < take; ++i)
        out[i] = n.data[files[fd].position + i];
    files[fd].position += take;
    return static_cast<int>(take);
}

int write(int fd, const void* buf, uint32_t count)
{
    if (fd < 0 || fd >= MAX_FD || !files[fd].used)
        return -VNU_EBADF;
    if (fd < 3 && files[fd].node < 0)
        return -VNU_EBADF;
    if (files[fd].node < 0)
        return -VNU_EBADF;
    Node& n = nodes[files[fd].node];
    if (n.dir)
        return -VNU_EISDIR;
    const auto* in = static_cast<const char*>(buf);

    if (n.dev != DevKind::None) {
        switch (n.dev) {
        case DevKind::Null:
        case DevKind::Zero:
        case DevKind::Random:
            return static_cast<int>(count); /* accepted and discarded */
        case DevKind::Full:
            return -VNU_ENOSPC; /* the whole point of /dev/full */
        case DevKind::Tty:
            return -VNU_EIO; /* routed by the syscall layer */
        case DevKind::None:
            break;
        }
    }
    /* /proc is read-only. */
    if (n.synth != SynthKind::None)
        return -VNU_EACCES;

    if (files[fd].flags & vnu::posix::O_APPEND)
        files[fd].position = n.size;
    uint32_t pos = files[fd].position;
    uint32_t written = 0;
    for (; written < count && pos < DATA_CAP; ++written, ++pos)
        n.data[pos] = in[written];
    if (pos > n.size)
        n.size = pos;
    files[fd].position = pos;
    return static_cast<int>(written);
}

int lseek(int fd, int32_t offset, int whence)
{
    if (fd < 0 || fd >= MAX_FD || !files[fd].used)
        return -VNU_EBADF;
    if (files[fd].node < 0)
        return 0;
    Node& n = nodes[files[fd].node];
    int32_t base = 0;
    if (whence == 1)
        base = static_cast<int32_t>(files[fd].position);
    else if (whence == 2)
        base = static_cast<int32_t>(n.size);
    int32_t np = base + offset;
    if (np < 0)
        np = 0;
    files[fd].position = static_cast<uint32_t>(np);
    return np;
}

int stat(const char* path, vnu::posix::Stat* st)
{
    if (!st)
        return -VNU_EFAULT;
    char abs[PATH_CAP];
    normalize(path, abs, PATH_CAP);
    Node* n = find(abs);
    if (!n)
        return -VNU_ENOENT;
    if (n->synth != SynthKind::None)
        regen_synth(*n); /* so st_size matches what a read would yield */
    if (n->dev != DevKind::None) {
        st->mode = vnu::posix::S_IFCHR | 0666;
        st->size = 0;
        st->type = 2; /* DT_CHR */
        return 0;
    }
    st->mode = n->dir ? (vnu::posix::S_IFDIR | 0755) : (vnu::posix::S_IFREG | 0644);
    st->size = n->size;
    st->type = n->dir ? 4 : 8;
    return 0;
}

int fstat(int fd, vnu::posix::Stat* st)
{
    if (!st)
        return -VNU_EFAULT;
    if (fd < 0 || fd >= MAX_FD || !files[fd].used)
        return -VNU_EBADF;
    if (files[fd].node < 0) {
        /* stdin/stdout/stderr: report them as character devices so
         * isatty() and friends behave sensibly. */
        st->mode = vnu::posix::S_IFCHR | 0666;
        st->size = 0;
        st->type = 2;
        return 0;
    }
    Node& n = nodes[files[fd].node];
    if (n.dev != DevKind::None) {
        st->mode = vnu::posix::S_IFCHR | 0666;
        st->size = 0;
        st->type = 2;
        return 0;
    }
    st->mode = n.dir ? (vnu::posix::S_IFDIR | 0755) : (vnu::posix::S_IFREG | 0644);
    st->size = n.size;
    st->type = n.dir ? 4 : 8;
    return 0;
}

DevKind fd_dev_kind(int fd)
{
    if (fd < 3 || fd >= MAX_FD || !files[fd].used)
        return DevKind::None;
    if (files[fd].node < 0)
        return DevKind::None;
    return nodes[files[fd].node].dev;
}

bool fd_is_redirected(int fd)
{
    if (fd < 0 || fd > 2 || !files[fd].used)
        return false;
    return files[fd].node >= 0;
}

bool fd_is_open(int fd)
{
    return fd >= 0 && fd < MAX_FD && files[fd].used;
}

void reset_stdio()
{
    for (int i = 0; i < 3; ++i)
        files[i] = {true, 0, 0, -1};
}

int dup_fd(int fd)
{
    if (fd < 0 || fd >= MAX_FD || !files[fd].used)
        return -VNU_EBADF;
    for (int i = 4; i < MAX_FD; ++i) {
        if (!files[i].used) {
            files[i] = files[fd];
            return i;
        }
    }
    return -VNU_ENOSPC;
}

int dup2_fd(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= MAX_FD || !files[oldfd].used)
        return -VNU_EBADF;
    if (newfd < 0 || newfd >= MAX_FD)
        return -VNU_EBADF;
    if (oldfd == newfd)
        return newfd;
    if (newfd >= 3 && files[newfd].used)
        files[newfd] = {false, 0, 0, -1};
    files[newfd] = files[oldfd];
    return newfd;
}

int touch(const char* path)
{
    char abs[PATH_CAP];
    normalize(path, abs, PATH_CAP);
    if (find(abs))
        return 0;
    return add(abs, false) ? 0 : -VNU_ENOSPC;
}

int mkdir(const char* path)
{
    char abs[PATH_CAP];
    normalize(path, abs, PATH_CAP);
    if (find(abs))
        return -VNU_EEXIST;
    return add(abs, true) ? 0 : -VNU_ENOSPC;
}

int rmdir(const char* path)
{
    char abs[PATH_CAP];
    normalize(path, abs, PATH_CAP);
    Node* n = find(abs);
    if (!n)
        return -VNU_ENOENT;
    if (!n->dir)
        return -VNU_ENOTDIR;
    if (same(abs, "/"))
        return -VNU_EPERM;
    /* non-empty? */
    for (int i = 0; i < MAX_N; ++i)
        if (nodes[i].used && is_prefix_child(abs, nodes[i].path))
            return -VNU_ENOTEMPTY;
    n->used = false;
    return 0;
}

int unlink(const char* path)
{
    char abs[PATH_CAP];
    normalize(path, abs, PATH_CAP);
    Node* n = find(abs);
    if (!n)
        return -VNU_ENOENT;
    if (n->dir)
        return -VNU_EISDIR;
    n->used = false;
    return 0;
}

int remove(const char* path)
{
    char abs[PATH_CAP];
    normalize(path, abs, PATH_CAP);
    Node* n = find(abs);
    if (!n)
        return -VNU_ENOENT;
    if (n->dir)
        return rmdir(abs);
    return unlink(abs);
}

int read_path(const char* path, char* buf, uint32_t count)
{
    char abs[PATH_CAP];
    normalize(path, abs, PATH_CAP);
    Node* n = find(abs);
    if (!n || n->dir)
        return -VNU_ENOENT;
    uint32_t z = n->size < count ? n->size : count;
    for (uint32_t i = 0; i < z; ++i)
        buf[i] = n->data[i];
    return static_cast<int>(z);
}

void list(char* buf, uint32_t count)
{
    uint32_t k = 0;
    for (int i = 0; i < MAX_N; ++i) {
        if (!nodes[i].used || k + 2 >= count)
            continue;
        const char* s = nodes[i].path;
        while (*s && k + 2 < count)
            buf[k++] = *s++;
        buf[k++] = '\n';
    }
    if (k < count)
        buf[k] = 0;
}

int getdents(int fd, void* buf, uint32_t count)
{
    if (fd < 0 || fd >= MAX_FD || !files[fd].used)
        return -VNU_EBADF;
    if (files[fd].node < 0)
        return -VNU_EBADF;
    Node& dir = nodes[files[fd].node];
    if (!dir.dir)
        return -VNU_ENOTDIR;

    auto* out = static_cast<uint8_t*>(buf);
    uint32_t written = 0;
    uint32_t skip = files[fd].position;
    uint32_t seen = 0;

    for (int i = 0; i < MAX_N; ++i) {
        if (!nodes[i].used)
            continue;
        if (!is_prefix_child(dir.path, nodes[i].path))
            continue;
        if (seen++ < skip)
            continue;

        Dirent de{};
        de.ino = static_cast<uint32_t>(i + 1);
        de.type = nodes[i].dir ? 4 : (nodes[i].dev != DevKind::None ? 2 /* DT_CHR */ : 8);
        const char* bn = basename(nodes[i].path);
        copy(de.name, bn, sizeof(de.name));
        de.reclen = static_cast<uint16_t>(sizeof(Dirent));
        if (written + de.reclen > count)
            break;
        auto* dst = out + written;
        for (uint16_t b = 0; b < de.reclen; ++b)
            dst[b] = reinterpret_cast<uint8_t*>(&de)[b];
        written += de.reclen;
        files[fd].position = seen;
    }
    return static_cast<int>(written);
}

int chdir(const char* path)
{
    char abs[PATH_CAP];
    normalize(path, abs, PATH_CAP);
    Node* n = find(abs);
    if (!n)
        return -VNU_ENOENT;
    if (!n->dir)
        return -VNU_ENOTDIR;
    copy(cwd, abs, PATH_CAP);
    return 0;
}

int getcwd(char* buf, uint32_t size)
{
    if (!buf || size == 0)
        return -VNU_EINVAL;
    uint32_t n = 0;
    while (cwd[n] && n + 1 < size) {
        buf[n] = cwd[n];
        ++n;
    }
    if (cwd[n] && n + 1 >= size)
        return -VNU_ERANGE;
    buf[n] = 0;
    return static_cast<int>(n);
}

} // namespace vnu::vfs
