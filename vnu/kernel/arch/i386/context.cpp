#include <vnu/arch/context.h>
#include <stddef.h>
#include <stdint.h>

/* The i386 port of vnu/arch/context.h. The register and frame layouts
 * come from <vnu/arch/context_defs.h>; everything below is the little
 * bit of i386 knowledge the generic kernel used to carry itself: the
 * pushad order, the offsets of eip/cs/eflags and eax inside a trap
 * frame, the shape of the cooperative switch frame, and the one-shot
 * flags the assembly return paths consume. */

extern "C" void vnu_enter_user(uint32_t entry, uint32_t user_esp);
extern "C" void vnu_proc_trampoline();
extern "C" void vnu_wintask_trampoline();
extern "C" uint32_t vnu_proc_pending_entry, vnu_proc_pending_stack;
extern "C" uint32_t vnu_wintask_pending_entry, vnu_wintask_pending_stack;

/* The single continuation vnu_enter_user() stashes for the console
 * return path (arch/i386/switch.s). */
extern "C" uint32_t vnu_saved_eip, vnu_saved_esp, vnu_saved_ebp;

/* Consumed by arch/i386/syscall/syscall.s and arch/i386/ctxswitch.s. */
extern "C" {
uint32_t vnu_pending_user_esp = 0;
uint8_t vnu_pending_user_esp_flag = 0;
uint32_t vnu_pending_user_eip = 0;
uint8_t vnu_pending_jump_flag = 0;
}

namespace {

/* Below the interrupted frame: a 16-byte-aligned slot holding what the
 * handler's `return` pops ([0] = the restorer, [4] = the signal number,
 * so a handler written as `void h(int sig)` gets its argument from the
 * same place a `ret` would leave it), and just below that the three
 * words an iret pops (handler, cs, eflags). */
constexpr uint32_t SIG_BLOCK_WORDS = 4; /* restorer, signo */
constexpr uint32_t SIG_IRET_WORDS = 3;  /* handler, cs, eflags */

} // namespace

namespace vnu::arch {

void regs_clear(Registers* r)
{
    *r = Registers{};
}

void regs_set_image(Registers* r, uint32_t entry, uint32_t stack, uint32_t flags)
{
    const uint32_t frame = r->frame;
    *r = Registers{};
    r->frame = frame;
    r->eip = entry;
    r->esp = stack;
    r->eflags = flags;
}

uint32_t regs_entry(const Registers* r) { return r->eip; }
uint32_t regs_stack(const Registers* r) { return r->esp; }
uint32_t regs_flags(const Registers* r) { return r->eflags; }
void regs_set_entry(Registers* r, uint32_t value) { r->eip = value; }
void regs_set_stack(Registers* r, uint32_t value) { r->esp = value; }
void regs_set_flags(Registers* r, uint32_t value) { r->eflags = value; }
void regs_set_return(Registers* r, uint32_t value) { r->eax = value; }

uint32_t regs_frame(const Registers* r) { return r->frame; }
void regs_set_frame(Registers* r, uintptr_t value)
{
    r->frame = static_cast<uint32_t>(value);
}

uint32_t frame_return(const TrapFrame* tf) { return tf->eax; }

uint32_t frame_resume_stack(uint32_t frame_base)
{
    return frame_base + sizeof(TrapFrame); /* where the iret leaves esp */
}

uint32_t frame_iret_stack(const TrapFrame* tf)
{
    /* popad discards the ESP the frame saved, so the return path is
     * pointed at the words the copy already wrote: their address is the
     * value pushad recorded in that slot (the frame's own eip). */
    return tf->esp;
}

void request_stack_switch(uint32_t esp)
{
    /* The flag is a single slot on purpose: it is set and consumed within
     * one return path, with interrupts off (a syscall handler runs
     * cli'd, the timer with the CLI in vnu_timer_isr), so no other
     * process can ever see it. */
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
    /* Five words the switch's epilogue pops as if a prior call had pushed
     * edi/esi/ebx/ebp and a return address, landing on the trampoline the
     * first time this process is switched into. */
    const uint32_t va = stack_top - 20;
    write(ctx, va + 0, 0); /* edi */
    write(ctx, va + 4, 0); /* esi */
    write(ctx, va + 8, 0); /* ebx */
    write(ctx, va + 12, 0); /* ebp */
    write(ctx, va + 16, reinterpret_cast<uint32_t>(trampoline));
    return va;
}

void fork_child_return(WordWriter write, void* ctx, uint32_t frame_base, uint32_t value)
{
    /* eax sits 28 bytes into a pushad frame (edi esi ebp esp ebx edx ecx
     * eax); the child's copy is patched in its own address space, the
     * parent's stays untouched. */
    write(ctx, frame_base + offsetof(TrapFrame, eax), value);
}

void build_signal_frame(uintptr_t frame_base, uintptr_t anchor,
                        uint32_t handler, uint32_t restorer, uint32_t sig)
{
    /* The block goes at the very bottom of what the kernel is using of
     * this process's stack, so that nothing that is still live - a
     * caller's locals on the way out of here, the frame the iret is about
     * to pop - writes over it. Usually that is this function's own frame,
     * the deepest thing on the stack right now. A caller that is running
     * on a *different* stack (the scheduler handing a preempted process
     * back in) has no frames on the process's stack to avoid and passes
     * the parked frame instead. */
    volatile char here = 0;
    uintptr_t low = anchor ? anchor : reinterpret_cast<uintptr_t>(&here);
    uintptr_t block = (low - SIG_BLOCK_WORDS * 4) & ~uintptr_t{0xF};
    uint32_t* w = reinterpret_cast<uint32_t*>(block);
    w[0] = restorer; /* where the handler's `return` lands */
    w[1] = sig;
    /* The iret pops its three words in increasing address order, and it
     * runs with esp = &w[-3]: eip, cs, eflags. */
    w[-3] = handler;
    w[-2] = *reinterpret_cast<const uint32_t*>(frame_base + offsetof(TrapFrame, cs));
    /* Same flags the context had, except that the handler runs with
     * interrupts on (the syscall path saved eflags with IF cleared by the
     * trap gate) and never with the trap flag still set, which would fire
     * a single-step interrupt out of the first instruction. */
    w[-1] = (*reinterpret_cast<const uint32_t*>(frame_base + offsetof(TrapFrame, eflags)) |
             0x200u) &
            ~0x100u;

    request_stack_switch(block - SIG_IRET_WORDS * 4);
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
