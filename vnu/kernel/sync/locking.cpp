#include <vnu/locking.h>
#include <vnu/process.h>

namespace vnu::sync {

void mutex_init(Mutex& m)
{
    m.holder = 0;
    m.guard.held = 0;
    m.chan = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&m)); /* unique per mutex */
}

void mutex_lock(Mutex& m)
{
    for (;;) {
        spin_lock(m.guard);
        if (m.holder == 0) {
            m.holder = vnu::proc::current_pid();
            spin_unlock(m.guard);
            return;
        }
        spin_unlock(m.guard);
        /* Contended: block until the unlocker's wakeup_one() lets us
         * try again. No lost wakeup — the whole check+park runs inside
         * a cli'd syscall handler and the unlock can only come from
         * another process's syscall. */
        vnu::proc::sleep_on(m.chan, 0);
    }
}

void mutex_unlock(Mutex& m)
{
    spin_lock(m.guard);
    m.holder = 0;
    spin_unlock(m.guard);
    vnu::proc::wakeup_one(m.chan);
}

} // namespace vnu::sync