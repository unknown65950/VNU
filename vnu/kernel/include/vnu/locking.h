#pragma once
#include <stdint.h>
#include <vnu/arch/cpu.h>

namespace vnu::sync {

/* Kernel spinlock. On this single CPU the real exclusion today comes
 * from every syscall handler running with IF cleared (so nothing can
 * preempt a critical section anyway); the primitives exist because they
 * are what keep the API honest the day an ISR has to touch the same
 * data as a syscall, and because the sleeping Mutex below builds on
 * them. Text-book test-and-set with a full barrier; the atomic and the
 * spin hint come from the architecture layer. */
struct Spinlock {
    uint32_t held; // 0 = free, 1 = held
};

inline void spin_lock(Spinlock& l)
{
    while (vnu::arch::atomic_exchange(&l.held, 1u) != 0)
        vnu::arch::relax();
}

inline void spin_unlock(Spinlock& l)
{
    vnu::arch::barrier();
    l.held = 0;
}

/* Sleeping mutex (single-owner, no recursion): a contended mutex_lock()
 * parks the caller on the mutex's wait channel (vnu::proc::sleep_on)
 * instead of spinning, so a holder that itself blocks mid-critical-
 * section — say it sleeps or waits on a pipe — cannot wedge the whole
 * machine; the unlocker's wakeup_one() resumes exactly one waiter.
 * All bookkeeping is wrapped in the guard for ISR-safety later; today
 * the cli'd syscall context already serializes it. */
struct Mutex {
    uint32_t chan;   // wait channel derived from this mutex's address
    uint32_t holder; // pid holding it; 0 = free
    Spinlock guard;  // protects chan/holder
};

void mutex_init(Mutex& m);
void mutex_lock(Mutex& m);
void mutex_unlock(Mutex& m);

} // namespace vnu::sync