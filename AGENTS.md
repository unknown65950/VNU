# AGENTS.md — VNU development conventions

This file is the first thing any developer or coding agent should read
before touching the repo. It summarizes the rules that keep the OS
consistent.

## Layout
- `vnu/kernel/` — the kernel (freestanding C++20, own build via CMake).
- `vnu/userspace/` — userspace programs compiled with `tools/vcc`
  (vash, vibecoreutils, vedit, gui apps, man, examples).
- `vlibc/` — freestanding C library (headers under `vlibc/include/vlibc/`).
- `tools/` — host toolchain (`vcc`, `v++`, `vld`) and `build_userspace.sh`.
- `sysroot/` — compiled userspace ELFs, embedded byte-for-byte into the
  kernel via generated `vnu/kernel/proc/embedded_*.h`.

## Build
The front-end is the top-level `Makefile` (`make help` lists every
target):
```bash
make doctor        # check the host has the tools it needs
make iso           # userspace + kernel -> vnu/vnu.iso
make run           # QEMU with a window; make run-headless / run-gpu vary it
make run-vhd       # boot the ISO with vnu/vnu.vhd attached, to install onto it
make run-installed # boot that disk alone, no ISO (what `vnu install` produced)
make vhd           # create the test disk vnu/vnu.vhd
make test          # automated guest tests in QEMU (tools/qemu_test.py)
make clean         # drop build dirs; make distclean drops everything
```
The Makefile is a thin front-end: the build logic still lives in the
scripts, so they remain a working path of their own:
```bash
./tools/build_userspace.sh   # compile userspace + regenerate embedded_*.h
./vnu/build_iso.sh           # cmake kernel build + grub-mkrescue -> vnu/vnu.iso
```
`./vnu/build_iso.sh` (like `./vnu/run.sh` and `make iso`) starts the
userspace build itself when `vnu/kernel/proc/embedded_*.h` are not
generated yet (a fresh clone), so a single `make iso` is enough from
scratch.

## Tests
`tools/qemu_test.py` boots the image in QEMU without graphics, types at
the guest through QMP `sendkey` (the keyboard driver reads PS/2, not
COM1) and asserts on what the guest prints. Add a new check as a
`(name, command, expected, forbidden)` entry in that file's `SUITE`
table; the targets are `make test`, `make test-gpu`, `make test-install`
and `make test-all`. A check for a full-screen program (one that owns the
terminal until a key is pressed) is a function next to `run_man_pager()`
instead, sending the keys with `guest._type("<down>q")`.

## Rules
- **Everything is written in English.** Source comments, commit messages,
  documentation, man pages, script output, Makefile targets and the
  text a program prints to the user — all of it in English, without
  exception. This file and the README set the style: read them before
  writing prose. A file that still carries text in another language is a
  bug to be fixed in the same change that touches it; when you edit a
  file, translate what is left of it. Identifiers, paths and
  user-supplied data keep whatever form they have.
- **Mandatory man pages.** Every user-visible command — vash builtins,
  each coreutils command (they are separate binaries under
  `vnu/userspace/vibecoreutils/`, one `.c` per command, not applets of a
  multi-call binary), and standalone binaries — MUST ship a manual
  page before the feature is considered done. Pages live in the
  `pages[]` table in `vnu/userspace/man/man.c` (the `man` binary is
  self-contained; there is no `/usr/share/man` filesystem). Add an entry
  there, run `./tools/build_userspace.sh`, and verify `man <cmd>` in
  QEMU. Reject any new command that lacks a page.
- **One command = one binary = its own userspace location.** New commands
  MUST be standalone binaries with a dedicated source location under
  `vnu/userspace/` (its own `.c`/directory, e.g.
  `vnu/userspace/vibecoreutils/<name>.c` for coreutils-style tools),
  registered in `tools/build_userspace.sh`, embedded as their own
  `embedded_<name>.h`, and added as their own `/bin/<name>` VFS node +
  `process.cpp` table entry. Do not add applets to a multi-call binary and
  do not share a binary slot across unrelated commands. (Reused
  `argv[0]` aliases, e.g. vash as `sh`/`init`/`term`, are the only
  exception.) Shared code goes in a header (`cu.h`) as `static inline`.
- **No library? Write a POSIX one.** If an application needs a capability
  for which VNU has no library yet (image decoding, arithmetic helpers,
  a parser, ...), implement the missing piece as a self-contained POSIX
  library that any other Unix program — this OS or a host system — could
  drop in and use unchanged: a freestanding header + implementation with
  no kernel-only or VNU-only dependencies, entry points that take plain
  buffers/pointers (no syscalls baked in), and a POSIX-friendly API. The
  `px.h` image decoder in `vnu/userspace/gui/` is the reference example:
  byte-for-byte pixel-exact vs. host tools and usable from both sides of
  the tree. Do not bury the capability inside a single app; put it in a
  shared library location so every Unix program can reuse it.
- **No new syscalls without ABI documentation.** New syscall numbers are
  appended in `vnu/kernel/include/vnu/abi.h` and mirrored to
  `vlibc/include/vnu/abi.h`; `vnu/abi/ABI.md` must be updated in the
  same change.
- **Host vcc == guest vcc.** The `vcc` binary (`vnu/userspace/vcc/vcc.c`)
  compiled with `tools/vcc` on the build host is functionally equivalent
  to the guest `vcc` embedded into the kernel and run inside VNU: the
  same source, the same feature set, the same output. Do not let the two
  diverge — a change to the compiler is verified host-side first
  (byte-identical `-c` object and full-link vs. the reference outputs)
  and must keep working in-guest via `./vnu/run.sh`; if a feature or bug
  fix is only possible in one environment, it is incomplete.
- **New devices must surface through `/dev`.** When support for a new
  device is added (sound, network, disk, HID, ...), the attached device
  MUST be accessible from userspace as a node in `/dev` — a character or
  block device registered via `add_dev()` in `vnu/kernel/fs/vfs.cpp`
  (like the existing `null`, `zero`, `tty`, `random` nodes), with reads/
  writes going through the node. A driver whose device cannot be opened,
  read and written by a userspace program is not done; if the device
  deals in streams of blocks/samples/packets, expose those as
  byte streams on the node. `/proc` or `ioctl`-style side channels alone
  do not count as device access.
- The kernel is freestanding C++20 (no exceptions/RTTI/STL) and may only
  include `<vnu/...>` headers. Userspace is C++17 with `vlibc` only.
- Keep the style of the file you edit — same indentation, same comment
  style, no new dependencies — except for the language, which is English
  everywhere.
- **Build outputs stay untracked.** `vnu/kernel/build/`, `vnu/vnu.iso`,
  `vnu/iso/`, `vnu/grub-seed/`, `vnu/.tmp_grub/`, `sysroot/` and
  `vnu/kernel/proc/embedded_*.h` are build artifacts and must never be
  committed. Whenever a build step starts producing a new artifact,
  add it to the repo's `.gitignore` in the same change (a fresh checkout
  regenerates everything via `./tools/build_userspace.sh` followed by
  `./vnu/build_iso.sh`). Artifacts already committed must be removed from
  the index (`git rm -r --cached`), never from disk.
- Don't commit secrets.
- **Commit after every change.** Each finished piece of work — a feature,
  a fix, a refactor — is committed as soon as it builds, passes its
  verification and the debug scaffolding has been removed. Do not let
  uncommitted work pile up across sessions. Follow the repo's commit
  style: one-line summary prefixed by the area (`gui:`, `net:`, ...),
  details in the body when useful.
