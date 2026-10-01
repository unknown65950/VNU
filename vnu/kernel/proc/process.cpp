#include <vnu/process.h>
#include <vnu/elf.h>
#include <vnu/images.h>
#include <vnu/abi.h>
#include <vnu/paging.h>
#include <vnu/pmm.h>
#include <vnu/vga_gfx.h>   /* console_tick(): the console on a pixelless display */
#include <vnu/vfs.h>
#include <vnu/posix.h>
#include "embedded_vash.h"
#include "embedded_hello.h"
#include "embedded_vedit.h"
#include "embedded_ttytest.h"
#include "embedded_man.h"
#include "embedded_echo.h"
#include "embedded_true.h"
#include "embedded_false.h"
#include "embedded_pwd.h"
#include "embedded_cat.h"
#include "embedded_ls.h"
#include "embedded_mkdir.h"
#include "embedded_rm.h"
#include "embedded_touch.h"
#include "embedded_uname.h"
#include "embedded_wallpaper.h"
#include "embedded_clear.h"
#include "embedded_wc.h"
#include "embedded_head.h"
#include "embedded_tail.h"
#include "embedded_grep.h"
#include "embedded_sort.h"
#include "embedded_cp.h"
#include "embedded_mv.h"
#include "embedded_basename.h"
#include "embedded_dirname.h"
#include "embedded_seq.h"
#include "embedded_man.h"
#include "embedded_vibecommander.h"
#include "embedded_sticky.h"
#include "embedded_df.h"
#include "embedded_ping.h"
#include "embedded_id.h"
#include "embedded_whoami.h"
#include "embedded_groups.h"
#include "embedded_useradd.h"
#include "embedded_passwd.h"
#include "embedded_su.h"
#include "embedded_vprobe.h"
#include "embedded_tlsdemo.h"
#include "embedded_echoserver.h"
#include "embedded_tlsserver.h"
#include "embedded_forkdemo.h"
#include "embedded_vcc.h"
#include "embedded_vnu.h"

extern "C" void vnu_proc_switch(uint32_t* old_esp_out, uint32_t new_esp);
extern "C" void vnu_proc_trampoline();
extern "C" uint32_t vnu_proc_pending_entry, vnu_proc_pending_stack;
extern "C" uint32_t vnu_proc_new_pd, vnu_proc_old_pd;
extern "C" void vnu_proc_preempt(uint32_t sched_esp);
extern "C" void vnu_proc_resume_preempted(uint32_t* old_esp_out, uint32_t new_esp);
extern "C" void vnu_timer_init();

/* Largest image the execve-from-VFS path will load (the app region cap).
 * The staging buffer is taken from the PMM for the file's own size and
 * given back as soon as the image is loaded: it used to be a 1 MiB
 * vnu_kalloc(), which is the bump arena the image decoders in
 * gui/wallpaper.cpp share through px.h - two execs of a VFS binary
 * filled it, and every later one failed for want of memory. */
constexpr uint32_t EXEC_FILE_CAP = 0x100000u;

namespace {

vnu::proc::Process table[vnu::proc::MAX_PROCS];
int current_idx = 0;
int next_pid = 1;
bool console_session = false;

struct EmbeddedProg {
    const char* path;
    const uint8_t* data;
    uint32_t size;
};

/* Each command is its own embedded ELF. Only vash is re-used under
 * several names (/bin/vash, /sbin/init, /bin/sh) — a login shell by
 * any other name. */
const EmbeddedProg embedded[] = {
    {"/bin/vash", embedded_vash_elf, embedded_vash_elf_size},
    {"/sbin/init", embedded_vash_elf, embedded_vash_elf_size},
    {"/bin/sh", embedded_vash_elf, embedded_vash_elf_size},
    {"/bin/hello", embedded_hello_elf, embedded_hello_elf_size},
    {"/bin/echo", embedded_echo_elf, embedded_echo_elf_size},
    {"/bin/true", embedded_true_elf, embedded_true_elf_size},
    {"/bin/false", embedded_false_elf, embedded_false_elf_size},
    {"/bin/pwd", embedded_pwd_elf, embedded_pwd_elf_size},
    {"/bin/cat", embedded_cat_elf, embedded_cat_elf_size},
    {"/bin/ls", embedded_ls_elf, embedded_ls_elf_size},
    {"/bin/mkdir", embedded_mkdir_elf, embedded_mkdir_elf_size},
    {"/bin/rm", embedded_rm_elf, embedded_rm_elf_size},
    {"/bin/touch", embedded_touch_elf, embedded_touch_elf_size},
    {"/bin/uname", embedded_uname_elf, embedded_uname_elf_size},
    {"/bin/wallpaper", embedded_wallpaper_elf, embedded_wallpaper_elf_size},
    {"/bin/clear", embedded_clear_elf, embedded_clear_elf_size},
    {"/bin/vedit", embedded_vedit_elf, embedded_vedit_elf_size},
    {"/bin/ttytest", embedded_ttytest_elf, embedded_ttytest_elf_size},
    {"/bin/wc", embedded_wc_elf, embedded_wc_elf_size},
    {"/bin/head", embedded_head_elf, embedded_head_elf_size},
    {"/bin/tail", embedded_tail_elf, embedded_tail_elf_size},
    {"/bin/grep", embedded_grep_elf, embedded_grep_elf_size},
    {"/bin/sort", embedded_sort_elf, embedded_sort_elf_size},
    {"/bin/cp", embedded_cp_elf, embedded_cp_elf_size},
    {"/bin/mv", embedded_mv_elf, embedded_mv_elf_size},
    {"/bin/basename", embedded_basename_elf, embedded_basename_elf_size},
    {"/bin/dirname", embedded_dirname_elf, embedded_dirname_elf_size},
    {"/bin/seq", embedded_seq_elf, embedded_seq_elf_size},
    {"/bin/df", embedded_df_elf, embedded_df_elf_size},
    {"/bin/ping", embedded_ping_elf, embedded_ping_elf_size},
    {"/bin/man", embedded_man_elf, embedded_man_elf_size},
    {"/bin/vibecommander", embedded_vibecommander_elf, embedded_vibecommander_elf_size},
    {"/bin/sticky", embedded_sticky_elf, embedded_sticky_elf_size},
    {"/bin/id", embedded_id_elf, embedded_id_elf_size},
    {"/bin/whoami", embedded_whoami_elf, embedded_whoami_elf_size},
    {"/bin/groups", embedded_groups_elf, embedded_groups_elf_size},
    {"/bin/useradd", embedded_useradd_elf, embedded_useradd_elf_size},
    {"/bin/passwd", embedded_passwd_elf, embedded_passwd_elf_size},
    {"/bin/su", embedded_su_elf, embedded_su_elf_size},
    {"/bin/vprobe", embedded_vprobe_elf, embedded_vprobe_elf_size},
    {"/bin/tlsdemo", embedded_tlsdemo_elf, embedded_tlsdemo_elf_size},
    {"/bin/echoserver", embedded_echoserver_elf, embedded_echoserver_elf_size},
    {"/bin/tlsserver", embedded_tlsserver_elf, embedded_tlsserver_elf_size},
    {"/bin/forkdemo", embedded_forkdemo_elf, embedded_forkdemo_elf_size},
    {"/bin/vcc", embedded_vcc_elf, embedded_vcc_elf_size},
    {"/bin/vnu", embedded_vnu_elf, embedded_vnu_elf_size},
    /* short names for convenience */
    {"hello", embedded_hello_elf, embedded_hello_elf_size},
    {"echo", embedded_echo_elf, embedded_echo_elf_size},
    {"vcc", embedded_vcc_elf, embedded_vcc_elf_size},
    {nullptr, nullptr, 0},
};

/* Count every userspace program into the image accounting /proc/images
 * reports. Only the canonical absolute paths are counted: the short
 * aliases below are the very same ELF, and counting them twice would
 * inflate the number. */
void register_embedded_sizes()
{
    for (int i = 0; embedded[i].path; ++i) {
        if (embedded[i].path[0] == '/')
            vnu::images::add(vnu::images::Userspace, embedded[i].size);
    }
}

const EmbeddedProg* find_embedded(const char* path)
{
    if (!path)
        return nullptr;
    for (int i = 0; embedded[i].path; ++i) {
        const char* a = embedded[i].path;
        const char* b = path;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (!*a && !*b)
            return &embedded[i];
    }
    return nullptr;
}

/* Record argv[0]'s basename as the process name ("/bin/ls" -> "ls"), as
 * /proc/self/status reports it. */
void set_proc_name(vnu::proc::Process& p, const char* path)
{
    const char* base = path;
    if (path) {
        for (const char* s = path; *s; ++s) {
            if (*s == '/')
                base = s + 1;
        }
    }

    /* Prefer the canonical name of the program over the name it was
     * invoked under: /sbin/init, /bin/sh and /bin/vash are one binary
     * (one embedded blob, listed under /bin/vash first), and every
     * user of these names wants to be told "vash", not which alias
     * happened to be used. */
    const EmbeddedProg* prog = find_embedded(path);
    if (prog) {
        for (int i = 0; embedded[i].path; ++i) {
            if (embedded[i].data == prog->data) {
                base = embedded[i].path;
                for (const char* s = base; *s; ++s) {
                    if (*s == '/')
                        base = s + 1;
                }
                break;
            }
        }
    }

    int i = 0;
    for (; base && base[i] && i < static_cast<int>(sizeof(p.name)) - 1; ++i)
        p.name[i] = base[i];
    p.name[i] = 0;
    if (i == 0) {
        p.name[0] = '?';
        p.name[1] = 0;
    }
}

int alloc_slot()
{
    for (int i = 0; i < vnu::proc::MAX_PROCS; ++i)
        if (table[i].state == vnu::proc::State::Unused)
            return i;
    return -1;
}

static void copy_str(char* d, const char* s, int n)
{
    int i = 0;
    for (; s && s[i] && i < n - 1; ++i)
        d[i] = s[i];
    d[i] = 0;
}

static int str_len(const char* s)
{
    int n = 0;
    while (s && s[n])
        ++n;
    return n;
}

/* Build classic argc/argv/envp stack. argv[0] = path (or basename). */

void setup_user_stack(vnu::proc::Process& p, int argc, char* const* argv, const char* path0)
{
    if (argc < 1)
        argc = 1;
    if (argc > 8)
        argc = 8;

    const char* a0 = path0 ? path0 : (argv && argv[0] ? argv[0] : "/bin/unknown");
    uint32_t sp = p.user_stack_top;
    uint32_t argv_addr[9] = {};

    for (int i = 0; i < argc; ++i) {
        const char* s = (i == 0) ? a0 : (argv && argv[i] ? argv[i] : "");
        int len = str_len(s) + 1;
        sp -= static_cast<uint32_t>((len + 3) & ~3u);
        copy_str(reinterpret_cast<char*>(sp), s, len);
        argv_addr[i] = sp;
    }

    /* envp: single NULL */
    sp -= 4;
    *reinterpret_cast<uint32_t*>(sp) = 0;

    /* argv NULL terminator */
    sp -= 4;
    *reinterpret_cast<uint32_t*>(sp) = 0;

    /* argv pointers (reverse push so argv[0] is lowest after argc) */
    for (int i = argc - 1; i >= 0; --i) {
        sp -= 4;
        *reinterpret_cast<uint32_t*>(sp) = argv_addr[i];
    }

    /* argc */
    sp -= 4;
    *reinterpret_cast<uint32_t*>(sp) = static_cast<uint32_t>(argc);
    p.regs.esp = sp;
}

} // namespace

