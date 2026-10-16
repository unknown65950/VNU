#pragma once
#include <stdint.h>

/* The x86_64 shape of vnu::arch::Registers and vnu::arch::TrapFrame,
 * pulled in by the generic interface in <vnu/arch/context.h>.
 *
 * The userspace is 32-bit on both ports (compat mode in long mode), so
 * the syscall ABI is the same 32-bit one: number in eax, arguments in
 * ebx/ecx/edx/esi/edi. The frame therefore carries the same field NAMES
 * as i386's TrapFrame, in 64-bit slots; only the widths differ, and the
 * generic kernel never sees these names.
 *
 * A long-mode frame is wider than i386's in two ways. The words a trap
 * leaves are eight bytes each, and the CPU pushes the complete five of
 * them (eip, cs, eflags, rsp, ss) on every interrupt and trap, with or
 * without a privilege change, so the return path irets straight out of
 * the hardware frame. Between the 32-bit registers and those words sit
 * r8-r15: 32-bit compat code cannot name them, so the core never does
 * either, but they are part of the live frame an interrupt saves and
 * restores. The saved ESP slot is the frame's own eip word, the value
 * pushad's ESP slot carried on i386.
 *
 * The generic kernel reaches these fields only through the accessors in
 * <vnu/arch/context.h>, so the widths and the extra slots do not leak
 * into the core. */

namespace vnu::arch {

struct Registers {
    uint64_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint64_t eip, eflags;
    /* The address of the live frame this snapshot was read from, in the
     * process's own address space; 0 when it has none. */
    uint64_t frame;
};

struct TrapFrame {
    /* Saved by the entry stub, lowest address first: the i386 pushad
     * order (edi esi ebp esp ebx edx ecx eax) in 64-bit slots, the
     * r8-r15 the 64-bit register file adds, then the five words a
     * long-mode iretq pops (eip, cs, eflags, rsp, ss). See
     * arch/x86_64/frame.inc for the assembly that builds this. */
    uint64_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t eip, cs, eflags, rsp, ss;
};

} // namespace vnu::arch
