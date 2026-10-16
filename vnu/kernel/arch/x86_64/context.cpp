#include <vnu/arch/context.h>
#include <stddef.h>
#include <stdint.h>

/* The x86_64 port of vnu/arch/context.h.
 *
 * The userspace is 32-bit on both ports - here it runs in long mode's
 * 32-bit compatibility mode - so the shapes this file works with are
 * the i386 ones: Registers and TrapFrame carry the same field names in
 * 64-bit slots (see arch/x86_64/include/vnu/arch/context_defs.h), and
 * the accessors below read them the same way arch/i386/context.cpp
 * does. What is genuinely different here is the width of a live frame
 * address (a uintptr_t, not a uint32_t) and the size of the words a
 * frame is made of: eight bytes, because the CPU pushes and pops them
 * that way in long mode. The generic kernel only ever sees this file
 * through the accessors declared in <vnu/arch/context.h>. */

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

namespace {

/* Below the interrupted frame: a slot holding what the handler's
 * `return` pops ([0] = the restorer, [4] = the signal number, so a
 * handler written as `void h(int sig)` gets its argument from the same
 * place a `ret` would leave it), and just below that the five 64-bit
 * words a long-mode iretq pops (handler, cs, eflags, rsp, ss).
 *
 * The two halves are sized differently on purpose: the iret words are
 * 8 bytes wide because that is what a long-mode iretq pops, while the
 * block the handler returns through is 4-byte words because the
 * handler itself is 32-bit compat code, whose `ret` pops a dword. */
constexpr uint32_t SIG_BLOCK_WORDS = 4; /* restorer, signo, + align slack */
constexpr uint32_t SIG_IRET_WORDS = 5;  /* handler, cs, eflags, rsp, ss */

} // namespace

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
    r->eip = entry;
    r->esp = stack;
    r->eflags = flags;
}

uint32_t regs_entry(const Registers* r) { return static_cast<uint32_t>(r->eip); }
uint32_t regs_stack(const Registers* r) { return static_cast<uint32_t>(r->esp); }
uint32_t regs_flags(const Registers* r) { return static_cast<uint32_t>(r->eflags); }
void regs_set_entry(Registers* r, uint32_t value) { r->eip = value; }
void regs_set_stack(Registers* r, uint32_t value) { r->esp = value; }
void regs_set_flags(Registers* r, uint32_t value) { r->eflags = value; }
void regs_set_return(Registers* r, uint32_t value) { r->eax = value; }

uint32_t regs_frame(const Registers* r) { return static_cast<uint32_t>(r->frame); }
void regs_set_frame(Registers* r, uintptr_t value) { r->frame = value; }

uint32_t frame_return(const TrapFrame* tf) { return static_cast<uint32_t>(tf->eax); }

uint32_t frame_resume_stack(uint32_t frame_base)
{
    /* The frame records the pre-trap RSP in its rsp slot; that is where
     * the return path's iretq leaves esp. (i386 can compute the same
     * value as frame_base + sizeof(TrapFrame) because its hardware
     * frame sits directly on top; long mode's five-word frame makes the
     * record explicit instead.) */
    return static_cast<uint32_t>(
        *reinterpret_cast<const uint64_t*>(frame_base + offsetof(TrapFrame, rsp)));
}

uint32_t frame_iret_stack(const TrapFrame* tf)
{
    /* The eip word is where the return path has to point the iretq, so
     * it hands back the words this frame already holds - computed from
     * the frame address rather than the saved-ESP slot, which carries
     * the same value only by convention. */
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tf) + offsetof(TrapFrame, eip));
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
    /* Seven 8-byte slots the switch's epilogue pops: the six callee-saved
     * registers ctxswitch.s pushes, then the return address that lands on
     * the trampoline the first time this process is switched into.
     * WordWriter is a 32-bit store, so each slot is written as two words
     * (the high word zero: the kernel and the trampoline live below
     * 4 GiB). Writing only the low word leaves the high word as whatever
     * the fresh stack happened to hold, which the first `ret` would then
     * jump to. */
    const uint32_t va = stack_top - 7 * 8;
    for (uint32_t off = 0; off < 6 * 8; off += 8) {
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
    /* eax sits 56 bytes into a frame of eight qword registers (edi esi
     * ebp esp ebx edx ecx eax); the child's copy is patched in its own
     * address space, the parent's stays untouched. */
    write(ctx, frame_base + offsetof(TrapFrame, eax), value);
}

void build_signal_frame(uintptr_t frame_base, uintptr_t anchor,
                        uint32_t handler, uint32_t restorer, uint32_t sig)
{
    /* The same shape arch/i386/context.cpp builds, at the same place:
     * the block goes at the very bottom of what the kernel is using of
     * this process's stack, so that nothing that is still live - a
     * caller's locals on the way out of here, the frame the iretq is
     * about to pop - writes over it. The floor is the stack pointer
     * itself: i386 can take the address of a first-declared local
     * because its arguments live above the frame and a local is always
     * the deepest thing, but on x86_64 the compiler is free to place
     * this function's own locals - `frame_base' among them - below that
     * local, and it does, so `&here' would put the block right on top of
     * `frame_base'. A caller that is running on a *different* stack (the
     * scheduler handing a preempted process back in) has no frames on
     * the process's stack to avoid and passes the parked frame instead.
     * Every address below stays inside the low identity-mapped window
     * every VNU stack lives in, which is why the pending-ESP slot can
     * still be a uint32_t. */
    uintptr_t sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    const uintptr_t low = anchor ? anchor : sp;
    const uintptr_t block = (low - SIG_BLOCK_WORDS * 4) & ~uintptr_t{0xF};
    uint32_t* w = reinterpret_cast<uint32_t*>(block);
    w[0] = restorer; /* where the handler's `return` lands */
    w[1] = sig;
    /* A long-mode iretq pops five qwords in increasing address order,
     * and it runs with rsp = block - 40: handler, cs, eflags, rsp, ss.
     * The rsp it loads is `block` itself, so the handler starts with the
     * restorer/signo pair on top of its stack, exactly as i386's three
     * iret words left it - and lands back in compat mode (SS 0x20) at
     * the same privilege it left. */
    uint64_t* iret = reinterpret_cast<uint64_t*>(block) - SIG_IRET_WORDS;
    iret[0] = handler;
    iret[1] = *reinterpret_cast<const uint64_t*>(frame_base + offsetof(TrapFrame, cs));
    /* Same flags the context had, except that the handler runs with
     * interrupts on (the syscall path saved eflags with IF cleared) and
     * never with the trap flag still set, which would fire a
     * single-step interrupt out of the first instruction. */
    iret[2] = (*reinterpret_cast<const uint64_t*>(frame_base + offsetof(TrapFrame, eflags)) |
               0x200u) &
              ~0x100u;
    iret[3] = block;
    iret[4] = 0x20; /* compat data selector */

    request_stack_switch(static_cast<uint32_t>(block - SIG_IRET_WORDS * 8));
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