namespace {

/* --- Coroutine scheduler for classic processes (parallel services) ---
 *
 * Kernel-main's boot loop hands control to run_scheduler(), a coroutine
 * that round-robins "coro" processes (spawned via sys_spawn, each in
 * its own private address space). A coro process runs like a wintask:
 * it executes until it blocks — waitpid, a pipe read, or a console key
 * read — at which point yield_current() swaps back to the scheduler
 * (parking the process's ESP inside the very syscall handler that
 * blocked) and the scheduler gives the next runnable process a slice.
 * See ctxswitch.s's vnu_proc_switch (separate new/old pd globals from
 * the GUI's vnu_wintask_*) and vnu_proc_trampoline (first-run entry). */

/* Byte/word stores into a *different* address space, same helpers as
 * the wintask manager: resolve the physical frame backing `vaddr` in
 * `pgdir` and write through its identity-mapped address. Lets spawn()
 * load a new process's ELF and build its argv stack without ever
 * switching CR3 away from the kernel's own directory. */
void phys_store(uint32_t pgdir, uint32_t vaddr, uint8_t v)
{
    uint32_t pa = vnu::paging::phys_frame_at(pgdir, vaddr & ~0xFFFu) + (vaddr & 0xFFF);
    *reinterpret_cast<uint8_t*>(pa) = v;
}

void phys_store32(uint32_t pgdir, uint32_t vaddr, uint32_t v)
{
    uint32_t pa = vnu::paging::phys_frame_at(pgdir, vaddr & ~0xFFFu) + (vaddr & 0xFFF);
    *reinterpret_cast<uint32_t*>(pa) = v;
}

/* ELF loader variant for a not-yet-running process: writes segments
 * into the private frames of `pgdir` instead of the current space. */
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

/* argc==1 argv/envp frame (argv[0] = program path), written into the
 * new process's private frames. Returns the resulting stack pointer. */
uint32_t build_argv_frame_into(uint32_t pgdir, uint32_t stack_top, const char* path0)
{
    const char* a0 = path0 ? path0 : "/bin/unknown";
    uint32_t sp = stack_top;
    int len = str_len(a0) + 1;
    sp -= static_cast<uint32_t>((len + 3) & ~3u);
    for (int c = 0; c < len; ++c)
        phys_store(pgdir, sp + static_cast<uint32_t>(c), static_cast<uint8_t>(a0[c]));
    uint32_t a0_addr = sp;

    sp -= 4;
    phys_store32(pgdir, sp, 0); /* envp NULL */
    sp -= 4;
    phys_store32(pgdir, sp, 0); /* argv NULL terminator */
    sp -= 4;
    phys_store32(pgdir, sp, a0_addr);
    sp -= 4;
    phys_store32(pgdir, sp, 1); /* argc */
    return sp;
}

/* First-run switch frame just below the argv frame: what vnu_proc_switch's
 * epilogue pops on the very first time the scheduler enters this process,
 * landing the `ret` on vnu_proc_trampoline. Returns its address. */
uint32_t setup_switch_frame(uint32_t pgdir, uint32_t stack_top)
{
    uint32_t va = stack_top - 20;
    phys_store32(pgdir, va + 0, 0); /* edi */
    phys_store32(pgdir, va + 4, 0); /* esi */
    phys_store32(pgdir, va + 8, 0); /* ebx */
    phys_store32(pgdir, va + 12, 0); /* ebp */
    phys_store32(pgdir, va + 16, reinterpret_cast<uint32_t>(&vnu_proc_trampoline));
    return va;
}

uint32_t g_sched_esp = 0;   /* scheduler coroutine's parked stack (in its
                               own kernel-low stack, shared by every CR3) */
uint32_t g_sched_pgdir = 0; /* page directory the scheduler runs in */
char g_spawn_path[96];
int g_last_slot = -1;       /* slot the most recent spawn() used */
uint32_t g_jiffies = 0;     /* 100 Hz tick counter (advanced by PIT IRQ0) */

/* Wait channels of the waitpid() blocking: a parent parks on either the
 * broadcast "any child exited" channel (pid == -1 waits) or the channel
 * keyed by the specific awaited child's pid. sys_exit() wakes both. */
constexpr uint32_t WAIT_ZOMBIE_ANY = 0x01000000u;
constexpr uint32_t wait_chan_of_child(int pid)
{
    return WAIT_ZOMBIE_ANY | (static_cast<uint32_t>(pid) & 0xFFFFu);
}

/* A process that has just stopped existing (an exit, or a signal whose
 * action is to terminate it) has to wake the parent that is asleep in
 * waitpid() on it: that wait is parked on a channel only an exit here
 * knows to wake, so without this the parent would sleep until the dead
 * child's own luck ran out. */
void wake_waiter_of(int pid)
{
    vnu::proc::wakeup(WAIT_ZOMBIE_ANY);
    vnu::proc::wakeup(wait_chan_of_child(pid));
}

/* Park the current coro process (park=true) and swap to the scheduler
 * coroutine, or (park=false, exit path) abandon its stack for good. */
void switch_to_scheduler(bool park)
{
    vnu_proc_new_pd = g_sched_pgdir;
    if (park) {
        vnu::proc::Process& p = table[current_idx];
        vnu_proc_switch(&p.coro_esp, g_sched_esp);
    } else {
        uint32_t dummy;
        vnu_proc_switch(&dummy, g_sched_esp);
        for (;;) {
        } /* unreachable */
    }
}

} // namespace

