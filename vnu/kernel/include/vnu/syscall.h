#pragma once
#include <cstdint>
struct vnu_idt_gate { std::uint16_t off_lo, sel; std::uint8_t zero, flags; std::uint16_t off_hi; } __attribute__((packed));
extern "C" void vnu_syscall_entry();
void vnu_syscall_install(vnu_idt_gate* idt);
extern "C" void vnu_idt_init();
