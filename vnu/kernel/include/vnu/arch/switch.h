#pragma once
#include <stdint.h>

/* vnu/arch/switch.h - the cooperative coroutine switch.
 *
 * The kernel runs several "coroutines" on the one CPU - the scheduler,
 * each classic process, each windowed task, the setup stack a program is
 * launched on - and giving control from one to another is the one thing
 * that is pure machine convention: which registers survive a call, where
 * a saved stack pointer points, how a fresh stack is entered. The core
 * never spells that out; it parks an opaque stack pointer and asks this
 * module to switch.
 *
 * A cooperative switch only ever happens at an explicit call site (a
 * blocking read, a yield, an exit), so by the ordinary calling convention
 * the caller has already accepted that the caller-saved registers may
 * not survive. The asynchronous case - the timer seizing the CPU - is the
 * exception and has its own pair below (preempt/resume_preempted), which
 * carry the interrupted process's whole register state.
 *
 * `new_pgdir` is the page directory to install before resuming on
 * `new_esp`; a caller that stays in the current address space passes the
 * `vnu::paging` directory it is already in (or uses switch_stack, which
 * does not touch the address space at all). The old directory is not
 * reported back - nothing needs it. See arch/README.md. */

namespace vnu::arch {

/* Save the current stack pointer to *old_esp_out and resume on new_esp,
 * in the current address space. */
void switch_stack(uintptr_t* old_esp_out, uintptr_t new_esp);

/* The same switch, but install new_pgdir as the active page directory
 * before resuming - for coroutines that live in different address
 * spaces (the GUI and a windowed task, the scheduler and a process). */
void switch_stack_pd(uintptr_t* old_esp_out, uintptr_t new_esp, uintptr_t new_pgdir);

/* The preemption side of the timer switch. Called from inside the timer
 * interrupt on the interrupted process's own stack: the caller has
 * already recorded the process's full interrupt frame, so this does NOT
 * save the current stack - it resumes the scheduler parked at sched_esp,
 * installing new_pgdir. Never returns. */
void preempt(uintptr_t sched_esp, uintptr_t new_pgdir);

/* The scheduler side: park the current stack in *old_esp_out and return
 * into the process's saved interrupt frame on new_esp, installing
 * new_pgdir - a full register restore rather than the callee-saved
 * switch above, because the process was interrupted mid-instruction. */
void resume_preempted(uintptr_t* old_esp_out, uintptr_t new_esp, uintptr_t new_pgdir);

/* Start the periodic preemption clock. The port calls back into the core
 * (a plain extern "C" vnu_timer_tick(uintptr_t frame_esp) in
 * kernel/proc/process.cpp) on every tick. */
void timer_init();

} // namespace vnu::arch
