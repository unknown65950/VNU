#pragma once
#include <stdint.h>

/* Desktop wallpaper support (kernel-side).
 *
 * On desktop startup the GUI calls load(), which reads the
 * /etc/vnu/wallpaper VFS node (a BMP/PNG, decoded with the
 * freestanding px.h) and renders it scaled across the whole
 * desktop, quantizing each pixel down to the 16-color
 * Catppuccin DAC palette the GUI displays. draw_wallpaper() then blits
 * that ready-made frame instead of the procedural scene whenever one
 * was loaded.
 *
 * The wallpaper in use is a plain file, so it can be replaced from
 * userspace — that is what `wallpaper` and the prefs picker do — and
 * apply() is the syscall behind both: decode the candidate first, and
 * only then make it the file on disk and the frame on screen.
 *
 * Its buffers - the desktop frame, the quantized source and px.h's
 * decode scratch - are taken from the PMM pool and sized for the mode
 * on screen, not for the top of the ladder, and unload() gives them
 * back when the desktop quits. current_name() survives that, so
 * /proc/gfx can still report which file is the wallpaper. */

namespace vnu::wallpaper {

/* Decode /etc/vnu/wallpaper and build the full-desktop palette frame.
 * Returns true (and flips ready()) only when a format was probed,
 * decoded and quantized end to end. Safe to call more than once: a
 * failed or missing file just leaves the previous frame (or the
 * procedural fallback) in place. */
bool load();

/* Hand the wallpaper's pixels back to the pool, at the end of a
 * desktop session. The next load() re-decodes the file; the name
 * current_name() reports is not affected. */
void unload();

/* Re-decode the current wallpaper for the mode now programmed, after
 * vgfx::set_resolution() changed it. A no-op when no picture is
 * loaded (the procedural scene scales itself); a re-decode that fails
 * leaves the procedural scene in place, so the desktop never blits a
 * frame stretched for the old size. */
void resize();

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
 * still for as long as it takes (about a second for a 512x384 PNG, most
 * of it the inflate); the redraw that follows picks the new frame up
 * immediately. resize() is far cheaper: it scales the image that has
 * already been decoded.
 *
 * Like the wallpaper, this lives in the RAM filesystem, so a choice
 * made through it is forgotten at reboot. */
int apply(const char* path);

/* Ready-made palette-index frame of the wallpaper, scaled to the mode
 * now programmed. Valid only when ready(). */
const uint8_t* frame();

/* True when there is a frame *for the mode now programmed*: the blit
 * copies a whole mode of pixels, so a frame left over from another
 * geometry counts as absent and the desktop draws its procedural scene
 * instead. */
bool ready();

/* Bare name of the image in use ("wallpaper", "sunset.png", ...), or an
 * empty string when no picture has ever been loaded. Reported by
 * /proc/gfx, so `wallpaper` can show what the desktop is drawing. */
const char* current_name();

} // namespace vnu::wallpaper
