#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/kernel/build"
SOURCE_DIR="$ROOT_DIR/kernel"

# CMake stores absolute source/build paths in CMakeCache.txt. A build
# directory copied from another machine/archive must be reconfigured.
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_home="$(sed -n 's|^CMAKE_HOME_DIRECTORY:INTERNAL=||p' "$BUILD_DIR/CMakeCache.txt" | head -n1 || true)"
    if [[ -n "$cached_home" && "$cached_home" != "$SOURCE_DIR" ]]; then
        echo "CMake cache belongs to: $cached_home"
        echo "Current source tree is: $SOURCE_DIR"
        echo "Removing stale build cache..."
        rm -rf "$BUILD_DIR"
    fi
fi

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 1)"

mkdir -p "$ROOT_DIR/iso/boot/grub"
cp "$BUILD_DIR/kernel.elf" "$ROOT_DIR/iso/boot/kernel.elf"

cat > "$ROOT_DIR/iso/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0
menuentry "VNU" {
    multiboot2 /boot/kernel.elf
    module2 /boot/kernel.elf
    boot
}
EOF

grub-mkrescue -o "$ROOT_DIR/vnu.iso" "$ROOT_DIR/iso"
echo "Готово: $ROOT_DIR/vnu.iso"
