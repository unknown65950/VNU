#pragma once
#include <stdint.h>

/* The x86_64 shape of vnu::arch::Registers and vnu::arch::TrapFrame,
 * pulled in by the generic interface in <vnu/arch/context.h>.
 *
 * STATUS: bring-up skeleton for the x86_64 port; the port does not yet
 * boot (see arch/README.md). A trap frame is the entry stub's saved
 * general registers followed by the words a long-mode int gate pushes
 * (rip, cs, rflags); rsp/ss only appear once a ring3 user path exists.
 * The generic kernel reaches these fields only through the accessors in
 * <vnu/arch/context.h>, so the widths here do not leak into the core. */

namespace vnu::arch {

struct Registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rsp, rbx, rdx, rcx, rax;
    uint64_t rip, rflags;
    /* The address of the live frame this snapshot was read from, in the
     * process's own address space; 0 when it has none. */
    uint64_t frame;
};

struct TrapFrame {
    /* Saved by the entry stub, lowest address first. */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    /* Pushed by the processor when the gate fires. */
    uint64_t rip, cs, rflags;
};

} // namespace vnu::arch
