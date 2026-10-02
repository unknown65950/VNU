#!/usr/bin/env python3
"""Build the third-party app modules in thams/ into the kernel image.

A THAM is a program that lives outside the system tree: its own
directory under thams/, its own sources, its own manual page, and
nothing in vnu/ edited to add it. This script is the whole of that
integration. It reads each module's tham.conf, compiles it with the
same tools/vcc the OS is built with, and writes the one generated
header the kernel links against.

    python3 tools/thams.py status     list the modules and their settings
    python3 tools/thams.py build      compile them into sysroot/thams/
    python3 tools/thams.py generate   compile, then write embedded_thams.h
    python3 tools/thams.py on|off     switch module building off or on

`generate` is what tools/build_userspace.sh calls, so `make iso`
picks up whatever is in thams/ with no separate step. `make thamoff`
leaves a marker that makes `generate` write an empty table, which is
how a module is kept out of the image without deleting it.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THAM_DIR = ROOT / "thams"
OUT_HEADER = ROOT / "vnu" / "kernel" / "proc" / "embedded_thams.h"
BIN_DIR = ROOT / "sysroot" / "thams"
MARKER = THAM_DIR / ".off"
VCC = ROOT / "tools" / "vcc"

MANIFEST = "tham.conf"

# A tile's colour and glyph are palette and pictogram slots the kernel
# already has; these are what a module gets when its tham.conf stays
# quiet about them.
DEFAULT_COLOR = 7      # vgfx::COLOR_LGRAY
DEFAULT_GLYPH = 0      # IconGlyph::ICON_LETTER

BOOL_KEYS = ("gui",)


class ModuleError(Exception):
    """A module's directory is not a module: worth stopping the build."""


def parse_manifest(path):
    """KEY=VALUE lines, with the quoting a shell would allow.

    Deliberately not a shell: this runs on every build, and a module
    from the outside world has no business executing anything to be
    described. An unknown key is an error rather than a shrug, so a
    typo in a manifest is reported instead of silently ignored.
    """
    known = {"name", "sources", "gui", "color", "glyph", "man", "about"}
    conf = {}
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ModuleError("%s:%d: not KEY=VALUE: %s"
                              % (path, lineno, raw))
        key, value = line.split("=", 1)
        key = key.strip().lower()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        if key not in known:
            raise ModuleError("%s:%d: unknown key %r (known: %s)"
                              % (path, lineno, key, ", ".join(sorted(known))))
        conf[key] = value
    return conf


def slot(conf, key, default, path):
    """A small integer setting, checked: a bad tile colour is a build
    error, not a desktop that draws nothing."""
    raw = conf.get(key)
    if raw is None or raw == "":
        return default
    try:
        value = int(raw, 0)
    except ValueError:
        raise ModuleError("%s: %s must be a number, not %r" % (path, key, raw))
    if not 0 <= value <= 15:
        raise ModuleError("%s: %s must be 0..15, not %d" % (path, key, value))
    return value


def truthy(raw):
    return str(raw).strip().lower() in ("1", "yes", "true", "on")


