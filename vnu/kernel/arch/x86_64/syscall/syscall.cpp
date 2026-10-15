#include <vnu/vfs.h>
#include <vnu/process.h>
#include <vnu/pipe.h>
#include <vnu/apps.h>
#include <vnu/pmm.h>
#include <vnu/paging.h>
#include <vnu/ata.h>
#include <vnu/host.h>
#include <vnu/mboot.h>
#include <vnu/net.h>
#include <vnu/tcp.h>
#include <vnu/images.h>
#include <vnu/initrd.h>
#include <vnu/vga_gfx.h>
#include <vnu/virtio_gpu.h>
#include <vnu/clipboard.h>
#include <vnu/gui.h>
#include <vnu/wintask.h>
#include <vnu/install.h>
#include <vnu/audio.h>
#include <vnu/wallpaper.h>
#include <vnu/arch/context.h>
#include <vnu/arch/cpu.h>
#include <vnu/arch/serial.h>

using vnu::arch::TrapFrame;

extern "C" void vnu_return_to_console();

static std::uint64_t dispatch_syscall(TrapFrame* tf)
{
    (void)tf;
    return -1ULL;
}

extern "C" std::uint64_t vnu_syscall_dispatch(TrapFrame* tf)
{
    return dispatch_syscall(tf);
}
