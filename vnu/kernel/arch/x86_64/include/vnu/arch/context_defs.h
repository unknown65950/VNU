#pragma once
#include <cstdint>

namespace vnu::arch {

struct Registers {
    std::uint64_t rax, rbx, rcx, rdx;
    std::uint64_t rsi, rdi, rbp, rsp;
    std::uint64_t r8, r9, r10, r11;
    std::uint64_t r12, r13, r14, r15;
    std::uint64_t rip;
    std::uint64_t rflags;
    std::uint64_t frame;
};

struct TrapFrame {
    std::uint64_t r15, r14, r13, r12;
    std::uint64_t r11, r10, r9, r8;
    std::uint64_t rdi, rsi, rbp, rbx;
    std::uint64_t rdx, rcx, rax;
    std::uint64_t errcode;
    std::uint64_t rip;
    std::uint64_t cs;
    std::uint64_t rflags;
    std::uint64_t rsp;
    std::uint64_t ss;
};

} // namespace vnu::arch
