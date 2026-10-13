#pragma once
#include <stdint.h>

/* The i386 shape of vnu::arch::Registers and vnu::arch::TrapFrame,
 * pulled in by the generic interface in <vnu/arch/context.h>.
 *
 * A trap frame is pushad's eight dwords followed by the three words the
 * interrupt/syscall gate pushed (eip, cs, eflags) - the exact 44-byte
 * layout arch/i386/syscall/syscall.s and arch/i386/ctxswitch.s leave on
 * the process's own stack and pop with popad + iretd. A saved register
 * set is the same eight general registers plus eip and eflags, plus the
 * address of the live frame it was read from (0 when there is none).
 *
 * The generic kernel never names these fields: it goes through the
 * accessors declared in vnu/arch/context.h. Only the i386 port - the
 * syscall dispatcher and arch/i386/context.cpp - reads them directly,
 * which is exactly why the definition lives here and not in include/. */

namespace vnu::arch {

struct Registers {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eip, eflags;
    /* The address of the live frame this snapshot was read from, in the
     * process's own address space; 0 when it has none. A forked child
     * resumes from its own copy of exactly that frame. */
    uint32_t frame;
};

struct TrapFrame {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags;
};

} // namespace vnu::arch
