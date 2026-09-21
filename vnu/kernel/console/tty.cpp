#include <vnu/tty.h>

namespace {

constexpr uint16_t W = 80, H = 25;
volatile uint16_t* const vga = reinterpret_cast<volatile uint16_t*>(0xB8000);
uint16_t row = 0, col = 0;
uint8_t attr = 0x07; /* VNU default: white on black */
bool cursor_dirty = false;

/* --- ANSI CSI state ---
 * The installer draws its own blue setup screens, so the console must
 * understand a small ANSI subset: SGR colors/bold/blink, cursor
 * positioning (H), clear screen (2J) and erase-line (2K/0K). Anything
 * else is ignored so random binary in device output can't wedge the
 * terminal. */
bool esc_pending = false;
bool in_csi = false;
uint8_t csi_params[8];
int csi_count = 0;
int csi_cur = 0;

uint8_t sgr_fg = 7; /* VNU default: white on black */
uint8_t sgr_bg = 0;
bool sgr_bright = false;
bool sgr_blink = false;

/* ANSI color numbers (30-37) don't line up with the PC text palette
 * (0 black,1 blue,2 green,3 cyan,4 red,5 magenta,6 brown,7 white), so
 * translate: YELLOW->brown, BLUE->blue, RED->red, CYAN->cyan. */
uint8_t ansi_to_pc(uint8_t an)
{
    static const uint8_t t[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return t[an & 7];
}

void csi_apply_sgr(const uint8_t* p, int n)
{
    for (int i = 0; i < n; ++i) {
        switch (p[i]) {
        case 0:
            sgr_fg = 7;
            sgr_bg = 0;
            sgr_bright = false;
            sgr_blink = false;
            break;
        case 1:
            sgr_bright = true;
            break;
        case 5:
            sgr_blink = true;
            break;
        case 7:
            /* reverse video swaps foreground/background */
            {
                uint8_t t = sgr_fg;
                sgr_fg = sgr_bg;
                sgr_bg = t;
            }
            break;
        case 30 ... 37:
            sgr_bright = false;
            sgr_fg = ansi_to_pc(static_cast<uint8_t>(p[i] - 30));
            break;
        case 39:
            sgr_fg = 7;
            sgr_bright = false;
            break;
        case 40 ... 47:
            sgr_bg = ansi_to_pc(static_cast<uint8_t>(p[i] - 40));
            break;
        case 49:
            sgr_bg = 0;
            break;
        case 90 ... 97:
            sgr_bright = true;
            sgr_fg = ansi_to_pc(static_cast<uint8_t>(p[i] - 90));
            break;
        default:
            break;
        }
    }
    attr = static_cast<uint8_t>((sgr_fg + (sgr_bright ? 8 : 0)) |
                                (sgr_bg << 4) | (sgr_blink ? 0x80 : 0));
}

void erase_to_line_end()
{
    uint16_t start = col;
    for (; col < W; ++col)
        vga[row * W + col] = static_cast<uint16_t>(' ') |
                             (static_cast<uint16_t>(attr) << 8);
    col = start;
    cursor_dirty = true;
}

void csi_finish(uint8_t final)
{
    if (csi_count < 8)
        csi_params[csi_count++] = static_cast<uint8_t>(csi_cur);
    csi_cur = 0;
    in_csi = false;
    switch (final) {
    case 'm':
        csi_apply_sgr(csi_params, csi_count);
        break;
    case 'H':
        /* CUP: default "1;1" homes; explicit row;col positions (1-based). */
        row = static_cast<uint16_t>(csi_params[0] ? csi_params[0] - 1 : 0);
        col = csi_count >= 2 ? static_cast<uint16_t>(csi_params[1] ? csi_params[1] - 1 : 0)
                             : 0;
        row = row < H ? row : static_cast<uint16_t>(H - 1);
        col = col < W ? col : static_cast<uint16_t>(W - 1);
        break;
    case 'J':
        if (csi_count == 0 || csi_params[0] == 2 || csi_params[0] == 3) {
const uint32_t cell = 0x20u | (static_cast<uint32_t>(attr) << 8);
            const uint32_t blank = cell | (cell << 16);
            auto* p = reinterpret_cast<volatile uint32_t*>(vga);
            for (size_t i = 0; i < (static_cast<size_t>(W * H) / 2); ++i)
                p[i] = blank;
            row = col = 0;
        } else if (csi_params[0] == 1) {
            for (uint16_t r = 0; r <= row; ++r)
                for (uint16_t c = 0; c < W; ++c)
                    vga[r * W + c] = static_cast<uint16_t>(' ') |
                                     (static_cast<uint16_t>(attr) << 8);
        } else {
            erase_to_line_end();
            for (uint16_t r = row + 1; r < H; ++r)
                for (uint16_t c = 0; c < W; ++c)
                    vga[r * W + c] = static_cast<uint16_t>(' ') |
                                     (static_cast<uint16_t>(attr) << 8);
        }
        cursor_dirty = true;
        break;
    case 'K':
        erase_to_line_end();
        break;
    default:
        break; /* unsupported CSI: consumed and dropped */
    }
}

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
    const uint32_t cell = 0x20u | (static_cast<uint32_t>(attr) << 8);
    const uint32_t blank = cell | (cell << 16);
    for (size_t i = 0; i < W / 2; ++i)
        last[i] = blank;
    if (row > 0)
        --row;
}

} // namespace

namespace vnu::tty {

void init()
{
    attr = 0x07;
    row = col = 0;
    cursor_dirty = true;
    flush_cursor();
}

void clear()
{
    const uint32_t cell = 0x20u | (static_cast<uint32_t>(attr) << 8);
    const uint32_t blank = cell | (cell << 16);
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
    if (esc_pending) {
        esc_pending = false;
        if (c == '[') {
            in_csi = true;
            csi_count = 0;
            csi_cur = 0;
        }
        cursor_dirty = true;
        return;
    }
    if (in_csi) {
        if (c >= '0' && c <= '9') {
            csi_cur = csi_cur * 10 + (c - '0');
            if (csi_cur > 255)
                csi_cur = 255;
        } else if (c == ';') {
            if (csi_count < 8)
                csi_params[csi_count++] = static_cast<uint8_t>(csi_cur);
            csi_cur = 0;
        } else if (c == 0x1B) {
            /* nested ESC: restart */
            esc_pending = true;
            in_csi = false;
        } else if (c >= 0x40 && c <= 0x7E) {
            csi_finish(static_cast<uint8_t>(c));
        } else {
            /* unknown control sequence: consume it and drop */
            in_csi = false;
        }
        cursor_dirty = true;
        return;
    }
    if (c == 0x1B) {
        esc_pending = true;
        cursor_dirty = true;
        return;
    }
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
