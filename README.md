# VNU — Vibe's Not UNIX!

A hobby operating system built from scratch for x86 (i386). GRUB loads a
**monolithic kernel** (freestanding C++20) via Multiboot2; the kernel sets up
paging, ring 3, and starts the userspace shell **vash** on top of its own VFS.
Instead of `libc` there is a custom **vlibc**; all of userspace is compiled
with the host toolchain `vcc`/`v++`/`vld` and embedded into the kernel as
`embedded_*.h` (prototype stage — no initrd yet).

```
VNU (Vibe's Not UNIX!) booted.
VNU login: root
Password:
root@vnu:~$ uname
VNU
root@vnu:~$ man ls
```

## Features

- **Monolithic kernel** in ring 0: VFS, processes, syscalls, TTY, keyboard,
  mouse, COM1, GUI — all in a single `kernel.elf`. No microkernel design;
  processes are isolated by address space (userspace at `0x400000`).
- **Userspace**: cooperative round-robin scheduler, PID 1 is `vash`. When a
  process exits (including after `execve` of a one-shot utility), the kernel
  respawns the shell.
- **Multi-user**: password login (`root`/`root`, `guest`/`guest`),
  `/etc/passwd` + `/etc/group`, `rwx` permissions with `uid`/`gid` on VFS
  nodes, builtins `id`, `whoami`, `groups`, `useradd`, `passwd`, `su`.
- **Man pages**: built-in page database inside the `man` binary (no
  `/usr/share/man`). `man` for listings and pages; mandatory for every
  command — see `AGENTS.md`.
- **VFS (RAM, FHS)**: `/bin`, `/sbin`, `/etc`, `/home`, `/root`, `/tmp`,
  `/usr`, `/var`, `/mnt`, `/dev` (char devices: `null`, `zero`, `full`,
  `random`/`urandom`, `tty`, `console`), `/proc` (synthetic: `version`,
  `cpuinfo`, `meminfo`, `mounts`, `uptime`, `self/status`).
- **GUI (VibeGraphics)**: the `gui` command launches a windowed environment
  with icons — terminal `term` (embedded vash), `vedit`, `calc`, `files`,
  `prefs`.
- **Disks**: ATA driver + FAT16, syscalls `blkcount`/`install` — the kernel
  can write itself (GRUB + partition) to a disk.
- **System calls**: `int 0x80`, append-only ABI v1 (0–34), see
  `vnu/abi/ABI.md`. New numbers start at 35 and only with documentation.

## Commands

vash builtins: `cd`, `export`, `unset`, `exit`, `help`, `type`, `which`
(history, Ctrl+C, Shift-symbols).

External `/bin/*` (each a standalone binary):

| Tool | Purpose |
|------|---------|
| `echo` `true` `false` `pwd` `cat` `ls` `mkdir` `rm` `touch` `uname` `clear` | basic utilities |
| `wc` `head` `tail` `grep` `sort` | text processing |
| `cp` `mv` `basename` `dirname` | file operations |
| `seq` `man` `vedit` `ttytest` | number generation, docs, editor, terminal test |
| `id` `whoami` `groups` `useradd` `passwd` `su` | accounts/identity tools (`su` starts a fresh shell as the user) |
| `install` | write the VNU image to an ATA disk (root) |
| `hello` (and `hello_cxx`) | userspace program examples |
| `gui` | virtual command → VibeGraphics windowed environment (intercepted by the kernel in `execve`) |

`/bin/sh`, `/bin/init` — aliases of `vash` (reused `argv[0]`).

## Repository layout

```
AGENTS.md                       development rules (read this first)
tools/                          vcc, v++, vld, build_userspace.sh
vlibc/                          freestanding C library and its headers
vnu/
├── kernel/                     the kernel (freestanding C++20, build/ via CMake)
│   ├── arch/i386/              boot (Multiboot2), GDT/IDT, syscalls
│   ├── console/ fs/ drv/ mm/ proc/ gui/ install/
│   └── proc/embedded_*.h       userspace ELFs embedded into kernel.elf
├── userspace/                  vash, vibecoreutils (one .c per command),
│                               usertools (accounts/install), man, vedit,
│                               GUI apps, examples, rootfs
├── abi/ABI.md                  ABI v1 description
├── build_iso.sh                cmake + grub-mkrescue -> vnu/vnu.iso
└── run.sh                      launch in QEMU (--headless via serial)
sysroot/                        built userspace ELFs (built there)
```

## Build and run

Dependencies (Debian/Ubuntu):

```bash
apt-get install -y build-essential cmake nasm gcc-multilib g++-multilib \
    qemu-system-x86 grub-pc-bin grub-common xorriso mtools
```

Build and run:

```bash
./tools/build_userspace.sh   # compile userspace + regenerate embedded_*.h
cd vnu && ./build_iso.sh      # cmake kernel build + grub-mkrescue -> vnu.iso
./run.sh                      # QEMU with a window
./run.sh --headless           # QEMU without graphics (output via serial)
```

After boot: log in as `root`/`root` (guest `guest`/`guest`). Then `help`,
`man` (list: `man`), `gui`.

## Testing

Automated tests drive QEMU headless: `qemu-system-i386 -cdrom vnu.iso -m 32
-display none -serial file:<log> -monitor none -qmp unix:<sock>,server,nowait`
and send keystrokes via QMP `human-monitor-command sendkey`. Example
harnesses live in `/tmp/opencode/` (multi-user 24/24, `man` 17/17, split
binaries 36/36). Kernel output (and userspace fd1/fd2) is mirrored to COM1 —
that is the serial log the assertions read.

## Development rules

- Every new userspace command is a **standalone binary** with its own location
  under `vnu/userspace/`, its own `embedded_<name>.h`, a `/bin/<name>` VFS
  node and a `process.cpp` table entry; no applets in a multi-call binary.
- A feature is not done until the command has a page in `man.c` (and has been
  verified in QEMU).
- New syscalls only as an append to both `abi.h` copies + `vnu/abi/ABI.md`.

The full ruleset is in `AGENTS.md`. GUI/window history and known
limitations are in `vnu/VIBEGRAPHICS_CHANGES.md`.

## License

MIT — see `LICENSE`.

## Roadmap

`initialD` (a userspace init, contract `/etc/initialD/*.init`) is declared but
deferred until proper `fork`/`execve` scenarios; for now PID 1 is vash itself.
FAT16/ATA exist, but at boot the system lives in RAM-VFS (useful when moving
to real hardware).