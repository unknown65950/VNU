#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vnu/abi.h>
#include <vnu/trapframe.h>
#include <vnu/wintask.h>

namespace vnu::proc {

constexpr int MAX_PROCS = 8;
constexpr uint32_t USER_STACK_BASE = 0x00600000;
constexpr uint32_t USER_STACK_SIZE = 0x10000;

enum class State : uint8_t { Unused=0, Runnable, Running, Blocked, Zombie };

struct Registers {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eip, eflags;
    /* Where this snapshot was read from, when the caller can say: the
     * address of the interrupt frame the CPU pushed on the *process's
     * own* stack (a syscall runs on it - this kernel has one address
     * space and no TSS ring stack), as a virtual address in that
     * process. A forked child resumes from its copy of exactly this
     * frame, which is why the value has to be a virtual address and
     * not just a register set (see sys_fork). 0 = not recorded. */
    uint32_t frame;
};

struct Process {
    int pid;
    int ppid;
    State state;
    Registers regs;
    uint32_t user_stack_top;
    int exit_code;
    uint32_t pgdir_phys; // 0 = none yet (shared/identity space)
    uint32_t uid;       // POSIX user id (0 = root); inherited through
                        // spawn/fork, kept across execve
    uint32_t gid;       // primary group id
    /* Coroutine-scheduler bookkeeping. Processes started via vnu::proc::
     * spawn() (scheduler-managed) live as coroutines in their own
     * private address space: while "running" they execute until they
     * block (waitpid/pipe/console read), at which point yield_current()
     * swaps back to the scheduler coroutine, which round-robins the
     * other runnable processes (see run_scheduler). All three fields
     * below are only meaningful for coro processes. */
    bool coro;          // managed by the classic-process scheduler
    bool started;       // first switch has happened (trampoline consumed)
    bool preempted;     // parked by the timer ISR: coro_esp holds a full
                        // pushad+hardware interrupt frame, and run_slice
                        // must resume it with popad+iretd rather than the
                        // cooperative yield's callee-saved switch (see
                        // kernel/proc/ctxswitch.s)
    uint32_t wait_chan;   // event wait channel while Blocked (0 = none);
                          // a wakeup(chan) call marks the process Runnable
    uint32_t sleep_until; // jiffies deadline while Blocked (0 = no timed
                          // wake); the scheduler wakes the process when
                          // now_jiffies() passes it
    uint32_t coro_esp;  // suspended stack pointer (switch frame, or mid-
                        // syscall stack once blocked and resumed)
    uint32_t entry;     // ELF entry point, for the first-run trampoline
    uint32_t argv_esp;  // freshly built argc/argv stack for that entry
    uint32_t brk;       // per-process heap break (coro processes only,
                        // so parallel services don't stomp a shared g_brk)
    uint32_t app_pages; // pages mapped in the 0x400000 app region
    /* argv[0] basename, as in /proc/self/status "Name:". Filled when the
     * program starts (spawn or exec), so ps-like tools show which
     * command a process actually runs instead of one global string. */
    char name[16];
    /* --- signals (numbers, flags and the action struct live in
     * <vnu/abi.h>) ---------------------------------------------------
     * A set of signals is one word, bit n being signal n. Nothing is
     * blocked and nothing is handled until a program says so: a fresh
     * process has a zeroed mask, so a signal that nobody asked about
     * does what a signal always does to an ordinary process, which is
     * terminate the process.
     *
     * sig_action[] is the whole answer to "what happens to this signal",
     * one entry per signal: the address its handler runs at (or one of
     * the two non-addresses, VNU_SIG_DFL / VNU_SIG_IGN) together with
     * the set it asks to be blocked for while that handler runs and the
     * restorer its `return` lands on. */
    struct vnu_sigaction sig_action[VNU_NSIG];
    uint32_t sig_pending;    /* raised, not yet delivered */
    uint32_t sig_mask;       /* blocked right now */
    uint32_t sig_saved_mask; /* the mask from before the running handler,
                              * put back when that handler is done */
    uint32_t sig_frame;      /* the base of the interrupted context's
                              * frame while a handler is running, else 0;
                              * sigreturn puts that context back */
    uint32_t alarm_at;       /* the jiffy SIGALRM is raised at, 0 when no
                              * alarm is set; see sys_alarm */
};

void init();
int current_pid();
Process* current();

/* Name of the shell that started the session this process belongs to
 * ("vash"): the closest ancestor running one of the shell aliases, as
 * a NUL-terminated string written into `out` (max `cap` bytes).
 * Returns 0 on success, -1 when the caller was not started by a shell
 * (the kernel's own pid 0). */
int session_shell(char* out, int cap);
int sys_fork(Registers* trap);
int sys_execve(Registers* trap, const char* path, char* const* argv);
void sys_exit(int status);
int sys_getpid();
int sys_waitpid(int pid, int* status, int options);
/* Sleep for `ms` milliseconds. Blocks the current coro process (the PIT
 * timer runs it back at the deadline); returns -EAGAIN for the legacy
 * one-way / windowed path that has no scheduler to come back to. */
int sys_sleep(uint32_t ms);
int sys_kill(int pid, int sig);
/* --- signals (see <vnu/abi.h> for the numbers and the flags) ----------
 * sys_rt_sigaction() reads the requested handler/flags/mask out of
 * `act` and, when `old` is not null, writes the previous one there;
 * the restorer is required, so that the kernel is always told where a
 * handler's `return` lands.
 * sys_rt_sigprocmask() adds (`how` = SIG_BLOCK), removes
 * (SIG_UNBLOCK) or replaces (SIG_SETMASK) signals in the blocked set
 * that applies outside a handler. SIGKILL is never in that set.
 * sys_rt_sigreturn() is not a normal syscall: it is the address
 * sigaction() puts in VNU_SA_RESTORER, and a handler's `return` jumps
 * to it, which puts the interrupted context back.
 * sys_sigraise() raises a signal in the calling process, which is how
 * a program asks to be interrupted itself.
 * sys_sigpending() writes the raised-but-undelivered signals as a set
 * (signal 0 is the "is anything pending" probe VNU_SYS_kill(pid, 0)
 * already offers, so it stays the only caller of that shape).
 * sys_alarm() arms SIGALRM `seconds` from now (0 cancels) and returns
 * what was left of the previous alarm, 0 if there was none.
 * signal_kill() is the entry point for raising a signal in another
 * process: SIGKILL and SIGINT are raised, and a signal that kills does
 * not wait to be delivered. */
int sys_rt_sigaction(int sig, const uint32_t* act, uint32_t* old);
int sys_rt_sigprocmask(int how, const uint32_t* set, uint32_t* old);
uint32_t sys_rt_sigreturn(TrapFrame* cur);
int sys_sigraise(int sig);
int sys_sigpending(uint32_t* set);
uint32_t sys_alarm(uint32_t seconds);
int signal_kill(int pid, int sig);
/* SIGINT at the console (^C), raised at whoever is reading it. Returns
 * true when that interrupt is waiting to be delivered, so the read that
 * noticed the key can report EINTR; false when the signal was ignored
 * or blocked, and the read goes on waiting for a key like nothing
 * happened. */
bool console_signal(int sig);
/* The signal that would be delivered next to this process, lowest
 * first, or 0 if none of its pending signals is deliverable. */
int pending_delivery(const Process* p);
/* True when the current process has a signal waiting that is neither
 * blocked nor being ignored, i.e. the syscall it is inside cannot
 * finish and has to say so with EINTR. */
bool interrupt_pending();
/* Build this process's handler frame below `stack_anchor` and switch to
 * it on the way out. Returns the signal delivered, or 0 if there was
 * nothing to deliver. A signal whose action is to terminate does not
 * come through here: it ends the process where it was raised.
 *
 *   frame_base  base of the context being abandoned, where the saved
 *               cs/eflags to run the handler with are read from.
 *   stack_anchor an address on the process's stack to build the frame
 *               below, or 0 to use this function's own frame - which is
 *               the right answer for a caller running on the interrupted
 *               process's stack, since the kernel has no stack of its
 *               own. The kernel runs on the interrupted process's own
 *               stack, so the C frames of the syscall/ISR path sit
 *               right underneath the interrupted frame; a handler frame
 *               placed there would land in a caller's locals. */
int deliver_signal_at(Process& p, uint32_t frame_base, uint32_t stack_anchor,
                      int sig);
/* The same, for a windowed task: its five signal fields live in the
 * Console, and this builds the handler frame the same way. */
int deliver_console_signal(vnu::wintask::Console& c, uint32_t frame_base,
                           uint32_t stack_anchor, int sig);
/* The same, for the frame the current process is about to return from
 * a syscall with (the syscall's own pushad frame, which the caller
 * holds). Does nothing for a process the scheduler does not own. */
int deliver_pending_signal(uint32_t frame_base);

/* --- Signals for windowed tasks (GUI apps) ---
 *
 * A windowed task is not a Process: the GUI keeps its own table (see
 * vnu::wintask), it runs on its own stack with its own cooperative switch
 * and it is never in the round-robin, so there is no Process entry and
 * no pid to raise a signal in. Its signal state therefore lives in its
 * Console, in the same fields a Process has, and these three are the
 * windowed equivalents of signal_kill() / pending_delivery() /
 * deliver_pending_signal().
 *
 * The actions are the same - a default action on a task closes the
 * window, SIG_IGN drops it, a handler waits for the next boundary - so
 * a program cannot tell from the outside which kind it is in except by
 * what its default action does. */
int signal_task(vnu::wintask::Console* c, int sig);
int pending_task_delivery(const vnu::wintask::Console* c);
int deliver_task_pending_signal(vnu::wintask::Console* c, uint32_t frame_base);
/* True when the calling task has a deliverable signal waiting, which is
 * what makes a blocking read report EINTR instead of going on waiting. */
bool task_interrupt_pending();
/* User identity. uid 0 (root) bypasses VFS permission checks. setuid/
 * setgid are only allowed for root or to keep your own ids. */
uint32_t sys_getuid();
uint32_t sys_getgid();
int sys_setuid(uint32_t uid);
int sys_setgid(uint32_t gid);
int run_program(const char* path);
/* Coroutine-scheduler API (parallel services). spawn() builds a
 * brand-new coro process in its own private address space (ELF image
 * loaded, argc/argv stack + first-run switch frame laid out) and
 * returns its pid WITHOUT running it — the scheduler picks it up on a
 * later pass. Used by userspace init (SYS_spawn) and by the kernel's
 * own boot path. */
int sys_spawn(const char* path);
/* True while the CURRENT process is scheduler-managed (only coro
 * processes may call yield_current; the legacy one-way run_program
 * path has no scheduler to come back to). */
bool can_yield();
/* Blocks the current coro process, parking its coro_esp, and returns
 * control to the scheduler coroutine. The syscall handler that called
 * this resumes in place on the next scheduler pass over this process. */
void yield_current();
/* --- The common wait mechanism (locks, pipes, blocking I/O) ----------
 * jiffies: 100 Hz tick counter advanced by the PIT IRQ0 handler.
 * sleep_on() blocks the current coro process until wakeup() on `chan`
 * runs OR the jiffies deadline `until_jiffies` passes (0 = event only).
 * Woken waiters are made Runnable and the caller's loop re-checks its
 * predicate — there are no lost wakeups on this single CPU because the
 * entire check+sleep sequence runs under a cli'd syscall handler and
 * wakeup() is only reachable from another process's syscall, which
 * cannot run in the middle of it. Non-coro processes cannot block and
 * return immediately. wakeup()/wakeup_one() are the interrupt-decoupled
 * producers (any process may wake a sleeper). */
uint32_t now_jiffies();
inline uint32_t ms_to_jiffies(uint32_t ms) { return ms ? (ms + 9) / 10 : 0; }
void sleep_on(uint32_t chan, uint32_t until_jiffies);
void wakeup(uint32_t chan);
void wakeup_one(uint32_t chan);
inline void sleep_ms(uint32_t ms) { sleep_on(0, now_jiffies() + ms_to_jiffies(ms)); }
/* Cooperative scheduler + init respawn loop. Enters the scheduler
 * coroutine (never returns on success): lazily spawns `primary` (or
 * `fallback` if that path isn't embedded), then round-robins one slice
 * to every runnable coro process per pass, reaping orphaned zombies
 * (ppid == 0). Returns 0 forever in practice; a negative error only if
 * neither init path can be spawned. */
int run_scheduler(const char* primary, const char* fallback);
/* Looks up a path (e.g. "/bin/ls") in the embedded-binary table used
 * by the classic execve() path, without launching it. Returns the ELF
 * bytes and size, or nullptr if there's no embedded program at that
 * path. Used by vnu::wintask's own execve bridge (see
 * kernel/gui/wintask.cpp) — a windowed app that calls execve()
 * (namely `term`/vash, running another command) needs the same
 * lookup, but can't go through run_program_from_memory() or
 * sys_execve(), neither of which know about wintask's separate
 * scheduling/stack model. */
const uint8_t* find_embedded_data(const char* path, uint32_t& out_size);
/* Launch an ELF image that is already sitting in memory (e.g. read out
 * of a VFS file) rather than the static embedded-binary table. Used by
 * the /apps/ launcher, since app binaries are real VFS files, not
 * compiled-in blobs. Same one-way handoff semantics as run_program:
 * on success this does not return (jumps into the new program), only
 * a negative error code comes back on failure. */
int run_program_from_memory(const char* argv0_path, const uint8_t* data, uint32_t size);
bool schedule();

/* Per-"process" brk/heap bump allocator (see kernel/proc/process.cpp).
 * There's a single flat address space and no paging yet, so this is a
 * single fixed-size region reused by whichever program is currently
 * running — brk_reset() is called for every fresh process launch so
 * each one starts with its own empty heap, the same way its .bss
 * starts freshly zeroed. Without that reset, the first program to
 * call malloc() would permanently claim the whole region and every
 * later malloc() in any later program would fail forever. */
constexpr uint32_t BRK_MIN = 0x00700000;
constexpr uint32_t BRK_MAX = 0x00800000; // vlibc's malloc() always grows the
                                          // heap by exactly 1 MiB on its very
                                          // first call (see vlibc/src/stdlib/
                                          // malloc.c) regardless of how much
                                          // was actually requested, so this
                                          // must stay a full MiB above
                                          // BRK_MIN or malloc() fails outright.
void brk_reset();
uint32_t brk_current();
uint32_t brk_set(uint32_t requested);

} // namespace vnu::proc
