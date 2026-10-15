#include <vnu/arch/context.h>
#include <stddef.h>
#include <stdint.h>

/* The x86_64 port of vnu/arch/context.h.
 *
 * STATUS: bring-up skeleton. The accessor shapes mirror
 * arch/i386/context.cpp, widened to the 64-bit register set. The frame
 * sizes and the first-run layout follow the switch in
 * arch/x86_64/ctxswitch.s. The signal-frame layout and the ring3 entry
 * path are not finished - the port does not boot yet; see
 * arch/README.md. The generic kernel only ever sees this file through
 * the accessors declared in <vnu/arch/context.h>. */

extern "C" void vnu_enter_user(uint32_t entry, uint32_t user_esp);
extern "C" void vnu_proc_trampoline();
extern "C" void vnu_wintask_trampoline();
extern "C" uint32_t vnu_proc_pending_entry, vnu_proc_pending_stack;
extern "C" uint32_t vnu_wintask_pending_entry, vnu_wintask_pending_stack;

extern "C" uint32_t vnu_saved_eip, vnu_saved_esp, vnu_saved_ebp;

/* Consumed by arch/x86_64/syscall/syscall.s and ctxswitch.s. */
extern "C" {
uint32_t vnu_pending_user_esp = 0;
uint8_t vnu_pending_user_esp_flag = 0;
uint32_t vnu_pending_user_eip = 0;
uint8_t vnu_pending_jump_flag = 0;
}

namespace vnu::arch {

void regs_clear(Registers* r)
{
    *r = Registers{};
}

void regs_set_image(Registers* r, uint32_t entry, uint32_t stack, uint32_t flags)
{
    const uint64_t frame = r->frame;
    *r = Registers{};
    r->frame = frame;
    r->rip = entry;
    r->rsp = stack;
    r->rflags = flags;
}

uint32_t regs_entry(const Registers* r) { return static_cast<uint32_t>(r->rip); }
uint32_t regs_stack(const Registers* r) { return static_cast<uint32_t>(r->rsp); }
uint32_t regs_flags(const Registers* r) { return static_cast<uint32_t>(r->rflags); }
void regs_set_entry(Registers* r, uint32_t value) { r->rip = value; }
void regs_set_stack(Registers* r, uint32_t value) { r->rsp = value; }
void regs_set_flags(Registers* r, uint32_t value) { r->rflags = value; }
void regs_set_return(Registers* r, uint32_t value) { r->rax = value; }

uint32_t regs_frame(const Registers* r) { return static_cast<uint32_t>(r->frame); }

uint32_t frame_return(const TrapFrame* tf) { return static_cast<uint32_t>(tf->rax); }

uint32_t frame_resume_stack(uint32_t frame_base)
{
    return frame_base + sizeof(TrapFrame);
}

uint32_t frame_iret_stack(const TrapFrame* tf)
{
    /* A ring0 int gate pushes no rsp, so there is no saved user stack in
     * the frame the way i386's pushad slot carried one. The ring3 path
     * will need its own answer here. */
    (void)tf;
    return 0;
}

void request_stack_switch(uint32_t esp)
{
    vnu_pending_user_esp = esp;
    vnu_pending_user_esp_flag = 1;
}

bool stack_switch_pending() { return vnu_pending_user_esp_flag != 0; }

void request_jump(uint32_t eip, uint32_t esp)
{
    vnu_pending_user_eip = eip;
    vnu_pending_user_esp = esp;
    vnu_pending_jump_flag = 1;
}

bool jump_pending() { return vnu_pending_jump_flag != 0; }

uint32_t build_first_frame(WordWriter write, void* ctx, uint32_t stack_top,
                           const void* trampoline)
{
    /* Seven 8-byte slots the switch's epilogue pops: the six callee-saved
     * registers ctxswitch.s pushes, then the return address that lands on
     * the trampoline the first time this process is switched into.
     * WordWriter is a 32-bit store, so each slot is written as two words
     * (the high word zero: the kernel and the trampoline live below
     * 4 GiB). Writing only the low word leaves the high word as whatever
     * the fresh stack happened to hold, which the first `ret` would then
     * jump to. */
    const uint32_t va = stack_top - 7 * 8;
    for (uint32_t off = 0; off < 48; off += 8) {
        write(ctx, va + off, 0);
        write(ctx, va + off + 4, 0);
    }
    const uint32_t entry = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(trampoline));
    write(ctx, va + 48, entry);
    write(ctx, va + 52, 0);
    return va;
}

void fork_child_return(WordWriter write, void* ctx, uint32_t frame_base, uint32_t value)
{
    write(ctx, frame_base + offsetof(TrapFrame, rax), value);
}

void build_signal_frame(uint32_t frame_base, uint32_t anchor,
                        uint32_t handler, uint32_t restorer, uint32_t sig)
{
    /* Not yet implemented for x86_64: a long-mode signal frame needs its
     * own iretq layout (rip/cs/rflags/rsp/ss) and a restorer trampoline.
     * Left as a no-op so the port links; see arch/README.md. */
    (void)frame_base;
    (void)anchor;
    (void)handler;
    (void)restorer;
    (void)sig;
}

void enter_user(uint32_t entry, uint32_t stack)
{
    vnu_enter_user(entry, stack);
}

ConsoleContinuation console_continuation()
{
    return ConsoleContinuation{vnu_saved_eip, vnu_saved_esp, vnu_saved_ebp};
}

void set_console_continuation(const ConsoleContinuation& c)
{
    vnu_saved_eip = c.eip;
    vnu_saved_esp = c.esp;
    vnu_saved_ebp = c.ebp;
}

const void* proc_trampoline()
{
    return reinterpret_cast<const void*>(&vnu_proc_trampoline);
}

const void* wintask_trampoline()
{
    return reinterpret_cast<const void*>(&vnu_wintask_trampoline);
}

void set_proc_entry(uint32_t entry, uint32_t stack)
{
    vnu_proc_pending_entry = entry;
    vnu_proc_pending_stack = stack;
}

void set_wintask_entry(uint32_t entry, uint32_t stack)
{
    vnu_wintask_pending_entry = entry;
    vnu_wintask_pending_stack = stack;
}

} // namespace vnu::arch
