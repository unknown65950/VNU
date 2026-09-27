#pragma once
#include <stdint.h>

/* Desktop wallpaper support (kernel-side).
 *
 * On desktop startup the GUI calls load(), which reads the
 * /etc/vnu/wallpaper VFS node (a BMP/PNG, decoded with the
 * freestanding px.h) and renders it stretched across the whole
 * 1024x768 desktop, quantizing each pixel down to the 16-color
 * Catppuccin DAC palette the GUI displays. draw_wallpaper() then blits
 * that ready-made frame instead of the procedural scene whenever one
 * was loaded.
 *
 * The wallpaper in use is a plain file, so it can be replaced from
 * userspace — that is what `wallpaper` and the prefs picker do — and
 * apply() is the syscall behind both: decode the candidate first, and
 * only then make it the file on disk and the frame on screen. */

namespace vnu::wallpaper {

/* Decode /etc/vnu/wallpaper and build the full-desktop palette frame.
 * Returns true (and flips ready()) only when a format was probed,
 * decoded and quantized end to end. Safe to call more than once: a
 * failed or missing file just leaves the previous frame (or the
 * procedural fallback) in place. */
bool load();

/* Decode the image at `path` and, only if that works, store it as the
 * wallpaper in use (overwriting /etc/vnu/wallpaper) and put it on
 * screen. Returns 0, or one of -VNU_ENOENT (no such file), -VNU_EINVAL
 * (JPEG, too big, over 512x384, or a decode failure) and -VNU_EIO
 * (decoded, but the file could not be rewritten).
 *
 * Unlike load() this is *not* a no-op on failure: a rejected candidate
 * leaves both the current frame and the current file untouched.
 *
 * The decode runs to completion inside the call, so the desktop stands
 * still for as long as it takes (tens of milliseconds for a 512x384
 * PNG); the redraw that follows picks the new frame up immediately. */
int apply(const char* path);

/* Ready-made 1024x768 palette-index frame of the wallpaper. Valid only
 * when ready(). */
const uint8_t* frame();

bool ready();

/* Bare name of the image in use ("wallpaper", "sunset.png", ...), or an
 * empty string when no picture has ever been loaded. Reported by
 * /proc/gfx, so `wallpaper` can show what the desktop is drawing. */
const char* current_name();

} // namespace vnu::wallpaper
