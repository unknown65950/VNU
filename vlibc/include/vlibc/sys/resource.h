#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* POSIX resource-usage interface, getrusage(2). The layout mirrors the
 * i386 struct rusage exactly, which is the wire format the kernel
 * writes under VNU_SYS_getrusage (see <vnu/abi.h>): two timevals
 * followed by fourteen longs, little-endian i386 longs being 32 bits. */
struct timeval {
    long tv_sec;  /* seconds */
    long tv_usec; /* microseconds */
};

struct rusage {
    struct timeval ru_utime; /* CPU time used (all of it: VNU does not
                              * split user and system time, so
                              * ru_stime is always 0) */
    struct timeval ru_stime; /* always 0 */
    long ru_maxrss;  /* 0: no resident-set accounting */
    long ru_ixrss;   /* 0 */
    long ru_idrss;   /* 0 */
    long ru_isrss;   /* 0 */
    long ru_minflt;  /* 0: the kernel allocates eagerly, so minor
                      * faults do not exist */
    long ru_majflt;  /* 0 */
    long ru_nswap;   /* 0 */
    long ru_inblock; /* 0: no block-I/O accounting */
    long ru_oublock; /* 0 */
    long ru_msgsnd;  /* 0: no SysV IPC */
    long ru_msgrcv;  /* 0 */
    long ru_nsignals;/* 0: not counted */
    long ru_nvcsw;   /* voluntary switches: cooperative yields */
    long ru_nivcsw;  /* involuntary switches: timer preemptions */
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

/* Fills `usage` with the resource usage of the calling process
 * (RUSAGE_SELF) or of its waited-for children (RUSAGE_CHILDREN,
 * accumulated at their exits). Returns 0, or a negative error number
 * for a bad pointer or an unknown `who`. */
int getrusage(int who, struct rusage* usage);

#ifdef __cplusplus
}
#endif