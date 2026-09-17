#include <vnu/tty.h>

namespace {

constexpr uint16_t W = 80, H = 25;
volatile uint16_t* const vga = reinterpret_cast<volatile uint16_t*>(0xB8000);
uint16_t row = 0, col = 0;
uint8_t attr = 0x0A;
bool cursor_dirty = false;

void outb(uint16_t port, uint8_t val)
{
    asm volatile("outb %0,%1" : : "a"(val), "Nd"(port));
}

void hw_cursor_now()
{
    uint16_t pos = static_cast<uint16_t>(row * W + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, static_cast<uint8_t>(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, static_cast<uint8_t>((pos >> 8) & 0xFF));
    cursor_dirty = false;
}

/* Fast scroll: copy row blocks as 32-bit words instead of cell-by-cell nested loops. */
void scroll_up()
{
    auto* dst = reinterpret_cast<volatile uint32_t*>(vga);
    auto* src = reinterpret_cast<volatile uint32_t*>(vga + W);
    /* (H-1) rows * W cells * 2 bytes / 4 = (H-1)*W/2 dwords */
    constexpr size_t dwords = (static_cast<size_t>(H - 1) * W) / 2;
    for (size_t i = 0; i < dwords; ++i)
        dst[i] = src[i];
    auto* last = reinterpret_cast<volatile uint32_t*>(vga + (H - 1) * W);
    const uint32_t blank = (0x07200720u); /* two ' ' with attr 0x07 */
    for (size_t i = 0; i < W / 2; ++i)
        last[i] = blank;
    if (row > 0)
        --row;
}

} // namespace

namespace vnu::tty {

void init()
{
    attr = 0x0A;
    row = col = 0;
    cursor_dirty = true;
    flush_cursor();
}

void clear()
{
    const uint32_t blank = 0x07200720u;
    auto* p = reinterpret_cast<volatile uint32_t*>(vga);
    for (size_t i = 0; i < (static_cast<size_t>(W * H) / 2); ++i)
        p[i] = blank;
    row = col = 0;
    hw_cursor_now();
}

void flush_cursor()
{
    if (cursor_dirty)
        hw_cursor_now();
}

void putc(char c)
{
    if (c == '\n') {
        col = 0;
        if (++row >= H) {
            scroll_up();
            row = H - 1;
        }
        cursor_dirty = true;
        return;
    }
    if (c == '\r') {
        col = 0;
        cursor_dirty = true;
        return;
    }
    if (c == '\b') {
        if (col > 0) {
            --col;
            vga[row * W + col] = static_cast<uint16_t>(' ') | (static_cast<uint16_t>(attr) << 8);
        } else if (row > 0) {
            --row;
            col = W - 1;
            vga[row * W + col] = static_cast<uint16_t>(' ') | (static_cast<uint16_t>(attr) << 8);
        }
        cursor_dirty = true;
        return;
    }
    if (c == '\t') {
        uint16_t n = static_cast<uint16_t>(4 - (col % 4));
        while (n--)
            putc(' ');
        return;
    }
    vga[row * W + col] = static_cast<uint16_t>(static_cast<uint8_t>(c)) |
                          (static_cast<uint16_t>(attr) << 8);
    if (++col >= W) {
        col = 0;
        if (++row >= H) {
            scroll_up();
            row = H - 1;
        }
    }
    cursor_dirty = true;
}

void write(const char* s, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        putc(s[i]);
    flush_cursor(); /* one CRTC update per write(2) batch */
}

void write_cstr(const char* s)
{
    if (!s)
        return;
    while (*s)
        putc(*s++);
    flush_cursor();
}

void set_cursor(uint16_t r, uint16_t c)
{
    row = r < H ? r : static_cast<uint16_t>(H - 1);
    col = c < W ? c : static_cast<uint16_t>(W - 1);
    cursor_dirty = true;
    flush_cursor();
}

void get_cursor(uint16_t* r, uint16_t* c)
{
    if (r)
        *r = row;
    if (c)
        *c = col;
}

} // namespace vnu::tty
