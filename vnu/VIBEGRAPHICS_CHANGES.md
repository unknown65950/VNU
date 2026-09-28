# VibeGraphics — changes in this drop

Added a Windows-3.1-style window manager, `VibeGraphics` (name isn't
shown anywhere in-UI), reachable by typing `gui` at the `vash` prompt.
`Esc` leaves it and returns to the shell.

## New files
- `kernel/include/vnu/vga_gfx.h`, `kernel/drivers/vga_gfx.cpp` —
  320x200x256 (VGA "mode 13h") driver. No BIOS calls (none are
  available once GRUB hands off), so the mode switch/restore is done
  by programming the CRTC/Sequencer/Graphics/Attribute registers
  directly, saving the console's own register state first so text
  mode can be restored exactly. The 8x16 font isn't hand-authored —
  it's captured out of VGA plane 2 while still in text mode, so GUI
  text matches the console font.
- `kernel/include/vnu/ps2mouse.h`, `kernel/drivers/ps2mouse.cpp` —
  polling PS/2 mouse driver (no IRQ12 handler; packets are drained by
  polling the 8042 status register, same style as the existing
  keyboard driver).
- `kernel/include/vnu/gui.h`, `kernel/gui/gui.cpp` — the window
  manager itself: desktop background, one draggable demo window
  (`VibeGraphics` title bar, close-box glyph), an icon grid for
  installed apps, and the main input/draw loop.
- `kernel/include/vnu/apps.h`, `kernel/gui/apps.cpp` — the `/apps/`
  launcher. Seeds `/apps/hello`, `/apps/vedit`, `/apps/term` at boot,
  each a real VFS directory containing:
  - `icon` — 1 byte (a VGA palette index used to tint the icon)
  - `bin`  — the actual ELF binary
  Clicking an icon reads `bin` out of the VFS and hands it to the
  loader exactly like a normal `execve()`.

## Changed files
- `kernel/drivers/kbd.cpp`, `kernel/include/vnu/kbd.h` — added a
  non-blocking `scancode_ready()`/`read_raw_scancode()` pair for the
  GUI's event loop. **Bug fixed along the way:** the first version of
  `scancode_ready()` only checked "output buffer full" and not "which
  device the byte came from", so it was silently stealing mouse packet
  bytes before the mouse driver ever saw them. Fixed to check both.
- `kernel/fs/vfs.cpp` — bumped `DATA_CAP` from 2048 to 20480 bytes and
  `MAX_N` from 48 to 64, so `/apps/<name>/bin` can hold a real embedded
  ELF binary (the biggest one shipped, `vedit`, is ~18 KiB).
- `kernel/proc/process.h`, `kernel/proc/process.cpp` — factored the
  "load an ELF and jump into it" logic out of `enter_program()` into a
  new `run_program_from_memory()`, used both by the existing embedded-
  binary path and by the new `/apps/` launcher (which reads bytes out
  of the VFS instead of the compiled-in table).
  **Second bug fixed along the way:** `vnu_enter_user()` /
  `vnu_return_to_console()` (see `proc/switch.s`) implement a *single*
  saved continuation, not a stack of them — calling `vnu_enter_user()`
  again while already inside one (which is exactly what happens when
  the GUI launches an app from inside a `vash` session that itself got
  to the GUI through the same mechanism) clobbered the outer
  continuation. In testing this reliably crashed the kernel the moment
  you left the GUI after having launched an app from it. Fixed by
  saving/restoring the trampoline's global state around each nested
  `run_program_from_memory()` call.
- `kernel/arch/i386/syscall/syscall.cpp` — `gui` isn't a real ELF on
  disk, so `execve("/bin/gui", ...)` is special-cased to call
  `vnu::gui::run()` directly instead of going through the normal
  loader; on return it ends the `vash` session the same way a normal
  external command finishing does, so the shell restarts cleanly.
- `kernel/kernel/kernel.cpp` — calls `vnu::apps::install_demo_apps()`
  once at boot, after `vfs::init()`.
- `kernel/CMakeLists.txt` — added the new source files.

## Verified in QEMU (`qemu-system-i386`, headless + monitor screendump)
- Mode switch + font capture + window/text rendering, first try.
- Mouse motion and click-drag of the demo window's title bar.
- Clicking an app icon leaves graphics mode, loads and runs the real
  binary out of `/apps/<name>/bin`, prints its actual output.
- Repeated launches from the same GUI session (checked 4 in a row) —
  no process-table leak, no crash.
- Full round trip: `gui` → launch app → back to desktop → `Esc` → shell
  still responsive to further commands (this is what the trampoline
  bug above broke before the fix).

## Known limitations / natural next steps
- **No real window-in-window multitasking.** This kernel has no
  paging and no preemptive scheduler yet — launching an app from the
  desktop leaves graphics mode and runs it full-screen, exactly like
  typing its name at the `vash` prompt. When it exits you're back on
  the desktop (if launched from there) or at the shell (if you `Esc`
  the GUI directly). A "real" app running *inside* a window needs a
  scheduler and per-process framebuffers, which don't exist yet.
- Icons are a generic colored square + first-letter glyph, not real
  bitmaps — there's no image decoder in the kernel. The `icon` file
  format (1 byte, a palette index) is deliberately simple; swapping in
  actual pixel art would mean picking a raw bitmap format and a
  slightly bigger `icon` file.
- Only 3 demo apps are seeded, all repackaged from binaries the kernel
  already had embedded. There's no way yet to add your own app short
  of extending `install_demo_apps()` — a real toolchain for building
  and dropping binaries into `/apps/` would be the natural next step.
- Single-click launches apps (no double-click gating) since there's no
  timer/clock driver wired up yet to time clicks reliably.
## Second drop: `ls` + `cd ..`

### The bugs
1. **`cd ..` didn't work at all**, anywhere, ever. `vnu::vfs::normalize()`
   only handled an absolute path or `cwd + "/" + path` concatenation —
   it never resolved `.` or `..` components, so `cd ..` literally
   looked up a node named `/etc/..`, which of course doesn't exist.
2. **`ls` worked exactly once per boot, then always failed** with
   `ls: cannot open <path>`, for *any* path, including ones that had
   just listed fine moments before (confirmed via repeated
   `ls /etc` — 1st call fine, every call after fails). Plain file
   reads (`cat`, 70 times in a loop) never showed this, which narrowed
   it to directory handling specifically. `ls` is built against
   `<vlibc/dirent.h>` — a library whose source isn't included in this
   checkout (only the compiled binaries that link it are) — and its
   `opendir()`/`readdir()`/`closedir()` evidently leak a directory
   handle. Since that source isn't available to inspect or rebuild,
   it can't be patched directly here.

