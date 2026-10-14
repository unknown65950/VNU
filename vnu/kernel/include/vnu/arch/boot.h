#pragma once

/* vnu/arch/boot.h - CPU bring-up and the interrupt table.
 *
 * kernel_main() runs with no C runtime under it, so the port has to put
 * the machine into a state the rest of the kernel can rely on before a
 * single instruction that can fault runs. The core calls these in order
 * (see kernel/kernel.cpp); a port with no equivalent of a step provides
 * an empty function rather than pretending the concept does not exist.
 *
 * Nothing here is PC-shaped on purpose: the i386 port happens to answer
 * interrupt_controller_init() by remapping an 8259, but a port with a
 * GIC or a PLIC answers the same call with its own controller.
 */

namespace vnu::arch {

/* Make sure paging is off (and protected mode on) whatever the boot
 * loader left behind, so vnu::paging::init() starts from a known state.
 * Called first, defensively. A port with no such notion is a no-op. */
void disable_paging();

/* Load the table that defines the segments (and privilege levels) the
 * kernel and userspace run on. */
void gdt_init();

/* Install the trap, exception and interrupt vector table: CPU faults,
 * the hardware IRQ vectors and the syscall gate. Must be in place before
 * the first instruction that can fault or the first sti. */
void idt_init();

/* Bring the interrupt controller up and move the hardware IRQ vectors to
 * offsets that do not collide with the CPU exception vectors (the PC's
 * 8259 powers up delivering IRQ0-7 at 0x08-0x0F, right on top of them).
 * Called after idt_init(), before any IRQ can be unmasked. */
void interrupt_controller_init();

} // namespace vnu::arch
