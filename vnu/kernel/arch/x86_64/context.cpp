#include <vnu/arch/context.h>
#include <vnu/arch/context_defs.h>
#include <cstdint>

using namespace vnu::arch;

namespace {
Registers* g_regs = nullptr;
TrapFrame* g_frame = nullptr;
std::uintptr_t g_pending_stack = 0;
bool g_pending_stack_flag = false;
std::uintptr_t g_pending_eip = 0;
bool g_pending_jump_flag = false;
}

Registers& vnu::arch::current_regs() { static Registers r; return g_regs ? *g_regs : r; }
TrapFrame* vnu::arch::current_frame() { return g_frame; }
void vnu::arch::set_current_frame(TrapFrame* f) { g_frame = f; }
void vnu::arch::set_current_regs(Registers* r) { g_regs = r; }
std::uintptr_t vnu::arch::regs_entry(const Registers&) { return 0; }
std::uintptr_t vnu::arch::regs_stack(const Registers& r) { return r.rsp; }
std::uintptr_t vnu::arch::regs_flags(const Registers&) { return 0; }
std::uintptr_t vnu::arch::frame_return(const TrapFrame&) { return 0; }
void vnu::arch::frame_resume_stack(TrapFrame&, std::uintptr_t) {}
void vnu::arch::frame_iret_stack(TrapFrame&, std::uintptr_t, std::uintptr_t) {}
void vnu::arch::request_stack_switch(std::uintptr_t sp) { g_pending_stack = sp; g_pending_stack_flag = true; }
void vnu::arch::request_jump(std::uintptr_t eip, std::uintptr_t sp) { g_pending_eip = eip; g_pending_stack = sp; g_pending_jump_flag = true; }
std::uintptr_t vnu::arch::pending_stack_switch() { return g_pending_stack; }
bool vnu::arch::has_pending_stack_switch() { return g_pending_stack_flag; }
void vnu::arch::clear_pending_stack_switch() { g_pending_stack_flag = false; }
std::uintptr_t vnu::arch::pending_jump_eip() { return g_pending_eip; }
std::uintptr_t vnu::arch::pending_jump_stack() { return g_pending_stack; }
bool vnu::arch::has_pending_jump() { return g_pending_jump_flag; }
void vnu::arch::clear_pending_jump() { g_pending_jump_flag = false; }
void vnu::arch::build_first_frame(TrapFrame*, std::uintptr_t, std::uintptr_t) {}
void vnu::arch::fork_child_return(Registers*, std::uintptr_t) {}
void vnu::arch::build_signal_frame(TrapFrame*, Registers*, std::uintptr_t, std::uintptr_t) {}
[[noreturn]] void vnu::arch::enter_user(std::uintptr_t, std::uintptr_t) { halt_forever(); }
void vnu::arch::console_continuation(void*, void*, void*, void*) {}
void vnu::arch::set_proc_entry(std::uintptr_t, std::uintptr_t) {}
void vnu::arch::set_wintask_entry(std::uintptr_t, std::uintptr_t) {}
void vnu::arch::proc_continuation(void*, void*, void*, void*) {}
void vnu::arch::wintask_continuation(void*, void*, void*, void*) {}
void vnu::arch::set_console_entry(std::uintptr_t, std::uintptr_t) {}
extern "C" { std::uintptr_t vnu_pending_user_esp = 0; char vnu_pending_user_esp_flag = 0; std::uintptr_t vnu_pending_user_eip = 0; char vnu_pending_jump_flag = 0; }