class Module:
    def __init__(self, directory):
        self.dir = directory
        self.name = directory.name
        manifest = directory / MANIFEST
        if not manifest.is_file():
            raise ModuleError("%s: no %s" % (directory, MANIFEST))
        conf = parse_manifest(manifest)

        named = conf.get("name") or self.name
        if named != self.name:
            raise ModuleError("%s: NAME is %r but the directory is %r; the "
                              "two have to agree, since the directory is what "
                              "the build finds" % (manifest, named, self.name))

        sources = conf.get("sources") or ""
        self.sources = [directory / s for s in sources.split()]
        if not self.sources:
            raise ModuleError("%s: SOURCES is empty" % manifest)
        for source in self.sources:
            if not source.is_file():
                raise ModuleError("%s: no such source %s"
                                  % (manifest, source.name))

        self.gui = truthy(conf.get("gui", "0"))
        self.color = slot(conf, "color", DEFAULT_COLOR, manifest)
        self.glyph = slot(conf, "glyph", DEFAULT_GLYPH, manifest)
        self.about = conf.get("about", "")

        man = conf.get("man") or ""
        self.man = directory / man if man else None
        if self.man is not None and not self.man.is_file():
            raise ModuleError("%s: MAN names %s, which is not there"
                              % (manifest, man))
        if self.man is None and not self.gui:
            # A command nobody can read about is half a module, so say so
            # rather than let it reach the image unannounced.
            print("  %s: no MAN page, and not a GUI app, so `man %s` will "
                  "find nothing" % (self.name, self.name),
                  file=sys.stderr)

    def compile(self):
        """Build it with the OS toolchain, into sysroot/thams/."""
        BIN_DIR.mkdir(parents=True, exist_ok=True)
        out = BIN_DIR / self.name
        cmd = [str(VCC)] + [str(s) for s in self.sources] + ["-o", str(out)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            raise ModuleError("%s: vcc failed" % self.name)
        return out


def discover():
    """Every module under thams/, in a stable order."""
    if not THAM_DIR.is_dir():
        return []
    return [Module(d) for d in sorted(THAM_DIR.iterdir())
            if d.is_dir() and not d.name.startswith(".")]


def enabled():
    return not MARKER.exists()


def write_header(modules):
    """The one generated header: the blobs and the table that names them.

    Written even with no modules, with a sentinel entry and a count of
    zero: a zero-length array is not C++, and the kernel must build
    from a fresh checkout before any module exists.
    """
    out = []
    out.append("#pragma once\n")
    out.append("/*\n")
    out.append(" * embedded_thams.h — generated by tools/thams.py from thams/.\n")
    out.append(" * Do not edit, and do not commit: rebuild with\n")
    out.append(" * `make thamupdate` (or `make thambuild`).\n")
    out.append(" */\n")
    out.append("#include <stdint.h>\n")
    out.append('#include "../include/vnu/thams.h"\n\n')

    for module in modules:
        elf = (BIN_DIR / module.name).read_bytes()
        out.append("static const uint8_t tham_%s_elf[] = {\n" % module.name)
        out.extend(hexrows(elf))
        out.append("};\n")
        out.append("static const uint32_t tham_%s_elf_size = %d;\n\n"
                   % (module.name, len(elf)))
        if module.man is not None:
            page = module.man.read_bytes()
            out.append("static const uint8_t tham_%s_man[] = {\n" % module.name)
            out.extend(hexrows(page))
            out.append("};\n")
            out.append("static const uint32_t tham_%s_man_size = %d;\n\n"
                       % (module.name, len(page)))

    out.append("/* name, program, page, gui, tile colour, tile glyph. */\n")
    if not modules:
        out.append("static const vnu::thams::Module tham_modules[] = {\n")
        out.append("    {EmbeddedProg{nullptr, nullptr, 0}, nullptr, nullptr, "
                   "0, false, 0, 0},\n")
        out.append("};\n")
        out.append("static const int tham_module_count = 0;\n")
    else:
        out.append("static const vnu::thams::Module tham_modules[] = {\n")
        for module in modules:
            page = ("tham_%s_man, tham_%s_man_size" % (module.name, module.name)
                    if module.man is not None else "nullptr, 0")
            out.append(
                '    {EmbeddedProg{"/bin/%s", tham_%s_elf, tham_%s_elf_size}, '
                '"%s", %s, %s, %d, %d},\n'
                % (module.name, module.name, module.name, module.name,
                   page, "true" if module.gui else "false",
                   module.color, module.glyph))
        out.append("};\n")
        out.append("static const int tham_module_count = %d;\n" % len(modules))

    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    OUT_HEADER.write_text("".join(out))


def hexrows(data):
    rows = []
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        rows.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",\n")
    return rows


def cmd_status(_args):
    modules = discover()
    print("thams: %s" % ("on" if enabled() else "off (make thamon to build them)"))
    if not modules:
        print("  (no modules — drop one in thams/<name>/ with a tham.conf)")
        return 0
    for module in modules:
        print("  %-12s %-8s %s%s"
              % (module.name, "gui" if module.gui else "command",
                 module.about,
                 "" if module.man else "  [no page]"))
    return 0


def cmd_build(args):
    if not enabled():
        print("thams: off (make thamon to build them)")
        return 0
    modules = discover()
    if not modules:
        print("thams: no modules")
        return 0
    for module in modules:
        out = module.compile()
        print("  %-12s %6d bytes" % (module.name, out.stat().st_size))
    return 0


def cmd_generate(args):
    modules = discover() if enabled() else []
    if modules and not VCC.is_file():
        raise ModuleError("%s is missing — run `make doctor`" % VCC)
    for module in modules:
        module.compile()
    write_header(modules)
    state = "built %d module(s)" % len(modules) if modules else "no modules"
    print("thams: %s" % state)
    return 0


def cmd_on(_args):
    if MARKER.exists():
        MARKER.unlink()
        print("thams: on")
    else:
        print("thams: already on")
    return 0


def cmd_off(_args):
    if MARKER.exists():
        print("thams: already off")
    else:
        MARKER.write_text("Switched off by `make thamoff`. Delete this file, or "
                          "run `make thamon`, to build the modules again.\n")
        print("thams: off (make thamon to build them)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command")
    for name, fn, helptext in (("status", cmd_status, "list the modules"),
                               ("build", cmd_build, "compile them"),
                               ("generate", cmd_generate,
                                "compile and write embedded_thams.h"),
                               ("on", cmd_on, "build modules into the image"),
                               ("off", cmd_off, "leave them out")):
        sub.add_parser(name, help=helptext)
    args = parser.parse_args()
    if not args.command:
        args.command = "status"
    try:
        return globals()["cmd_" + args.command](args)
    except ModuleError as exc:
        print("thams: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())