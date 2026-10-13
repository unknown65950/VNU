#include <vnu/arch/cpu.h>

// arch/i386/cpu.cpp - the i386 half of vnu/arch/cpu.h. Every "cli",
// "sti", "hlt", "pause" and locked read-modify-write in the kernel core
// ends up here.

namespace vnu::arch {

IrqState irq_save()
{
    IrqState flags;
    asm volatile("pushfl; popl %0" : "=r"(flags) :: "memory");
    asm volatile("cli" ::: "memory");
    return flags;
}

void irq_restore(IrqState flags)
{
    if (flags & 0x200u) /* EFLAGS.IF */
        asm volatile("sti" ::: "memory");
    else
        asm volatile("cli" ::: "memory");
}

void irq_disable()
{
    asm volatile("cli" ::: "memory");
}

void irq_enable()
{
    asm volatile("sti" ::: "memory");
}

bool interrupts_enabled()
{
    IrqState flags;
    asm volatile("pushfl; popl %0" : "=r"(flags));
    return (flags & 0x200u) != 0;
}

void halt()
{
    asm volatile("hlt");
}

void wait_for_interrupt()
{
    /* sti is not in effect until after the next instruction, so the hlt
     * is guaranteed to be interruptible even if a tick was already
     * pending; cli restores the kernel's usual interrupts-off state on
     * the way out. */
    asm volatile("sti; hlt; cli" ::: "memory");
}

void relax()
{
    asm volatile("pause" ::: "memory");
}

void barrier()
{
    asm volatile("" ::: "memory");
}

[[noreturn]] void halt_forever()
{
    for (;;)
        asm volatile("hlt");
}

namespace {
// One saved FPU state. A static global rather than the caller's stack:
// fxsave faults on a misaligned operand and a syscall handler runs on
// the process's own stack, whose alignment cannot be trusted. alignas
// here is a promise the linker keeps. The kernel has a single CPU and
// does not nest its FPU borrows, so one buffer is enough.
alignas(16) uint8_t g_fpu_state[512];
}

void fpu_save()
{
    asm volatile("fxsave %0" : : "m"(g_fpu_state) : "memory");
}

void fpu_restore()
{
    asm volatile("fxrstor %0" : : "m"(g_fpu_state) : "memory");
}

uint32_t atomic_exchange(volatile uint32_t* cell, uint32_t value)
{
    uint32_t old = value;
    asm volatile("lock xchgl %0, %1" : "+r"(old), "+m"(*cell) :: "memory");
    return old;
}

CpuId cpu_id()
{
    CpuId id{};
    uint32_t eax, ebx, ecx, edx;

    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    auto put4 = [&](int off, uint32_t v) {
        id.vendor[off + 0] = static_cast<char>(v & 0xFF);
        id.vendor[off + 1] = static_cast<char>((v >> 8) & 0xFF);
        id.vendor[off + 2] = static_cast<char>((v >> 16) & 0xFF);
        id.vendor[off + 3] = static_cast<char>((v >> 24) & 0xFF);
    };
    put4(0, ebx);
    put4(4, edx);
    put4(8, ecx);
    id.vendor[12] = 0;

    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    id.family = (eax >> 8) & 0xF;
    id.model = (eax >> 4) & 0xF;
    id.stepping = eax & 0xF;
    id.fpu = (edx & 1) != 0;
    return id;
}

[[noreturn]] void reboot()
{
    /* The 8042 keyboard controller's pulse-reset line is the only reset
     * that works after the firmware has handed control over. Give the
     * previous port writes time to drain first. */
    for (unsigned i = 0; i < 100000; ++i)
        asm volatile("" ::: "memory");
    asm volatile("outb %0, %1" : : "a"(static_cast<uint8_t>(0xFE)),
                 "Nd"(static_cast<uint16_t>(0x64)));
    halt_forever();
}

} // namespace vnu::arch
