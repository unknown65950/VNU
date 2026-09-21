#include <vnu/wintask.h>
#include <vnu/abi.h>
#include <vnu/elf.h>
#include <vnu/process.h>
#include <vnu/paging.h>
#include <vnu/pmm.h>

extern "C" void vnu_swtch_pd(uint32_t* old_esp_out, uint32_t new_esp);
extern "C" void vnu_wintask_trampoline();
extern "C" uint32_t vnu_wintask_pending_entry, vnu_wintask_pending_stack;
extern "C" uint32_t vnu_wintask_new_pd, vnu_wintask_old_pd;

namespace {

using vnu::wintask::CON_COLS;
using vnu::wintask::CON_ROWS;
using vnu::wintask::SCROLL_ROWS;
using vnu::wintask::Console;
using vnu::wintask::TITLE_CAP;
using vnu::wintask::INPUT_QUEUE_CAP;
using vnu::wintask::MAX_TASKS;

/* App image / stack / heap layout (per task; each is backed by private
 * physical frames in the task's own address space). Same addresses the
 * apps were always linked for. */
constexpr uint32_t APP_BASE = 0x00400000;
constexpr uint32_t APP_PAGES = 64; /* 256 KiB app code+data+.bss
                                      (a gfx app's fb is 480x340=159 KiB) */
constexpr uint32_t STACK_BASE = 0x00900000;
constexpr uint32_t STACK_SIZE = 0x00010000;
constexpr uint32_t STACK_TOP = STACK_BASE + STACK_SIZE;

enum class State : uint8_t { Unused, Runnable, Blocked, Done };

struct Task {
    State state = State::Unused;
    uint32_t saved_esp = 0;   /* this task's parked stack pointer */
    uint32_t gui_saved_esp = 0; /* where the GUI stack parked while we ran */
    uint32_t pgdir = 0;       /* this task's private page directory */
    Console con{};
    /* The program this task was spawned with (the desktop icon target),
     * copied into PMM-backed storage so it survives other spawns. Kept
     * so `term`'s shell can come back after a command exits. */
    uint32_t root_base = 0;
    uint32_t root_size = 0;
    char root_path[64]{};
    bool child_running = false;
    uint32_t brk = 0; /* per-task heap break (SYS_brk bookkeeping) */
};

/* The whole task table lives in PMM frames. .bss ends at ~0x3F8000 and
 * each task's Console holds a 163200-byte pixel buffer (480x340), so
 * (~160 KiB × MAX_TASKS) simply doesn't fit below the 4 MiB line. The
 * PMM pool (20-30 MiB) is identity-mapped in *every* page directory, so
 * GUI and tasks alike can always reach this array. It's one contiguous
 * block so we can index it as an array. */
Task* g_tasks = nullptr;

uint32_t g_base_pgdir = 0; /* page directory the GUI's own stack lives in */
bool g_active = false;     /* true only while control is inside a task */
int g_active_index = -1;

int str_len(const char* s)
{
    int n = 0;
    while (s && s[n])
        ++n;
    return n;
}

void str_copy(char* d, const char* s, int cap)
{
    int i = 0;
    for (; s && s[i] && i < cap - 1; ++i)
        d[i] = s[i];
    d[i] = 0;
}

Task& cur_task()
{
    return g_tasks[g_active_index];
}

/* Byte/word stores into a *different* address space: resolve the
 * physical frame backing `vaddr` in `pgdir` and write through its
 * identity-mapped address. Used to load a new task's image and build
 * its argv stack without ever switching CR3 in the GUI's context. */
inline void phys_store(uint32_t pgdir, uint32_t vaddr, uint8_t v)
{
    uint32_t pa = vnu::paging::phys_frame_at(pgdir, vaddr & ~0xFFFu) + (vaddr & 0xFFF);
    *reinterpret_cast<uint8_t*>(pa) = v;
}

inline void phys_store32(uint32_t pgdir, uint32_t vaddr, uint32_t v)
{
    uint32_t pa = vnu::paging::phys_frame_at(pgdir, vaddr & ~0xFFFu) + (vaddr & 0xFFF);
    *reinterpret_cast<uint32_t*>(pa) = v;
}

/* Same argc/argv layout as build_argv_frame below (which mirrors
 * proc/process.cpp's setup_user_stack), but written into the private
 * frames of the address space `pgdir` via phys_store. Returns the
 * resulting (virtual) stack pointer. */
uint32_t build_argv_frame_into(uint32_t pgdir, uint32_t stack_top, int argc,
                               char* const* argv, const char* path0)
{
    if (argc < 1)
        argc = 1;
    if (argc > 8)
        argc = 8;

    const char* a0 = path0 ? path0 : (argv && argv[0] ? argv[0] : "/bin/unknown");
    uint32_t sp = stack_top;
    uint32_t argv_addr[9] = {};

    for (int i = 0; i < argc; ++i) {
        const char* s = (i == 0) ? a0 : (argv && argv[i] ? argv[i] : "");
        int len = str_len(s) + 1;
        sp -= static_cast<uint32_t>((len + 3) & ~3u);
        for (int c = 0; c < len; ++c)
            phys_store(pgdir, sp + static_cast<uint32_t>(c), static_cast<uint8_t>(s[c]));
        argv_addr[i] = sp;
    }

    sp -= 4;
    phys_store32(pgdir, sp, 0); /* envp NULL */
    sp -= 4;
    phys_store32(pgdir, sp, 0); /* argv NULL terminator */
    for (int i = argc - 1; i >= 0; --i) {
        sp -= 4;
        phys_store32(pgdir, sp, argv_addr[i]);
    }
    sp -= 4;
    phys_store32(pgdir, sp, static_cast<uint32_t>(argc));
    return sp;
}

/* Same layout, but writes go straight to the CURRENT address space
 * (the executing task's own, for exit_respawns_root/exec_current). */
uint32_t build_argv_frame(uint32_t stack_top, int argc, char* const* argv, const char* path0)
{
    if (argc < 1)
        argc = 1;
    if (argc > 8)
        argc = 8;

    const char* a0 = path0 ? path0 : (argv && argv[0] ? argv[0] : "/bin/unknown");
    uint8_t* sp = reinterpret_cast<uint8_t*>(stack_top);
    uint32_t argv_addr[9] = {};

    for (int i = 0; i < argc; ++i) {
        const char* s = (i == 0) ? a0 : (argv && argv[i] ? argv[i] : "");
        int len = str_len(s) + 1;
        sp -= (len + 3) & ~3u;
        for (int c = 0; c < len; ++c)
            sp[c] = static_cast<uint8_t>(s[c]);
        argv_addr[i] = reinterpret_cast<uint32_t>(sp);
    }

    sp -= 4;
    *reinterpret_cast<uint32_t*>(sp) = 0; /* envp NULL */
    sp -= 4;
    *reinterpret_cast<uint32_t*>(sp) = 0; /* argv NULL terminator */
    for (int i = argc - 1; i >= 0; --i) {
        sp -= 4;
        *reinterpret_cast<uint32_t*>(sp) = argv_addr[i];
    }
    sp -= 4;
    *reinterpret_cast<uint32_t*>(sp) = static_cast<uint32_t>(argc);
    return reinterpret_cast<uint32_t>(sp);
}

/* ELF loader variant for a not-yet-running task: writes segments into
 * the private frames of `pgdir` instead of the current address space. */
uint32_t elf_load_into(uint32_t pgdir, const uint8_t* image, uint32_t size)
{
    using namespace vnu::elf;
    if (size < sizeof(Ehdr))
        return 0;
    const Ehdr* eh = reinterpret_cast<const Ehdr*>(image);
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_386)
        return 0;
    if (eh->e_phoff + static_cast<uint32_t>(eh->e_phnum) * sizeof(Phdr) > size)
        return 0;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const Phdr* ph = reinterpret_cast<const Phdr*>(image + eh->e_phoff + i * sizeof(Phdr));
        if (ph->p_type != PT_LOAD)
            continue;
        for (uint32_t k = 0; k < ph->p_memsz; ++k) {
            uint32_t va = ph->p_vaddr + k;
            uint32_t pa = vnu::paging::phys_frame_at(pgdir, va & ~0xFFFu);
            if (!pa)
                return 0;
            pa += va & 0xFFF;
            *reinterpret_cast<uint8_t*>(pa) = (k < ph->p_filesz) ? image[ph->p_offset + k] : 0;
        }
    }
    return eh->e_entry;
}

