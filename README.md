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
tools/                          vcc, v++, vld, build_userspace.sh,
                                qemu_test.py (guest test harness)
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

Everything is driven from the top-level `Makefile` (`make help` lists all
targets):

```bash
make doctor        # check the host has everything needed
make iso           # userspace + kernel + vnu/vnu.iso
make run           # boot it in QEMU (window)
make run-headless  # boot without graphics, serial console in this terminal
make run-gpu       # window, but the desktop goes through virtio-gpu
make vhd           # create the test disk vnu/vnu.vhd
make test          # automated guest tests in QEMU
make clean         # drop build dirs and stray objects
```

The Makefile is a thin front-end: the build itself still lives in
`tools/build_userspace.sh` and `vnu/build_iso.sh`, so the plain script
sequence below keeps working (and is what the Makefile calls).

Dependencies (Debian/Ubuntu):

```bash
apt-get install -y build-essential cmake nasm gcc-multilib g++-multilib \
    qemu-system-x86 grub-pc-bin grub-common xorriso mtools
```

Build and run without make:

```bash
./tools/build_userspace.sh   # compile userspace + regenerate embedded_*.h
cd vnu && ./build_iso.sh      # cmake kernel build + grub-mkrescue -> vnu.iso
./run.sh                      # QEMU with a window
./run.sh --headless           # QEMU without graphics (output via serial)
```

Note: `make run virtio-gpu` would ask make for *two* goals (QEMU would
start twice), so variants have their own targets: `make run-gpu`,
`make run-headless-gpu`, or `make run RUN_ARGS=virtio-gpu`.

### Cross-building on Termux (ARM/Android)

There is no `grub-mkrescue`/multilib gcc on Termux, so `build_iso.sh`
switches itself to a portable path automatically (host detected via
`uname -m`):

- **Compiler**: the kernel and userspace compile with `clang --target
  i386-none-elf` instead of `gcc -m32` (vcc/v++/CMake do this on ARM).
- **Linker**: `ld.lld` is used directly (the Termux clang driver has no
  linker for a bare i386 triple). On LLD, the kernel linker script uses
  `ALIGN(4K)` instead of the GNU-only `BLOCK(4K)`.
- **ISO**: without `grub-mkrescue`, the ISO is rebuilt with `xorriso`
  reusing GRUB boot blobs (`vnu/grub-seed/`) extracted from an existing
  `vnu.iso` (always present after one build; regenerated on demand).
  `grub-mkimage` is not needed — the in-kernel `install` payload becomes
  empty stubs (kernel boots fine, disk-writer is empty).
- **Scripts**: executable scripts carry a `/bin/sh` trampoline that
  re-execs `bash` from `$PATH`, so `#!/usr/bin/env bash` (missing on
  Termux without `termux-exec`) is never needed.
- **Running**: QEMU on Termux has no windowing backend, so use
  `./run.sh --headless` (serial console in the terminal).

Dependencies:

```bash
pkg install -y clang cmake nasm binutils python xorriso qemu-system-i386
```

Then the same commands work:

```bash
./tools/build_userspace.sh
cd vnu && ./build_iso.sh
./run.sh --headless
```

On this machine the flow is tested end-to-end: boot to `VNU login:`,
`root`/`root`, then `uname`, `ls /bin`, `seq 3`, `man ls`, `echo`.

After boot: log in as `root`/`root` (guest `guest`/`guest`). Then `help`,
`man` (list: `man`), `gui`.

## Testing

`tools/qemu_test.py` boots the ISO headless (`-serial file:<log>`,
`-qmp unix:<sock>`), types into the guest through QMP
`human-monitor-command sendkey` — the keyboard driver reads PS/2, not COM1,
so a test has to press real keys — and asserts on what the session prints.
Kernel output and userspace fd1/fd2 are mirrored to COM1, which is the log
the assertions read.

```bash
make test                      # 41 cases: vnu, /proc, uname, coreutils, man
                               #   plus the interactive man pager session
make test-gpu                  # the same suite on a virtio-gpu display
make test-install              # install 0 to a fresh disk, boot that disk,
                               #   and check /proc/boot reports installed ata0
make test-all                  # ISO + virtio-gpu + install in one go
TEST_ARGS="--only vnu -v" make test      # filter cases, verbose
./tools/qemu_test.py --list               # case names
```

The harness is the place for a new check: add a `(name, command, expected,
forbidden)` entry to its `SUITE` table, where the patterns are regular
expressions matched against the output of that one command.

A check that has to *drive* the guest — a full-screen program like the
`man` pager, which owns the terminal until you press `q` — is a function
of its own instead, next to `run_man_pager()`: the keys are sent with
`guest._type("<down><down>q")` (`<name>` is any key in `SPECIAL_KEYS`) and
the session is checked once the prompt is back. `run_suite()` calls it
after the table, so `make test` covers both.

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