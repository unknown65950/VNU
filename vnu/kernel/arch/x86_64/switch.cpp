#include <vnu/arch/switch.h>
#include <stdint.h>

/* The i386 port of vnu/arch/switch.h.
 *
 * The switch itself is assembly (arch/i386/ctxswitch.s); this file is
 * only the C names the core calls and the ABI they share. Keeping the
 * page directory an argument rather than a global - it used to live in
 * the vnu_*_new_pd slots the C side had to write first - is what lets the
 * classic-process scheduler and the windowed-task manager stop sharing
 * (and clobbering) scratch state through the assembly. */

extern "C" void vnu_swtch(uintptr_t* old_esp_out, uintptr_t new_esp);
extern "C" void vnu_swtch_pd(uintptr_t* old_esp_out, uintptr_t new_esp, uintptr_t new_pgdir);
extern "C" void vnu_proc_preempt(uintptr_t sched_esp, uintptr_t new_pgdir);
extern "C" void vnu_proc_resume_preempted(uintptr_t* old_esp_out, uintptr_t new_esp,
                                          uintptr_t new_pgdir);
extern "C" void vnu_timer_init();

namespace vnu::arch {

void switch_stack(uintptr_t* old_esp_out, uintptr_t new_esp)
{
    vnu_swtch(old_esp_out, new_esp);
}

void switch_stack_pd(uintptr_t* old_esp_out, uintptr_t new_esp, uintptr_t new_pgdir)
{
    vnu_swtch_pd(old_esp_out, new_esp, new_pgdir);
}

void preempt(uintptr_t sched_esp, uintptr_t new_pgdir)
{
    vnu_proc_preempt(sched_esp, new_pgdir);
}

void resume_preempted(uintptr_t* old_esp_out, uintptr_t new_esp, uintptr_t new_pgdir)
{
    vnu_proc_resume_preempted(old_esp_out, new_esp, new_pgdir);
}

void timer_init()
{
    vnu_timer_init();
}

} // namespace vnu::arch
