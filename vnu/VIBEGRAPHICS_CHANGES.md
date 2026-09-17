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
  original bug report ("ломается графика при запуске демо
  приложений"): `term` runs the classic full-screen hand-off with a
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
