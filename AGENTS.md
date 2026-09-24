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
```bash
./tools/build_userspace.sh   # compile userspace + regenerate embedded_*.h
./vnu/build_iso.sh           # cmake kernel build + grub-mkrescue -> vnu/vnu.iso
```
`./vnu/build_iso.sh` (и `./vnu/run.sh`) сами запускают сборку
userspace, если `vnu/kernel/proc/embedded_*.h` ещё не сгенерированы
(свежий клон), так что с нуля достаточно одной команды
`./vnu/build_iso.sh`.

## Rules
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
- Keep the style of the file you edit: comments in the language it
  already uses, same indentation, no new dependencies.
- **Build outputs stay untracked.** `vnu/kernel/build/`, `vnu/vnu.iso`,
  `vnu/iso/`, `vnu/grub-seed/`, `vnu/.tmp_grub/`, `sysroot/` and
  `vnu/kernel/proc/embedded_*.h` are build artifacts and must never be
  committed. Whenever a build step starts producing a new artifact,
  add it to the repo's `.gitignore` in the same change (a fresh checkout
  regenerates everything via `./tools/build_userspace.sh` followed by
  `./vnu/build_iso.sh`). Artifacts already committed must be removed from
  the index (`git rm -r --cached`), never from disk.
- Don't commit secrets.
