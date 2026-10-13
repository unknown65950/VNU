#pragma once
#include <stdint.h>

// vnu/arch/cpu.h - the processor half of the architecture interface.
//
// Nothing in the kernel core may spell out an instruction-set detail
// such as "cli", "sti", "hlt" or a test-and-set sequence: it calls the
// functions below instead, and exactly one file per architecture -
// arch/<arch>/cpu.cpp - knows the machine. Adding a port means
// providing that file (and the rest of this directory's headers), not
// editing the core. See arch/README.md.

namespace vnu::arch {

// Interrupt masking. save() turns interrupts off and returns an opaque
// cookie holding the previous state; hand that cookie back to restore()
// to put the machine exactly as it was. Callers must not assume the
// cookie is a flags word, nor that restoring is free.
using IrqState = unsigned long;

IrqState irq_save();
void irq_restore(IrqState state);
void irq_disable();
void irq_enable();
bool interrupts_enabled();

// Idle and spin hints.
void halt();               // stop until the next interrupt, then return
void wait_for_interrupt(); // enable interrupts, halt once, disable again
void relax();              // hint that this CPU is busy-waiting
void barrier();            // full compiler and memory ordering barrier

[[noreturn]] void halt_forever();

// Floating-point state. The core runs with the FPU off, and the compiler
// is told never to emit FP code for it; the JPEG decoder in
// gui/wallpaper.cpp is the single exception and borrows the unit for the
// length of a decode, saving the previous state first so whatever a
// program left in the registers is what it finds again. Not nestable -
// the kernel's borrows do not overlap.
void fpu_save();
void fpu_restore();

// Atomically store `value` into *cell and return the old contents. This
// is the primitive vnu::sync's spinlock is built on; every architecture
// has to offer it (an acquire/release pair is an acceptable stand-in,
// as long as the exchange itself is atomic).
uint32_t atomic_exchange(volatile uint32_t* cell, uint32_t value);

// Identity of the processor, for /proc/cpuinfo. An architecture with no
// equivalent notion reports a zeroed vendor, zero family/model/stepping
// and fpu = false.
struct CpuId {
    char vendor[13];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    bool fpu;
};

CpuId cpu_id();

// Hard platform reset. Never returns.
[[noreturn]] void reboot();

} // namespace vnu::arch
