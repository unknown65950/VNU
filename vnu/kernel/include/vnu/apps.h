#pragma once
#include <stdint.h>

// The /apps/ launcher: each app is a real VFS directory
// /apps/<name>/ containing exactly two files:
//   icon  — 2 bytes: a VGA palette index used to tint the icon glyph,
//           then an IconGlyph id selecting which pictogram the desktop
//           draws inside the tile (see gui/gui.cpp draw_*_icon).
//   bin   — the raw ELF binary (small enough to fit vnu::vfs's
//           per-file capacity, see kernel/fs/vfs.cpp DATA_CAP)
//
// install_demo_apps() seeds a few of the kernel's already-embedded
// programs into /apps/ as a demonstration. A future toolchain could
// let users drop their own compiled binaries in there instead.

namespace vnu::apps {

constexpr int MAX_APPS = 16;
constexpr int NAME_CAP = 24;

// Per-app desktop pictogram, drawn kernel-side (gui/gui.cpp).
// ICON_LETTER falls back to the app name's first letter.
enum IconGlyph {
    ICON_LETTER = 0,
    ICON_SMILE,      // hello — happy face
    ICON_DOC,        // vedit — document with text lines
    ICON_TERM,       // term — terminal window with prompt
    ICON_KEYPAD,     // calc — display + key grid
    ICON_FOLDER,     // files — folder
    ICON_PICTURE,    // picview — framed sun + mountains
    ICON_SLIDERS,    // prefs — slider rows
    ICON_CLOCK,      // clock — analogue face
};

struct AppEntry {
    char name[NAME_CAP];
    uint8_t icon_color;
    uint8_t icon_glyph;
};

void install_demo_apps();

// Mounts the demo photograph pack under /pics (one VFS node per image,
// sourced from the embedded bytes) so picview has content to show.
void install_demo_pics();

// Scans /apps/ for subdirectories and fills out[] (up to max). Returns
// the number of apps found.
int list(AppEntry* out, int max);

// Reads /apps/<name>/bin into buf (capacity cap) without launching it.
// Used by the GUI's windowed-task path, which needs the raw bytes to
// hand to vnu::wintask::spawn() instead of the classic one-way
// process handoff. Returns the number of bytes read, or 0 on failure.
uint32_t read_bin(const char* name, uint8_t* buf, uint32_t cap);

// Reads /apps/<name>/bin into memory and hands off to it. Same
// non-returning-on-success semantics as vnu::proc::run_program().
int launch(const char* name);

} // namespace vnu::apps
