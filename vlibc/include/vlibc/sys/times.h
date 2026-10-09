#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* Process CPU times, POSIX times(2). */
typedef long clock_t;

/* The scheduler's tick rate: 100 Hz, the same clock /proc/stat counts. */
#define CLK_TCK 100

struct tms {
    clock_t tms_utime;  /* ticks this process ran (all CPU time: VNU
                         * does not split user and system time, so
                         * stime below is always 0 — see <vnu/abi.h>) */
    clock_t tms_stime;  /* always 0 */
    clock_t tms_cutime; /* utime of waited-for children */
    clock_t tms_cstime; /* always 0 */
};

/* Fills `buf` with the caller's CPU times, in CLK_TCK units, and
 * returns the number of clock ticks since boot. A failing call returns
 * a negative error number (the struct is untouched). */
clock_t times(struct tms* buf);

#ifdef __cplusplus
}
#endif