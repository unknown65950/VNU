#pragma once
#include <vnu/abi.h>
#include <stdint.h>

// /dev/fb0 - the display's own pixels, for a program that is not a
// windowed app.
//
// Every other way of putting pixels on this machine's screen belongs to
// VibeGraphics: a window, a canvas the compositor blits, a 16-colour
// palette. That is the right shape for a desktop, and the wrong one for
// the thing this device exists for - a program that wants to know what
// the hardware does with a picture it decoded itself, with no window
// manager, no palette and no compositor between the pixels and the
// screen. So this is deliberately not a vgfx surface: it is the frame
// the display is being scanned out of, at the display's own depth,
// mapped straight into the caller's address space.
//
// What a program does with it:
//
//     fd = open("/dev/fb0", O_RDWR);          // maps the frame
//     read(fd, &info, sizeof(info));          // where and how big
//     pixels = (uint8_t*)info.va;             // draw into this
//     write(fd, 0, 0);                        // put it on the screen
//
// The mapping is at a fixed address (VNU_FB_VA in vnu/abi.h) rather than
// a pointer the kernel picks, for the reason every other fixed address
// in this system is fixed: the program is compiled ahead of the run and
// the address has to be in its object code. It is above the 32 MiB
// identity map on purpose - that region is kernel image, .bss and the
// frame pool, all of which the kernel reaches by identity, and a private
// mapping over any of it would be a page of somebody else's memory.
//
// Three things follow from the frame belonging to the display and not to
// the caller, and they are the whole contract:
//
//   - The mapping is *borrowed* (paging::map_borrowed_frames). A process
//     that exits, forks or execs takes the mapping with it and the frames
//     stay with the display; unmap/destroy never hand the framebuffer
//     back to the pool.
//   - A mode change replaces the frame. The mapping stays mapped and
//     stays writable, but what is behind it is now a different run, so
//     the pixels a program just drew went into memory the pool has since
//     handed to somebody else. That is why every read re-checks the
//     mapping against the frame in use and re-maps it: a program that
//     polls its geometry the way it should cannot be left drawing into
//     freed memory. A program that draws for a long time without a single
//     read can be, and the way out is the one every framebuffer has - do
//     not change the mode under a program that is drawing.
//   - The display has to be in a graphics mode for any of this to reach
//     the screen, and opening the device enters one if the card is still
//     in the text mode the console lives in (see ensure_frame()). From
//     the open to the close of the last descriptor the display belongs
//     to that program: the console's own scheduler pass draws nothing
//     while a descriptor is open, because a console is pixels like
//     anything else, and a pass that repaints it over a picture destroys
//     the picture - on a display that has a text mode it draws the
//     characters straight over the frame, and on one that has none it
//     takes the whole scanout back at the console's geometry. Closing
//     the last descriptor is what gives the console back.

namespace vnu::fb {

// Put the display into a graphics mode if it is not in one already, and
// leave the frame it takes mapped to nothing in particular. This is what
// an open does first: a frame in a text-mode card's memory is a picture
// nobody can see, since presenting it goes to an address the card is not
// reading.
//
// The mode is the one the last session chose (/etc/vnuconfig/gfx.conf,
// the same preference the desktop starts from, and a config that cannot
// be read leaves the driver's own default). A card that will not take a
// mode, or a pool with nothing left in it, answers false and the open is
// refused: a picture that is never shown is not worth mapping.
//
// True also says the display is the caller's own: it stays that way
// until release(), and nothing else draws on it in between.
bool ensure_frame();

// True when the display is in a graphics mode and holds a frame, so that
// what follows has something to answer with. Does not enter one: this is
// the question, not the action.
bool available();

// Give the display back, when this is what put it into a graphics mode:
// the text mode the console was in, its characters readable on the card
// again. Called when the last descriptor on the device is closed, and a
// no-op when a desktop is presenting the screen (see release()'s own
// comments for both halves of that).
void release();

// Map the frame into `pgdir_phys` (the address space the caller is
// running in) if it is not already the one mapped there. Returns false
// when there is no frame, when it does not fit VNU_FB_VA's window, or
// when the pool of page tables is out.
//
// Called on open and again on every read and write, because a fork or an
// execve builds a fresh address space and a mapping does not follow it
// there. Re-mapping is idempotent.
bool map_for(uint32_t pgdir_phys);

// Fill `out` with the geometry and the address, mapping first so the
// va it reports is one the caller can actually touch. False when there
// is no frame or it could not be mapped.
bool info(uint32_t pgdir_phys, vnu_fb_info* out);

// Put the frame on the screen: everything the program drew into the
// mapping counts as damage, and the display is handed the damage. This is
// what write(fd, 0, 0) means.
void present();

// Copy `count` bytes into the frame at `offset` bytes from its start,
// clipping to the frame and to the last row the copy reaches, and put
// the rows it touched on the screen. What a write(fd, buf, n) with data
// means. `offset` past the end of the frame copies nothing and fails,
// which is what makes a stream that runs off the end an error instead of
// a write into whatever is next.
int write_pixels(uint32_t offset, const void* buf, uint32_t count);

} // namespace vnu::fb