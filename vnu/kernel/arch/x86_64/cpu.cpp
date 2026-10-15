#include <vnu/arch/cpu.h>
#include <vnu/arch/io.h>
#include <cstdint>

std::uint32_t vnu::arch::atomic_exchange(std::uint32_t* ptr, std::uint32_t val) {
    asm volatile("lock xchg %0, (%1)" : "+r"(val) : "r"(ptr) : "memory");
    return val;
}

vnu::arch::IrqState vnu::arch::irq_save() {
    std::uint64_t rflags;
    asm volatile("pushfq; popq %0" : "=r"(rflags));
    IrqState s = static_cast<IrqState>(rflags);
    asm volatile("cli");
    return s;
}
void vnu::arch::irq_restore(IrqState s) {
    asm volatile("pushq %0; popfq" : : "r"(static_cast<std::uint64_t>(s)) : "memory");
}
void vnu::arch::irq_disable() {
    asm volatile("cli");
}
void vnu::arch::irq_enable() {
    asm volatile("sti");
}
void vnu::arch::halt() {
    asm volatile("hlt");
}
void vnu::arch::wait_for_interrupt() {
    asm volatile("hlt");
}
void vnu::arch::relax() {
    asm volatile("pause");
}
void vnu::arch::barrier() {
    asm volatile("mfence");
}
[[noreturn]] void vnu::arch::halt_forever() {
    for (;;) { asm volatile("hlt"); }
}
void vnu::arch::fpu_save(void* state) {
    asm volatile("fxsave64 (%0)" : : "r"(state) : "memory");
}
void vnu::arch::fpu_restore(void* state) {
    asm volatile("fxrstor64 (%0)" : : "r"(state) : "memory");
}
void vnu::arch::cpuid(vnu::arch::CpuId* out) {
    if (!out) return;
    std::uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    out->vendor[0] = ebx & 0xFF; out->vendor[1] = (ebx >> 8) & 0xFF; out->vendor[2] = (ebx >> 16) & 0xFF; out->vendor[3] = (ebx >> 24) & 0xFF;
    out->vendor[4] = edx & 0xFF; out->vendor[5] = (edx >> 8) & 0xFF; out->vendor[6] = (edx >> 16) & 0xFF; out->vendor[7] = (edx >> 24) & 0xFF;
    out->vendor[8] = ecx & 0xFF; out->vendor[9] = (ecx >> 8) & 0xFF; out->vendor[10] = (ecx >> 16) & 0xFF; out->vendor[11] = (ecx >> 24) & 0xFF;
    out->vendor[12] = 0;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    out->family = (eax >> 8) & 0xF;
    out->model = (eax >> 4) & 0xF;
    out->stepping = eax & 0xF;
    out->fpu = (edx >> 0) & 1;
}
void vnu::arch::reboot() {
    for (;;) {
        std::uint8_t v = vnu::arch::io_in8(0x64);
        if ((v & 0x02) == 0) break;
    }
    vnu::arch::io_out8(0x64, 0xFE);
    halt_forever();
}
