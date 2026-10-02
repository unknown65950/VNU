# thams/ — third-party app modules

A **THAM** (Third-Party App Module) is a program that lives here instead
of inside the system tree. It keeps its own directory, its own sources
and its own manual page, and adding one to the image requires no edit to
`vnu/`, to `vlibc/` or to any kernel file: drop the directory in, build,
and the module is there.

That separation is the whole idea. The OS is small, has a style and has
rules; a module is somebody's code with its own idea of what a tool is.
The two never have to agree about more than the toolchain.

```
thams/
  README.md            this file
  <name>/
    tham.conf          what the build needs to know (required)
    *.c *.h            the sources
    man/<name>.1       the manual page, as the text man prints (expected)
    LICENSE            the upstream licence, if the code was vendored in
```

## What a module gets

Every module is compiled into its own static ELF and mounted at
`/bin/<name>`, so the shell runs it like any other command:

```
$ tree /etc
etc/
|-- group
`-- vnuconfig/

6 directories
```

Do not check for a module with `which`: vash answers `which` with a
best-guess `/bin/<cmd>` for anything that is not a builtin, so it says
`/bin/tree` whether or not there is a tree. Ask the filesystem instead —
`ls /apps/<name>` lists what the module mounted.

A module marked `GUI=1` is also installed under `/apps/<name>` and gets
a tile on the desktop. A command-only module gets `/apps/<name>/` with
its page and no tile, which is how a terminal tool stays off the
desktop's face while `man` can still reach it.

A module's manual page is mounted at `/apps/<name>/man/<name>` and
`man <name>` prints it. The page is the finished text — the same layout
as the built-in pages, without C string quoting:

```
NAME
    tree - list directories as a tree

SYNOPSIS
    tree [option]... [path]...

DESCRIPTION
    ...
```

## tham.conf

One `KEY=VALUE` per line, `#` for a comment. Values may be quoted.
An unknown key is a build error, not a shrug, so a typo is reported
rather than silently ignored.

| key | meaning |
|---|---|
| `NAME` | the module's name; must equal the directory name |
| `SOURCES` | the `.c` files to compile, space separated (required) |
| `GUI` | `1` for a desktop tile, `0` for a command (default `0`) |
| `COLOR` | tile colour, a palette slot `0..15` (default `7`, grey) |
| `GLYPH` | tile pictogram id `0..15` (default `0`, a letter) |
| `MAN` | the page file, relative to the module (expected of every module) |
| `ABOUT` | one line for `make thamstatus` |

`NAME` has to agree with the directory name because the directory is
what the build scans; a mismatch is an error rather than a rename.

Sources are compiled by one `tools/vcc` invocation with the host's
include path, so a module may have as many files as it likes and may
`#include "..."` its own headers.

## Building

| command | what it does |
|---|---|
| `make thamstatus` | list the modules and what each one brings |
| `make thambuild` | compile them once into `sysroot/thams/` — fast, no kernel |
| `make thamupdate` | put them in the image (rebuilds the ISO) |
| `make thamoff` | leave them out, keeping the sources |
| `make thamon` | build them into the image again |

`make thamupdate` is the one that matters: a module's bytes are part of
the kernel image, so changing a module means rebuilding it. `make iso`
does the same through `tools/build_userspace.sh`, which regenerates the
module header on every userspace build, so nothing has to be asked for
twice. `make thambuild` is the cheap half on its own — it compiles the
modules and stops, which is all that is wanted while working on one.

`make thamoff` writes `thams/.off` (ignored by git, as a local choice
should be) and `make thamon` deletes it.

## What a module must and must not do

The toolchain is shared, so the floor is the same everywhere:

- **English.** Comments, help text, the manual page — all of it.
- **`tools/vcc` and `vlibc` only.** No host libc, no host headers. The
  guest `vcc` must be able to build the module too, which is why nothing
  may depend on the host compiler's extensions.
- **No kernel changes.** A module is a program, not a driver. If it
  needs something the kernel does not do, that is a change to the OS,
  made in the OS, in its own commit — not smuggled in through a module.
- **No new syscalls.** A syscall is ABI: `vnu/kernel/include/vnu/abi.h`,
  `vlibc/include/vnu/abi.h` and `vnu/abi/ABI.md` move together, in the
  OS tree.
- **A manual page.** `make thamstatus` warns when a module that is not a
  GUI app has none, and it should have one either way: a command nobody
  can read about is half a module.
- **Its own licence.** Code vendored in keeps its upstream `LICENSE` next
  to it, unmodified. Anything permissive is fine; nothing that pulls the
  OS into obligations it has not agreed to.

A module may also be a *library* the OS does not use: a header plus its
sources, compiled on demand by whatever needs it. Nothing here requires
a module to produce a command.

## How it works

`tools/thams.py` is the whole integration. It reads each `tham.conf`,
compiles the module with `tools/vcc`, and writes one generated header,
`vnu/kernel/proc/embedded_thams.h`, holding the module bytes and the
table that names them.

`vnu/kernel/proc/thams.cpp` is the only translation unit that includes
that header — deliberately, because a blob included twice is stored
twice in the image. Both users go through its two functions instead:
`proc/process.cpp` asks for `/bin/<name>` when the shell runs a command,
and `gui/apps.cpp` installs the module under `/apps`.

Everything the kernel knows about modules is in
`vnu/kernel/include/vnu/thams.h`. That file plus `tools/thams.py` is the
whole contract on the kernel's side: a module needs no kernel source
change, and adding a kernel feature for modules does not need a module
change.