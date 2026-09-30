#pragma once
#include <stdint.h>

// virtio-gpu driver (QEMU: `-device virtio-gpu-pci`, or the
// VGA-compatible `virtio-vga` which can drive the sole display).
//
// The device carries no scanout framebuffer of its own the way VBE
// does: a "resource" is a block of the guest's own memory (attached as
// backing pages) which the host copies into its display on demand. That
// is also why this display is 32-bpp: with a resource to write into
// there is no DAC in the way, so the desktop composites in the scanout's
// own format (B8G8R8X8) and present() copies the runs out to the host
// with a full-frame TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. An 8-bpp
// backbuffer - a VBE mode kept over from an earlier session - is still
// expanded through its palette on the way, as it always was.
//
// The scanout buffer and every virtqueue live in physical frames from
// vnu::pmm, which paging keeps identity-mapped, so the device can DMA
// them directly. The scanout is a *list* of frame runs, not one run:
// see scanout_segment(). The pool also holds the VFS nodes and every
// window's surfaces and is filled from the bottom up, so a mode change
// releases the resource in use before it takes the frames for the new
// one and never needs both sizes at once - which is what makes
// 1280x1024 fit in that pool at all.

namespace vnu::virtio_gpu {

// One run of frames behind the scanout resource. The pool is
// identity-mapped, so phys is also a pointer the guest can write
// through. The runs together hold width*height*4 bytes in order: byte n
// of the image is byte n of the list.
struct ScanoutSegment {
    uint32_t phys;
    uint32_t bytes;
};

// Probe PCI for a virtio-gpu family device (vendor 0x1AF4) and bring
// it up: negotiate VIRTIO_F_VERSION_1, wire the control queue, create
// a 2D scanout resource the size of the desktop and attach the
// framebuffer as its backing store. Returns false (silently) when no
// such device exists, so a VBE-only boot is completely unaffected.
// Called once from kernel_main, after paging/pmm are up.
bool init();

// True once the virtio-gpu display is live; vga_gfx::present() routes
// the desktop through this driver while it holds.
bool active();

// The backing runs of the scanout in the mode in use, format
// B8G8R8X8 (byte order B,G,R,X, so a pixel is 0x00RRGGBB). A 32-bpp
// backbuffer is written in this format already, so present() copies it
// in; an 8-bpp one is expanded on the way. Zero segments means a mode
// change is between resources: callers must treat that as "nothing to
// draw into yet".
uint32_t scanout_segments();
ScanoutSegment scanout_segment(uint32_t index);

// Push what vgfx expanded into the segments to the host display
// (transfer to host + flush), for the x, y, w, h rectangle of the
// resource that changed. A rectangle rather than the whole frame because
// the transfers are what a 32bpp frame is expensive for - 3 MiB to the
// host every pass at 1024x768 - and the device has a command for
// exactly this: a dirty rectangle, which lets the host re-upload only
// that area. The rectangle is clipped to the resource; a 0-width or
// 0-height one means the frame changed nothing and nothing is sent.
// No-op unless init() succeeded and there are segments.
void present(int x, int y, int w, int h);

// Who the host is showing. A virtio-vga is a VGA-compatible display
// with two planes, and the host renders whichever one is pointed at it:
// the legacy VGA text mode, or a virtio-gpu scanout resource. Pointing
// at the resource is a command (SET_SCANOUT); unpointing is the same
// command with resource_id 0, and it does not hand the display back to
// the text plane - the host simply freezes the last frame it was given
// and nothing transferred afterwards shows up (QEMU 11.1). So the
// resource is built and backed at init() but not shown, leaving a
// machine at a text prompt on its console, and a desktop session takes
// the display over for good: the console a session ends in is drawn
// into the scanout (see vgfx::present_text) rather than handed back.
// No-op when no device is live.
void show_scanout();

// Move the display to w x h: the scanout resource in use is released,
// its frames go back to the pool and a resource of the new size is built
// over fresh ones, which the host is then pointed at. The desktop has
// already redrawn at the new size by the time the next present() runs.
// Returns false when no device is live, when the mode is already in use,
// or when the pool has no room or the host refused a step — in which case
// the display keeps the host's last copy of the old frame and a later
// attempt can still make the change.
bool set_resolution(int w, int h);

} // namespace vnu::virtio_gpu