namespace vnu::proc {

void init()
{
    for (int i = 0; i < MAX_PROCS; ++i) {
        table[i] = {};
        table[i].pid = 0;
        table[i].state = State::Unused;
        table[i].user_stack_top =
            USER_STACK_BASE + static_cast<uint32_t>((i + 1) * USER_STACK_SIZE);
    }
    table[0].pid = 0;
    table[0].state = State::Running;
    /* The scheduler/console pseudo-process (pid 0) is the identity
     * parent of every boot-spawned init: uid 0 → init comes up as root. */
    table[0].uid = 0;
    table[0].gid = 0;
    set_proc_name(table[0], "kernel");
    register_embedded_sizes();
    current_idx = 0;
    next_pid = 1;
    console_session = false;
}

int current_pid() { return table[current_idx].pid; }
Process* current() { return &table[current_idx]; }
/* The vash binary answers to several names (see the embedded table);
 * whichever of them is running is the shell a user sits at. */
static bool is_shell_name(const char* n)
{
    if (!n)
        return false;
    if (n[0] == 'v' && n[1] == 'a' && n[2] == 's' && n[3] == 'h' && !n[4])
        return true;
    if (n[0] == 'i' && n[1] == 'n' && n[2] == 'i' && n[3] == 't' && !n[4])
        return true;
    if (n[0] == 's' && n[1] == 'h' && !n[2])
        return true;
    return false;
}

int session_shell(char* out, int cap)
{
    if (cap < 2)
        return -1;
    out[0] = 0;

    /* Walk the parent chain, closest ancestor first. */
    Process* p = current();
    for (int guard = 0; p && guard < MAX_PROCS; ++guard) {
        if (is_shell_name(p->name)) {
            int i = 0;
            for (; p->name[i] && i < cap - 1; ++i)
                out[i] = p->name[i];
            out[i] = 0;
            return 0;
        }
        Process* parent = nullptr;
        for (int i = 0; i < MAX_PROCS; ++i) {
            if (table[i].state != State::Unused && table[i].pid == p->ppid) {
                parent = &table[i];
                break;
            }
        }
        p = parent;
    }
    return -1;
}

int sys_getpid() { return table[current_idx].pid; }

uint32_t sys_getuid() { return table[current_idx].uid; }
uint32_t sys_getgid() { return table[current_idx].gid; }

int sys_setuid(uint32_t uid)
{
    Process& p = table[current_idx];
    if (p.uid != 0 && uid != p.uid)
        return -VNU_EPERM;
    p.uid = uid;
    return 0;
}

int sys_setgid(uint32_t gid)
{
    Process& p = table[current_idx];
    if (p.uid != 0 && gid != p.gid)
        return -VNU_EPERM;
    p.gid = gid;
    return 0;
}

/* fork(2): a second process with a copy of this one's memory, its own
 * identity, and the same place in the instruction stream.
 *
 * What it used to be is worth keeping in mind. The child got a process
 * slot, a pid and a copy of the registers, and nothing else: no address
 * space of its own (so both "processes" wrote the same memory), and no
 * coroutine to run it in, so the only way it ever ran was sys_exit()
 * handing the *parent* its saved registers back - a fork that could
 * not overlap the caller, and whose child existed only to be waited
 * for. It is now a real one:
 *
 *   - its own address space, built exactly like a spawned process's
 *     (app region, a stack, the heap) and then *filled in* from the
 *     parent's frames: fork is a copy, not a shared mapping, so a
 *     write in one process is invisible in the other;
 *   - its own stack region at the *same* virtual address as the
 *     parent's - the child resumes with the parent's esp, so its copy
 *     of the stack has to be where the copy of esp points. The frames
 *     are private, and only one process runs at a time, so the shared
 *     address costs nothing (it does mean the stack's address no
 *     longer names the slot, which nothing depends on);
 *   - a first slice that needs no trampoline: the frame the CPU is
 *     leaving right now - the syscall's own pushad + interrupt frame,
 *     on the process's stack, and therefore already inside the memory
 *     being copied - *is* the child's saved state. Switching into the
 *     child with preempted=true pops it and irets straight back into
 *     user mode at the instruction after int $0x80, which is the same
 *     path a preempted process takes. Only the child's copy of the
 *     return value is changed, to the 0 fork() promises.
 */
int sys_fork(Registers* trap)
{
    Process& parent = table[current_idx];
    /* The legacy one-way path (a windowed task, or a program run
     * straight from a file with no scheduler behind it) has no second
     * place to run a process, and no address space of its own to copy.
     * -EAGAIN rather than a process that can never be scheduled. */
    if (!parent.coro || !parent.pgdir_phys || !trap->frame)
        return -VNU_EAGAIN;
    int slot = alloc_slot();
    if (slot <= 0) /* slot 0 is the scheduler/console process */
        return -VNU_ENOMEM;

    const uint32_t stack_base = parent.user_stack_top - USER_STACK_SIZE;
    const uint32_t stack_pages = USER_STACK_SIZE / vnu::paging::PAGE_SIZE;
    const uint32_t heap_pages = (BRK_MAX - BRK_MIN) / vnu::paging::PAGE_SIZE;
    /* vlibc's malloc() claims the whole 1 MiB in one brk() on its
     * first call, so a break past BRK_MIN means the whole heap region
     * is the parent's to hand over. A process that never allocated
     * forks for its app pages and its stack alone (64 KiB), not 1.1 MiB. */
    const uint32_t heap_in_use = (parent.brk > BRK_MIN) ? heap_pages : 0;
    vnu::paging::MapRange ranges[3] = {
        {0x00400000, parent.app_pages},
        {stack_base, stack_pages},
        {BRK_MIN, heap_in_use},
    };
    uint32_t pgdir = vnu::paging::create_address_space(ranges, heap_in_use ? 3 : 2);
    if (!pgdir)
        return -VNU_ENOMEM;

    const uint32_t src = parent.pgdir_phys;
    bool ok = vnu::paging::copy_pages(pgdir, src, 0x00400000, parent.app_pages) &&
              vnu::paging::copy_pages(pgdir, src, stack_base, stack_pages) &&
              (heap_in_use == 0 ||
               vnu::paging::copy_pages(pgdir, src, BRK_MIN, heap_in_use));
    if (!ok) {
        vnu::paging::destroy_address_space(pgdir);
        return -VNU_ENOMEM;
    }
    /* The child's own copy of the frame, with fork()'s return value:
     * eax sits 28 bytes into a pushad frame (edi esi ebp esp(ignored)
     * ebx edx ecx eax). The parent's frame is untouched - it is about
     * to be popped with the pid in eax. */
    phys_store32(pgdir, trap->frame + 28, 0);

    Process& child = table[slot];
    child = {};
    child.pid = next_pid++;
    child.ppid = parent.pid;
    child.state = State::Runnable;
    child.uid = parent.uid;
    child.gid = parent.gid;
    child.pgdir_phys = pgdir;
    child.user_stack_top = parent.user_stack_top;
    child.brk = parent.brk;
    child.app_pages = parent.app_pages;
    child.coro = true;
    child.started = true;
    child.preempted = true;   /* the frame above is a full one: popad+iret */
    child.coro_esp = trap->frame;
    child.regs = *trap;       /* for /proc's sake; the resume path above */
    child.regs.eax = 0;
    child.regs.esp = trap->frame + 44; /* where the iret leaves it */
    child.regs.eflags = trap->eflags ? trap->eflags : 0x202;
    copy_str(child.name, parent.name, sizeof(child.name));
    return child.pid;
}

/* How many 4 KiB pages does an ELF payload span in the app region,
 * starting at 0x400000? */
uint32_t elf_need_pages(const uint8_t* image, uint32_t size);

int sys_execve(Registers* trap, const char* path, char* const* argv)
{
    const EmbeddedProg* prog = find_embedded(path);

    /* Snapshot argv strings BEFORE load() overwrites userspace at 0x400000. */
    char arg_store[8][96];
    char* arg_ptrs[9];
    int argc = 0;
    if (argv) {
        while (argv[argc] && argc < 8) {
            const char* s = argv[argc];
            int j = 0;
            for (; s[j] && j < 95; ++j)
                arg_store[argc][j] = s[j];
            arg_store[argc][j] = 0;
            arg_ptrs[argc] = arg_store[argc];
            ++argc;
        }
    }
    if (argc == 0) {
        int j = 0;
        for (; path && path[j] && j < 95; ++j)
            arg_store[0][j] = path[j];
        arg_store[0][j] = 0;
        arg_ptrs[0] = arg_store[0];
        argc = 1;
    }
    arg_ptrs[argc] = 0;

    char path_copy[96];
    {
        int j = 0;
        for (; path && path[j] && j < 95; ++j)
            path_copy[j] = path[j];
        path_copy[j] = 0;
    }

    /* execve of a real VFS file — a guest-compiled binary, for example.
     * Read it into a kernel staging buffer (grown into the process's
     * app region first if the new image is bigger than the old one),
     * then load it in place just like the embedded path. */
    uint8_t* image = nullptr;
    uint32_t image_frames = 0;
    uint32_t size = 0;
    if (!prog) {
        int fd = vnu::vfs::open(path, static_cast<int>(vnu::posix::O_RDONLY));
        if (fd < 0)
            return -VNU_ENOENT;
        /* The file's own size, so a 6 KiB program stages in two frames
         * instead of the 256 the cap would ask for. */
        vnu::posix::Stat st;
        int sr = vnu::vfs::fstat(fd, &st);
        if (sr != 0 || st.size < sizeof(vnu::elf::Ehdr) ||
            st.size > EXEC_FILE_CAP) {
            vnu::vfs::close(fd);
            return -VNU_ENOEXEC;
        }
        size = st.size;
        image_frames = (size + vnu::pmm::FRAME_SIZE - 1u) / vnu::pmm::FRAME_SIZE;
        const uint32_t phys = vnu::pmm::alloc_contig(image_frames);
        if (!phys) {
            vnu::vfs::close(fd);
            return -VNU_ENOMEM;
        }
        /* Identity-mapped by paging::init(), so the run is usable as a
         * pointer here and stays mapped in the new process. */
        image = reinterpret_cast<uint8_t*>(phys);
        uint32_t n = 0;
        while (n < size) {
            int r = vnu::vfs::read(fd, image + n, size - n);
            if (r <= 0)
                break;
            n += static_cast<uint32_t>(r);
        }
        vnu::vfs::close(fd);
        if (n != size) {
            vnu::pmm::free_contig(phys, image_frames);
            return -VNU_ENOEXEC;
        }

        Process& cp = table[current_idx];
        uint32_t need = elf_need_pages(image, size);
        if (cp.pgdir_phys && need > cp.app_pages) {
            if (!vnu::paging::extend_address_space(
                    cp.pgdir_phys,
                    0x400000u + cp.app_pages * vnu::paging::PAGE_SIZE,
                    need - cp.app_pages)) {
                vnu::pmm::free_contig(phys, image_frames);
                return -VNU_ENOMEM;
            }
            cp.app_pages = need;
        }
    }

    uint32_t entry;
    if (prog) {
        entry = vnu::elf::load(prog->data, prog->size);
    } else {
        entry = vnu::elf::load(image, size);
        /* The segments are mapped now; the staging run is dead weight
         * from here on, and the pool is the one place a big binary's
         * read buffer can come from without being lost for good. */
        vnu::pmm::free_contig(reinterpret_cast<uint32_t>(image),
                              image_frames);
        image = nullptr;
    }
    if (!entry)
        return -VNU_ENOEXEC;

    Process& p = table[current_idx];
    p.regs = {};
    p.regs.eip = entry;
    p.regs.eflags = 0x202;
    setup_user_stack(p, argc, arg_ptrs, path_copy);
    /* What POSIX says execve does to signals, and why each half is the
     * way it is: the new image is a new program, so what it does about
     * a signal is its own business and every handler goes back to the
     * default action (a handler address in the old image is not even a
     * valid address in the new one) - but the mask is the process's,
     * not the program's, and stays. The alarm belongs to the old
     * program and its deadline goes with it, and no handler is running
     * across the image change, so there is no frame to give back. */
    for (int i = 0; i < VNU_NSIG; ++i)
        p.sig_action[i] = {};
    p.sig_frame = 0;
    p.sig_saved_mask = 0;
    p.alarm_at = 0;

    trap->eip = p.regs.eip;
    trap->esp = p.regs.esp;
    trap->eflags = p.regs.eflags;
    trap->eax = 0;
    trap->ebx = trap->ecx = trap->edx = 0;
    trap->esi = trap->edi = trap->ebp = 0;
    return 0;
}

extern "C" void vnu_enter_user(uint32_t entry, uint32_t user_esp);

static int enter_program(const char* path)
{
    const EmbeddedProg* prog = find_embedded(path);
    if (!prog)
        return -VNU_ENOENT;
    return run_program_from_memory(path, prog->data, prog->size);
}

void sys_exit(int status)
{
    Process& p = table[current_idx];
    p.exit_code = status;

    if (p.coro) {
        /* Scheduler-managed process: turn into a zombie for the parent
         * (init) to reap via waitpid, and hand control back to the
         * scheduler coroutine — its stack is abandoned for good. */
        p.state = State::Zombie;
        wake_waiter_of(p.pid);
        switch_to_scheduler(false);
        return; /* not reached */
    }

    int ppid = p.ppid;

    if (console_session && ppid == 0) {
        p.state = State::Unused;
        console_session = false;
        current_idx = 0;
        table[0].state = State::Running;
        return;
    }

    p.state = State::Zombie;

    for (int i = 0; i < MAX_PROCS; ++i) {
        if (table[i].pid == ppid &&
            (table[i].state == State::Runnable || table[i].state == State::Running)) {
            current_idx = i;
            table[i].state = State::Running;
            return;
        }
    }
    for (int i = 0; i < MAX_PROCS; ++i) {
        if (table[i].state == State::Runnable) {
            current_idx = i;
            table[i].state = State::Running;
            return;
        }
    }
    current_idx = 0;
    table[0].state = State::Running;
}

int sys_waitpid(int pid, int* status, int options)
{
    int self = table[current_idx].pid;
    for (;;) {
        /* A signal takes the process out of the wait, not just out of
         * this pass: POSIX has waitpid() fail with EINTR so the program
         * can decide what to do about the signal that woke it. */
        if (interrupt_pending())
            return -VNU_EINTR;
        for (int i = 0; i < MAX_PROCS; ++i) {
            Process& c = table[i];
            if (c.state != State::Zombie)
                continue;
            if (c.ppid != self)
                continue;
            if (pid != -1 && c.pid != pid)
                continue;
            if (status)
                *status = c.exit_code;
            int r = c.pid;
            if (c.coro)
                vnu::paging::destroy_address_space(c.pgdir_phys);
            c = {};
            c.pid = 0;
            c.state = State::Unused;
            c.user_stack_top = USER_STACK_BASE + static_cast<uint32_t>((i + 1) * USER_STACK_SIZE);
            return r;
        }

        /* No matching zombie this pass. If this caller has no children
         * at all, report ECHILD outright; if it asked not to block
         * (WNOHANG), report "none dead yet"; otherwise, when running as
         * a scheduler-managed process, park on the scheduler until a
         * child dies. The legacy one-way path has no scheduler to wait
         * on, so it falls through to ECHILD as before. */
        bool has_children = false;
        for (int i = 0; i < MAX_PROCS; ++i) {
            const Process& c = table[i];
            if (c.pid == 0 || c.state == State::Unused)
                continue;
            if (c.ppid == self) {
                has_children = true;
                break;
            }
        }
        if (!has_children)
            return -VNU_ECHILD;
        if (options)
            return 0;
        if (!table[current_idx].coro)
            return -VNU_ECHILD;
        /* Block properly instead of poll-yielding every pass: the child
         * wakes us from sys_exit() on this channel the moment it turns
         * into a zombie. Spurious wakeups (a sibling's exit on the
         * "any child" channel) are harmless — the loop just re-scans. */
        uint32_t chan = (pid == -1)
                            ? WAIT_ZOMBIE_ANY
                            : wait_chan_of_child(static_cast<int>(pid));
        sleep_on(chan, 0);
        /* resumed by the scheduler → loop and look for a zombie again */
    }
}

int sys_kill(int pid, int sig)
{
    /* Signal 0 is the POSIX liveness probe - "may I signal this pid?" -
     * and it must not signal anything: `kill(pid, 0)` is exactly how a
     * program asks whether its child is still there. */
    if (sig == 0) {
        for (int i = 0; i < MAX_PROCS; ++i)
            if (table[i].state != State::Unused && table[i].pid == pid)
                return 0;
        return -VNU_ESRCH;
    }
    return signal_kill(pid, sig);
}

/* --- signals ----------------------------------------------------------
 *
 * Raising a signal and delivering it are two different moments, and the
 * gap between them is what makes this behave like a signal system
 * rather than a kill. Raising it (signal_kill) only records the signal
 * and, for a signal whose action is "terminate", ends the process right
 * there: a process that has not asked for anything cannot be asked to
 * run any code, so a default signal has to be its last. A signal with a
 * handler waits in sig_pending until the process is about to run its own
 * code again, which is delivery, and delivery is the only place that
 * touches the user's stack.
 *
 * Delivery never rewrites the interrupted context. It builds the
 * handler's own entry frame on the process's stack, just below the frame
 * it is interrupting, and the return path in assembly switches to that
 * instead of popping the old one. The old frame is still there, byte for
 * byte, with the syscall's return value already in its EAX slot, so
 * sigreturn only has to point the popad+iretd back at it and the program
 * continues exactly where it was, with exactly the values it had - which
 * is why a signal can interrupt a program anywhere, not just between
 * two convenient places in the kernel. */
extern "C" std::uint32_t vnu_pending_user_esp;
extern "C" std::uint8_t vnu_pending_user_esp_flag;

namespace {

/* The interrupted frame both delivery points see: pushad's 8 dwords
 * followed by the 3 dwords the gate pushed (eip, cs, eflags). See
 * arch/i386/syscall/syscall.s and proc/ctxswitch.s, which both popad
 * from it and then iretd out of the last 3 words. */
constexpr uint32_t FRAME_EIP = 32;
constexpr uint32_t FRAME_CS = 36;
constexpr uint32_t FRAME_EFLAGS = 40;

/* Below the interrupted frame: a 16-byte-aligned slot holding what the
 * handler's `return` pops ([0] = the restorer, [4] = the signal number,
 * so a handler written as `void h(int sig)` gets its argument from the
 * same place a `ret` would leave it), and just below that the three
 * words an iret pops (handler, cs, eflags). The slot is not built on the
 * interrupted frame itself but below the kernel's own C frames, which sit
 * between the two: the kernel runs on the interrupted process's stack, so
 * a frame placed right under the pushad frame would land in the locals of
 * the call that is still running. Nothing writes below the anchor the
 * caller passes until the iret, a few instructions later. */
constexpr uint32_t SIG_BLOCK_WORDS = 4; /* restorer, signo */
constexpr uint32_t SIG_IRET_WORDS = 3;  /* handler, cs, eflags */

} // namespace

int pending_delivery(const Process* p)
{
    uint32_t ready = p->sig_pending & ~p->sig_mask;
    for (int sig = 1; sig < VNU_NSIG; ++sig)
        if (ready & (1u << sig))
            return sig;
    return 0;
}

bool interrupt_pending()
{
    return pending_delivery(&table[current_idx]) != 0;
}

/* The part of delivery that touches the user's stack, written against
 * the five signal fields rather than a Process: a windowed task's Console
 * has exactly the same five, and the part that builds a frame on the
 * interrupted process's stack and sets the one-shot return flag must not
 * exist twice. Returns the signal delivered, or 0. */
static int deliver_common(uint32_t* act, uint32_t& pending, uint32_t& mask,
                          uint32_t& saved_mask, uint32_t& frame_out,
                          uint32_t frame_base, uint32_t stack_anchor, int sig)
{
    if (sig == 0)
        return 0;
    /* A default or ignored action never gets this far: both are carried
     * out where the signal is raised (see signal_kill() and the alarm
     * deadline in vnu_timer_tick()). What is left is a real handler,
     * whose address is where the process was told to go. `act` is the
     * base of the VNU_NSIG-entry table, and a struct vnu_sigaction is
     * four words, so the entry this signal owns is the sig-th one. */
    const struct vnu_sigaction a =
        *reinterpret_cast<const struct vnu_sigaction*>(&act[sig * 4]);
    if (a.handler <= VNU_SIG_IGN || !a.restorer)
        return 0;
    /* The block goes at the very bottom of what the kernel is using of
     * this process's stack, so that nothing that is still live - a
     * caller's locals on the way out of here, the frame the iret is
     * about to pop - writes over it. Usually that is this function's own
     * frame, the deepest thing on the stack right now. A caller that is
     * running on a *different* stack (the scheduler handing a preempted
     * process back in) has no frames on the process's stack to avoid and
     * passes the parked frame instead. */
    volatile char here = 0;
    uint32_t low = stack_anchor ? stack_anchor : reinterpret_cast<uint32_t>(&here);
    uint32_t block = (low - SIG_BLOCK_WORDS * 4) & ~0xFu;
    uint32_t* w = reinterpret_cast<uint32_t*>(block);
    w[0] = a.restorer; /* where the handler's `return` lands */
    w[1] = static_cast<uint32_t>(sig);
    /* The iret pops its three words in increasing address order, and it
     * runs with esp = &w[-3]: eip, cs, eflags. */
    w[-3] = a.handler;
    w[-2] = *reinterpret_cast<uint32_t*>(frame_base + FRAME_CS);
    /* Same flags the context had, except that the handler runs with
     * interrupts on (the syscall path saved eflags with IF cleared by
     * the trap gate) and never with the trap flag still set, which would
     * fire a single-step interrupt out of the first instruction. */
    w[-1] = (*reinterpret_cast<uint32_t*>(frame_base + FRAME_EFLAGS) | 0x200u) & ~0x100u;

    pending &= ~(1u << sig);
    saved_mask = mask;
    /* Block the signal itself (unless the action says not to) plus the
     * signals it asked for, so a handler cannot be interrupted by the
     * signal it is handling unless it asked for that. */
    uint32_t block_now = a.mask;
    if (!(a.flags & VNU_SA_NODEFER))
        block_now |= 1u << sig;
    mask |= block_now;
    if (a.flags & VNU_SA_RESETHAND) {
        act[sig * 4 + 0] = VNU_SIG_DFL;
        act[sig * 4 + 3] = 0;
    }
    frame_out = frame_base;

    /* Tell the assembly return path to switch stacks instead of popping
     * the frame it is looking at. The flag is a single slot on purpose:
     * it is set and consumed within one return path, with interrupts
     * off (a syscall handler runs cli'd, the timer with the CLI in
     * vnu_timer_isr), so no other process can ever see it. */
    vnu_pending_user_esp = block - SIG_IRET_WORDS * 4;
    vnu_pending_user_esp_flag = 1;
    return sig;
}

int deliver_signal_at(Process& p, uint32_t frame_base, uint32_t stack_anchor, int sig)
{
    return deliver_common(reinterpret_cast<uint32_t*>(p.sig_action), p.sig_pending, p.sig_mask,
                          p.sig_saved_mask, p.sig_frame, frame_base, stack_anchor, sig);
}

int deliver_console_signal(vnu::wintask::Console& c, uint32_t frame_base,
                           uint32_t stack_anchor, int sig)
{
    return deliver_common(reinterpret_cast<uint32_t*>(c.sig_action), c.sig_pending, c.sig_mask,
                          c.sig_saved_mask, c.sig_frame, frame_base, stack_anchor, sig);
}

int deliver_pending_signal(uint32_t frame_base)
{
    Process& p = table[current_idx];
    if (!p.coro || vnu_pending_user_esp_flag)
        return 0;
    /* This runs on the process's own stack, in between the syscall's
     * frame and the assembly that is about to iret, so there is nothing
     * on it but the frames of this very call chain. */
    return deliver_signal_at(p, frame_base, 0, pending_delivery(&p));
}

/* --- Signals for windowed tasks ---
 *
 * A windowed task has no Process and no pid: the GUI's own task table is
 * where it lives, and that is also where its signal state has to be, or
 * a SIGWINCH the GUI raises would have nowhere to land. So these three
 * mirror signal_kill() / pending_delivery() / deliver_pending_signal()
 * against a Console's signal fields instead of a Process's.
 *
 * The duplication is deliberate and small: the delivery machinery below
 * is factored out of deliver_signal_at() and both call it, so the part
 * that touches the user's stack - the only part where being subtly
 * different would be a bug - exists once. What differs is the answer to
 * "what does the default action mean", which is a window's business and
 * not a process's: closing the window is what terminate means here. */

int pending_task_delivery(const vnu::wintask::Console* c)
{
    uint32_t ready = c->sig_pending & ~c->sig_mask;
    for (int sig = 1; sig < VNU_NSIG; ++sig)
        if (ready & (1u << sig))
            return sig;
    return 0;
}

bool task_interrupt_pending()
{
    vnu::wintask::Console* c = vnu::wintask::console_of_current();
    return c ? pending_task_delivery(c) != 0 : false;
}

int signal_task(vnu::wintask::Console* c, int sig)
{
    if (!c || sig <= 0 || sig >= VNU_NSIG)
        return -VNU_EINVAL;
    if (sig == VNU_SIGKILL) {
        /* The one signal with no answer to give: a task that gets it is
         * simply gone, no handler and no block. */
        vnu::wintask::close_task_of(c);
        return 0;
    }
    const uint32_t handler = c->sig_action[sig].handler;
    if (handler == VNU_SIG_IGN)
        return 0; /* ignore means ignore, bit included */
    if (handler == VNU_SIG_DFL) {
        /* SIGWINCH is the one signal whose default is *ignore*, and that
         * is POSIX rather than a VNU choice: the window changed size,
         * which is news rather than an emergency, and a program that has
         * not asked to hear it must not be killed for the window being
         * dragged. Every other signal's default here terminates, and for
         * a task that is closing its window - the pages and the slot go
         * back the way exit() would have taken them. */
        if (sig == VNU_SIGWINCH)
            return 0;
        vnu::wintask::close_task_of(c);
        return 0;
    }
    c->sig_pending |= 1u << sig;
    /* A task asleep in a read has to come back to be told: without this
     * the signal would sit pending until the next thing it waits for,
     * which for an idle gfx app could be the once-a-second tick. */
    vnu::wintask::wake_task_of(c);
    return 0;
}

int deliver_task_pending_signal(vnu::wintask::Console* c, uint32_t frame_base)
{
    if (!c || vnu_pending_user_esp_flag)
        return 0;
    return deliver_console_signal(*c, frame_base, 0, pending_task_delivery(c));
}

int sys_rt_sigaction(int sig, const uint32_t* act_in, uint32_t* old_out)
{
    if (sig <= 0 || sig >= VNU_NSIG)
        return -VNU_EINVAL;
    if (sig == VNU_SIGKILL) /* not even root may catch or block this one */
        return -VNU_EINVAL;
    /* A windowed task's actions live in its Console; a process's, in its
     * own entry in the table. Same struct, same rules, different home. */
    vnu::wintask::Console* wcon = vnu::wintask::console_of_current();
    struct vnu_sigaction* slot = wcon ? &wcon->sig_action[sig] : &table[current_idx].sig_action[sig];
    if (old_out) {
        old_out[0] = slot->handler;
        old_out[1] = slot->mask;
        old_out[2] = slot->flags;
        old_out[3] = slot->restorer;
    }
    if (!act_in)
        return 0; /* a NULL act is the "just tell me" query */
    struct vnu_sigaction act;
    act.handler = act_in[0];
    act.mask = act_in[1] & ~(1u << VNU_SIGKILL);
    act.flags = act_in[2];
    act.restorer = act_in[3];
    /* A handler is an address, and the kernel has to be able to get the
     * program back out of it: the `return` at the end of the handler
     * goes wherever the restorer says, and a handler without one would
     * jump into whatever happens to follow it. */
    if (act.handler > VNU_SIG_IGN && !act.restorer)
        return -VNU_EINVAL;
    *slot = act;
    return 0;
}

int sys_rt_sigprocmask(int how, const uint32_t* set, uint32_t* old_out)
{
    if (how != VNU_SIG_BLOCK && how != VNU_SIG_UNBLOCK && how != VNU_SIG_SETMASK)
        return -VNU_EINVAL;
    /* A windowed task's mask lives in its Console; a process's, in its
     * own entry in the table. Same field, same rules, different home. */
    vnu::wintask::Console* wcon = vnu::wintask::console_of_current();
    uint32_t* mask = wcon ? &wcon->sig_mask : &table[current_idx].sig_mask;
    if (old_out)
        *old_out = *mask;
    if (!set)
        return 0;
    uint32_t add = *set & ~(1u << VNU_SIGKILL); /* never blockable */
    if (how == VNU_SIG_BLOCK)
        *mask |= add;
    else if (how == VNU_SIG_UNBLOCK)
        *mask &= ~add;
    else
        *mask = add;
    return 0;
}

uint32_t sys_rt_sigreturn(TrapFrame* cur)
{
    /* Same five fields, either home; the restoration below is identical
     * and must be: the interrupted frame is the same kind of thing
     * whether the kernel got here for a process or for a windowed task. */
    vnu::wintask::Console* wcon = vnu::wintask::console_of_current();
    uint32_t& saved = wcon ? wcon->sig_saved_mask : table[current_idx].sig_saved_mask;
    uint32_t& frame = wcon ? wcon->sig_frame : table[current_idx].sig_frame;
    uint32_t& mask = wcon ? wcon->sig_mask : table[current_idx].sig_mask;
    if (!frame)
        return 0; /* not from a handler: nothing to go back to */
    mask = saved;
    saved = 0;
    TrapFrame* interrupted = reinterpret_cast<TrapFrame*>(frame);
    frame = 0;
    *cur = *interrupted;
    /* popad restores the eight registers but *discards* the ESP the frame
     * saved, so a copy alone resumes the process with esp = tf + 32, in
     * the middle of this frame instead of where it was interrupted. The
     * interrupted frame's own esp is the address its eip/cs/eflags sit
     * at, so pointing the return path there hands the iret exactly the
     * words the copy just wrote, and leaves the process with the esp the
     * interrupted instruction had. */
    vnu_pending_user_esp = interrupted->esp;
    vnu_pending_user_esp_flag = 1;
    return cur->eax;
}

int sys_sigraise(int sig)
{
    if (sig <= 0 || sig >= VNU_NSIG)
        return -VNU_EINVAL;
    if (sig == VNU_SIGKILL) {
        sys_exit(128 + sig);
        return 0; /* not reached */
    }
    vnu::wintask::Console* wcon = vnu::wintask::console_of_current();
    /* raise() on a signal whose action is to terminate dies here and now,
     * the same as kill(getpid(), sig): there is nothing to defer it to,
     * the caller is the one that stops. For a windowed task that means
     * its window closes, from the same place exit() would have. */
    if (wcon) {
        if (wcon->sig_action[sig].handler == VNU_SIG_DFL) {
            vnu::wintask::close_task_of(wcon);
            return 0;
        }
        wcon->sig_pending |= 1u << sig;
        vnu::wintask::wake_task_of(wcon);
        return 0;
    }
    if (table[current_idx].sig_action[sig].handler == VNU_SIG_DFL) {
        sys_exit(128 + sig);
        return 0; /* not reached */
    }
    table[current_idx].sig_pending |= 1u << sig;
    return 0;
}

int sys_sigpending(uint32_t* set)
{
    if (!set)
        return 0;
    vnu::wintask::Console* wcon = vnu::wintask::console_of_current();
    *set = wcon ? wcon->sig_pending : table[current_idx].sig_pending;
    return 0;
}

uint32_t sys_alarm(uint32_t seconds)
{
    Process& p = table[current_idx];
    uint32_t left = 0;
    if (p.alarm_at) {
        int32_t pending = static_cast<int32_t>(p.alarm_at - now_jiffies());
        left = pending > 0 ? (pending + 9) / 10 : 0;
    }
    p.alarm_at = seconds ? now_jiffies() + seconds * 100u : 0;
    return left;
}

int signal_kill(int pid, int sig)
{
    if (sig <= 0 || sig >= VNU_NSIG)
        return -VNU_EINVAL;
    for (int i = 0; i < MAX_PROCS; ++i) {
        Process& p = table[i];
        if (p.state == State::Unused || p.pid != pid)
            continue;
        uint32_t handler = p.sig_action[sig].handler;
        if (sig == VNU_SIGKILL) {
            /* The one signal with no answer to give: no handler, no
             * block, no pending. */
            if (i != current_idx) {
                /* Another process: the death is its own to have, so
                 * leave it as a zombie for its parent to reap, the same
                 * shape an exit leaves. */
                p.exit_code = 128 + sig;
                p.state = State::Zombie;
                wake_waiter_of(p.pid);
                return 0;
            }
            sys_exit(128 + sig);
            return 0; /* not reached */
        }
        if (handler == VNU_SIG_IGN)
            return 0; /* ignore means ignore, byte included */
        if (handler == VNU_SIG_DFL) {
            /* A blocked signal still gets its default action in
             * VNU: there is no "terminate" action to defer, and a
             * program that blocks a signal and then never unblocks it
             * has asked for a process it cannot leave anyway.
             *
             * SIGWINCH is the exception, in the POSIX sense: its default
             * action is ignore, so a program that never asked to hear
             * about a resize is not killed by one. */
            if (sig == VNU_SIGWINCH)
                return 0;
            if (i != current_idx) {
                /* Another process: the death is its own to have, so
                 * leave it as a zombie for its parent to reap, the same
                 * shape an exit leaves. */
                p.exit_code = 128 + sig;
                p.state = State::Zombie;
                wake_waiter_of(p.pid);
                return 0;
            }
            sys_exit(128 + sig);
            return 0; /* not reached */
        }
        p.sig_pending |= 1u << sig;
        /* A process asleep in a syscall has to come back to be told:
         * without this the signal would sit in sig_pending until the
         * next thing it happens to wait for. */
        if (p.state == State::Blocked) {
            p.state = State::Runnable;
            p.wait_chan = 0;
            p.sleep_until = 0;
        }
        return 0;
    }
    return -VNU_ESRCH;
}

bool console_signal(int sig)
{
    /* The process reading the console is the process the console is
     * talking to: VNU has no process groups, and it does not need one,
     * because vash execve()s over itself for every external command, so
     * the shell's own process IS the command. ^C therefore belongs to
     * whoever is at the keyboard right now, which is what the reader of
     * the console is by construction. */
    if (signal_kill(current_pid(), sig) < 0)
        return false;
    /* signal_kill either recorded the signal or dropped it (an ignored
     * action), and if the action was "terminate" it never came back at
     * all. All that is left to say is whether anything is left to
     * interrupt: a blocked ^C stays pending, exactly as POSIX asks, and
     * the console read goes on waiting for a key. */
    return interrupt_pending();
}

bool schedule()
{
    for (int i = 0; i < MAX_PROCS; ++i) {
        int idx = (current_idx + 1 + i) % MAX_PROCS;
        if (table[idx].state == State::Runnable) {
            if (table[current_idx].state == State::Running)
                table[current_idx].state = State::Runnable;
            current_idx = idx;
            table[idx].state = State::Running;
            return true;
        }
    }
    return false;
}

extern "C" uint32_t vnu_saved_eip, vnu_saved_esp, vnu_saved_ebp;
extern "C" void vnu_swtch(uint32_t* old_esp_out, uint32_t new_esp);

namespace {

uint32_t g_brk = BRK_MIN;

/* Scratch stack used ONLY for the brief window between switching CR3
 * to a brand-new process's private address space and actually jumping
 * into it (see run_program_from_memory below). That window has to run
 * SOMEWHERE, and it can't run on the caller's own stack: if this is a
 * nested launch (e.g. the GUI launching "term" from inside a vash
 * session that's itself a process with its own private stack — see
 * kernel/gui/apps.cpp), the caller's stack lives in the very region
 * (0x400000-0x7FFFFF) that just became a *different* process's private
 * mapping the instant CR3 changed, so it silently stops being valid.
 * This buffer instead sits in the kernel's own low, always-shared
 * memory, so it stays mapped identically no matter which CR3 is
 * loaded, and vnu_swtch (kernel/proc/ctxswitch.s) gets us on and off
 * it safely. (Found the hard way: switching CR3 while still running
 * on the old stack faults on the very next instruction that touches
 * the stack — here, silently, that meant a page fault immediately
 * inside vnu::paging::switch_to() itself, cascading into a triple
 * fault. Booting `term` from the GUI reproduced it every time.) */
alignas(16) uint8_t g_setup_stack[16384];

struct PendingLaunch {
    const uint8_t* data;
    uint32_t size;
    Process* p;
    const char* argv0_path;
    uint32_t pgdir;
    uint32_t prev_pgdir;
    bool ok;
} g_pending;

/* argv0_path often points into the CALLER's own stack (e.g.
 * kernel/gui/apps.cpp's launch() builds it in a local char[]) — which,
 * for a nested launch, is that caller's *private* per-process stack.
 * setup_and_enter() reads it only after switching CR3 to the new
 * process's address space, by which point the caller's stack is no
 * longer mapped at all, so dereferencing the original pointer faults.
 * (This one was sneaky: it doesn't fault where you'd expect, on some
 * write into the new process — it faults inside str_len(), reading a
 * perfectly ordinary-looking C string that just happens to no longer
 * be accessible.) Copy the string into kernel-owned memory, which
 * stays mapped identically under every CR3, before the switch. */
char g_argv0_copy[64];
uint32_t g_caller_esp;

void setup_and_enter()
{
    /* Interrupts stay off from here through vnu_enter_user() (which
     * itself does its own `cli`, with no matching `sti` — this kernel
     * relies on polling PS/2 status registers directly for keyboard
     * and mouse input, not on interrupt delivery, so this costs
     * nothing). Found the hard way: a stray hardware interrupt landing
     * in this exact window — after CR3 has switched to the new
     * process's private address space but before its own stack is
     * live — pushes its return frame onto whatever ESP happens to be
     * current, and an unlucky value there faulted almost immediately,
     * cascading into a triple fault. Booting `term` from the GUI
     * reproduced it non-deterministically (exact timing-dependent),
     * which is what made it look like a paging bug at first. */
    asm volatile("cli");

    /* Now running on g_setup_stack — safe to switch CR3 here, unlike
     * on the caller's own stack (see g_setup_stack's comment). */
    vnu::paging::switch_to(g_pending.pgdir);

    PendingLaunch& pl = g_pending;
    uint32_t entry = vnu::elf::load(pl.data, pl.size);
    if (!entry) {
        pl.ok = false;
        /* Back to the caller's own address space BEFORE swtch-ing back
         * to its stack — that stack only exists under the OLD CR3
         * (see g_setup_stack's comment; same reasoning, just run in
         * reverse). Still safe to do from g_setup_stack, which is
         * mapped identically under every CR3. */
        vnu::paging::switch_to(pl.prev_pgdir);
        asm volatile("sti");
        uint32_t dummy;
        vnu_swtch(&dummy, g_caller_esp);
        for (;;) {
        } /* unreachable */
    }

    brk_reset();

    Process& p = *pl.p;
    p.regs.eip = entry;
    p.regs.eflags = 0x202;
    char* av[] = {const_cast<char*>(pl.argv0_path), nullptr};
    setup_user_stack(p, 1, av, pl.argv0_path);

    pl.ok = true;
    vnu_enter_user(p.regs.eip, p.regs.esp);

    /* Resumed here once the process has fully exited (its own exit()
     * unwound back to right after that vnu_enter_user call above, via
     * proc/switch.s's single-slot trampoline — same mechanism as the
     * classic non-windowed path). Still running on g_setup_stack under
     * the process's own (now-finished) CR3 — restore the caller's
     * before handing back control, for the same reason as above. */
    vnu::paging::switch_to(pl.prev_pgdir);
    uint32_t dummy;
    vnu_swtch(&dummy, g_caller_esp);
    for (;;) {
    } /* unreachable */
}

/* Primes g_setup_stack with a fresh vnu_swtch-compatible frame so the
 * very first switch onto it lands in setup_and_enter(). Must be
 * called again before every launch (the stack is reused each time). */
uint32_t prime_setup_stack()
{
    uint32_t top = reinterpret_cast<uint32_t>(g_setup_stack) + sizeof(g_setup_stack);
    uint32_t* frame = reinterpret_cast<uint32_t*>(top) - 5;
    frame[0] = 0; /* edi */
    frame[1] = 0; /* esi */
    frame[2] = 0; /* ebx */
    frame[3] = 0; /* ebp */
    frame[4] = reinterpret_cast<uint32_t>(&setup_and_enter);
    return reinterpret_cast<uint32_t>(&frame[0]);
}

} // namespace

void brk_reset()
{
    g_brk = BRK_MIN;
}

uint32_t brk_current()
{
    if (table[current_idx].coro)
        return table[current_idx].brk;
    return g_brk;
}

uint32_t brk_set(uint32_t requested)
{
    if (table[current_idx].coro) {
        table[current_idx].brk = requested;
        return requested;
    }
    g_brk = requested;
    return g_brk;
}

int run_program_from_memory(const char* argv0_path, const uint8_t* data, uint32_t size)
{
    int slot = alloc_slot();
    if (slot < 0)
        return -VNU_ENOMEM;

    uint32_t stack_base = USER_STACK_BASE + static_cast<uint32_t>(slot * USER_STACK_SIZE);

    /* Give this process its own private view of the app-image region
     * (0x400000, code+data - 24 pages/96 KiB, comfortably more than any of
     * our current binaries need, see VIBEGRAPHICS_CHANGES.md), its own
     * stack slot, AND its own heap (0x700000, matching
     * vnu::proc::BRK_MIN/BRK_MAX) - all backed by freshly allocated
     * physical frames. Heap and stack/code sit in the *same* page
     * directory entry (0x400000-0x7FFFFF is one 4 MiB PDE), so once
     * that PDE is privatized, anything in its span that isn't
     * explicitly listed here goes from "shared identity mapping" to
     * "not present" - the heap must be listed too, or it silently
     * vanishes and the first malloc() page-faults (every command that
     * never touches the heap - cat/echo/mkdir - worked fine, but `ls`,
     * the one thing that calls opendir(), which mallocs a DIR, crashed
     * immediately, until the heap was added here too). Everything else
     * (kernel, VFS, etc.) stays on the shared identity map - see
     * kernel/include/vnu/paging.h. This is what actually fixes
     * "graphics breaks"/only-one-app-at-a-time: two processes' virtual
     * 0x400000 (and heap, and stack) are physically different memory,
     * instead of all racing to use the same RAM. */
    uint32_t app_pages = elf_need_pages(data, size);
    vnu::paging::MapRange ranges[3] = {
        {0x00400000, app_pages},
        {stack_base, USER_STACK_SIZE / vnu::paging::PAGE_SIZE},
        {BRK_MIN, (BRK_MAX - BRK_MIN) / vnu::paging::PAGE_SIZE},
    };
    uint32_t pgdir = vnu::paging::create_address_space(ranges, 3);
    if (!pgdir)
        return -VNU_ENOMEM;

    Process& p = table[slot];
    p = {};
    p.pid = next_pid++;
    p.ppid = 0;
    p.state = State::Running;
    p.uid = table[current_idx].uid;
    p.gid = table[current_idx].gid;
    p.pgdir_phys = pgdir;
    p.user_stack_top = stack_base + USER_STACK_SIZE;
    p.app_pages = app_pages;
    set_proc_name(p, argv0_path);

    table[0].state = State::Runnable;
    current_idx = slot;
    console_session = true;

    /* vnu_enter_user() (called from setup_and_enter() below) stashes a
     * *single* continuation (eip/esp/ebp) for vnu_return_to_console()
     * to resume later - see proc/switch.s. That makes it non-reentrant:
     * if we're already nested inside an earlier vnu_enter_user (e.g.
     * the GUI's app launcher calling this from inside a vash session
     * that itself got here through one), jumping in again clobbers the
     * outer continuation. Save it here and put it back once this
     * nested program has run and unwound back to us, so whoever owns
     * the outer continuation still lands in the right place later
     * instead of jumping into a stale, already-reused stack. */
    uint32_t prev_eip = vnu_saved_eip;
    uint32_t prev_esp = vnu_saved_esp;
    uint32_t prev_ebp = vnu_saved_ebp;

    uint32_t prev_pgdir = vnu::paging::current_pgdir();

    /* The tricky part: switching CR3 to the new process's private
     * space, loading its ELF, and jumping in all has to happen without
     * ever touching the CALLER's stack again once CR3 has changed -
     * see g_setup_stack's comment above for why. Move onto that safe
     * scratch stack FIRST — this vnu_swtch doesn't touch CR3, so it's
     * safe to do from here — and only switch CR3 once already running
     * on it (inside setup_and_enter()). */
    g_pending.data = data;
    g_pending.size = size;
    g_pending.p = &p;
    {
        int i = 0;
        for (; argv0_path && argv0_path[i] && i < static_cast<int>(sizeof(g_argv0_copy)) - 1; ++i)
            g_argv0_copy[i] = argv0_path[i];
        g_argv0_copy[i] = 0;
    }
    g_pending.argv0_path = g_argv0_copy;
    g_pending.pgdir = pgdir;
    g_pending.prev_pgdir = prev_pgdir;
    uint32_t setup_esp = prime_setup_stack();

    vnu_swtch(&g_caller_esp, setup_esp);
    /* Resumed here (back on THIS stack, under THIS stack's own CR3 —
     * restored by setup_and_enter() before it switched back to us)
     * once setup_and_enter() has either failed to load the ELF or the
     * process it launched has run to completion and exited. */

    vnu_saved_eip = prev_eip;
    vnu_saved_esp = prev_esp;
    vnu_saved_ebp = prev_ebp;

    bool ok = g_pending.ok;
    vnu::paging::destroy_address_space(pgdir);
    if (!ok) {
        p.state = State::Unused;
        return -VNU_ENOEXEC;
    }
    return 0;
}
const uint8_t* find_embedded_data(const char* path, uint32_t& out_size)
{
    const EmbeddedProg* prog = find_embedded(path);
    if (!prog)
        return nullptr;
    out_size = prog->size;
    return prog->data;
}

int run_program(const char* path)
{
    return enter_program(path);
}

/* How many 4 KiB pages does an ELF payload span in the app region,
 * starting at 0x400000? Sized from the program headers so each process
 * only maps as much as its image really needs (a guest C compiler needs
 * a lot more than hello.c). The app region must stay below the user
 * stack slots (USER_STACK_BASE), so the biggest useful image is
 * (0x600000 - 0x400000) / 4K = 512 pages (2 MiB); beyond that the
 * loader would fault in the .bss memset of load() (see elf.cpp). */
uint32_t elf_need_pages(const uint8_t* image, uint32_t size)
{
    if (!image || size < sizeof(vnu::elf::Ehdr))
        return 1;
    auto* eh = reinterpret_cast<const vnu::elf::Ehdr*>(image);
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' || eh->e_type != 2)
        return 1;
    uint32_t top = 0;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (eh->e_phoff + static_cast<uint32_t>(i + 1) * eh->e_phentsize > size)
            break;
        auto* ph = reinterpret_cast<const vnu::elf::Phdr*>(
            image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != vnu::elf::PT_LOAD)
            continue;
        uint32_t end = ph->p_vaddr + ph->p_memsz;
        if (end > top)
            top = end;
    }
    if (top <= 0x400000u)
        return 1;
    uint32_t pages = (top - 0x400000u + vnu::paging::PAGE_SIZE - 1) / vnu::paging::PAGE_SIZE;
    /* Keep the image clear of the stack slots (USER_STACK_BASE) and, a
     * fortiori, of the heap (0x700000). */
    uint32_t max_app_pages = (USER_STACK_BASE - 0x400000u) / vnu::paging::PAGE_SIZE;
    if (pages > max_app_pages)
        pages = max_app_pages;
    return pages;
}

