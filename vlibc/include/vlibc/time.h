#pragma once
#include <vlibc/sys/syscall.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef long time_t;

/* Wall clock: whole seconds since local midnight (0..86399). Reads the
 * CMOS RTC through the `time` syscall; resolution is one second. If
 * `out` is non-NULL the value is also stored there. */
time_t time(time_t* out);

/* Whole seconds since boot (measured as an RTC delta). Monotonic within
 * a day, so it is the elapsed-time base for the GUI stopwatch and timer:
 * take a sample at start, another at each tick, and subtract. */
unsigned long uptime(void);

#ifdef __cplusplus
}
#endif