void setup_switch_frame(uint32_t pgdir, Task& t)
{
    /* Craft the initial swtch stack just below the argv frame: five
     * words that vnu_swtch_pd's epilogue will pop as if a prior call
     * had pushed ebp/ebx/esi/edi and a return address — landing on the
     * trampoline the first time this task is switched into. */
    const uint32_t fbase = STACK_BASE + 0x1000;
    uint32_t va = fbase - 20;
    phys_store32(pgdir, va + 0, 0); /* edi */
    phys_store32(pgdir, va + 4, 0); /* esi */
    phys_store32(pgdir, va + 8, 0); /* ebx */
    phys_store32(pgdir, va + 12, 0); /* ebp */
    phys_store32(pgdir, va + 16, reinterpret_cast<uint32_t>(&vnu_wintask_trampoline));
    t.saved_esp = va;
}

void console_putc(Console& c, char ch)
{
    if (ch == '\n') {
        c.cur_col = 0;
        ++c.cur_row;
    } else if (ch == '\r') {
        c.cur_col = 0;
    } else if (ch == '\b') {
        if (c.cur_col > 0)
            --c.cur_col;
    } else {
        if (c.cur_col >= CON_COLS) {
            c.cur_col = 0;
            ++c.cur_row;
        }
        c.cell[c.cur_row][c.cur_col] = ch;
        ++c.cur_col;
    }
    int overflow = c.cur_row - (CON_ROWS - 1);
    if (overflow > 0) {
        /* Park the overflowed row(s) in the scrollback ring. */
        for (int k = 0; k < overflow; ++k) {
            int dst = c.hist_tail;
            for (int cc = 0; cc < CON_COLS; ++cc)
                c.hist[dst][cc] = c.cell[0][cc];
            c.hist_tail = (c.hist_tail + 1) % SCROLL_ROWS;
            if (c.hist_n < SCROLL_ROWS)
                ++c.hist_n;
            if (c.scroll_off > 0)
                ++c.scroll_off;
        }
        for (int r = 1; r < CON_ROWS; ++r)
            for (int cc = 0; cc < CON_COLS; ++cc)
                c.cell[r - 1][cc] = c.cell[r][cc];
        for (int cc = 0; cc < CON_COLS; ++cc)
            c.cell[CON_ROWS - 1][cc] = ' ';
        c.cur_row = CON_ROWS - 1;
    }
}

} // namespace

