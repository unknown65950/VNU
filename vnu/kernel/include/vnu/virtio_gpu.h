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
// them directly. Buffer size: vnu::vgfx::WIDTH * vnu::vgfx::HEIGHT * 4.

namespace vnu::virtio_gpu {

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

// The driver-owned 32-bpp scanout buffer (WIDTH*HEIGHT*4 bytes, format
// B8G8R8X8 — byte order B,G,R,X, so a pixel is 0x00RRGGBB). The
// desktop is 8-bpp, so vgfx expands its backbuffer into this buffer
// before each present().
uint8_t* framebuffer();

// Push framebuffer() to the host display (transfer to host + flush).
// No-op unless init() succeeded.
void present();

} // namespace vnu::virtio_gpu