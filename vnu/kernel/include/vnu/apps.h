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

// Launcher read window for /apps/<name>/bin. Big enough for the largest
// embedded binary we ship (vash, ~113 KiB of ELF). The VFS is growable
// beyond any per-file cap, so this is purely the launcher's read window;
// binaries at or above it are refused (read_bin returns 0) rather than
// silently truncated. It is the cap both launchers agree on, not a
// buffer size: the classic one-way handoff keeps a fixed static window
// (apps.cpp g_load_buf), while the GUI's windowed-task path allocates
// its read window transiently from the PMM pool, sized to the file, with
// this constant as the upper bound (kernel globals in .bss shrink the
// PMM pool, see mm/pmm.cpp — so a megabyte-scale load buffer must not
// live in the kernel image).
constexpr uint32_t LOAD_BUF_SIZE = 262144;

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
    ICON_MUSIC,      // play — quarter note
    ICON_NOTE,       // sticky — a note with its corner turned up
    ICON_CLIPBOARD,  // clipboard — a board with a clip and text lines
    ICON_EYES,       // vibeeyes — a pair of eyes
};

struct AppEntry {
    char name[NAME_CAP];
    uint8_t icon_color;
    uint8_t icon_glyph;
};

void install_demo_apps();

// Mounts the demo photograph pack under /etc/vnu/pics (one VFS node per
// image, sourced from the embedded bytes) so picview has content to
// show, and seeds the desktop wallpaper from the embedded image: the
// shipped original and the copy in use (see include/vnu/media.h).
void install_demo_pics();

// Mounts the demo WAV clips under /etc/vnu/sounds (one VFS node per
// file, sourced from the embedded bytes) so the play app has content to
// play.
void install_demo_sounds();

// Mounts the manual pages under /etc/vnu/manpages (one VFS node per
// command, sourced from the embedded page files) so `man <name>` can
// read /etc/vnu/manpages/<name>; see vnu/userspace/man/man.c and
// tools/manpages.py.
void install_manpages();

// Scans /apps/ for subdirectories and fills out[] (up to max). Returns
// the number of apps found.
int list(AppEntry* out, int max);

// Reads /apps/<name>/bin into buf (capacity cap) without launching it.
// Used by the GUI's windowed-task path, which needs the raw bytes to
// hand to vnu::wintask::spawn() instead of the classic one-way
// process handoff. Returns the number of bytes read, or 0 on failure.
uint32_t read_bin(const char* name, uint8_t* buf, uint32_t cap);

// Exact size of /apps/<name>/bin (0 when the app has no binary), so a
// caller can size its read window to the file instead of guessing.
uint32_t bin_size(const char* name);

// Reads /apps/<name>/bin into memory and hands off to it. Same
// non-returning-on-success semantics as vnu::proc::run_program().
int launch(const char* name);

} // namespace vnu::apps
