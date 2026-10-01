#include <vnu/ps2mouse.h>

namespace {

inline void outb(uint16_t port, uint8_t val)
{
    asm volatile("outb %0,%1" : : "a"(val), "Nd"(port));
}

inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

constexpr uint16_t PORT_DATA = 0x60;
constexpr uint16_t PORT_STATUS = 0x64;
constexpr uint16_t PORT_CMD = 0x64;

void wait_input_clear()
{
    /* Bit1 of status: 1 = controller input buffer still full. */
    for (int i = 0; i < 100000; ++i) {
        if (!(inb(PORT_STATUS) & 0x02))
            return;
    }
}

void wait_output_full()
{
    /* Bit0 of status: 1 = output buffer has data for us. */
    for (int i = 0; i < 100000; ++i) {
        if (inb(PORT_STATUS) & 0x01)
            return;
    }
}

void send_to_aux(uint8_t byte)
{
    wait_input_clear();
    outb(PORT_CMD, 0xD4); /* next byte goes to the aux (mouse) device */
    wait_input_clear();
    outb(PORT_DATA, byte);
}

uint8_t read_ack()
{
    wait_output_full();
    return inb(PORT_DATA);
}

/* Empty the controller's output buffer, and only mouse bytes: the
 * keyboard's own reader checks the aux bit before it takes anything (a
 * pending mouse packet must not be read out from under poll()), which
 * is right while the desktop is up and polling this device and is
 * exactly what strands the keyboard when nobody is polling it any
 * more. */
void drain_pending()
{
    for (int i = 0; i < 32; ++i) {
        if ((inb(PORT_STATUS) & 0x21) != 0x21)
            break;
        (void)inb(PORT_DATA);
    }
}

int8_t g_packet[4];
int g_packet_idx = 0;
bool g_wheel = false;

} // namespace

namespace vnu::mouse {

void init()
{
    /* Whatever the event loop that has just ended left in the output
     * buffer goes first: the release of the click that closed it, or a
     * move that arrived after it. Every handshake below reads exactly
     * one byte and takes it for an ack, so a stale packet byte here
     * shifts the whole conversation - including the identify reply
     * that decides whether this is a 3-byte or a 4-byte packet stream,
     * and a wrong answer there garbles every packet that follows: a
     * data byte read as a flags byte leaves the button looking stuck
     * down, and no click the user makes ever registers again. */
    drain_pending();

    wait_input_clear();
    outb(PORT_CMD, 0xA8); /* enable auxiliary device */

    wait_input_clear();
    outb(PORT_CMD, 0x20); /* "get compaq status byte" */
    uint8_t status = read_ack();
    status |= 0x02;  /* enable IRQ12 line (harmless even though we poll) */
    status &= ~0x20; /* make sure aux clock isn't disabled */

    wait_input_clear();
    outb(PORT_CMD, 0x60); /* "set compaq status byte" */
    wait_input_clear();
    outb(PORT_DATA, status);

    send_to_aux(0xF6); /* set defaults */
    (void)read_ack();

    /* Try to promote the device to IntelliMouse wheel mode using the
     * standard sample-rate handshake (200/100/80). Devices that support
     * it report ID 3 on the subsequent Identify and then stream 4-byte
     * packets with a Z delta in the last byte; everything else keeps
     * sending 3-byte packets and we just never read a fourth byte. */
    send_to_aux(0xF3);
    (void)read_ack();
    send_to_aux(200);
    (void)read_ack();
    send_to_aux(0xF3);
    (void)read_ack();
    send_to_aux(100);
    (void)read_ack();
    send_to_aux(0xF3);
    (void)read_ack();
    send_to_aux(80);
    (void)read_ack();
    send_to_aux(0xF2); /* identify */
    g_wheel = read_ack() == 3;

    send_to_aux(0xF4); /* enable data reporting (stream mode) */
    (void)read_ack();

    /* One more drain, now that reporting is on: the packets that came
     * in between the drain at the top and here are the tail of the
     * event loop before, and the stream the caller is about to read has
     * to start on a packet boundary, not in the middle of one. */
    drain_pending();

    g_packet_idx = 0;
}

void shutdown()
{
    /* Reporting off first, so no packet arrives to be stranded in the
     * output buffer after the last one has been read. */
    send_to_aux(0xF5);
    (void)read_ack();

    /* Then whatever the last move already left there. */
    drain_pending();
    g_packet_idx = 0;
}

bool poll(int& dx, int& dy, uint8_t& buttons, int8_t& wheel)
{
    bool got_full = false;
    wheel = 0;

    /* Drain whatever bytes are currently sitting in the aux output
     * buffer; assemble at most one full packet per call so the caller
     * gets smooth per-frame deltas rather than a burst all at once. */
    const int plen = g_wheel ? 4 : 3;
    while (!got_full && (inb(PORT_STATUS) & 0x21) == 0x21) {
        uint8_t byte = inb(PORT_DATA);

        if (g_packet_idx == 0 && !(byte & 0x08)) {
            /* Not the first byte of a packet (sync bit not set) — the
             * stream is misaligned (e.g. we started mid-packet); drop
             * it and keep looking for a valid first byte. */
            continue;
        }

        g_packet[g_packet_idx++] = static_cast<int8_t>(byte);
        if (g_packet_idx < plen)
            continue;

        g_packet_idx = 0;
        uint8_t flags = static_cast<uint8_t>(g_packet[0]);
        int raw_dx = g_packet[1];
        int raw_dy = g_packet[2];

        if (flags & 0x40 || flags & 0x80) {
            /* X or Y overflow — discard this packet, garbage delta. */
            continue;
        }

        dx = raw_dx;
        dy = -raw_dy; /* PS/2 Y grows upward; screen Y grows downward */
        buttons = static_cast<uint8_t>(flags & 0x07);
        if (g_wheel)
            wheel = g_packet[3]; /* IntelliMouse Z delta, signed */
        got_full = true;
    }

    return got_full;
}

} // namespace vnu::mouse
