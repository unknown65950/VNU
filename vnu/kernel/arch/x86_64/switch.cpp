#include <vnu/arch/switch.h>
#include <cstdint>

extern "C" void vnu_swtch(std::uintptr_t* old_esp, std::uintptr_t new_esp);
extern "C" void vnu_swtch_pd(std::uintptr_t* old_esp, std::uintptr_t new_esp, std::uintptr_t new_pgdir);
extern "C" void vnu_proc_preempt(std::uintptr_t sched_esp, std::uintptr_t new_pgdir);
extern "C" void vnu_proc_resume_preempted(std::uintptr_t* old_esp, std::uintptr_t new_esp, std::uintptr_t new_pgdir);
extern "C" void vnu_timer_init();

void vnu::arch::switch_stack(std::uintptr_t* old_esp_out, std::uintptr_t new_esp) {
    vnu_swtch(old_esp_out, new_esp);
}
void vnu::arch::switch_stack_pd(std::uintptr_t* old_esp_out, std::uintptr_t new_esp, std::uintptr_t new_pgdir) {
    vnu_swtch_pd(old_esp_out, new_esp, new_pgdir);
}
void vnu::arch::preempt(std::uintptr_t sched_esp, std::uintptr_t new_pgdir) {
    vnu_proc_preempt(sched_esp, new_pgdir);
}
void vnu::arch::resume_preempted(std::uintptr_t* old_esp_out, std::uintptr_t new_esp, std::uintptr_t new_pgdir) {
    vnu_proc_resume_preempted(old_esp_out, new_esp, new_pgdir);
}
void vnu::arch::timer_init() {
    vnu_timer_init();
}
