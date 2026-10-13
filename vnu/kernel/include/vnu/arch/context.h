#pragma once
#include <stdint.h>

/* vnu/arch/context.h - the user register context.
 *
 * Two shapes matter to the generic kernel and both are opaque here:
 *
 *   Registers   the saved register set of a user process (Process::regs,
 *               and the scratch context execve passes around). The core
 *               moves whole sets around and reads or writes the fields
 *               it cares about only through the accessors below.
 *   TrapFrame   the live frame a trap leaves on the process's own
 *               stack, plus the word that says where that frame is.
 *
 * The generic core never names a register; it never knows what a frame
 * looks like on the stack or which word holds the syscall result. The
 * port gives the two structs their storage in vnu/arch/context_defs.h
 * and implements this file (registers, frame construction, the return
 * path's one-shot requests) under arch/<arch>/. See arch/README.md. */

#include <vnu/arch/context_defs.h>

namespace vnu::arch {

/* --- the saved register set --------------------------------------- */

/* A blank context, and one set up to start a new image at `entry` on
 * `stack`, flags `flags`, argument registers zeroed. The latter leaves
 * `frame` alone so an in-place exec keeps the process's saved frame. */
void regs_clear(Registers* r);
void regs_set_image(Registers* r, uint32_t entry, uint32_t stack, uint32_t flags);

uint32_t regs_entry(const Registers* r);   /* where execution resumes */
uint32_t regs_stack(const Registers* r);   /* the user stack pointer */
uint32_t regs_flags(const Registers* r);   /* processor flags word */
void regs_set_entry(Registers* r, uint32_t value);
void regs_set_stack(Registers* r, uint32_t value);
void regs_set_flags(Registers* r, uint32_t value);
void regs_set_return(Registers* r, uint32_t value);

/* The address of the live frame this snapshot came from, in the
 * process's address space; 0 if it has none. */
uint32_t regs_frame(const Registers* r);

/* --- the live trap frame ------------------------------------------ */

uint32_t frame_return(const TrapFrame* tf); /* the syscall result */

/* The stack pointer the process has once the frame at `frame_base` has
 * been popped, and the pointer the return path must adopt to resume
 * through `tf` (what sigreturn hands back). */
uint32_t frame_resume_stack(uint32_t frame_base);
uint32_t frame_iret_stack(const TrapFrame* tf);

/* --- the return path's one-shot requests -------------------------- */

/* Send the return path to `esp` instead of popping the frame it is
 * looking at: a freshly built signal handler, or a sigreturn. */
void request_stack_switch(uint32_t esp);
bool stack_switch_pending();

/* Abandon the trap frame and hand control to `eip` on `esp`: an execve
 * or a windowed task's landing. */
void request_jump(uint32_t eip, uint32_t esp);
bool jump_pending();

/* --- first-run and signal frames ---------------------------------- */

/* Writes a 32-bit word at `vaddr` in the address space `ctx` names. The
 * core supplies it because it, not the port, owns how a foreign address
 * space is reached (physical paging). */
using WordWriter = void (*)(void* ctx, uint32_t vaddr, uint32_t value);

/* Lay out the frame a process is switched into for the very first time,
 * just below `stack_top`, so the switch's epilogue pops the saved
 * registers and returns into `trampoline`. Returns the frame address. */
uint32_t build_first_frame(WordWriter write, void* ctx, uint32_t stack_top,
                           const void* trampoline);

/* Patch a forked child's own copy of the frame at `frame_base` so its
 * syscall returns `value`. */
void fork_child_return(WordWriter write, void* ctx, uint32_t frame_base, uint32_t value);

/* Install the words an iret pops to enter a signal handler below the
 * interrupted frame at `frame_base`, and arm the return path to switch
 * to it. `anchor` is the lowest address the fresh frame may use; 0 means
 * this call's own deepest local. */
void build_signal_frame(uint32_t frame_base, uint32_t anchor,
                        uint32_t handler, uint32_t restorer, uint32_t sig);

/* --- entering user mode ------------------------------------------- */

/* Enter user `entry` on `stack`. Returns when the process winds back to
 * the kernel through the legacy one-way path. */
void enter_user(uint32_t entry, uint32_t stack);

/* The legacy one-way console continuation: entering user stashes a
 * single (eip, esp, ebp) the console return path resumes, so a nested
 * launch saves and puts it back around the inner program. */
struct ConsoleContinuation {
    uint32_t eip, esp, ebp;
};
ConsoleContinuation console_continuation();
void set_console_continuation(const ConsoleContinuation& c);

/* The first-run frame's return address, one per kind of task, and the
 * entry/stack the matching trampoline will jump to on the next switch. */
const void* proc_trampoline();
const void* wintask_trampoline();
void set_proc_entry(uint32_t entry, uint32_t stack);
void set_wintask_entry(uint32_t entry, uint32_t stack);

} // namespace vnu::arch
