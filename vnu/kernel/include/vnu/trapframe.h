#pragma once
#include <cstdint>

/* An interrupted context pushed on a process's stack, in the exact
 * order the CPU and pushad leave it: pushad's eight registers (lowest
 * address first) followed by the hardware frame's eip, cs, eflags.
 * Both the syscall entry and the timer ISR produce this identical
 * 44-byte layout, and every return tail pops it with popad + iretd. */
struct TrapFrame {
    std::uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    std::uint32_t eip, cs, eflags;
};