int sys_spawn(const char* path)
{
    int i = 0;
    for (; path && path[i] && i < static_cast<int>(sizeof(g_spawn_path)) - 1; ++i)
        g_spawn_path[i] = path[i];
    g_spawn_path[i] = 0;

    const EmbeddedProg* prog = find_embedded(g_spawn_path);
    if (!prog)
        return -VNU_ENOENT;

    int slot = alloc_slot();
    if (slot <= 0) /* slot 0 belongs to the scheduler/console process */
        return -VNU_ENOMEM;

    uint32_t stack_base = USER_STACK_BASE + static_cast<uint32_t>(slot * USER_STACK_SIZE);
    uint32_t app_pages = elf_need_pages(prog->data, prog->size);
    vnu::paging::MapRange ranges[3] = {
        {0x00400000, app_pages},
        {stack_base, USER_STACK_SIZE / vnu::paging::PAGE_SIZE},
        {BRK_MIN, (BRK_MAX - BRK_MIN) / vnu::paging::PAGE_SIZE},
    };
    uint32_t pgdir = vnu::paging::create_address_space(ranges, 3);
    if (!pgdir)
        return -VNU_ENOMEM;

    uint32_t entry = elf_load_into(pgdir, prog->data, prog->size);
    if (!entry) {
        vnu::paging::destroy_address_space(pgdir);
        return -VNU_ENOEXEC;
    }

    uint32_t stack_top = stack_base + USER_STACK_SIZE;
    uint32_t argv_esp = build_argv_frame_into(pgdir, stack_top, g_spawn_path);
    uint32_t fva = setup_switch_frame(pgdir, stack_top);

    Process& p = table[slot];
    p = {};
    p.pid = next_pid++;
    p.ppid = table[current_idx].pid; /* 0 during the kernel boot spawn */
    p.state = State::Runnable;
    p.uid = table[current_idx].uid;
    p.gid = table[current_idx].gid;
    p.pgdir_phys = pgdir;
    p.user_stack_top = stack_top;
    p.coro = true;
    p.coro_esp = fva;
    p.entry = entry;
    p.argv_esp = argv_esp;
    p.brk = BRK_MIN;
    p.app_pages = app_pages;
    set_proc_name(p, g_spawn_path);
    g_last_slot = slot;
    return p.pid;
}

