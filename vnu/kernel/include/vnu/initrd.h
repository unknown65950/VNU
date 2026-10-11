#pragma once
#include <stdint.h>

// The initial ramdisk.
//
// The boot loader loads an uncompressed POSIX ustar archive (initrd.tar)
// as a Multiboot2 module; the kernel unpacks it into /initrd of the RAM
// filesystem before userspace starts. It is data the image can carry
// without rebuilding the kernel: drop a file in vnu/initrd/, run make,
// and it is at /initrd/<name> in the guest. The archive is read with
// vnu/ustar.h and never written back - the unpacked /initrd tree is an
// ordinary part of the VFS afterwards.
namespace vnu::initrd {

// Find the archive among the boot modules and unpack it into /initrd.
// Safe to call when no initrd was loaded: it then does nothing. Called
// once from kernel_main() after vfs::init().
void unpack();

// The archive the boot loader handed over, or nullptr when there is
// none. Used by the disk installer to copy the same initrd onto an
// installed disk. Valid after unpack().
const void* image();
uint32_t image_size();

bool present();
uint32_t files();   // regular files unpacked
uint32_t skipped(); // entries dropped (unsafe or too long for the VFS)

} // namespace vnu::initrd