namespace vnu::wintask {

void init()
{
    if (!g_tasks) {
        uint32_t need = static_cast<uint32_t>(sizeof(Task)) * static_cast<uint32_t>(MAX_TASKS);
        uint32_t pages = (need + 0xFFFu) >> 12;
        uint32_t base = vnu::pmm::alloc_contig(pages);
        g_tasks = base ? reinterpret_cast<Task*>(base) : nullptr;
    }
    if (g_tasks)
        for (int i = 0; i < MAX_TASKS; ++i)
            g_tasks[i] = Task{};
    {
        uint32_t real_cr3;
        asm volatile("mov %%cr3, %0" : "=r"(real_cr3));
        g_base_pgdir = real_cr3;
    }
    g_active = false;
    g_active_index = -1;
}

TaskHandle spawn(const char* argv0_path, const uint8_t* data, uint32_t size, const char* title)
{
    if (!data || size == 0 || !g_tasks)
        return NO_TASK;

    int h = -1;
    for (int i = 0; i < MAX_TASKS; ++i)
        if (g_tasks[i].state == State::Unused) {
            h = i;
            break;
        }
    if (h < 0)
        return NO_TASK;

    uint32_t root_pages = (size + 0xFFFu) >> 12;
    uint32_t root_base = vnu::pmm::alloc_contig(root_pages);
    if (!root_base)
        return NO_TASK;
    for (uint32_t i = 0; i < size; ++i)
        *reinterpret_cast<uint8_t*>(root_base + i) = data[i];

    vnu::paging::MapRange ranges[3] = {
        {APP_BASE, APP_PAGES},
        {STACK_BASE, STACK_SIZE / vnu::paging::PAGE_SIZE},
        {vnu::proc::BRK_MIN,
         (vnu::proc::BRK_MAX - vnu::proc::BRK_MIN) / vnu::paging::PAGE_SIZE},
    };
    uint32_t pgdir = vnu::paging::create_address_space(ranges, 3);
    if (!pgdir) {
        vnu::pmm::free_contig(root_base, root_pages);
        return NO_TASK;
    }

    uint32_t entry = elf_load_into(pgdir, reinterpret_cast<const uint8_t*>(root_base), size);
    if (!entry) {
        vnu::paging::destroy_address_space(pgdir);
        vnu::pmm::free_contig(root_base, root_pages);
        return NO_TASK;
    }

    Task& t = g_tasks[h];
    t = Task{};
    for (auto& row : t.con.cell)
        for (auto& ch : row)
            ch = ' ';
    str_copy(t.con.title, title, TITLE_CAP);
    t.pgdir = pgdir;
    t.root_base = root_base;
    t.root_size = size;
    str_copy(t.root_path, argv0_path, sizeof(t.root_path));
    t.child_running = false;
    t.brk = vnu::proc::BRK_MIN;

    uint32_t user_esp = build_argv_frame_into(pgdir, STACK_TOP, 1, nullptr, argv0_path);
    setup_switch_frame(pgdir, t);

    vnu_wintask_pending_entry = entry;
    vnu_wintask_pending_stack = user_esp;

    t.state = State::Runnable;
    return h;
}

void close_task(TaskHandle h)
{
    if (h < 0 || h >= MAX_TASKS || !g_tasks)
        return;
    Task& t = g_tasks[h];
    if (t.state == State::Unused)
        return;
    if (t.pgdir)
        vnu::paging::destroy_address_space(t.pgdir);
    if (t.root_base) {
        uint32_t pages = (t.root_size + 0xFFFu) >> 12;
        vnu::pmm::free_contig(t.root_base, pages);
    }
    t = Task{};
}

bool is_running(TaskHandle h)
{
    if (h < 0 || h >= MAX_TASKS || !g_tasks)
        return false;
    State s = g_tasks[h].state;
    return s == State::Runnable || s == State::Blocked;
}

bool has_window(TaskHandle h)
{
    if (h < 0 || h >= MAX_TASKS || !g_tasks)
        return false;
    return g_tasks[h].state != State::Unused;
}

Console* console(TaskHandle h)
{
    if (h < 0 || h >= MAX_TASKS || !g_tasks)
        return nullptr;
    return has_window(h) ? &g_tasks[h].con : nullptr;
}

int live_count()
{
    if (!g_tasks)
        return 0;
    int n = 0;
    for (int i = 0; i < MAX_TASKS; ++i)
        if (g_tasks[i].state != State::Unused)
            ++n;
    return n;
}

void run_slice(TaskHandle h)
{
    if (h < 0 || h >= MAX_TASKS || !g_tasks)
        return;
    Task& t = g_tasks[h];
    if (!(t.state == State::Runnable || t.state == State::Blocked))
        return;
    vnu_wintask_new_pd = t.pgdir;
    g_active = true;
    g_active_index = h;
    vnu_swtch_pd(&t.gui_saved_esp, t.saved_esp);
    g_active = false;
    g_active_index = -1;
}

void run_all_slices()
{
    if (!g_tasks)
        return;
    for (int i = 0; i < MAX_TASKS; ++i)
        if (g_tasks[i].state == State::Runnable || g_tasks[i].state == State::Blocked)
            run_slice(i);
}

void heartbeat_gfx_tasks()
{
    if (!g_tasks)
        return;
    for (int i = 0; i < MAX_TASKS; ++i) {
        Task& t = g_tasks[i];
        if (!(t.state == State::Runnable || t.state == State::Blocked))
            continue;
        if (!t.con.gfx)
            continue; /* the tick is for pixel apps, not text consoles */
        Console& c = t.con;
        int next = (c.in_tail + 1) % INPUT_QUEUE_CAP;
        if (next == c.in_head)
            continue; /* queue full, drop the tick rather than a real key */
        c.input[c.in_tail] = TICK_BYTE;
        c.in_tail = next;
    }
}

void feed_input(TaskHandle h, char ch)
{
    if (!is_running(h))
        return;
    Console& c = g_tasks[h].con;
    int next = (c.in_tail + 1) % INPUT_QUEUE_CAP;
    if (next == c.in_head)
        return; /* queue full, drop */
    c.input[c.in_tail] = ch;
    c.in_tail = next;
}

void feed_mouse(TaskHandle h, int button, int px, int py)
{
    if (!is_running(h))
        return;
    Console& c = g_tasks[h].con;
    int free_space = (c.in_head + INPUT_QUEUE_CAP - c.in_tail - 1) % INPUT_QUEUE_CAP;
    if (free_space < MOUSE_MSG_LEN)
        return;
    auto push = [&](char ch) {
        c.input[c.in_tail] = ch;
        c.in_tail = (c.in_tail + 1) % INPUT_QUEUE_CAP;
    };
    push(MOUSE_ESC);
    push('[');
    push('M');
    push(static_cast<char>(button));
    push(static_cast<char>(px & 0xFF));         /* little-endian 16-bit */
    push(static_cast<char>((px >> 8) & 0xFF));
    push(static_cast<char>(py & 0xFF));
    push(static_cast<char>((py >> 8) & 0xFF));
}

/* --- Gfx surface: fd 3 writes fill the pixel framebuffer --- */

uint32_t task_gfx_write(const char* buf, uint32_t n)
{
    if (!g_active || n == 0 || !buf)
        return 0;
    Console& c = cur_task().con;
    c.gfx = true;
    constexpr uint32_t buf_size = static_cast<uint32_t>(GFX_W) * GFX_H;
    if (c.gfx_cursor >= buf_size)
        return 0;
    uint32_t room = buf_size - c.gfx_cursor;
    uint32_t count = n < room ? n : room;
    for (uint32_t i = 0; i < count; ++i)
        c.pixel[c.gfx_cursor + i] = static_cast<uint8_t>(buf[i]);
    c.gfx_cursor += count;
    return count;
}

int task_gfx_seek(int32_t offset, int whence)
{
    if (!g_active)
        return -1;
    Console& c = cur_task().con;
    constexpr uint32_t buf_size = static_cast<uint32_t>(GFX_W) * GFX_H;
    int32_t new_pos = static_cast<int32_t>(c.gfx_cursor);
    switch (whence) {
    case 0:
        new_pos = offset;
        break;
    case 1:
        new_pos += offset;
        break;
    case 2:
        new_pos = static_cast<int32_t>(buf_size) + offset;
        break;
    default:
        return -1;
    }
    if (new_pos < 0)
        new_pos = 0;
    if (static_cast<uint32_t>(new_pos) > buf_size)
        new_pos = static_cast<int32_t>(buf_size);
    c.gfx_cursor = static_cast<uint32_t>(new_pos);
    return new_pos;
}

uint32_t task_brk(uint32_t req)
{
    if (!g_active)
        return 0;
    Task& t = cur_task();
    if (req == 0)
        return t.brk;
    if (req < vnu::proc::BRK_MIN || req > vnu::proc::BRK_MAX)
        return static_cast<uint32_t>(-VNU_ENOMEM);
    t.brk = req;
    return req;
}

} // namespace vnu::wintask

