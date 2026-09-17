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
./build_iso.sh               # cmake kernel build + grub-mkrescue -> vnu/vnu.iso
```

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
- **No new syscalls without ABI documentation.** New syscall numbers are
  appended in `vnu/kernel/include/vnu/abi.h` and mirrored to
  `vlibc/include/vnu/abi.h`; `vnu/abi/ABI.md` must be updated in the
  same change.
- The kernel is freestanding C++20 (no exceptions/RTTI/STL) and may only
  include `<vnu/...>` headers. Userspace is C++17 with `vlibc` only.
- Keep the style of the file you edit: comments in the language it
  already uses, same indentation, no new dependencies.
- Don't commit secrets. Don't commit `vnu/kernel/build/` or ISO output
  unless the repo already tracks them.