bool can_yield()
{
    return table[current_idx].coro;
}

void yield_current()
{
    Process& p = table[current_idx];
    if (!p.coro)
        return;
    switch_to_scheduler(true);
}

/* --- Common wait mechanism (see vnu/process.h) ------------------------
 * All block/wake transitions happen inside cli'd syscall handlers on a
 * single CPU, so checking a predicate and registering the block in
 * sleep_on() is atomic w.r.t. wakeup() producers — no lost wakeups. */

uint32_t now_jiffies()
{
    return g_jiffies;
}

void sleep_on(uint32_t chan, uint32_t until_jiffies)
{
    Process& p = table[current_idx];
    if (!p.coro) /* legacy one-way path: no scheduler to come back to */
        return;
    p.state = State::Blocked;
    p.wait_chan = chan;
    p.sleep_until = until_jiffies;
    yield_current();
    /* Woken (by wakeup() or the deadline scan): back inside this syscall
     * handler; drop the stale wait bookkeeping and let the caller's
     * re-check loop decide whether to wait again. */
    p.wait_chan = 0;
    p.sleep_until = 0;
}

void wakeup(uint32_t chan)
{
    for (int i = 1; i < MAX_PROCS; ++i) {
        Process& p = table[i];
        if (p.state == State::Blocked && p.wait_chan == chan) {
            p.state = State::Runnable;
            p.wait_chan = 0;
            p.sleep_until = 0;
        }
    }
}

