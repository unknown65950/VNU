# VNU Userspace ABI v1

Architecture: i386, little-endian, 32-bit pointers.

## System calls
`int $0x80` enters the kernel. `eax` contains the syscall number. Arguments are
`ebx`, `ecx`, `edx`, `esi`, `edi` in that order. Return value is in `eax`.
Failures are negative `-VNU_E*` values. Numbers are append-only and ABI v1 values
must never be renumbered.

### Stable (implemented / reserved by kernel)

| #  | Name   |
|----|--------|
| 0  | read   |
| 1  | write  |
| 2  | open   |
| 3  | close  |
| 4  | exit   |
| 5  | lseek  |
| 6  | stat   |
| 7  | fstat  |
| 8  | brk    |
| 9  | getpid |
| 10 | chdir  |
| 11 | getcwd |
| 12 | fork   |
| 13 | execve |
| 14 | getuid |
| 15 | getgid |
| 16 | mkdir  |
| 17 | rmdir  |
| 18 | unlink |
| 19 | getdents |
| 20 | waitpid |
| 21 | pipe   |
| 22 | dup    |
| 23 | dup2   |
| 24 | kill   |
| 25 | uname  |
| 26 | getppid |
| 27 | isatty |
| 28 | spawn  |
| 29 | blkcount |
| 30 | install |
| 31 | setuid |
| 32 | setgid |
| 33 | chmod  |
| 34 | chown  |
| 35 | reboot |
| 36 | time   |
| 37 | uptime |

`stat`/`fstat` report owner/group and permission bits: `st_uid`, `st_gid`,
and the low 9 bits of `st_mode` are the `rwx` bits. `chown(path, uid, gid)`
with `-1` leaves a field unchanged; only root may chown.

### Notes on argument shapes
- `blkcount(ebx)` — returns the number of ATA disks the installer sees.
- `install(ebx, ecx)` — `ebx` is the target ATA disk index; `ecx` is the
  partition size in MiB (0 = as much as the disk/FAT16 allows). Returns 0
  or a negative errno. Root only — the write is destructive.
- `reboot()` — triggers a system reset via the 8042 keyboard controller.
  Root only.
- `time()` — returns the RTC wall-clock time as whole seconds since local
  midnight (0..86399). BCD-aware; the kernel's only clock is the CMOS RTC.
- `uptime()` — returns whole seconds since boot, measured as a delta of
  `time()` against a boot-time snapshot (so it wraps every local midnight;
  one-second resolution, no timer interrupt). Backs the analog-clock GUI
  apps' stopwatch/timer elapsed-time readings.

### Removed

Not applicable — numbers are never reused; old gaps stay reserved.

### Future (append-only, start at 38)

Unsupported calls return `-VNU_ENOSYS`.

## Interrupt gate
Vector `0x80`, selector `0x08`, DPL=3, present 32-bit interrupt gate (`0xEE`).
The kernel must validate all userspace pointers before dereferencing them once
ring-3 address spaces are enabled.
