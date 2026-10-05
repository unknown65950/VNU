#include <vnu/fb.h>
#include <vnu/gfxconf.h>
#include <vnu/paging.h>
#include <vnu/tty.h>
#include <vnu/posix.h>
#include <vnu/vga_gfx.h>

extern "C" void* memcpy(void* dst, const void* src, unsigned long count);

namespace vnu::fb {

namespace {

/* The frame as the driver holds it, re-read every time: everything here
 * can change under a program between two syscalls (a mode change from
 * the desktop's own F12, a resolution picked in prefs, a desktop
 * starting or stopping), and none of it can be cached across the call
 * that has to act on it. */
struct Frame {
    uint32_t phys;
    uint32_t pages;
    int width;
    int height;
    int bpp;
};

Frame current()
{
    Frame f{};
    f.phys = vnu::vgfx::backbuffer_phys();
    f.pages = vnu::vgfx::backbuffer_pages();
    f.width = vnu::vgfx::width();
    f.height = vnu::vgfx::height();
    f.bpp = static_cast<int>(vnu::vgfx::bpp());
    /* A frame and a geometry that disagree is a frame mid-change: the
     * mode was programmed between the two reads above. There is nothing
     * coherent to hand out, and the next call will find one. */
    if (!f.phys || f.pages == 0 || f.width <= 0 || f.height <= 0 || f.bpp <= 0)
        return Frame{};
    const uint32_t need = static_cast<uint32_t>(f.pages);
    if (need > VNU_FB_MAX_PAGES)
        return Frame{};
    return f;
}

uint32_t stride_of(const Frame& f)
{
    return static_cast<uint32_t>(f.width) * static_cast<uint32_t>(f.bpp / 8);
}

} // namespace

bool available()
{
    /* The frame alone is not enough: at the console there is one - a
     * 720x400 stand-in the console draws text into, standing in for a
     * text mode the card can show by itself - and it is not what the card
     * is scanning out. A device that handed that out as the display's
     * frame would take a program's picture and show nothing. */
    return vnu::vgfx::in_gfx_mode() && current().phys != 0;
}

/* Whether entering the mode was this device's doing. A frame the desktop
 * took is the desktop's to give back, and a display nobody asked to
 * change is not ours to put back either. */
bool g_took_mode = false;

bool ensure_frame()
{
    if (available()) {
        /* Someone is already presenting the display - a desktop, or
         * another program - and the descriptor that is being opened is
         * one more thing painting the same frame. */
        vnu::vgfx::program_presenting_set(true);
        return true;
    }
    /* The mode the last session chose, before the card is asked for
     * anything: from text mode there are no registers to program, which
     * is why the desktop loads it and only then calls enter_gfx_mode().
     * A config that will not load leaves the driver's own mode, which is
     * what a machine with no config file has anyway. */
    (void)vnu::gfxconf::load();
    if (!vnu::vgfx::enter_gfx_mode())
        return false;
    g_took_mode = true;
    if (!available()) {
        /* A mode without a frame in it is a mode nobody can draw into, so
         * the mode is given back rather than left taken: an open that is
         * refused must not leave the machine in a state the refused open
         * is what put it in. */
        release();
        return false;
    }
    /* The display is now this program's to paint, and the console's
     * scheduler pass has to keep off it until the descriptor is closed:
     * the console is pixels like anything else, and a pass that redraws
     * a keystroke's row over a picture destroys the picture. */
    vnu::vgfx::program_presenting_set(true);
    return true;
}

void release()
{
    /* The last descriptor on the device is gone, so nobody is painting
     * the frame and the console may have it back - whether or not the
     * mode below is ours to give back. A frame a desktop was already
     * presenting stays with the desktop, which says so itself. */
    vnu::vgfx::program_presenting_set(false);
    if (!g_took_mode)
        return;   /* the mode was there before we were asked for it */
    g_took_mode = false;
    /* A desktop that started while this program was drawing owns the
     * display now, and putting it back to text mode would take the
     * screen away from it. */
    if (vnu::vgfx::desktop_presenting())
        return;
    /* The console has been sitting in the tty's cells the whole time,
     * unreadable, because the card was reading a framebuffer: giving the
     * mode back is what puts those characters on the screen again. The
     * alternative - leaving a graphical console with a 720x400 console
     * drawn in the middle of a 1024x768 card - is a machine nobody asked
     * for, and the picture is gone either way by the time anyone types. */
    vnu::vgfx::exit_to_text();
}

bool map_for(uint32_t pgdir_phys)
{
    const Frame f = current();
    if (!f.phys || !pgdir_phys)
        return false;
    /* Already this frame? Then the mapping is still the right one and
     * rewriting it would only cost a page-table walk. Both ends have to
     * agree: the pool hands a replaced frame back at the same address
     * often enough, and the first page alone cannot tell a run that grew
     * from one that did not. A run whose first and last page are the
     * ones mapped is the run that is mapped, since it is contiguous. */
    const uint32_t last = VNU_FB_VA + (f.pages - 1) * 4096u;
    if (vnu::paging::phys_frame_at(pgdir_phys, VNU_FB_VA) == f.phys &&
        vnu::paging::phys_frame_at(pgdir_phys, last) == f.phys + (f.pages - 1) * 4096u)
        return true;
    return vnu::paging::map_borrowed_frames(pgdir_phys, VNU_FB_VA, f.pages,
                                            f.phys);
}

bool info(uint32_t pgdir_phys, vnu_fb_info* out)
{
    if (!out)
        return false;
    const Frame f = current();
    if (!f.phys)
        return false;
    if (!map_for(pgdir_phys))
        return false;
    out->va = VNU_FB_VA;
    out->width = static_cast<uint32_t>(f.width);
    out->height = static_cast<uint32_t>(f.height);
    out->bpp = static_cast<uint32_t>(f.bpp);
    out->stride = stride_of(f);
    out->size = stride_of(f) * static_cast<uint32_t>(f.height);
    return true;
}

void present()
{
    /* Nothing on the screen to hand over: present() moves the damage to
     * the card, and with no frame held there is no card to move it to.
     * Guarded rather than called blind, because a program that presents
     * after the desktop has quit is asking for a display that is gone. */
    if (!current().phys)
        return;
    vnu::vgfx::damage_all();
    vnu::vgfx::present();
}

int write_pixels(uint32_t offset, const void* buf, uint32_t count)
{
    const Frame f = current();
    if (!buf)
        return -VNU_EIO;
    /* Same refusal as open: there is no frame to write into, and saying
     * EIO would send a program looking at the disk. */
    if (!f.phys)
        return -VNU_ENODEV;
    const uint32_t stride = stride_of(f);
    const uint32_t size = stride * static_cast<uint32_t>(f.height);
    /* Offset past the end is an error, not a silent nothing: a caller
     * that computed a position from a geometry that has since changed is
     * better off hearing about it than drawing somewhere else. */
    if (offset >= size)
        return -VNU_EINVAL;
    if (count > size - offset)
        count = size - offset;

    /* The kernel writes through the pool's identity mapping, which is
     * the same bytes the program's own mapping shows it. */
    auto* dst = reinterpret_cast<uint8_t*>(f.phys);

    /* Row by row, so the damage is the rows that really changed: a copy
     * that stops in the middle of the last row damages that row, not the
     * screen below it. */
    uint32_t done = 0;
    int first_row = -1;
    int last_row = -1;
    while (done < count) {
        const uint32_t at = offset + done;
        const uint32_t row = at / stride;
        const uint32_t col = at % stride;
        const uint32_t run = (count - done < stride - col) ? count - done
                                                            : stride - col;
        memcpy(dst + at, static_cast<const uint8_t*>(buf) + done, run);
        if (first_row < 0)
            first_row = static_cast<int>(row);
        last_row = static_cast<int>(row);
        done += run;
    }
    if (first_row >= 0) {
        /* Damage what was written, and only if something is there to show
         * it: with no frame the copy above went nowhere. */
        vnu::vgfx::damage_add(0, first_row, f.width, last_row - first_row + 1);
        vnu::vgfx::present();
    }
    return static_cast<int>(count);
}

} // namespace vnu::fb