void wakeup_one(uint32_t chan)
{
    for (int i = 1; i < MAX_PROCS; ++i) {
        Process& p = table[i];
        if (p.state == State::Blocked && p.wait_chan == chan) {
            p.state = State::Runnable;
            p.wait_chan = 0;
            p.sleep_until = 0;
            return;
        }
    }
}

int sys_sleep(uint32_t ms)
{
    if (!table[current_idx].coro)
        return -VNU_EAGAIN; /* windowed/legacy: no scheduler to wait on */
    if (interrupt_pending())
        return -VNU_EINTR;
    sleep_ms(ms);
    /* A signal raised while asleep wakes the process (signal_kill makes
     * it runnable), which lands here again; the sleep is then over as
     * far as the program is concerned. */
    return interrupt_pending() ? -VNU_EINTR : 0;
}

/* PIT IRQ0 entry (see ctxswitch.s's vnu_timer_isr). Returns 0 when the
 * tick must not switch (scheduler itself running, or nothing else to
 * run); on a real preemption it parks the CURRENT process — full
 * interrupt frame pointer into coro_esp, preempted=true — and hops to
 * the scheduler coroutine, so the return is never reached. */
extern "C" uint32_t vnu_timer_tick(uint32_t frame_esp)
{
    /* The scheduler clock: every IRQ0 advances jiffies, whether or not
     * this tick preempts anyone (it is what wakes timed sleepers). */
    ++g_jiffies;
    if (current_idx == 0)
        return 0;
    Process& cur = table[current_idx];
    if (!cur.coro)
        return 0;

    /* An alarm that ran out raises SIGALRM, and the tick is the only
     * thing that ever notices the deadline passing: nothing in userspace
     * is running, let alone asking what time it is. raise_not_self()
     * rather than signal_kill() because a SIGALRM whose action is to
     * terminate must not make the dying process switch stacks from
     * inside the interrupt frame that is about to be thrown away. */
    if (cur.alarm_at && static_cast<int32_t>(g_jiffies - cur.alarm_at) >= 0) {
        cur.alarm_at = 0;
        const uint32_t handler = cur.sig_action[VNU_SIGALRM].handler;
        if (handler == VNU_SIG_IGN) {
            /* ignored: the deadline is simply gone */
        } else if (handler == VNU_SIG_DFL) {
            /* The default action for SIGALRM is to terminate. Done here
             * rather than by queueing the signal, because the only return
             * path out of this frame leads straight back into the code
             * being interrupted - there is no place for a handler frame
             * to be built, and sys_exit() switches stacks itself. */
            sys_exit(128 + VNU_SIGALRM);
            return 0; /* not reached */
        } else {
            cur.sig_pending |= 1u << VNU_SIGALRM;
        }
    }

    /* Round-robin: only preempt if some OTHER process is on the ready
     * list (blocked processes keep their Runnable state and are
     * re-visited exactly like they are today, on every pass). A single
     * busy process otherwise just churns through pointless timer-driven
     * switches; it gets the CPU back the instant a second one appears. */
    bool other = false;
    for (int k = 1; k < MAX_PROCS; ++k) {
        int idx = (current_idx + k) % MAX_PROCS;
        if (table[idx].coro && table[idx].state == State::Runnable) {
            other = true;
            break;
        }
    }
    if (!other) {
        /* Nobody to give the CPU to, so this tick preempts nothing and
         * the ISR pops the frame and returns into the interrupted
         * instruction. That is also the moment to hand a pending signal
         * to the process: it is about to run its own code, which is
         * exactly where a signal belongs, and the flag is consumed a few
         * instructions later by that same return path (IF is off in
         * here, so nothing can come between the two). */
        int sig = pending_delivery(&cur);
        if (sig) {
            /* This runs on the interrupted process's own stack, inside
             * the timer ISR, so 0 puts the handler's frame at the
             * bottom of the whole call chain. */
            deliver_signal_at(cur, frame_esp, 0, sig);
        }
        return 0;
    }

    /* Preemption: park the whole frame and let the scheduler decide who
     * runs next. Whether this process has a signal waiting is not
     * decided here - run_slice() asks again on the way back in, which
     * is the only point where the flag it sets is certain to be the
     * next return path to consume it. */
    cur.coro_esp = frame_esp;
    cur.preempted = true;
    vnu_proc_new_pd = g_sched_pgdir;
    vnu_proc_preempt(g_sched_esp);
    return 0; /* not reached */
}

