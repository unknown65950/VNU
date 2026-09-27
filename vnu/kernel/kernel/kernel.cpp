// kernel.cpp — the entry point of the VNU kernel (Vibe's Not UNIX!).
//
// It is the bare minimum: make sure GRUB really handed control to our
// code, and print a greeting through the VGA text mode (0xB8000)
// without relying on any libc - there is none here.

#include <cstddef>
#include <cstdint>
#include <vnu/vfs.h>
#include <vnu/process.h>
#include <vnu/syscall.h>
#include <vnu/tty.h>
#include <vnu/pipe.h>
#include <vnu/apps.h>
#include <vnu/pmm.h>
#include <vnu/paging.h>
#include <vnu/ata.h>
#include <vnu/mboot.h>
#include <vnu/net.h>
#include <vnu/tcp.h>
#include <vnu/images.h>
#include <vnu/vga_gfx.h>
#include <vnu/virtio_gpu.h>
#include <vnu/audio.h>
extern "C" void vnu_console_start();
extern "C" void vnu_debug_putc(char c);

namespace
{
    // All screen output goes through vnu::tty:: and nothing else - it
    // has a single cursor (software and hardware) and does scrolling
    // properly. There used to be a separate vga_print() with its own
    // vga_row/vga_col writing into the same 0xB8000 independently of
    // tty:: and without moving the hardware cursor, so the real vash
    // output and the kernel_main output overwrote each other (two
    // cursors in one buffer). Do not bring it back.
    void vga_print(const char* str, std::uint8_t /*color*/ = 0)
    {
        vnu::tty::write_cstr(str);
    }

    // Output also goes to the COM1 serial port (0x3F8), so the log is
    // visible through `qemu -serial stdio` even without graphics, which
    // is convenient for CI.
    void outb(std::uint16_t port, std::uint8_t value)
    {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
    }

    std::uint8_t inb(std::uint16_t port)
    {
        std::uint8_t value;
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
        return value;
    }

    void serial_init()
    {
        outb(0x3F8 + 1, 0x00); // disable interrupts
        outb(0x3F8 + 3, 0x80); // enable DLAB
        outb(0x3F8 + 0, 0x03); // divisor = 3 -> 38400 baud
        outb(0x3F8 + 1, 0x00);
        outb(0x3F8 + 3, 0x03); // 8 bits, no parity, 1 stop bit
        outb(0x3F8 + 2, 0xC7); // enable the FIFO
        outb(0x3F8 + 4, 0x0B); // IRQs enabled, RTS/DSR set
    }

    bool serial_transmit_empty()
    {
        return (inb(0x3F8 + 5) & 0x20) != 0;
    }


}


extern "C" void vnu_gdt_init();
extern "C" void vnu_disable_paging();
extern "C" void vnu_idt_init();
extern "C" void vnu_pic_remap();
extern "C" void vnu_console_start();