/* --- Syscall-facing hooks, called directly from
 * kernel/arch/i386/syscall/syscall.cpp. --- */
namespace vnu::wintask {

bool current_is_task()
{
    return g_active;
}

void task_write(const char* buf, uint32_t n)
{
    if (!g_active)
        return;
    Console& c = cur_task().con;
    for (uint32_t i = 0; i < n; ++i)
        console_putc(c, buf[i]);
}

char task_getch_blocking()
{
    Task& t = cur_task();
    Console& c = t.con;
    for (;;) {
        if (c.in_head != c.in_tail) {
            char ch = c.input[c.in_head];
            c.in_head = (c.in_head + 1) % INPUT_QUEUE_CAP;
            return ch;
        }
        /* Nothing to read — yield back to the GUI's own stack (parked
         * in run_slice()), switching back to the GUI's page directory
         * so its stack (private to the console process) is visible
         * again. */
        t.state = State::Blocked;
        vnu_wintask_new_pd = g_base_pgdir;
        vnu_swtch_pd(&t.saved_esp, t.gui_saved_esp);
        t.state = State::Runnable;
    }
}

bool exit_respawns_root(uint32_t& out_eip, uint32_t& out_esp)
{
    Task& t = cur_task();
    if (!t.child_running || !t.root_base)
        return false;

    uint32_t entry = vnu::elf::load(reinterpret_cast<const uint8_t*>(t.root_base), t.root_size);
    if (!entry)
        return false;

    char path_copy[64];
    str_copy(path_copy, t.root_path, sizeof(path_copy));
    out_eip = entry;
    out_esp = build_argv_frame(STACK_TOP, 1, nullptr, path_copy);
    t.child_running = false;
    return true;
}

void task_exit()
{
    Task& t = cur_task();
    t.state = State::Done;
    /* One-way trip back to the GUI — this task's stack is abandoned
     * for good, matching how the classic process path abandons a
     * finished process's stack too. */
    vnu_wintask_new_pd = g_base_pgdir;
    uint32_t dummy;
    vnu_swtch_pd(&dummy, t.gui_saved_esp);
    for (;;) {
    } /* unreachable: defensive only, vnu_swtch_pd never returns here */
}

bool exec_current(const char* path, char* const* argv, uint32_t& out_eip, uint32_t& out_esp)
{
    uint32_t size = 0;
    const uint8_t* data = vnu::proc::find_embedded_data(path, size);
    if (!data)
        return false;

    char arg_store[8][96];
    char* arg_ptrs[9];
    int argc = 0;
    if (argv) {
        while (argv[argc] && argc < 8) {
            str_copy(arg_store[argc], argv[argc], sizeof(arg_store[argc]));
            arg_ptrs[argc] = arg_store[argc];
            ++argc;
        }
    }
    if (argc == 0) {
        str_copy(arg_store[0], path, sizeof(arg_store[0]));
        arg_ptrs[0] = arg_store[0];
        argc = 1;
    }
    arg_ptrs[argc] = nullptr;

    char path_copy[96];
    str_copy(path_copy, path, sizeof(path_copy));

    uint32_t entry = vnu::elf::load(data, size);
    if (!entry)
        return false;

    out_eip = entry;
    out_esp = build_argv_frame(STACK_TOP, argc, arg_ptrs, path_copy);
    cur_task().child_running = true;
    return true;
}

} // namespace vnu::wintask