void run_slice(int i)
{
    if (i <= 0 || i >= MAX_PROCS)
        return;
    Process& p = table[i];
    if (!p.coro || p.state != State::Runnable)
        return;

    current_idx = i;
    if (!p.started) {
        /* First switch into this process: prime the trampoline's globals
         * with this process's own ELF entry + freshly built argv stack. */
        vnu_proc_pending_entry = p.entry;
        vnu_proc_pending_stack = p.argv_esp;
        p.started = true;
    }
    vnu_proc_new_pd = p.pgdir_phys;
    if (p.preempted) {
        /* The timer parked this process with its whole register state:
         * popad+iret back into whatever instruction it was interrupted
         * in, instead of the cooperative callee-saved switch. */
        p.preempted = false;
        /* A signal raised while it was parked is delivered on the way
         * back in - the frame it is about to be resumed through is
         * exactly the interrupted context, and the handler is built on
         * it right here so the flag below cannot be seen by (or stolen
         * by) another process: the scheduler runs with interrupts off
         * and consumes it in the very next call. */
        int sig = pending_delivery(&p);
        if (sig) {
            /* Delivered from the scheduler, whose stack is not this
             * process's, so the parked frame is the only thing on the
             * process's stack and makes a safe anchor by itself. */
            deliver_signal_at(p, p.coro_esp, p.coro_esp, sig);
        }
        vnu_proc_resume_preempted(&g_sched_esp, p.coro_esp);
    } else {
        vnu_proc_switch(&g_sched_esp, p.coro_esp);
    }
    /* Resumed here (on the scheduler's own stack) once the process has
     * yielded, been preempted, or exited. */
    current_idx = 0;

    if (p.state == State::Zombie && p.ppid == 0) {
        /* init died with no parent to reap it — the scheduler adopts it. */
        vnu::paging::destroy_address_space(p.pgdir_phys);
        p = {};
        p.pid = 0;
        p.state = State::Unused;
        p.user_stack_top = USER_STACK_BASE + static_cast<uint32_t>((i + 1) * USER_STACK_SIZE);
    }
}

