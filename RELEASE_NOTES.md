# VNU 0.7 "Canyon"

The first release of VNU with a codename, and the first one whose notes
are written down. 36 commits between 27 September and 1 October 2026,
since 0.5.

VNU is a hobby operating system for x86 (i386) built from scratch: GRUB
loads a monolithic freestanding C++20 kernel via Multiboot2, the kernel
sets up paging and ring 3 and starts **vash** on top of its own VFS, and
instead of `libc` there is **vlibc**. All of userspace is compiled with
the host toolchain `vcc`/`v++`/`vld` and embedded into the kernel.

## The display

The mode is a runtime choice now. `gfx_setmode`/`gfx_getmode`/`gfx_getinfo`
(62/63/64) let a program ask what the display is and change it, and
`gfx_surface` (61) hands back a canvas in the **display's own depth** with
a way to name a colour, so an app draws in the depth the screen is in
rather than picking one and having it converted. A windowed app draws into
**shared pages**, not a descriptor it has to be handed and told about.

The desktop's buffers are the size of the mode and come out of the PMM
pool, which now starts where the kernel's `.bss` ends.

On **virtio-gpu** the console is pixels on a display with no text mode at
all, the scanout is the size of the mode the display is in, and after a
desktop session the console is the live one — a mode change no longer
leaves the monitor showing a stale frame. On **VGA** the composited frame
follows the display's own depth, and the text console is repainted row by
row rather than buffer by buffer.

Two things came out of that work, and they are the reason the numbers
above matter:

- **Damage tracking.** `present()` used to copy a full 1024x768 frame on
  every call — 786 KB on 8bpp, 3 MB at 32bpp. Now damage is one union box
  per pass, only its scanlines are copied, and virtio-gpu gets the
  rectangle as a dirty region so the host can skip the rest. At 32bpp this
  is the difference between usable and not.
- **Occlusion culling.** A window whose rectangle is entirely under opaque
  windows is neither drawn nor damaged. Whether the covers add up is a
  union-of-areas question, not "is there a window on top": the calculator
  under a terminal has to be hidden by *all* of it. A gfx cover counts as
  opaque only below 32bpp, where a pixel has no alpha byte to make itself
  see-through — at 32bpp an app may have written translucent pixels, and
  "a translucent cover hides this corner" is exactly the picture that must
  not change.

Accounting for both is in `/proc/gfx`: `last_present` is the last pass,
`present_total` is every byte since boot, and a desktop session is the
difference of two reads.

The panel is not a clock. It used to draw a wall clock and a date, so the
desktop claimed to know the time and was wrong as often as it was right.
The clock is a program, it is on the dock as `clock`, and a panel that
misreports is worse than a panel that says nothing.

The picture pack, the sound clips and the desktop wallpaper moved out of
the VFS root into `/etc/vnu/` — they are what the machine boots with, which
is what `/etc` is for. `wallpaper` is a command and a pane in prefs.

## Processes and signals

`fork` is a real process now: its own page directory, frames copied through
`paging::copy_pages()`, the child waking in `popad+iretd` out of a copy of
the very frame the kernel left `int $0x80` in. Not copy-on-write — there is
no fault handler — and the file descriptor table is still one for all
processes.

Ctrl+C is a signal and the shell survives it. `rt_sigaction`,
`rt_sigprocmask`, `rt_sigreturn` (66/67/68) and `alarm`, `sigraise`,
`sigpending` (69/70/71) came with it. SIGPIPE and SIGCHLD are reserved but
not raised: the numbers are claimed so nothing else takes them.

The PS/2 keyboard has one queue and one decoder behind the data port, so a
keystroke cannot be decoded twice or lost between two readers. And the
mouse now has a `shutdown()`: the keyboard and the mouse share the 8042's
single output buffer, so a mouse packet left in it when the desktop exits
is a byte the keyboard cannot deliver — which silenced the keyboard until
reboot, and took the second desktop session with it.

## Commands

- **`vnu`** — `vnu fetch` (driver, resolution, depth), `vnu version`, `vnu size`, and `vnu install`, which stamps an MBR, records the host name in the config sector and builds a FAT16 partition that boots on its own with no CD.
- **`vibecommander`** — a two-panel Norton-Commander-style file manager, directories bright blue and readable on black.
- **`wallpaper`** — pick the desktop background.
- **`forkdemo`** — proof that `fork` is a process and not a saved register set.
- **`hostname`** — `/etc/hostname`, which `vnu install` writes and the prompt then follows.
- **`man`** pages on a terminal, with a white status bar.

## Building and testing

`make` is the front end: `doctor`, `iso`, `run`, `run-headless`,
`run-vhd`, `run-installed`, `vhd`, `test`, `test-gpu`, `test-install`,
`test-all`, `clean`. The build logic still lives in the scripts, which
remain a working path of their own, and a fresh checkout regenerates
everything.

The guest test harness (`tools/qemu_test.py`) boots the ISO headless and
types into the guest through QMP `sendkey` — the keyboard driver reads
PS/2, not COM1, so a test has to press real keys — and asserts on what the
session prints. 90 cases: 81 table entries plus 9 that have to *drive* the
guest, because a full-screen program like the `man` pager owns the
terminal until you press a key.

The occlusion check is the only one that can see the compositor leave a
window alone, and it is not a table entry: each session is a desktop of its
own and swallows every keystroke, so both are driven from the harness and
the whole thing is judged at the end. It runs the desktop twice, the
second time with the calculator open under the terminal, and compares what
the two sessions handed the host.

Worth knowing if you extend it: **bytes cannot prove occlusion.** The
hidden window's damage lies inside the terminal's own rectangle and one
transfer presents one bounding box for all of it, so a compositor that
composited the hidden window anyway would move exactly the same bytes. The
proof is the frames — both sessions park the pointer on the same spot and
the frames differ by 1 pixel.

## Verified, and known not to be

Green on the ISO from this tree: the occlusion check, three runs in a row,
across a control session, a session with the calculator, and the frame
comparison; `proc-gfx`; the version surface (`uname -a` reports `0.7.0
Canyon`, `/proc/version` reports `version 0.7 (Canyon) i386`).

**`gfx-damage` currently fails**: rows 34..767 of the desktop still differ
once a window is closed, so the ground it vacated is not repainted. This
is not new in 0.7 — it reproduces on 0.5's tree with this release's test
harness, and `gui.cpp`, `vga_gfx.cpp` and `wallpaper.cpp` have not been
touched since 0.5 shipped, so the cause is elsewhere (most likely the
desktop's app set, which gained vibecommander) and is not yet located.

Also open: no copy-on-write for `fork`; one file descriptor table for all
processes, which will have to be per-process before pipes; the console
still upscales a 480x340 canvas nearest-neighbour above 640x480, which is
what the next milestone is for.

## Documentation

`README.md` is the front door: features, commands, layout, build and run,
testing, rules. `AGENTS.md` is the ruleset, including the rule that
everything in the tree is English. `vnu/VIBEGRAPHICS_CHANGES.md` is the
GUI and window history, drop by drop. `vnu/abi/ABI.md` documents every
syscall, and `make test` is how any of it is checked.