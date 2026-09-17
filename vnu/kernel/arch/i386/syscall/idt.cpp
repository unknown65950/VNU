#include <vnu/syscall.h>
#include <cstdint>

namespace {

vnu_idt_gate idt[256];

struct IdtPtr {
    std::uint16_t limit;
    std::uint32_t base;
} __attribute__((packed));

void set_gate(int vec, void (*handler)(), std::uint8_t flags)
{
    auto a = reinterpret_cast<std::uintptr_t>(handler);
    idt[vec].off_lo = static_cast<std::uint16_t>(a & 0xFFFF);
    idt[vec].sel = 0x08; /* kernel code segment from GRUB */
    idt[vec].zero = 0;
    idt[vec].flags = flags;
    idt[vec].off_hi = static_cast<std::uint16_t>((a >> 16) & 0xFFFF);
}

void outb(std::uint16_t port, std::uint8_t val)
{
    asm volatile("outb %0,%1" : : "a"(val), "Nd"(port));
}

std::uint8_t inb(std::uint16_t port)
{
    std::uint8_t v;
    asm volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

/* GAS (AT&T): use "iret", not NASM's "iretd".
 * Only for real CPU exceptions (vectors 0-31) that we don't otherwise
 * handle yet. NOTE: vectors 8, 10-14, 17 push an error code that this
 * stub does not pop — acceptable for now since they're rare/fatal
 * anyway, but a proper panic handler should account for that. */
extern "C" void vnu_ignore_irq();
__asm__(
    ".global vnu_ignore_irq\n"
    ".type vnu_ignore_irq, @function\n"
    "vnu_ignore_irq:\n"
    "    iret\n"
);

/* Hardware IRQ stub: acknowledge with EOI before returning, otherwise
 * the 8259 latches the in-service bit forever and stops delivering
 * further interrupts of equal/lower priority (looks like a random
 * freeze). Master-only version, for IRQ0-7 (remapped to 0x20-0x27). */
extern "C" void vnu_irq_master_ack();
__asm__(
    ".global vnu_irq_master_ack\n"
    ".type vnu_irq_master_ack, @function\n"
    "vnu_irq_master_ack:\n"
    "    push %eax\n"
    "    mov $0x20, %al\n"
    "    out %al, $0x20\n" /* EOI -> master PIC command port */
    "    pop %eax\n"
    "    iret\n"
);

/* Same, but also ACKs the slave PIC first — for IRQ8-15
 * (remapped to 0x28-0x2F), which are cascaded through the master. */
extern "C" void vnu_irq_slave_ack();
__asm__(
    ".global vnu_irq_slave_ack\n"
    ".type vnu_irq_slave_ack, @function\n"
    "vnu_irq_slave_ack:\n"
    "    push %eax\n"
    "    mov $0x20, %al\n"
    "    out %al, $0xA0\n" /* EOI -> slave PIC first */
    "    out %al, $0x20\n" /* then EOI -> master PIC */
    "    pop %eax\n"
    "    iret\n"
);

} // namespace

/* Remap the 8259 PIC so hardware IRQ0-15 land on vectors 0x20-0x2F
 * instead of the BIOS-default 0x08-0x0F/0x70-0x77, which collide with
 * CPU exception vectors (0x08 = #DF, 0x0D = #GP, 0x0E = #PF, ...).
 * MUST run before the first `sti` anywhere in the kernel. */
extern "C" void vnu_pic_remap()
{
    std::uint8_t mask1 = inb(0x21);
    std::uint8_t mask2 = inb(0xA1);

    outb(0x20, 0x11); /* ICW1: init, cascade, edge-triggered */
    outb(0xA0, 0x11);
    outb(0x21, 0x20); /* ICW2: master offset -> IRQ0-7 = vectors 0x20-0x27 */
    outb(0xA1, 0x28); /* ICW2: slave offset  -> IRQ8-15 = vectors 0x28-0x2F */
    outb(0x21, 0x04); /* ICW3: tell master there's a slave on IRQ2 */
    outb(0xA1, 0x02); /* ICW3: tell slave its cascade identity */
    outb(0x21, 0x01); /* ICW4: 8086 mode */
    outb(0xA1, 0x01);

    outb(0x21, mask1); /* restore previous IRQ masks */
    outb(0xA1, mask2);
}

void vnu_syscall_install(vnu_idt_gate* /*unused*/)
{
    for (int i = 0; i < 256; ++i)
        set_gate(i, vnu_ignore_irq, 0x8E); /* present, DPL0, 32-bit interrupt gate */

    /* Hardware IRQs, now remapped to 0x20-0x2F by vnu_pic_remap(). */
    for (int i = 0x20; i <= 0x27; ++i)
        set_gate(i, vnu_irq_master_ack, 0x8E);
    for (int i = 0x28; i <= 0x2F; ++i)
        set_gate(i, vnu_irq_slave_ack, 0x8E);

    /* Syscall vector: DPL=3 so ring3 can invoke; we still run ring0 for now. */
    set_gate(0x80, vnu_syscall_entry, 0xEE);

    IdtPtr ptr{};
    ptr.limit = static_cast<std::uint16_t>(sizeof(idt) - 1);
    ptr.base = reinterpret_cast<std::uint32_t>(&idt[0]);
    asm volatile("lidt %0" : : "m"(ptr));
}

extern "C" void vnu_idt_init()
{
    vnu_syscall_install(nullptr);
}