int run_scheduler(const char* primary, const char* fallback)
{
    /* The PIT timer is what makes the round-robin preemptive: IRQ0
     * (100 Hz) seizes the CPU from a running process and falls back to
     * this loop. Init it once, before any process gets its first slice
     * (the loop may otherwise exit into kernel_main directly). */
    vnu_timer_init();
    g_sched_pgdir = vnu::paging::current_pgdir();
    const char* next = primary;
    int init_slot = -1;

    for (;;) {
        current_idx = 0;

        /* Park the CPU until the next PIT tick (100 Hz) before every
         * scheduling pass. IRQ0 is what advances g_jiffies — the clock
         * timed sleepers and mutex waiters are woken from — and the
         * scheduler spends its time between processes with interrupts
         * off, so without this window the tick is rarely ever taken
         * and a sleeping process can wait forever while the machine
         * spins. hlt() returns immediately when the tick is already
         * pending, so a busy pass costs nothing. */
        asm volatile("sti; hlt; cli");

        /* Wake timed sleepers whose deadline has passed (the 100 Hz
         * jiffies clock advanced while they were parked). Signed
         * compare handles the 32-bit tick counter wrapping. */
        for (int i = 1; i < MAX_PROCS; ++i) {
            Process& p = table[i];
            if (p.state == State::Blocked && p.sleep_until &&
                (static_cast<int32_t>(g_jiffies - p.sleep_until) >= 0)) {
                p.state = State::Runnable;
                p.sleep_until = 0;
            }
        }

        /* A display that shows the console only as pixels needs telling
         * that it has changed, and the scheduler is the one thing that
         * runs whether or not any process is runnable: a shell waiting
         * on a keystroke is the state a console is in most of the time.
         * Free on a display with a text mode of its own. */
        vnu::vgfx::console_tick();

        if (init_slot < 0 || table[init_slot].state == State::Unused) {
            /* A previous session may have left a redirection (> file) on
             * fd 0/1/2 — the VFS fd table is global, so make sure the
             * respawned shell talks to the real console again. */
            vnu::vfs::reset_stdio();
            int rc = sys_spawn(next);
            if (rc < 0 && next == primary) {
                next = fallback;
                continue;
            }
            if (rc < 0)
                return rc;
            init_slot = g_last_slot;
        }

        for (int i = 1; i < MAX_PROCS; ++i) {
            run_slice(i);
        }
    }
}

} // namespace vnu::proc
