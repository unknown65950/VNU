#include <vnu/arch/cpu.h>
#include <stddef.h>

// arch/x86_64/cpu.cpp - the x86_64 half of vnu/arch/cpu.h. The same
// instructions as arch/i386/cpu.cpp, but the flags word is 64 bits
// (pushfq/popfq) and the FPU state is saved in the 64-bit FXSAVE
// format, so a pointer in it is a full pointer and not a truncated one.

namespace vnu::arch {

IrqState irq_save()
{
    IrqState flags;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    asm volatile("cli" ::: "memory");
    return flags;
}

void irq_restore(IrqState flags)
{
    /* RFLAGS.IF. A 64-bit IrqState is fine: only the low bits matter. */
    if (flags & 0x200u)
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
    asm volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & 0x200u) != 0;
}

void halt()
{
    asm volatile("hlt");
}

void wait_for_interrupt()
{
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
/* One saved FPU state; 512 bytes covers the 64-bit FXSAVE image. A
 * static global rather than the caller's stack: fxsave64 faults on a
 * misaligned operand and the syscall handler's stack alignment cannot
 * be trusted. The kernel has a single CPU and does not nest borrows. */
alignas(16) uint8_t g_fpu_state[512];
}

void fpu_save()
{
    asm volatile("fxsave64 %0" : : "m"(g_fpu_state) : "memory");
}

void fpu_restore()
{
    asm volatile("fxrstor64 %0" : : "m"(g_fpu_state) : "memory");
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
    for (unsigned i = 0; i < 100000; ++i)
        asm volatile("" ::: "memory");
    asm volatile("outb %0, %1" : : "a"(static_cast<uint8_t>(0xFE)),
                 "Nd"(static_cast<uint16_t>(0x64)));
    halt_forever();
}

} // namespace vnu::arch
