#include <vnu/arch/boot.h>
#include <vnu/arch/cpu.h>
#include <vnu/arch/io.h>
#include <vnu/arch/serial.h>
#include <stdint.h>

/* arch/x86_64/syscall/idt.cpp - the x86_64 interrupt descriptor table
 * and the PIC, the 64-bit counterpart of arch/i386/syscall/idt.cpp. A
 * long-mode gate is 16 bytes: a 64-bit handler address split across
 * three fields, an IST index (0 - this port runs on the current stack)
 * and the attribute byte.
 *
 * Unlike i386, this port does not yet run the (32-bit) userspace, so a
 * CPU exception is expected: the ELF loader loads an EM_386 image and
 * the port jumps into it in long mode. Rather than the bare `iretq`
 * i386 can afford (nothing faults there in normal use), every exception
 * here reports itself on the serial log and stops, on a private stack,
 * so the failure is visible instead of becoming an infinite fault loop
 * after the faulting stack is already gone. See arch/README.md. */

extern "C" void vnu_syscall_entry();
extern "C" void vnu_timer_isr();

/* The serial fault report. Defined below; the stub calls it on a
 * dedicated emergency stack because the faulting stack may be gone. */
extern "C" void vnu_x64_fault(uint64_t err, uint64_t rip, uint64_t cr2, uint64_t cs);

