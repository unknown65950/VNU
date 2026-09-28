#pragma once
#include <stdint.h>

// virtio-gpu driver (QEMU: `-device virtio-gpu-pci`, or the
// VGA-compatible `virtio-vga` which can drive the sole display).
//
// The device carries no scanout framebuffer of its own the way VBE
// does: a "resource" is a block of the guest's own memory (attached as
// backing pages) which the host copies into its display on demand. The
// desktop therefore keeps rendering into the 8-bpp VGA backbuffer as
// always; vga_gfx::present() expands it to true-colour B8G8R8X8 into
// this driver's scanout buffer, then present() sends a full-frame
// TRANSFER_TO_HOST_2D + RESOURCE_FLUSH so the host shows it.
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
// B8G8R8X8 (byte order B,G,R,X, so a pixel is 0x00RRGGBB). The desktop
// is 8-bpp, so vgfx expands its backbuffer into them before each
// present(). Zero segments means a mode change is between resources:
// callers must treat that as "nothing to draw into yet".
uint32_t scanout_segments();
ScanoutSegment scanout_segment(uint32_t index);

// Push what vgfx expanded into the segments to the host display
// (transfer to host + flush). No-op unless init() succeeded and there
// are segments.
void present();

// Show a blank frame: zero the scanout and push it. Called when the
// desktop hands the display back (vgfx::exit_to_text), because this
// display has no text mode of its own and the host would otherwise keep
// showing the last desktop frame for good.
void blank();

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
