#pragma once
#include <stdint.h>
#include <stddef.h>

namespace vnu::proc {

constexpr int MAX_PROCS = 8;
constexpr uint32_t USER_STACK_BASE = 0x00600000;
constexpr uint32_t USER_STACK_SIZE = 0x10000;

enum class State : uint8_t { Unused=0, Runnable, Running, Blocked, Zombie };

struct Registers {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eip, eflags;
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
};

void init();
int current_pid();
Process* current();
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
