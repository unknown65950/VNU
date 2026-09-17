#pragma once
#include <stdint.h>
#include <stddef.h>

namespace vnu::proc {

constexpr int MAX_PROCS = 8;
constexpr uint32_t USER_STACK_BASE = 0x00600000;
constexpr uint32_t USER_STACK_SIZE = 0x10000;

enum class State : uint8_t { Unused=0, Runnable, Running, Zombie };

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
    /* Coroutine-scheduler bookkeeping. Processes started via vnu::proc::
     * spawn() (scheduler-managed) live as coroutines in their own
     * private address space: while "running" they execute until they
     * block (waitpid/pipe/console read), at which point yield_current()
     * swaps back to the scheduler coroutine, which round-robins the
     * other runnable processes (see run_scheduler). All three fields
     * below are only meaningful for coro processes. */
    bool coro;          // managed by the classic-process scheduler
    bool started;       // first switch has happened (trampoline consumed)
    uint32_t coro_esp;  // suspended stack pointer (switch frame, or mid-
                        // syscall stack once blocked and resumed)
    uint32_t entry;     // ELF entry point, for the first-run trampoline
    uint32_t argv_esp;  // freshly built argc/argv stack for that entry
    uint32_t brk;       // per-process heap break (coro processes only,
                        // so parallel services don't stomp a shared g_brk)
};

void init();
int current_pid();
Process* current();
int sys_fork(Registers* trap);
int sys_execve(Registers* trap, const char* path, char* const* argv);
void sys_exit(int status);
int sys_getpid();
int sys_waitpid(int pid, int* status, int options);
int sys_kill(int pid, int sig);
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
