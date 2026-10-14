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

extern "C" void vnu_swtch(uint32_t* old_esp_out, uint32_t new_esp);
extern "C" void vnu_swtch_pd(uint32_t* old_esp_out, uint32_t new_esp, uint32_t new_pgdir);
extern "C" void vnu_proc_preempt(uint32_t sched_esp, uint32_t new_pgdir);
extern "C" void vnu_proc_resume_preempted(uint32_t* old_esp_out, uint32_t new_esp,
                                          uint32_t new_pgdir);
extern "C" void vnu_timer_init();

namespace vnu::arch {

void switch_stack(uint32_t* old_esp_out, uint32_t new_esp)
{
    vnu_swtch(old_esp_out, new_esp);
}

void switch_stack_pd(uint32_t* old_esp_out, uint32_t new_esp, uint32_t new_pgdir)
{
    vnu_swtch_pd(old_esp_out, new_esp, new_pgdir);
}

void preempt(uint32_t sched_esp, uint32_t new_pgdir)
{
    vnu_proc_preempt(sched_esp, new_pgdir);
}

void resume_preempted(uint32_t* old_esp_out, uint32_t new_esp, uint32_t new_pgdir)
{
    vnu_proc_resume_preempted(old_esp_out, new_esp, new_pgdir);
}

void timer_init()
{
    vnu_timer_init();
}

} // namespace vnu::arch