extern "C" void kernel_main(std::uint32_t magic, std::uint32_t info_addr)
{
    // Remember the Multiboot2 info pointer so the disk installer can
    // find the running kernel image among the loaded modules and copy
    // it onto the target volume.
    if (magic == 0x36d76289)
        vnu::mboot::set_info(info_addr);
    // The order is critical: GDT -> IDT -> PIC remap must be in place
    // BEFORE a single byte of code runs that can fault (proc::init(),
    // vfs::init() and so on), and certainly before the first sti (it
    // hides in proc/switch.s on the return from a userspace process).
    // Otherwise nobody is there to service a fault or an IRQ - either a
    // triple fault, or IRQ0/IRQ1 colliding with the CPU exception
    // vectors 0x08/0x09 (the PIC is not remapped yet).
    // vnu_disable_paging() first, defensively: makes sure we start
    // from a known state (PE on, PG off) regardless of what the
    // bootloader left behind, before vnu::paging::init() builds real
    // page tables and turns PG back on for good. Without this, a
    // future bootloader change that happened to leave stray page
    // tables mapped could make paging::init()'s CR3 load do something
    // unexpected before our own tables are actually in place.
    vnu_disable_paging();
    vnu_gdt_init();
    vnu_idt_init();
    vnu_pic_remap();
    vnu::pmm::init();
    vnu::paging::init();
    vnu::vfs::init();
    vnu::proc::init();
    /* Glyph tables are part of the image from the start, whether or not
     * the desktop ever captures the VGA font. */
    vnu::images::add(vnu::images::Fonts, vnu::vgfx::font_bytes());
    vnu::pipe::init();
    vnu::apps::install_demo_apps();
    vnu::apps::install_demo_pics();
    vnu::apps::install_demo_sounds();
    vnu::ata::init();
    serial_init();
    vnu::tty::init();
    vnu::tty::clear();
    vnu::net::init(); /* QEMU's default e1000 NIC — backs the `ping` command */
    vnu::audio::init(); /* QEMU's AC'97 sound card (-soundhw ac97); runs a
                           short playback self-test, no-op without the card */
    vnu::virtio_gpu::init(); /* QEMU's virtio-gpu/virtio-vga display, if any;
                                silent no-op otherwise (desktop stays on VBE) */
    if (magic != 0x36d76289) {
        vga_print("invalid multiboot2 magic\n", 4);
        for (;;)
            asm volatile("hlt");
    }
    vga_print("VNU: starting init (/sbin/init)\n");

    /* Network self-test: verify ARP + ICMP ping to the gateway. */
    {
        auto putdec = [](unsigned long v) {
            if (v == 0) { vnu_debug_putc('0'); return; }
            char buf[12]; int n = 0;
            while (v) { buf[n++] = static_cast<char>('0' + v % 10); v /= 10; }
            while (n) vnu_debug_putc(buf[--n]);
        };
        auto putstr = [](const char* s) { for (; *s; ++s) vnu_debug_putc(*s); };
        long rtt = vnu::net::ping(vnu::net::GW_IP, 4000);
        putstr("NET SELFTEST ping 10.0.2.2 > ");
        if (rtt >= 0) { putdec(rtt); putstr(" ms\n"); }
        else { putstr("FAIL rc="); putdec(-rtt); putstr("\n"); }
    }
    /* TCP echo self-test: open a client socket, connect to the
     * host-loopback echo service (10.0.2.2:7777) which slirp forwards
     * to 127.0.0.1:7777 on the host, send a marker and print however
     * many bytes the server bounces back. Runs at boot, no keyboard,
     * purely to validate the connect()/send()/recv() path end-to-end. */
    {
        auto putdec = [](unsigned long v) {
            if (v == 0) { vnu_debug_putc('0'); return; }
            char buf[12]; int n = 0;
            while (v) { buf[n++] = static_cast<char>('0' + v % 10); v /= 10; }
            while (n) vnu_debug_putc(buf[--n]);
        };
        auto putstr = [](const char* s) { for (; *s; ++s) vnu_debug_putc(*s); };
        int sock = vnu::tcp::socket_open(2, 1); /* AF_INET, SOCK_STREAM */
        if (sock < 0) {
            putstr("TCP SELFTEST socket: "); putdec(-sock); putstr("\n");
        } else {
            long rc = vnu::tcp::socket_connect(sock, vnu::net::GW_IP, 7777, 6000);
            if (rc < 0) {
                putstr("TCP SELFTEST connect: "); putdec(-rc); putstr("\n");
            } else {
                putstr("TCP SELFTEST connected\n");
                static const char marker[] = "VNU-TCP-EO";
                rc = vnu::tcp::socket_send(sock, marker,
                                            static_cast<long>(sizeof(marker) - 1), 3000);
                if (rc < 0) {
                    putstr("TCP SELFTEST send: "); putdec(-rc); putstr("\n");
                } else {
                    char in[128];
                    rc = vnu::tcp::socket_recv(sock, in, sizeof(in), 8000);
                    putstr("TCP SELFTEST recv: "); putdec(rc); putstr(" :");
                    if (rc > 0) {
                        for (long i = 0; i < rc; ++i) vnu_debug_putc(in[i]);
                    }
                    putstr("\n");
                }
            }
            vnu::tcp::socket_close(sock);
        }
    }

    /* Unified system: kernel + vlibc programs. Boot /sbin/init as a
     * scheduler-managed coroutine (PID 1); init spawns services and
     * the console shell, all running in parallel under the cooperative
     * round-robin scheduler in proc::run_scheduler(). Respawn init on
     * exit; if neither /sbin/init nor /bin/vash is embedded, fall back
     * to the native kernel console. */
    for (;;) {
        /* The VFS fd table is global to the kernel, so a redirection
         * left behind by the previous session (`cmd > file`) would
         * otherwise still be in effect for the new shell. */
        vnu::vfs::reset_stdio();
        int rc = vnu::proc::run_scheduler("/sbin/init", "/bin/vash");
        if (rc < 0) {
            vga_print("init failed — falling back to native console\n", 4);
            vnu_console_start();
            for (;;)
                asm volatile("hlt");
        }
        vga_print("[init exited — respawning init]\n");
    }
}
