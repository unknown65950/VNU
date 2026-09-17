#include <vnu/process.h>
#include <vnu/elf.h>
#include <vnu/abi.h>
#include <vnu/paging.h>
#include <vnu/vfs.h>
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

extern "C" void vnu_proc_switch(uint32_t* old_esp_out, uint32_t new_esp);
extern "C" void vnu_proc_trampoline();
extern "C" uint32_t vnu_proc_pending_entry, vnu_proc_pending_stack;
extern "C" uint32_t vnu_proc_new_pd, vnu_proc_old_pd;

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
    {"/bin/man", embedded_man_elf, embedded_man_elf_size},
    /* short names for convenience */
    {"hello", embedded_hello_elf, embedded_hello_elf_size},
    {"echo", embedded_echo_elf, embedded_echo_elf_size},
    {nullptr, nullptr, 0},
};

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
    current_idx = 0;
    next_pid = 1;
    console_session = false;
}

int current_pid() { return table[current_idx].pid; }
Process* current() { return &table[current_idx]; }
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

int sys_fork(Registers* trap)
{
    int slot = alloc_slot();
    if (slot < 0)
        return -VNU_ENOMEM;
    Process& parent = table[current_idx];
    Process& child = table[slot];
    child = {};
    child.pid = next_pid++;
    child.ppid = parent.pid;
    child.state = State::Runnable;
    child.uid = parent.uid;
    child.gid = parent.gid;
    child.user_stack_top =
        USER_STACK_BASE + static_cast<uint32_t>((slot + 1) * USER_STACK_SIZE);
    child.regs = *trap;
    child.regs.eax = 0;
    child.regs.eflags = trap->eflags ? trap->eflags : 0x202;
    return child.pid;
}

int sys_execve(Registers* trap, const char* path, char* const* argv)
{
    const EmbeddedProg* prog = find_embedded(path);
    if (!prog)
        return -VNU_ENOENT;

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

    uint32_t entry = vnu::elf::load(prog->data, prog->size);
    if (!entry)
        return -VNU_ENOEXEC;

    Process& p = table[current_idx];
    p.regs = {};
    p.regs.eip = entry;
    p.regs.eflags = 0x202;
    setup_user_stack(p, argc, arg_ptrs, path_copy);

    trap->eip = p.regs.eip;
    trap->esp = p.regs.esp;
    trap->eflags = p.regs.eflags;
    trap->eax = 0;
    trap->ebx = trap->ecx = trap->edx = 0;
    trap->esi = trap->edi = trap->ebp = 0;
    return 0;
}

extern "C" void vnu_enter_user(uint32_t entry, uint32_t user_esp);

static int enter_program(const char* path, int ppid)
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
        yield_current();
        /* resumed by the scheduler → loop and look for a zombie again */
    }
}

int sys_kill(int pid, int)
{
    for (int i = 0; i < MAX_PROCS; ++i) {
        if (table[i].state != State::Unused && table[i].pid == pid) {
            table[i].exit_code = 9;
            table[i].state = State::Zombie;
            return 0;
        }
    }
    return -VNU_ESRCH;
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
    vnu::paging::MapRange ranges[3] = {
        {0x00400000, 24},
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
    return enter_program(path, 0);
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
    vnu::paging::MapRange ranges[3] = {
        {0x00400000, 24},
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
    vnu_proc_switch(&g_sched_esp, p.coro_esp);
    /* Resumed here (on the scheduler's own stack) once the process has
     * yielded or exited. */
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
    g_sched_pgdir = vnu::paging::current_pgdir();
    const char* next = primary;
    int init_slot = -1;

    for (;;) {
        current_idx = 0;

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