### The fixes
- **`kernel/fs/vfs.cpp`, `normalize()`** — rewritten to actually walk
  the path segment by segment, popping the stack on `..`, skipping
  `.`, and clamping at root instead of underflowing. Purely a kernel
  change; no userspace rebuild involved. Verified: `cd ..`, multi-level
  `cd ../..`, `cd` around `/apps/<name>/`, and `cd ..` from `/` (stays
  at `/`, doesn't go negative).
- **New `/bin/ls`** — `userspace/coreutils/ls_raw.c` (+ `ls_raw_start.S`
  crt0, `ls_raw.ld` linker script) is a from-scratch `ls` that talks to
  the kernel purely through raw `int 0x80` syscalls (`open`,
  `getdents`, `close`, `getcwd`, `write`, `exit` — see
  `kernel/include/vnu/abi.h` for numbers and
  `kernel/arch/i386/syscall/syscall.s` for the calling convention), so
  it never touches the leaking library at all. Built with the system's
  `gcc -m32`/`ld` (no `vcc`/`vld` needed, since this bypasses vlibc
  entirely) and embedded as `kernel/proc/embedded_ls.h`, wired in to
  replace only the `/bin/ls` table entry — `/bin/coreutils`, `/bin/cat`,
  `/bin/pwd`, etc. are untouched (they don't use the directory-stream
  code, and testing confirmed they don't leak). See
  `userspace/coreutils/README_ls_raw.md` for the rebuild recipe if you
  ever want to change it.

### Verified in QEMU
- `ls /bin` repeated back-to-back — works every time now (previously
  failed on the 2nd call).
- `cd etc` → `pwd` → `ls` → `cd ..` → `pwd` → `ls` — full round trip,
  correct contents and correct cwd at each step.
- `cd bin` → `cd ..` → `cd apps` → `ls` → `cd hello` → `ls` → `cd ../..`
  → `pwd` — multi-level nesting through the `/apps/` tree added
  earlier, ends back at `/`.
- `cd /` → `cd ..` → `pwd` — clamps at root instead of misbehaving.
- The VibeGraphics GUI + `/apps/` launcher round trip (from the first
  drop) re-tested after these changes — still clean.

## Third drop: the real root cause of the `ls` bug (thanks to the `vlibc` + `tools/` sources)

The second drop's diagnosis was wrong in one detail — it correctly
identified "malloc must be failing on the 2nd+ directory open" territory
but, without `vlibc`'s source, guessed it was a leaked directory
*handle*. With the real source now available, the actual bug turned
out to be simpler and further down the stack:

### The real bug
`kernel/arch/i386/syscall/syscall.cpp`'s `VNU_SYS_brk` handler kept a
**single, kernel-wide `static` heap-break pointer** for a **fixed
0x00700000–0x00800000 (1 MiB) region**, and never reset it between
process launches:

```cpp
case VNU_SYS_brk: {
    static std::uint32_t brk = 0x00700000;   // <- never reset!
    constexpr std::uint32_t BRK_MAX = 0x00800000;
    ...
}
```

`vlibc`'s `malloc()` (`vlibc/src/stdlib/malloc.c`) grows the heap by
requesting `brk() + 1 MiB` the first time it's ever called in a
process. The **first** program that ever calls `malloc()` — which for
a typical `vash` session is whichever runs `opendir()` first, since
that's the one vlibc function that heap-allocates (a `DIR*`) — gets
handed the *entire* 1 MiB region in one shot, pushing the shared `brk`
straight to `BRK_MAX`. Every process after that starts with `brk()`
already sitting at `BRK_MAX`, so its own first growth request
(`BRK_MAX + 1 MiB`) is rejected (`> BRK_MAX`), `malloc()` returns
`NULL`, and `opendir()` returns `NULL` — hence `ls: cannot open`,
forever, from the second `ls` onward, regardless of path. `cat`/`pwd`/
`echo` never called `malloc()` at all (their buffers are plain stack
arrays), which is exactly why only directory listing was affected.

### The fix
- `kernel/include/vnu/process.h` / `kernel/proc/process.cpp` — the
  `brk` state moved out of that local `static` into
  `vnu::proc::brk_current()` / `brk_set()` / `brk_reset()`, and
  `run_program_from_memory()` (the single choke point every process
  launch already goes through — both the classic embedded-binary path
  and the `/apps/` launcher) now calls `brk_reset()` before jumping in.
  So every fresh process starts with its own empty 1 MiB heap, exactly
  the same way its `.bss` starts freshly zeroed.
- `kernel/arch/i386/syscall/syscall.cpp` — `VNU_SYS_brk` now just calls
  through to those.
- **The second drop's raw-syscall `/bin/ls` workaround was removed.**
  It's no longer needed — `vlibc`'s real `opendir()`/`readdir()`/
  `closedir()` (`vlibc/src/dirent/dirent.c`) were fine all along, so
  `/bin/ls` is back to being built from
  `userspace/vibecoreutils/coreutils.c` like every other coreutils
  command.
- **All userspace binaries were rebuilt from source with the real
  toolchain** (`tools/vcc` + `tools/vld` + `vlibc/`, now included in
  this drop) instead of relying on the previously-shipped compiled
  blobs: `vash`, `coreutils` (`ls`/`cat`/`pwd`/.../`mkdir`/`rm`/etc.),
  `vedit`, `hello`. Byte-for-byte the same sizes as what shipped
  before, confirming these sources are in fact what produced the
  original `kernel/proc/embedded_*.h` files.

### Verified in QEMU
- `ls /bin` **three times in a row** (previously failed on the 2nd) —
  clean every time.
- Full `cd`/`pwd`/`ls` walk: root → `/etc` → back to `/` → `/apps` →
  `/apps/hello` → back to `/` (via `cd ../..`) → `ls` **three more
  times** at the end — all correct, no degradation.
- GUI round trip with **three** app launches in the same session this
  time (previously tested with `hello` under the old workaround) —
  desktop icons, launch, real program output, back to desktop, `Esc`,
  shell still responsive to `echo` and `ls` afterward.

## Fourth drop: real cooperative multitasking — apps run *inside a window*

Launching `hello`/`vedit` from the desktop used to leave graphics mode
entirely (full-screen text, then back to the desktop on exit) — the
"graphics breaks" symptom. This drop makes that actually windowed:
the app now runs concurrently with the GUI's own event loop, in a
window on the desktop, without ever leaving mode 13h.

### Why not "real" preemptive multitasking
This kernel has no paging and no timer-driven preemption. Two hard
constraints shaped the design:
- **Only one app can be resident at a time.** App ELF binaries are
  non-PIE, compiled to load at a fixed address (`0x400000` — see
  `kernel/proc/elf.cpp`). Without paging to give each process its own
  virtual `0x400000`, two different binaries can't both be loaded
  simultaneously — they'd overwrite each other's code. The GUI itself
  is compiled into the kernel at a completely different address, so it
  never collides with the one app that *is* loaded; that's what makes
  one concurrent window safe.
- **Switching is cooperative, not preemptive**, and only happens at
  two points: a blocking stdin read, or `exit()`. A tiny hand-rolled
  coroutine switch (`kernel/proc/ctxswitch.s`, the same pattern
  teaching kernels like xv6 use) saves/restores just the four
  callee-saved registers + stack pointer — safe *only* because the
  switch always happens at a deliberate call site, never an
  asynchronous interrupt.
- **Only apps that never call `execve()` are windowable.** `term`
  (`vash`) execs a fresh program for every command typed at its
  prompt — doing that from inside this lightweight scheduler would
  jump into the unrelated classic process path mid-slice. So `hello`
  and `vedit` run as real windows; `term` keeps the old full-screen
  hand-off.

### What's new
- `kernel/proc/ctxswitch.s` — `vnu_swtch()` (the coroutine switch) and
  a small trampoline used only the first time a brand-new task runs.
- `kernel/include/vnu/wintask.h` / `kernel/gui/wintask.cpp` — the
  scheduler: `spawn()` loads an ELF and sets up its initial context;
  `run_slice()` gives it one burst of execution; a 30x7 character-grid
  `Console` stands in for its stdout; `feed_input()`/
  `task_getch_blocking()` carry keystrokes in, with the read blocking
  cooperatively (switching back to the GUI, not spinning) whenever the
  queue is empty.
- `kernel/arch/i386/syscall/syscall.cpp` — `write()`/`read()` for fd
  0/1/2 and `exit()` now check `vnu::wintask::current_is_task()` first
  and redirect to the windowed console instead of real VGA text/
  keyboard hardware when it's set.
- `kernel/drivers/kbd.cpp` / `kernel/include/vnu/kbd.h` — the scancode
  → ASCII decoder (shift/ctrl/extended-key state machine) was factored
  out of `getch_blocking()`'s loop into a shared `decode_scancode()`,
  so a new non-blocking `poll_char()` can reuse the exact same mapping
  for the GUI's per-frame keyboard polling.
- `kernel/gui/gui.cpp` — the desktop now renders the running app's
  window (or its last output, until another app is launched) as a
  real character grid, forwards non-Esc keystrokes to it when
  something's running, and gives it a time slice once per frame.
  `Esc` still closes the whole desktop, but only when nothing is
  running, so an app that wants Esc for its own UI (`vedit`'s command
  line) still gets it.
- `kernel/gui/apps.cpp` — added `read_bin()` (reads an app's `bin`
  file into a caller-supplied buffer without launching it), used to
  hand the bytes to `wintask::spawn()` instead of the classic one-way
  process handoff.

### Verified in QEMU
- `hello` (non-interactive, exits immediately): its window shows the
  real program output; the desktop never left graphics mode.
- `vedit` (interactive): opens its own UI in the window, genuinely
  blocks waiting for keystrokes — confirmed the mouse cursor kept
  moving independently while it was blocked — accepts typed input
  (watched its line/modified indicator update live), and a clean
  `Esc` → `q` → Enter quit frees the slot.
- Launching `hello` again immediately after `vedit` quits — starts
  fresh, no stale state.
- Three `hello` launches in a row, then `vedit`, quit, then `hello`
  again, then `Esc` out of the GUI entirely, then `ls`/`echo` at the
  `vash` prompt — shell fully responsive throughout.
- `term` still opens a real full-screen `vash` session exactly as
  before (unaffected by the new windowed path, since it isn't
  windowable).

### Known limitations
- Exactly one windowed app at a time, for the memory-layout reason
  above. Clicking a different app's icon while one is already running
  is currently just ignored.
- `term` (or any app that calls `execve()`/`fork()`) can't be
  windowed with this scheme — real per-process address spaces (i.e.
  paging) would be needed for that.
- The console is a fixed 30x7 character grid — no resizing, no color,
  no cursor blink (a static underline marks the input position
  instead).
- No true preemption: a windowed app that runs a long time between
  reads (a tight compute loop, say) would freeze the GUI for that
  whole burst, since `run_slice()` doesn't return until it yields.
  Not an issue for `hello`/`vedit`, which are either instantly
  finished or spend virtually all their time blocked on input. (The
  fifth drop below adds real address-space isolation but doesn't
  change this — it's still cooperative, not preemptive.)

## Fifth drop: real kernel-level multitasking (paging)

The fourth drop's cooperative "wintask" scheduler was explicitly a
workaround for a hard constraint: no paging meant no way for two
different ELF binaries to be resident at once, since they're all
compiled to load at the same fixed address (`0x400000`). This drop
removes that constraint at the root — every process now gets its own
page directory, so `0x400000` (and its stack, and its heap) means
physically different memory for each one. `term` (`vash`), previously
excluded from windowing because it calls `execve()`, now runs on the
exact same real-isolation path as `hello`/`vedit` and everything else;
the wintask module from the fourth drop is unaffected and still used
for actual in-window rendering, but the *hard* one-process-at-a-time
ceiling it documented is gone.

### What's new
- `kernel/include/vnu/pmm.h` / `kernel/mm/pmm.cpp` — a bitmap physical
  frame allocator over a fixed 10 MiB pool (`0x01400000`-`0x01E00000`),
  comfortably clear of everything else this kernel's fixed memory
  layout already uses, given QEMU is launched with `-m 32` (see
  `run.sh`).
- `kernel/include/vnu/paging.h` / `kernel/mm/paging.cpp` — builds a
  shared, identity-mapped page directory covering the low 32 MiB
  (kernel, VFS, the frame pool itself, etc.) and enables paging
  (`kernel/kernel/kernel.cpp` no longer calls `vnu_disable_paging()`
  to force it off). `create_address_space()` clones that shared
  mapping but gives specific virtual ranges (app code, stack, heap)
  their own private page table backed by freshly allocated frames —
  everything else stays shared.
- `kernel/proc/process.cpp`'s `run_program_from_memory()` — the single
  choke point every process launch already went through — now builds
  a private address space (app code at `0x400000`: 8 pages; that
  process's stack slot: 16 pages; its heap at `vnu::proc::BRK_MIN`:
  256 pages / 1 MiB) before loading the ELF, and tears it down when
  the process exits.

### Three bugs found getting this actually working
Paging is unforgiving — each of these turned "silently works" into
"triple fault" the moment it was wrong, which made them fast to
notice but not always fast to place:

1. **The heap shares a page-directory entry with the app code and
   stack.** `0x400000`-`0x7FFFFF` is a single 4 MiB PDE. Privatizing
   that PDE for isolation but only explicitly backing the app-code and
   stack ranges silently *unmapped* the heap (anything in a privatized
   PDE's span that isn't listed goes from "shared" to "not present").
   Every command that never allocates — `cat`, `echo`, `mkdir` — kept
   working; `ls`, the one thing that calls `opendir()` (which
   `malloc()`s a `DIR`), page-faulted on its very first heap access.
   Fixed by listing the heap as a third private range.
2. **Switching CR3 while still running on the *old* stack.** A nested
   launch (e.g. the GUI launching `term` from inside a `vash` session
   that's itself a process with its own private stack) executes
   `run_program_from_memory()` on that caller's own stack. The instant
   CR3 switches to the new process's address space, that stack
   — living in the very region that just became someone else's private
   mapping — stops being valid, and the next instruction touching it
   (even just a function's own `pop`/`ret` epilogue) page-faults. Fixed
   by moving onto a small scratch stack in shared kernel memory
   (`g_setup_stack`, always identically mapped regardless of which CR3
   is loaded) *before* switching CR3, using the same cooperative-switch
   primitive from the fourth drop (`vnu_swtch`, in
   `kernel/proc/ctxswitch.s`) to get on and back off it safely.
3. **`argv0_path` pointing into the caller's own stack.** The `/apps/`
   launcher (`kernel/gui/apps.cpp`) builds the program's path in a
   local `char[]`. That pointer is passed all the way through to
   `setup_and_enter()`, which only reads it *after* switching to the
   new process's address space — by which point the caller's stack
   (where that string actually lives) isn't mapped there either. This
   one didn't fault where you'd expect, on some obviously-new-process
   write; it faulted deep inside `str_len()`, dereferencing a
   perfectly normal-looking `const char*` that had quietly stopped
   being valid. Fixed by copying the string into a small static kernel
   buffer before the CR3 switch.

(A fourth thing looked like a bug and wasn't: a stray hardware
interrupt landing mid-transition seemed like a plausible culprit for a
while, since the crash was intermittent — but disabling interrupts
around the transition didn't fix it, which is what pointed at #2 and
#3 instead being genuine, deterministic ordering bugs whose exact
faulting instruction just happened to depend on timing-sensitive
memory contents.)

### Verified in QEMU
- Full shell regression (`ls` repeated, `cd`/`pwd` walk through
  `/apps/`, `echo`) — unaffected, still clean.
- GUI: `hello` and `vedit` in their windows (fourth drop's wintask
  path) — unaffected, still clean.
- **`term` launched from the GUI — no longer crashes.** This was the
  original bug report ("the graphics break when the demo
  applications start"): `term` runs the classic full-screen hand-off with a
  real private address space now, comes back cleanly, and the shell
  keeps working afterward (`echo term_ok` succeeds post-launch).
- Multiple `term` launches in sequence, interleaved with shell
  commands and other GUI apps — no crashes, no leaked frames observed
  across repeated cycles.

### Known limitations
- `fork()` still doesn't give the child its own address space (nothing
  in this codebase's userspace actually calls `fork()`, so this is
  unexercised, documented territory rather than a live bug).
- Still cooperative, not preemptive — there's no timer interrupt
  driving a scheduler. Two processes never truly run *simultaneously*;
  what changed is that they can now each have a *turn* without
  corrupting each other's memory, since their virtual address spaces
  are real and separate. A genuinely preemptive scheduler (PIT timer +
  saving/restoring full CPU state including CR3 on every tick) is the
  natural next step if real concurrency (not just isolation) is
  wanted.
- The physical frame pool is a fixed 10 MiB; it isn't derived from an
  actual memory map (this kernel doesn't parse the Multiboot memory
  map at all yet), just placed to comfortably clear everything the
  existing fixed layout uses at the `-m 32` QEMU is launched with.

## Sixth drop: `term` in a window, more coreutils, and a real screen-tearing bug

### `term` now runs in a window too
The fifth drop's paging work removed the *memory* reason `term`
couldn't be windowed (two processes' `0x400000` no longer collide),
but there was a second, separate reason: `term` is `vash`, and `vash`
`execve()`s a fresh program for every command typed — and the classic
`execve()` path (`vnu::proc::sys_execve()`) knows nothing about
`kernel/gui/wintask.cpp`'s cooperative scheduler; jumping into it
mid-slice would use the wrong stack and scheduling model entirely.

Fixed by giving wintask its own `execve()`: `vnu::wintask::exec_current()`
(in `kernel/gui/wintask.cpp`) looks up the target binary via a newly
exported `vnu::proc::find_embedded_data()`, reloads it into the same
app-image region, and rebuilds the argv frame on the task's own stack
— then `kernel/arch/i386/syscall/syscall.cpp`'s `VNU_SYS_execve`
handler checks `vnu::wintask::current_is_task()` first and, if so,
routes through that instead of the classic path, handing off through
the exact same "pending jump" mechanism (`syscall.s`) either way — it
doesn't care which subsystem set up the new `eip`/`esp`, just that
they're valid. `term`'s own `exit` builtin (a direct `exit()` call, no
`execve()` involved) already worked correctly, since `VNU_SYS_exit`
was already wintask-aware from the fourth drop.

Verified in QEMU: `term` opens in a window, runs multiple commands in
a row inside it (including `ls` repeated, which exercises the
heap/malloc path), `exit` ends the task cleanly and the GUI stays
responsive, and the classic (non-windowed) apps and shell are all
unaffected.

### More coreutils
Added to `userspace/vibecoreutils/coreutils.c` (rebuilt with the real
toolchain — see "If you want to rebuild yourself" below): `wc`, `head`,
`tail`, `grep` (plain substring, no regex), `sort` (whole-file,
ascending), `cp`, `mv` (there's no `rename()` in this VFS yet, so `mv`
is a copy + `unlink`), `basename`, `seq`. Registered in the same
argv[0]-dispatch table as the existing applets, and added to `/bin/`'s
VFS listing and the embedded-binary table so they show up in `ls /bin`
and resolve through `vash`'s normal `PATH` lookup like everything else.
`head`/`tail`/`grep`/`sort` share a small fixed-capacity line buffer
(128 lines × 100 chars) — sized to fit comfortably inside a process's
private app-image region (see below) rather than to handle arbitrary
files, which fits this OS's small VFS (files cap out at 20 KiB) fine.

Growing coreutils this much pushed its actual loaded size (code + data
+ that line buffer) to about 66 KiB, comfortably past the 32 KiB the
app-image region had been sized for — so `run_program_from_memory()`'s
private mapping for `0x400000` grew from 8 to 24 pages (96 KiB) to fit
it with headroom, and the per-address-space frame budget
(`kernel/mm/paging.cpp`) grew to match.

### A real bug, not a Limbo bug: VGA present() had no vsync
The screenshot that prompted this ("Limbo x86 PC Emul...", vertical
teal bands tearing through the whole desktop) turned out not to be
specific to that emulator — the same capture, taken with plain QEMU on
this machine, reproduced the identical vertical-band tearing by
accident once mid-testing. `vnu::vgfx::present()` was copying the
320×200 backbuffer to VGA memory (`0xA0000`) with a plain `memcpy` and
no coordination with the display's scanout at all; whenever a screen
capture (or, per this bug report, an emulator's own rendering) sampled
the framebuffer mid-copy, it caught a mix of the old and new frame,
sliced into vertical bands matching however far the copy had gotten.
Fixed with the standard technique: poll the VGA input status register
(`0x3DA`) for vertical retrace, wait for the *current* retrace to end
and the *next* one to start, and only then copy — giving the whole
retrace interval to do it safely off-screen. Verified with 5 separate
screen captures at the GUI's desktop, all clean, and confirmed the
wait doesn't introduce any noticeable input lag (`vedit` typing,
quitting, and returning to the shell all still felt immediate).

### If you want to rebuild yourself
Same as before — from the repo root containing `tools/`, `vlibc/`,
and `vnu/`:
```
./tools/build_userspace.sh   # rebuilds vash/coreutils/vedit/hello and
                              # regenerates vnu/kernel/proc/embedded_*.h
cd vnu && ./build_iso.sh     # rebuilds the kernel + ISO
```
Remember to `rm -rf vnu/kernel/build` first if you're rebuilding at a
path you've built at before (see the warning earlier in this file).

### If you want to rebuild yourself
This drop ships `tools/` and `vlibc/` alongside `vnu/`, laid out the
way `tools/build_userspace.sh` expects (repo root containing all
three). From that root:
```
./tools/build_userspace.sh   # rebuilds vash/coreutils/vedit/hello and
                              # regenerates vnu/kernel/proc/embedded_*.h
cd vnu && ./build_iso.sh     # rebuilds the kernel + ISO
```
IMPORTANT: always `rm -rf vnu/kernel/build` before rebuilding if you
copied these files over an existing checkout. `build_iso.sh` only
wipes its CMake cache when the *source path* changes, so if you've
built at this same path before, stale `.o` files from an earlier
version can get silently relinked instead of the new sources — exactly
what happened with the `ls`/`brk` fix earlier in this file.




## Seventh drop: FHS (`/dev`, `/proc`) and POSIX compatibility

### `/dev` — real character devices
The VFS only knew two kinds of node (regular file and directory), so
character devices needed a new node kind rather than fake files.
`kernel/include/vnu/vfs.h` now has a `DevKind`, and `kernel/fs/vfs.cpp`
implements the semantics directly in `read()`/`write()`:
`/dev/null` (EOF on read, discards writes), `/dev/zero`, `/dev/full`
(writes fail with `ENOSPC` — the whole point of it), `/dev/random` and
`/dev/urandom` (xorshift PRNG seeded from the RTC — *not* a CSPRNG,
there's no entropy source in this kernel), plus `/dev/tty` and
`/dev/console`.

The tty nodes are the exception: they're routed by the syscall layer
rather than the VFS, via `fd_dev_kind()`, so they pick up exactly the
same console/keyboard handling as fd 0/1/2 — including the
windowed-task redirection from the fourth drop, so `/dev/tty` works
correctly inside a VibeGraphics window too.

### `/proc` — generated from live kernel state
Content is regenerated on every `open()` (and on `stat()`, so
`st_size` matches), so readers always see current values:
- `/proc/cpuinfo` — real `CPUID` output (vendor string, family, model,
  stepping, FPU bit)
- `/proc/meminfo` — real numbers from the frame allocator, which
  gained `total_frames()`/`free_frames()` for this
- `/proc/self/status` — the calling process's Pid/PPid
- `/proc/version`, `/proc/mounts`
- `/proc/uptime` — computed against a boot-time RTC snapshot. The RTC
  (CMOS ports 0x70/0x71) is the only real clock available here, since
  this kernel still has no timer interrupt.

Plus the conventional empty FHS directories: `/usr`, `/usr/bin`,
`/var`, `/var/log`, `/home`, `/root`, `/mnt`.

`MAX_N` went from 64 to 96 nodes to fit all this. Every node carries a
full `DATA_CAP` buffer, so that's the dominant consumer of kernel
`.bss` — it now ends at `0x3414B8`, still clear of the `0x400000`
app-image region but with only ~780 KiB of headroom, which is worth
watching if many more nodes get added.

### POSIX compatibility
- **`stat`/`fstat` file types.** Previously only ever reported
  directory or regular. Now report proper `S_IFCHR`/`S_IFDIR`/`S_IFREG`
  mode bits (and `DT_CHR` in `getdents`), with `S_IS*` macros added to
  vlibc's `sys/stat.h`.
- **`dup`/`dup2` for real files.** They existed but only handled pipe
  fds; ordinary files and `/dev` nodes now work too.
- **New syscalls** `getppid` (26) and `isatty` (27), with vlibc
  wrappers.
- **`vlibc/include/vlibc/fcntl.h`** — the `O_*` flags had no header at
  all, so callers were spelling out raw numbers (`0x40` for `O_CREAT`).
  Added and adopted in coreutils and vash.

### Shell I/O redirection
`vash` had no redirection whatsoever. Added `>`, `>>` and `<` (spaced
or attached), built on the now-working `dup2`. This needed a kernel
change too: the syscall layer sent *all* fd 1/2 traffic straight to
the console before ever consulting the VFS, so `> file` could never
have worked — it now checks `vnu::vfs::fd_is_redirected()` first.
Since the VFS fd table is global to the kernel (not per-process),
`vnu::vfs::reset_stdio()` is called at the start of each shell session
so one command's redirection can't leak into later ones.

### Build-system fixes found along the way
- `tools/vcc` hardcodes its list of vlibc sources, so newly added
  `.c` files were silently not compiled into `libvlibc.a` (the link
  failed with undefined references until they were registered).
- `vlibc/include/vnu/abi.h` is a *copy* of the kernel's `abi.h`, not a
  symlink — adding syscall numbers on the kernel side left userspace
  unable to see them until the copy was synced. Worth remembering
  whenever the ABI changes.

### Verified in QEMU
- `ls /` shows the full FHS tree; `ls /dev` and `ls /proc` list
  correctly; every `/proc` file returns sane live content.
- `echo x > file`, `>>` append, `< file` as stdin (checked with `wc`),
  and `> /dev/null` discarding output — all working, shell survives.
- New `/bin/ttytest` (`userspace/examples/ttytest.c`) exercises the
  POSIX additions: `isatty` correctly reports 1 on a terminal and
  **0 when stdout is redirected to a file** (this needed a fix — the
  first version returned 1 for fd 0/1/2 unconditionally), `stat`
  classifies chardev/dir/regular correctly, `getppid` works.
- Full regression: repeated `ls`, `cd` through `/apps/`, the new
  coreutils, GUI with windowed `hello`/`vedit`/`term`, and `/proc`
  reads from *inside* the windowed terminal — all clean.

### Known limitations
- `/proc` has no per-pid directories — only `/proc/self`. Listing
  real pids would need dynamic directory entries, which the static
  node table doesn't do.
- `head`/`tail`/`grep`/`sort` stop after 128 lines. That's also what
  keeps them from hanging forever on `/dev/zero` and `/dev/urandom`,
  which never return EOF. `cat /dev/zero` *will* still spin — same as
  on Linux.
- No pipes in the shell yet (`|`), even though `pipe()` exists in the
  kernel; only redirection was added.

## Eighth drop: terminal no longer dies after one command, plus real line editing

### The windowed terminal went dead after the first command
`term` is `vash`, which `execve()`s each command over itself. When
that command called `exit()`, `vnu::wintask::task_exit()` ended the
*whole windowed task* — so the window survived visually (it still had
content to show) but was dead: no new prompt, no response to input.

The classic console never had this problem because the kernel's
`kernel_main` loop respawns `/bin/vash` whenever a program exits. The
windowed path had no equivalent. Added one: `Task` now remembers the
program it was spawned with (`root_data`/`root_size`/`root_path`), and
`vnu::wintask::exit_respawns_root()` reloads it when a program that
was exec'd *over* the root exits. `VNU_SYS_exit` calls that first and
only falls through to `task_exit()` when the root program itself is
what exited — so `vash`'s own `exit` builtin still closes the window,
as before.

### Arrow keys were silently dropped — two separate bugs
Neither history (which already existed in `vash`) nor cursor movement
worked, for two unrelated reasons, both introduced earlier in this
series:

1. **`kernel/drivers/kbd.cpp`** — the sixth drop factored the scancode
   decoder into a shared `decode_scancode()` returning `int`, with
   `-1` meaning "nothing decoded yet". But the `K_*` pseudo-codes are
   `0x81`-`0x87` stored in a *signed* `char`, i.e. negative — so every
   arrow/Home/End/Delete press returned a negative value and was read
   as "nothing decoded" and thrown away. Fixed by widening them
   through `uint8_t`.
2. **`kernel/gui/gui.cpp`** — the GUI loop had an `else if
   (scancode_ready())` branch that drained "leftover" bytes. Extended
   keys arrive as a `0xE0` prefix *plus* a second byte; `poll_char()`
   consumes the prefix and reports "nothing yet", and then that drain
   branch ate the second byte. So arrows never reached a windowed app
   even after fix #1. Removed the branch — `poll_char()` already
   consumes exactly one byte per call.

### Line editing in vash
`read_line()` only ever appended at the end. Rewritten with a real
cursor position: Left/Right, Home/End, Delete, and Backspace that all
work mid-line, with characters inserted at the cursor rather than
appended. Since this terminal has no escape sequences at all, every
mid-line edit is repainted by hand (`repaint_tail()`) using only
printable characters and backspaces.

### History now actually persists
History was in-memory, and `vash` is replaced by `execve()` on every
external command — so it was wiped after literally every command that
wasn't a builtin. Now stored in `/tmp/.vash_history`, loaded at
startup and rewritten on each new entry, so Up/Down work across the
restarts in both the console and a VibeGraphics terminal window.

### Verified in QEMU
- Text console: `echo ac` + Left + `b` → runs `echo abc`; `echo xqy` +
  Left + Backspace → `echo xy`; Home + insert + End; Delete mid-line —
  all correct.
- History: Up twice recalls the older command and runs it; Down walks
  back toward the current draft; history survives across commands.
- Windowed terminal: three commands in a row each returning to a fresh
  prompt (the original bug); Up-recall; mid-line insert and Delete —
  all working there too.
- `exit` still closes the terminal window and frees the task slot, and
  another app (`hello`) launches cleanly afterward.

### Known limitations
- The windowed console is a 30-column grid and `console_putc` ignores
  a backspace at column 0, so editing a line long enough to wrap
  doesn't repaint correctly across the wrap.
- History is capped at 16 entries and shared by every shell session
  (there's one history file, no per-session separation).

## Ninth drop: cherry-picked from a diverged branch — real `uname`, `dirname`

The person had a separate `vnu` branch that forked off after the fifth
drop (paging) and independently grew a proper POSIX-flagged `uname`
and a `dirname` utility, never merged forward into this one. Ported
just those two pieces — confirmed via a full recursive diff that
nothing else in that branch was actually ahead of this one; everything
else it was missing (`/dev`, `/proc`, redirection, line editing,
`tail`/`grep`/`sort`/`cp`/`mv`/`seq`, the windowed-terminal respawn
fix) is this branch's own later work that the other one just hadn't
caught up on.

- **`uname`** now takes real flags: `-a/-s/-n/-r/-v/-m/-p/-i/-o` and
  their long forms (`--all`, `--kernel-name`, ...), combinable
  (`-sr`), plus `--help`/`--version`. With no arguments it still just
  prints the kernel name (`-s`), matching real `uname(1)`.
- **`vnu_utsname`** (`kernel/include/vnu/utsname.h`, mirrored in
  `vlibc/include/vlibc/sys/utsname.h`) gained `processor`,
  `hardware_platform`, and `operating_system` fields alongside the
  existing five, populated by the `VNU_SYS_uname` handler
  (`kernel/arch/i386/syscall/syscall.cpp`) — this is a struct-layout
  change, so both copies had to move together.
- **`dirname`** — the `basename` complement, added to
  `userspace/vibecoreutils/coreutils.c` and wired into `/bin/` (VFS
  listing + embedded-binary table) the same way every other coreutils
  applet is.

Verified in QEMU: `uname` (bare, `-a`, combined `-sr`, single `-p`/
`-m`), `dirname` against a plain path, a nested path, and a path with
no slash (`.` per POSIX); `uname -a` through shell redirection
(`> /tmp/u` then `cat`); and `uname -a` from *inside* a VibeGraphics
windowed terminal, followed by another command and `exit`, confirming
the eighth drop's terminal-respawn fix and this new applet don't
interact badly. Full shell regression (repeated `ls`, `cd` through
`/apps/`, history recall) stayed clean.

## Tenth drop: own-house flat GUI style + 1:1 HiDPI canvas

The two demo GUI apps (`files`, `prefs`) used to copy NeXT and Windows
3.1 styling (flat gray, thin single-pixel borders, 8x8 icons, cramped
8-row lists). Per the style directive — "forget NeXT and Windows 3.1,
mold our own system" — both were rewritten in an own-house flat look:
dark header/status bars, colored tiles, full-width selection bar,
8x16 face only. Along the way the root cause of them looking
"pixelated/giant" turned out to be architectural, not cosmetic:

- **The gfx canvas was 240x170 and the WM upscaled it 2x**, turning
  every 8x16 glyph into 16x32 — twice the size of native console text.
  The canvas is now **480x340 (= 60 cols x 21 rows of the 8x16 face)
  and windows display it 1:1**, so gfx-app text matches the console
  exactly. The on-screen window geometry is unchanged (488x372).
- **The mouse protocol grew from one byte to two** per coordinate:
  `ESC [ M <btn> <xl> <xh> <yl> <yh>` (little-endian u16, 8 bytes
  total, enqueued atomically behind `MOUSE_MSG_LEN`) because 480
  doesn't fit in 8 bits. Kernel encoder (`wintask.cpp feed_mouse`) and
  userspace parser (`vgfx.c`) were changed together and stay in sync
  via the shared header docs.
- **App budget raised to 64 pages** (`APP_PAGES` 24 -> 64, 256 KiB):
  a gfx app's static `fb[480*340]` (159 KiB of `.bss`) no longer fits
  in the old 96 KiB app-image mapping. `MAX_FRAMES_PER_SPACE`
  (app + stack + 1 MiB heap = 336 pinned frames/task) was raised
  320 -> 512 to match; the old cap silently made `create_address_space`
  fail and apps simply never opened.
- **Userspace moved to the new layout:** `files.c` (19 visible rows,
  j/k navigation, full-width selection, parent tile), `prefs.c`
  (selector panes About/Memory/Mounts/CPU with colored tiles,
  wider 92-px column), both with updated `man(1)` pages; `calc` and
  `picview` are layout-macro-driven so they picked up 1:1 text with no
  layout changes.
- Verified: userspace + kernel build clean (no new warnings), ISO
  regenerated. QEMU screenshot check outstanding.

## Eleventh drop: runtime resolution, a shared canvas, a chosen wallpaper

The mode used to be a build-time constant: `vga_gfx.h` carried
`WIDTH`/`HEIGHT` as `constexpr`, the VBE registers were programmed once
at desktop startup, and the wallpaper was decoded once for exactly
that size. Changing the resolution meant rebuilding. This drop makes
it a value the driver holds and the desktop can change while it runs,
gives both display drivers one interface for it, and replaces the fd-3
framebuffer hack with memory an app and the compositor share.

### Runtime resolution

- **The ladder lives in the driver** (`vga_gfx.h`): `vgfx::Mode`,
  `MODE_COUNT`, `DEFAULT_MODE` (1024x768), `MAX_MODE` (1280x1024) and
  `modes()` / `width()` / `height()` / `mode_supported()` /
  `set_resolution()`. Every `WIDTH`/`HEIGHT` use became
  `width()`/`height()`, including the procedural wallpaper, which is
  still drawn in 1024x768 coordinates and scaled through `px()`/`py()`/
  `pr()` so the sun, clouds and hill heights land where they were
  authored.
- **`g_backbuf` is sized for `MAX_MODE`, not for the current mode.**
  Both it and `g_wall` were `WIDTH * HEIGHT` (768 KiB at 1024x768) and
  are 1.25 MiB now, so a mode change is a register write: nothing is
  reallocated and no frame is lost mid-composite. The price is `.bss`
  that the low modes sit on unused - the trade the header already named
  as a ceiling, now written down as a number.
- **F12 steps the ladder** on a live desktop. The keyboard decodes
  F1..F12 as 0x88..0x93, the same values as `VNU_KEY_F*` in
  `vlibc/keys.h`, so a key a desktop shortcut consumed and one a
  program reads from `getch()` cannot disagree. Two syscalls carry the
  same thing to userspace: `VNU_SYS_gfx_setmode` (62) and
  `VNU_SYS_gfx_getmode` (63), wrapped as `vgfx_set_resolution()` /
  `vgfx_get_resolution()`. Both drivers answer identically, so a
  program never has to parse `/proc/gfx` to know where it stands -
  which now also lists the whole ladder as `modes` beside the mode in
  use.
- **The choice is remembered** in `/etc/vnuconfig/gfx.conf`
  (`mode 1024x768`), a new file seeded with the default, rewritten on
  every change and read before the card is programmed, so the next
  `gui` comes up in the mode the last one left. It is in the RAM VFS
  like everything else under `/etc`: a reboot starts over from the
  built-in default.
- **Relayout policy** (`gui.cpp`): a mode change closes nothing and
  loses nothing. A window that fits keeps its size and is clamped back
  inside; one too big for the new screen shrinks to it, and a text
  window just shows fewer columns and rows, since `fit_cols`/`fit_rows`
  read the client area off the rectangle. The cascade offset restarts
  (an offset chosen for the old geometry lands off the new screen) and
  the cursor is brought back inside. Everything else the desktop shows
  is redrawn from scratch every frame, so nothing else needs redoing.
- **A mode change no longer re-decodes the wallpaper.** The image is
  decoded once, quantized at its own size into `g_small` (512x384, the
  decoder's own limit) and re-scaled by nearest neighbour on every
  resize, so a switch only scales: a 1280x1024 desktop repaints in
  well under a second instead of standing blank for two.
- **The console comes back afterwards.** A graphics mode paints over
  the text plane *and* the console font in plane 2, so
  `tty::save_screen()` captures the 80x25 cell buffer on the way in and
  `restore_font()` + `tty::restore_screen()` put both plane 2 and the
  text back on the way out. Two register-level details: the attribute
  controller shares one flip-flop between its index and its data
  writes, so the `inb(0x3DA)` in front of the video-enable write is
  load-bearing, and the saved sequencer state is 8 registers wide (was
  5), or the mode switch left registers it had no business touching.
- **virtio-gpu follows the mode.** The scanout resource is exactly the
  size of the mode in use and backed by a *list* of 256 KiB segments:
  the PMM pool is shared with the VFS nodes and every window's
  surface, and a 5 MiB contiguous run is not something it can often
  offer. `present()` walks the runs row by row, so a row may straddle
  two of them. Resource ids are handed out in sequence and never
  reused, because a create on a just-unreffed id is refused by the host
  with `ERR_INVALID_RESOURCE_ID`; a switch unrefs the old resource
  *before* returning its frames, which is also the only order in which
  the two buffers never exist at the same time (at the top mode, 5 MiB
  against 10 of a 14.4 MiB pool). `submit_and_wait()` samples the used
  ring *before* the kick - reading it after loses the race the wrong
  way round, and a warm command that completed already would be called
  a failure. The device is named in the log when a command is refused
  or never completes, instead of the desktop quietly stopping.

### The canvas is shared memory

- **`VNU_SYS_gfx_surface` (61)** returns the virtual address of the
  480x340 canvas, mapped on first use inside the app window's own 4 MiB
  page and into the kernel through its physical address. The app
  writes pixels and the compositor blits those same bytes on its next
  pass: no copy, no frame ever crosses a syscall, and asking is also
  what puts a window into pixel mode, which is how the desktop knows to
  size it to what the app draws. Returns 0 when the caller has no
  window, and there is no unmap - the frames go back with the address
  space.
- **fd 3 is an ordinary descriptor now.** The `write(3, ...)` /
  `lseek(3, ...)` intercept is gone, so `vfs.cpp` and `pipe.cpp` hand
  out numbers from 3 up instead of reserving one, and every file a
  windowed app opens stops being a hazard.
- **`extend_address_space()` had a bug this exposed:** an address space
  seeded from the identity map has *present* entries, but they are
  shared with every other space and with the kernel. Extending over
  one skipped the "already mapped" test and left the new mapping
  pointing at memory it did not own; it now checks the frame is one of
  the space's own.
- **vgfx keeps its API.** `fb` is the shared canvas where there is a
  window and a private buffer where there is not (a program drawing
  with no window has to draw into something; the result is not shown).
  `vgfx_flush()` stays, because every app ends a frame with it and
  there is nothing left to push. `vgfx_event_t.key` is unsigned, since
  the kernel normalises the PS/2 scancodes for the arrows into
  0x81..0x87 and a signed char made every comparison against them
  false.

### The wallpaper is choosable

- **`wallpaper`** is its own binary (`userspace/vibecoreutils/`) with a
  `man` page, as every command here has one. With no argument it prints
  the background in use (or `none` while the procedural sky is up);
  with a path it hands it to `VNU_SYS_wallpaper` (60), which decodes
  the candidate *first* and only swaps it in when that works - a
  refused picture leaves both the screen and `/etc/vnu/wallpaper` as
  they were. BMP and PNG up to 512x384 and 64 KiB are accepted; a JPEG
  is listed by `--list` and then refused, since its IDCT is the one
  piece of float code a `-mno-80387` kernel cannot link.
- **`prefs` grows a Wallpaper pane**: the same candidates as a
  tile list, Up/Down moves the cursor (clicking moves it too), Enter
  applies, and the status line says why a pick was refused.
- `/etc/vnu/wallpaper.default` is never overwritten, so the desktop
  VNU ships with is always there to come back to. The shipped image is
  embedded as `wallpaper_png` now, leaving the plain name to the
  command's own binary. `/proc/gfx` names the background in use.

Verified in QEMU: `make test` 84/84, `make test-gpu` 84/84 (the
virtio-gpu display, where a mode change rebuilds the scanout) and
`make test-install` 9/9. The resolution case walks the whole ladder by
F12 on a live desktop and checks at each size that the screen is a
drawn desktop (`gfx-resolution`: `mode-config-boot` on a fresh image,
`mode-proc`, `mode-config`, the second desktop coming up in the
recorded mode, and a `calc` window that stays inside the frame at the
smallest mode and at the top one - not pinned to a corner). The
surface case (`gfx-surface`) works off QEMU screendumps - a gfx window
is exactly the case where the serial log proves nothing - and looks for
the band of changed pixels a 480x340 canvas has to produce, at the
width and depth a window's client area is and where a centred window
goes, with enough colour changes along its middle row to be the app's
picture rather than a blank canvas; then it brackets two desktop
sessions and checks the console text came back, allowing for the lines
the harness itself scrolled. `wallpaper-*` covers the command against a JPEG, a
missing file, a text file, the shipped desktop and a real picture;
`prefs-wallpaper` drives the pane with real key events.

Known limitations / natural next steps:

- The mode and the wallpaper live in the RAM VFS, so a reboot starts
  over. An installed disk has one spare config sector (`VNUCFG1`, the
  hostname's) that could carry the mode; the wallpaper is a file and
  would need a real filesystem first.
- `g_backbuf` and `g_wall` are still static arrays in `.bss` (1.25 MiB
  each), and `g_small` adds 192 KiB. A mode-sized allocation from the
  PMM in `enter_gfx_mode()` is the fix, and it is what makes 32bpp
  reachable at all. (Done, one drop later: see the Twelfth drop.)
- Still 8bpp with a 16-entry DAC, so there is no alpha and no
  compositing: two render paths (VBE flat 8bpp, virtio-gpu B8G8R8X8)
  would have to coexist for a 32bpp mode, and `mode_supported()` would
  have to refuse it while virtio-gpu is not the active driver.
- `present()` still copies the whole frame every time, which 8bpp
  tolerates and 32bpp would not.
- A window's canvas is still 480x340 whatever the mode, upscaled by
  the compositor: a bigger mode buys desktop room, not sharper apps.
  Making the canvas follow the window is an Expose-style change.
- Resolution changes are visible only to the display: a wall clock
  (`tty_size`) and a wallpaper refresh in a text window are not told.

## Twelfth drop: the buffers are the mode's size, and the pool follows .bss

`MAX_MODE` was not only the top of the resolution ladder. For as long as
the backbuffer and the wallpaper frame were static arrays, it was also
their *size*, and a machine paid for it whether or not it ever drew a
pixel at that resolution. Four `.bss` arrays stood between the desktop
and the pool:

- `g_backbuf`, the framebuffer: 1.25 MiB at 1280x1024.
- `g_wall`, the wallpaper frame, the same.
- `g_small`, the quantized source image: 192 KiB.
- `g_px_pool`, px.h's decode arena: 2 MiB, resident for the kernel's
  whole life although a decode is over in a second.

4.7 MiB, every boot, for a desktop that at 640x480 uses 0.8 MiB of it.
This drop moves all four into the PMM pool, sized for the mode actually
programmed, and hands them back when they are not in use.

### The framebuffer

- **`enter_gfx_mode()` returns bool and allocates.** The run is taken
  before the card is programmed, and reused if `set_resolution()` had
  already taken one for this mode from text mode (which is how the
  seeded `gfx.conf` gets its mode: a console-time `set_resolution()`
  finds the pool far emptier than a desktop does). The only thing that
  can stop a desktop now is a pool with nothing left, and then the
  desktop says so on the console (`vnu: not enough memory for the
  desktop`) and hands the shell back - the text mode is never
  programmed, so there is no half-state to clean up.
- **`set_resolution()` takes the new framebuffer before it touches
  anything else.** Both allocations that can fail are asked in the
  order that leaves the display untouched if either says no: the
  framebuffer, then the virtio-gpu scanout. The old run is released
  only once the new one is in hand, so the switch is a pointer swap
  rather than a free followed by an allocation that might not come.
- **`exit_to_text()` gives it back.** A machine sitting at the console
  holds no framebuffer at all; the next desktop session takes a fresh
  run for whatever mode is configured by then. `wallpaper::unload()`
  does the same for the wallpaper's pixels, in the same place, so
  quitting the desktop hands ~2.7 MiB at the top mode back to the pool.

### The wallpaper's three buffers

- The frame is `width() x height()` of the mode on screen, and the
  quantized source is `MAX_PIX` (512x384) as before - the pixel bound
  of what we decode, not of the mode. The arena is one pool run per
  decode, so a `wallpaper set` that fails releases it again.
- `load()` starts by releasing whatever the last session left, so a
  machine that has run a few desktops in a row is in the state of a
  fresh boot plus one desktop. `apply()` decodes into *new* runs and
  only swaps them in once the decode worked, so a rejected candidate
  (JPEG, oversized, or simply out of pool) still leaves the desktop
  exactly as it was.
- `resize()` grows the frame to the new mode and, if even that cannot be
  had, drops back to the procedural scene - which fits any mode - rather
  than blitting a frame stretched for the old geometry. `ready()` now
  means "there is a frame *for the mode now programmed*", so that
  invariant is checked where it is relied upon.
- The name is still a static array: releasing the pixels does not
  forget the choice, so `/proc/gfx` still reports which file is the
  wallpaper after the desktop has quit.

### The pool follows the kernel's .bss

- The freed 4.7 MiB would have been a hole between `__bss_end` and a
  pool base that is a hand-written constant, so `pmm::init()` reads
  `__bss_end` from the linker now. The pool is everything between the
  kernel's own globals and the last usable byte of RAM, and the two
  cannot drift apart. This retires a footgun that had already bitten
  twice, both times as a triple fault: `alloc_frame()` zeroes every
  frame it hands out, so a base that lags behind a grown `.bss` wipes
  the bitmap, the identity page tables and the kernel page directory,
  and the allocator then re-issues those in-use frames.
- `__bss_end` goes 0xF93754 -> 0xAE3754 and the pool grows from
  17..30.75 MiB to 10.9..30.75: **20.5 MiB, 5256 frames**, up from
  14.4 MiB and 3688. `/proc/meminfo` says `PoolTotal: 21040 kB` where
  it said `14784 kB`.

### What this unblocks

Milestone B (32bpp) was blocked on exactly this: at 32bpp the same pair
of buffers is 10 MiB of `.bss`, and a 1280x1024 desktop plus its
virtio-gpu scanout (5 MiB) plus one window would not have fitted in the
old pool at all. Both are now the mode's size, taken when they are
needed, and the pool has 6 MiB more to give.

### The bug this uncovered

`execve` of a binary read out of the VFS staged it in a 1 MiB
`vnu_kalloc()`. That symbol is the *image decoders'* bump allocator -
the one `px.h`'s `malloc` is `#define`d to, implemented in
`wallpaper.cpp` over the static arena above - so the exec path and
`px.h` were quietly sharing one 2 MiB bump: two execs of a VFS binary
filled it and every later one failed with ENOMEM. It is now sized from
the file (a 6 KiB program stages in two frames) and released as soon as
the image is mapped. Which is also why `vnu_kalloc` can now honestly
return null outside a decode.

### Verified in QEMU

- `make test` 85/85, `make test-gpu` 85/85, `make test-install` 9/9.
- `gfx-resolution` cycles the whole ladder (F12) on both displays with
  the desktop drawing a wallpaper throughout, and `gfx-surface` still
  sees a window's pixels on the screen - both are the checks that would
  notice a framebuffer that is the wrong size for the mode.

### Still open

- The buffers are 8bpp palette indices, so a desktop session now costs
  pool frames proportional to the mode instead of a fixed `.bss`, but
  the pixel format itself is unchanged (Milestone B).
- `present()` still copies the whole frame to the card every frame
  (Milestone C), and an app's canvas is still 480x340 whatever the mode
  (the Expose-style change named in `vga_gfx.h`).
- The pool base is now read at runtime, so a kernel whose `.bss` grew
  past `POOL_END` would come up with an empty pool and a machine that
  fails every allocation. The link checks it now
  (`vnu/kernel/linker.ld`: `ASSERT(__bss_end < 0x01F70000, ...)`), so
  the mistake is a link error next to the `.bss` that caused it.

### Thirteenth drop: a program can ask what the display is

`gfx_getmode` (63) answered two out-pointers: the width and the height
of the mode in use. The depth of that mode had nowhere to go. It could
have been a third pointer, and that is exactly the shape that must not
be added to an existing syscall: vlibc's `syscall()` is variadic, so a
caller that passes two arguments leaves whatever it happened to have in
`edx`, and a kernel that wrote a `uint32` through `edx` would be writing
to a stack address picked out of a register that nobody meant. So the
answer is a syscall of its own:

    VNU_SYS_gfx_getinfo (64)   ebx = struct vnu_gfx_info*

    struct vnu_gfx_info {
        uint32_t width;   /* pixels */
        uint32_t height;
        uint32_t bpp;     /* bits per pixel a program handles */
        uint32_t driver;  /* VNU_GFX_DRIVER_VGA | _VIRTIO_GPU */
    };

The struct lives in `vnu/abi.h` on both sides of the boundary, so the
kernel and vlibc cannot disagree about it - the first shared struct in
the ABI that is defined once. `vgfx_get_info()` wraps it for vgfx apps,
and `vnu fetch` prints the result instead of parsing `/proc/gfx`:

    Graphics :  vga 1024x768 8bpp        (no virtio display)
    Graphics :  virtio-gpu 1024x768 8bpp (with one)

`/proc/gfx` reports its `bpp` from the same `vgfx::bpp()` the syscall
does, so the text file and the syscall cannot tell a program two
different stories - and the suite now checks that, at a mode that is
not the default, where a stale struct would show up.

`bpp` is 8 on both paths and that is honest rather than lazy: a window's
canvas is 480x340 one byte per pixel, so the desktop composites palette
indices either way. virtio-gpu already presents through a 32bpp
B8G8R8X8 scanout, but `present()` expands the 8bpp backbuffer through
the DAC on the way there. Reporting 32 would promise a pixel format no
program can draw into yet; the field becomes 32 when the composited
format follows the scanout (Milestone B).

### Fourteenth drop: a console on a display that has no text mode

`./vnu/run.sh virtio-gpu` booted to a window with nothing in it: the
prompt was on the serial line and nowhere else. The same display had a
second version of the same fault - a desktop that quit left its last
frame on the monitor, because a virtio-gpu has no text mode for the host
to fall back to. Both come from the same place, and neither is a
driver bug in the sense of a missing register write.

The host shows whichever plane a virtio-vga was pointed at last: its own
VGA text plane, or a virtio-gpu scanout resource. The driver pointed it
at the resource in `init()`, so a machine sitting at its prompt was
showing a scanout that nothing had drawn yet - a black screen with a
prompt somewhere else entirely. And the obvious way back does not work:
unpointing the resource (`SET_SCANOUT` with `resource_id 0`) does not
hand the display back to the text plane, it freezes the last frame the
card was given, and nothing transferred afterwards ever shows up
(QEMU 11.1). That is why quitting the desktop used to `blank()` the
frames and present them: the console's own black, with the console
itself nowhere on it.

So the resource is built and backed at `init()` and left alone, and a
desktop session takes the display over when it enters graphics mode -
once the mode is programmed, so a mode change that fails leaves the
console on the screen where it already was, and again when a session
changes the mode, because a new resource is a new thing to be shown.
The console a session ends in is drawn into the scanout the way the
desktop draws: `vgfx::present_text()` puts the 80x25 grid on black in
the 9x16 cell the text mode uses (8 wide where a mode is too narrow for
a row of 80, which lands on 640), in the palette the text mode booted
with, and presents it before the mode is given up. A cell is read
through `tty::cell()`, which is the console's own saved copy while a
saved screen is saved: `0xB8000` is not the text plane then, it is
inside the aperture the mode has pointed at the framebuffer instead.

The text needs a font, and the font capture came back empty: a
virtio-gpu has no 8K of glyph ROM, so the font window reads back zeroes
and every glyph drawn from that table is a blank cell - which is what
the desktop's own small text was drawing. A card that hands back no font
at all is given the one it would have had: the VGA 8x16 font the BIOS
ROMs carry, kept as `kernel/console/font8x16.cpp` (the same bytes
QEMU's `vgabios` has at offset `0x6720`, so it is the letterforms a VGA
text mode would have shown). Captured or built in, the console and a gfx
app draw the same shapes.

The suite gained a full-screen check of it. `console-screen` takes a
screendump and looks for the console as a text mode puts it on a
display: at boot the 720x400 grid, in the mode the desktop ran in the
same grid centred and at text-mode ink density, so a scanout full of
flat gray fails it. `gfx-surface` still brackets a session and compares
the two consoles, now through the same helpers and allowing for the
scrollback the commands typed in between have moved: the frames are
compared at every line offset and the smallest difference decides, so a
console that came back a screenful lower than it started still passes
while one that did not come back matches at no offset at all.

- `make test` 86/86, `make test-gpu` 86/86, `make test-install` 9/9.
