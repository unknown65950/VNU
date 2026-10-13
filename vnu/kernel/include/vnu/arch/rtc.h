#pragma once
#include <stdint.h>

// vnu/arch/rtc.h - the platform wall clock.
//
// This kernel has no timer interrupt, so the real-time clock is the only
// real clock: /proc/uptime and the time()/uptime() syscalls all read it,
// and /dev/random is seeded from it. A port backs it with whatever
// battery-backed or hypervisor clock it has.

namespace vnu::arch {

// Seconds since local midnight (0..86399). The caller derives uptime as
// a delta against a boot-time snapshot, wrapping at the day boundary.
uint32_t rtc_seconds_of_day();

} // namespace vnu::arch