namespace {

struct IdtGate {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t ist;
    uint8_t flags;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

IdtGate idt[256];

struct IdtPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void set_gate(int vec, void (*handler)(), uint8_t flags)
{
    auto a = reinterpret_cast<uint64_t>(handler);
    idt[vec].off_lo = static_cast<uint16_t>(a & 0xFFFF);
    idt[vec].sel = 0x08; /* kernel code segment */
    idt[vec].ist = 0;
    idt[vec].flags = flags;
    idt[vec].off_mid = static_cast<uint16_t>((a >> 16) & 0xFFFF);
    idt[vec].off_hi = static_cast<uint32_t>((a >> 32) & 0xFFFFFFFF);
    idt[vec].zero = 0;
}

extern "C" void vnu_ignore_irq();
__asm__(
    ".global vnu_ignore_irq\n"
    ".type vnu_ignore_irq, @function\n"
    "vnu_ignore_irq:\n"
    "    iretq\n"
);

/* CPU exceptions. The port cannot run the 32-bit userspace yet, so the
 * first ELF entry faults; report it and stop. Two entry points because
 * some vectors push an error code and some do not: the no-error one
 * pushes a dummy so the common path always sees [rsp]=error code,
 * [rsp+8]=faulting rip. The report runs on its own stack. */
extern "C" void vnu_exc_common();
__asm__(
    ".global vnu_exc_common\n"
    ".type vnu_exc_common, @function\n"
    "vnu_exc_common:\n"
    "    mov %rsp, %rax\n"
    "    mov 0(%rax), %rdi\n"          /* error code */
    "    mov 8(%rax), %rsi\n"          /* rip */
    "    mov %cr2, %rdx\n"
    "    mov 16(%rax), %rcx\n"         /* cs */
    "    lea exc_stack_top(%rip), %rsp\n"
    "    and $-16, %rsp\n"
    "    call vnu_x64_fault\n"
    "1:  hlt\n"
    "    jmp 1b\n"
);
extern "C" void vnu_exc_noerr();
__asm__(
    ".global vnu_exc_noerr\n"
    ".type vnu_exc_noerr, @function\n"
    "vnu_exc_noerr:\n"
    "    pushq $0\n"
    "    jmp vnu_exc_common\n"
);

__asm__(
    ".section .bss\n"
    ".align 16\n"
    "exc_stack:\n"
    "    .skip 8192\n"
    "exc_stack_top:\n"
    ".previous\n"
);

/* Called by vnu_exc_common on the emergency stack. Prints one line to
 * the serial log and stops the machine. */
extern "C" void vnu_x64_fault(uint64_t err, uint64_t rip, uint64_t cr2, uint64_t cs)
{
    auto put = [](const char* s) {
        for (; *s; ++s)
            vnu::arch::debug_putc(*s);
    };
    auto hex = [&](uint64_t v) {
        char b[19];
        b[0] = '0';
        b[1] = 'x';
        for (int i = 0; i < 16; ++i) {
            const int nib = static_cast<int>((v >> ((15 - i) * 4)) & 0xF);
            b[2 + i] = static_cast<char>(nib < 10 ? '0' + nib : 'a' + nib - 10);
        }
        b[18] = 0;
        put(b);
    };
    put("\nx86_64 port: CPU exception (userspace is 32-bit; not run in long mode yet)\n");
    put("  err=");
    hex(err);
    put(" rip=");
    hex(rip);
    put(" cr2=");
    hex(cr2);
    put(" cs=");
    hex(cs);
    put("\n");
    vnu::arch::halt_forever();
}

extern "C" void vnu_irq_master_ack();
__asm__(
    ".global vnu_irq_master_ack\n"
    ".type vnu_irq_master_ack, @function\n"
    "vnu_irq_master_ack:\n"
    "    push %rax\n"
    "    mov $0x20, %al\n"
    "    out %al, $0x20\n"
    "    pop %rax\n"
    "    iretq\n"
);

extern "C" void vnu_irq_slave_ack();
__asm__(
    ".global vnu_irq_slave_ack\n"
    ".type vnu_irq_slave_ack, @function\n"
    "vnu_irq_slave_ack:\n"
    "    push %rax\n"
    "    mov $0x20, %al\n"
    "    out %al, $0xA0\n"
    "    out %al, $0x20\n"
    "    pop %rax\n"
    "    iretq\n"
);

} // namespace

void vnu::arch::interrupt_controller_init()
{
    uint8_t mask1 = io_in8(0x21);
    uint8_t mask2 = io_in8(0xA1);

    io_out8(0x20, 0x11);
    io_out8(0xA0, 0x11);
    io_out8(0x21, 0x20);
    io_out8(0xA1, 0x28);
    io_out8(0x21, 0x04);
    io_out8(0xA1, 0x02);
    io_out8(0x21, 0x01);
    io_out8(0xA1, 0x01);

    io_out8(0x21, mask1);
    io_out8(0xA1, mask2);
}

static void install_idt()
{
    for (int i = 0; i < 256; ++i)
        set_gate(i, vnu_ignore_irq, 0x8E);

    /* CPU exceptions 0..31: report and stop (see the stubs above). A few
     * push an error word; the rest get the dummy-pushing entry point. */
    for (int i = 0; i < 32; ++i) {
        const bool has_error = (i == 8 || i == 10 || i == 11 || i == 12 ||
                                i == 13 || i == 14 || i == 17 || i == 21 ||
                                i == 29 || i == 30);
        set_gate(i, has_error ? vnu_exc_common : vnu_exc_noerr, 0x8E);
    }

    set_gate(0x20, vnu_timer_isr, 0x8E);
    for (int i = 0x21; i <= 0x27; ++i)
        set_gate(i, vnu_irq_master_ack, 0x8E);
    for (int i = 0x28; i <= 0x2F; ++i)
        set_gate(i, vnu_irq_slave_ack, 0x8E);

    set_gate(0x80, vnu_syscall_entry, 0xEE);

    IdtPtr ptr{};
    ptr.limit = static_cast<uint16_t>(sizeof(idt) - 1);
    ptr.base = reinterpret_cast<uint64_t>(&idt[0]);
    asm volatile("lidt %0" : : "m"(ptr));
}

extern "C" void vnu_timer_init()
{
    constexpr uint16_t PIT_RELOAD = static_cast<uint16_t>(1193182 / 100);
    vnu::arch::io_out8(0x43, 0x36);
    vnu::arch::io_out8(0x40, static_cast<uint8_t>(PIT_RELOAD & 0xFF));
    vnu::arch::io_out8(0x40, static_cast<uint8_t>(PIT_RELOAD >> 8));
    vnu::arch::io_out8(0x21, static_cast<uint8_t>(vnu::arch::io_in8(0x21) & ~0x01u));
}

void vnu::arch::idt_init()
{
    install_idt();
}
