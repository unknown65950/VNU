#!/bin/sh
# Bash trampoline: /usr/bin/env is missing on some hosts (e.g. Termux
# without termux-exec) -- if not under bash already, re-exec via PATH.
if [ -z "${BASH_VERSION:-}" ]; then exec env bash "$0" "$@"; fi
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VCC="$ROOT/tools/vcc"
OUT="$ROOT/vnu/kernel/proc"
mkdir -p "$OUT" "$ROOT/sysroot/bin"

embed() {
  python3 - "$1" "$OUT/embedded_$2.h" "$2" <<'PY'
import sys
from pathlib import Path
data=Path(sys.argv[1]).read_bytes(); name=sys.argv[3]; out=Path(sys.argv[2])
sym=f"embedded_{name}_elf"
with out.open("w") as f:
    f.write("#pragma once\n#include <stdint.h>\n")
    f.write(f"static const uint8_t {sym}[] = {{\n")
    for i in range(0,len(data),12):
        c=data[i:i+12]
        f.write("    "+", ".join(f"0x{b:02x}" for b in c)+",\n")
    f.write("};\n")
    f.write(f"static const uint32_t {sym}_size = {len(data)};\n")
print("embedded", name, len(data))
PY
}

echo "==> userspace"
"$VCC" "$ROOT/vnu/userspace/vash/vash.c" -o "$ROOT/sysroot/bin/vash"
"$VCC" "$ROOT/vnu/userspace/editors/vedit.c" -o "$ROOT/sysroot/bin/vedit"
"$VCC" "$ROOT/vnu/userspace/gui/calc.c" -o "$ROOT/sysroot/bin/calc"
"$VCC" "$ROOT/vnu/userspace/gui/files.c" -o "$ROOT/sysroot/bin/files"
"$VCC" "$ROOT/vnu/userspace/gui/prefs.c" -o "$ROOT/sysroot/bin/prefs"
"$VCC" "$ROOT/vnu/userspace/gui/picview.c" -o "$ROOT/sysroot/bin/picview"
"$VCC" "$ROOT/vnu/userspace/gui/clock.c" -o "$ROOT/sysroot/bin/clock"
"$VCC" "$ROOT/vnu/userspace/examples/hello/hello.c" -o "$ROOT/sysroot/bin/hello"
"$VCC" "$ROOT/vnu/userspace/examples/ttytest.c" -o "$ROOT/sysroot/bin/ttytest"
"$VCC" "$ROOT/vnu/userspace/man/man.c" -o "$ROOT/sysroot/bin/man"
# coreutils: one separate binary per command (no multi-call applet).
for c in echo true false pwd cat ls mkdir rm touch uname clear \
         wc head tail grep sort cp mv basename dirname seq df ping; do
  "$VCC" "$ROOT/vnu/userspace/vibecoreutils/$c.c" -o "$ROOT/sysroot/bin/$c"
done
# account/install tools: one separate binary per command.
for c in id whoami groups useradd passwd su install; do
  "$VCC" "$ROOT/vnu/userspace/usertools/$c.c" -o "$ROOT/sysroot/bin/$c"
done

embed "$ROOT/sysroot/bin/vash" vash
embed "$ROOT/sysroot/bin/vedit" vedit
embed "$ROOT/sysroot/bin/calc" calc
embed "$ROOT/sysroot/bin/files" files
embed "$ROOT/sysroot/bin/prefs" prefs
embed "$ROOT/sysroot/bin/picview" picview
embed "$ROOT/sysroot/bin/clock" clock
embed "$ROOT/vnu/userspace/gui/pics/flower.bmp" pic_flower
embed "$ROOT/vnu/userspace/gui/pics/sunset.png" pic_sunset
embed "$ROOT/vnu/userspace/gui/pics/logo.jpg" pic_logo
embed "$ROOT/vnu/userspace/gui/pics/gray_alpha.png" pic_gray_alpha
embed "$ROOT/vnu/userspace/gui/pics/shapes_pal.png" pic_shapes_pal
embed "$ROOT/vnu/userspace/gui/pics/shapes_rgba.png" pic_shapes_rgba
embed "$ROOT/vnu/userspace/gui/pics/wallpaper.png" wallpaper
embed "$ROOT/sysroot/bin/hello" hello
embed "$ROOT/sysroot/bin/ttytest" ttytest
embed "$ROOT/sysroot/bin/man" man
for c in echo true false pwd cat ls mkdir rm touch uname clear \
         wc head tail grep sort cp mv basename dirname seq df ping; do
  embed "$ROOT/sysroot/bin/$c" "$c"
done
for c in id whoami groups useradd passwd su install; do
  embed "$ROOT/sysroot/bin/$c" "$c"
done
echo "==> done"
