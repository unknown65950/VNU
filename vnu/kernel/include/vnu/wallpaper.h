#pragma once
#include <stdint.h>

/* Desktop wallpaper support (kernel-side).
 *
 * A wallpaper is a file, and it is one of two things. A *picture* (a
 * BMP, PNG or JPEG px.h reads) is decoded, quantized to the picture half
 * of the display palette and scaled across the whole desktop; a *flat
 * colour* is a seven-byte marker file (see is_solid) whose colour is put
 * in one palette slot and painted straight into the desktop. On desktop
 * startup the GUI calls load(), which reads /etc/vnu/wallpaper and does
 * whichever of the two it finds; draw_wallpaper() then draws the loaded
 * wallpaper instead of the procedural scene.
 *
 * The shipped default is a flat grey, so a fresh boot paints the desktop
 * with nothing decoded and no frame held - which is the whole reason the
 * flat form exists.
 *
 * The wallpaper in use is a plain file, so it can be replaced from
 * userspace — that is what `wallpaper` and the prefs picker do — and
 * apply() is the syscall behind both: decode the candidate first, and
 * only then make it the file on disk and the frame on screen.
 *
 * A picture's buffers - the desktop frame, the quantized source and
 * px.h's decode scratch - are taken from the PMM pool and sized for the
 * mode on screen, not for the top of the ladder, and unload() gives them
 * back when the desktop quits. A flat colour holds no frame at all.
 * current_name() survives the release, so /proc/gfx can still report
 * which file is the wallpaper. */

/* A flat-colour wallpaper is exactly SOLID_SIZE bytes: the four magic
 * bytes of SOLID_MAGIC, then one 8-bit red, green and blue value. It is
 * drawn directly by draw_wallpaper(), so it costs no decode, no frame
 * and no memory - which is why the shipped desktop is one, and why any
 * program may write one to set a flat background of its own.
 *
 * SOLID_DEFAULT_RGB is the shipped grey, #383838: an exact 6-bit DAC
 * value (56 = 14<<2), so it looks identical on an 8bpp display and on a
 * 32bpp one. */
constexpr uint32_t SOLID_SIZE = 7;
constexpr uint8_t SOLID_MAGIC[4] = {'V', 'N', 'U', 'W'};
constexpr uint8_t SOLID_DEFAULT_RGB = 0x38;

/* True when `b`'s first `n` bytes are a flat-colour header. The three
 * colour bytes follow the magic, at offsets 4, 5 and 6 (red, green,
 * blue). */
inline bool is_solid(const uint8_t* b, uint32_t n)
{
    return n == SOLID_SIZE && b[0] == SOLID_MAGIC[0] &&
           b[1] == SOLID_MAGIC[1] && b[2] == SOLID_MAGIC[2] &&
           b[3] == SOLID_MAGIC[3];
}

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
 * screen. A flat-colour marker file (is_solid) is accepted too, and
 * installs the colour with no decode. Returns 0, or one of -VNU_ENOENT
 * (no such file), -VNU_EINVAL (unreadable image, too big for the mode
 * ladder, or a decode failure) and -VNU_EIO (accepted, but the file
 * could not be rewritten).
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
 * instead. A flat-colour wallpaper holds no frame, so it is never
 * ready(); draw_wallpaper() checks solid() first. */
bool ready();

/* True when the wallpaper in use is a flat colour rather than a picture:
 * there is no frame, and the desktop is filled with the single palette
 * slot solid_color() names. */
bool solid();

/* The picture-palette slot a flat-colour wallpaper was installed in.
 * Valid only when solid(); draw_wallpaper() fills the whole clip with
 * it. */
uint8_t solid_color();

/* Bare name of the image in use ("wallpaper", "sunset.png", ...), or an
 * empty string when no picture has ever been loaded. Reported by
 * /proc/gfx, so `wallpaper` can show what the desktop is drawing. */
const char* current_name();

} // namespace vnu::wallpaper
