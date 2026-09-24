// kernel.cpp — точка входа ядра VNU (Vibe's Not UNIX!).
//
// Это самый минимум: убедиться, что GRUB действительно передал
// управление нашему коду, и вывести приветствие через VGA text mode
// (адрес 0xB8000), не полагаясь ни на какую libc — её здесь нет.

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
#include <vnu/virtio_gpu.h>
extern "C" void vnu_console_start();
extern "C" void vnu_debug_putc(char c);

namespace
{
    // Весь вывод на экран идёт ТОЛЬКО через vnu::tty:: — у него один
    // курсор (программный + аппаратный) и нормальный скролл. Раньше
    // здесь был отдельный vga_print() со своими vga_row/vga_col, который
    // писал в тот же 0xB8000 независимо от tty:: и не двигал аппаратный
    // курсор — из-за этого вывод боевого vash и вывод из kernel_main
    // затирали друг друга (два курсора в одном буфере). Не возвращать.
    void vga_print(const char* str, std::uint8_t /*color*/ = 0)
    {
        vnu::tty::write_cstr(str);
    }

    // Пишем что-то и в serial-порт COM1 (0x3F8) — так лог видно
    // через `qemu -serial stdio` даже без графики, что удобно для CI.
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
        outb(0x3F8 + 1, 0x00); // отключить прерывания
        outb(0x3F8 + 3, 0x80); // включить DLAB
        outb(0x3F8 + 0, 0x03); // делитель = 3 -> 38400 baud
        outb(0x3F8 + 1, 0x00);
        outb(0x3F8 + 3, 0x03); // 8 бит, без чётности, 1 стоп-бит
        outb(0x3F8 + 2, 0xC7); // включить FIFO
        outb(0x3F8 + 4, 0x0B); // IRQs включены, RTS/DSR set
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
    // Порядок критичен: GDT -> IDT -> PIC remap должны быть готовы
    // ДО того, как выполнится хоть один байт кода, который может
    // упасть (proc::init(), vfs::init() и т.д.), и уж точно до
    // первого sti (он прячется в proc/switch.s при возврате из
    // пользовательского процесса). Иначе исключение/IRQ обслуживать
    // некому — либо triple fault, либо коллизия IRQ0/IRQ1 с
    // векторами CPU-исключений 0x08/0x09 (PIC ещё не remap-нут).
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
    vnu::pipe::init();
    vnu::apps::install_demo_apps();
    vnu::apps::install_demo_pics();
    vnu::ata::init();
    serial_init();
    vnu::tty::init();
    vnu::tty::clear();
    vnu::net::init(); /* QEMU's default e1000 NIC — backs the `ping` command */
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
