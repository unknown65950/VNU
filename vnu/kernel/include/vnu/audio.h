#pragma once
#include <stdint.h>

// AC'97 audio subsystem (Intel 82801AA / ICH, PCI 8086:2415 — the
// device QEMU provides with `-soundhw ac97`). Exposes the playback
// backend behind the audio_* syscalls (VNU_SYS_audio_open..56).
//
// The controller only plays 16-bit stereo at the codec's sample rate,
// so set_format() programmes the codec rate register and write_data()
// converts 8-bit/mono PCM to 16-bit stereo while feeding the DMA ring.
//
// There is no IRQ plumbing in the kernel: the driver is polled, and
// write_data() is deliberately NON-blocking (copies as much as fits,
// returns the byte count) so a windowed task can keep servicing GUI
// events while the DMA plays from the ring.
namespace vnu::audio {

// Probes the PCI bus for the AC'97 controller and prepares the DMA
// ring. Safe to call always; a silent no-op when no sound hardware is
// present (returns 0). Also runs a short boot self-test.
int init();

// Syscall backend (see vnu/abi/ABI.md for the full contract).
int open_device();
int set_format(uint32_t rate, uint32_t channels, uint32_t bits);
long write_data(const void* buf, uint32_t len);
int drain();
long pending();
int pause_device();
int reset_device();
int close_device();

} // namespace vnu::audio