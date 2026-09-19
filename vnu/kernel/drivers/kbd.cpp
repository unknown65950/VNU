#include <vnu/kbd.h>

namespace {

uint8_t inb(uint16_t p)
{
    uint8_t x;
    asm volatile("inb %1,%0" : "=a"(x) : "Nd"(p));
    return x;
}

constexpr char K_ESC = 27;
constexpr char K_LEFT = (char)0x81;
constexpr char K_RIGHT = (char)0x82;
constexpr char K_UP = (char)0x83;
constexpr char K_DOWN = (char)0x84;
constexpr char K_HOME = (char)0x85;
constexpr char K_END = (char)0x86;
constexpr char K_DEL = (char)0x87;
constexpr char K_INTR = 3; /* Ctrl+C */

/* Unshifted US QWERTY (set 1). */
const char map[0x58] = {
    /*00*/ 0, K_ESC,
    /*02*/ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    /*0E*/ '\b', '\t',
    /*10*/ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    /*1C*/ '\n',
    /*1D*/ 0, /* LCtrl */
    /*1E*/ 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'',
    /*29*/ '`',
    /*2A*/ 0, /* LShift */
    /*2B*/ '\\',
    /*2C*/ 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    /*36*/ 0, /* RShift */
    /*37*/ 0, 0,
    /*39*/ ' ',
};

/* Shifted digits and symbols (same indices as map). */
const char map_shift[0x58] = {
    /*00*/ 0, K_ESC,
    /*02*/ '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    /*0E*/ '\b', '\t',
    /*10*/ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    /*1C*/ '\n',
    /*1D*/ 0,
    /*1E*/ 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"',
    /*29*/ '~',
    /*2A*/ 0,
    /*2B*/ '|',
    /*2C*/ 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    /*36*/ 0,
    /*37*/ 0, 0,
    /*39*/ ' ',
};

int is_shift_sc(uint8_t c) { return c == 0x2A || c == 0x36; }
int is_ctrl_sc(uint8_t c) { return c == 0x1D; }

struct DecoderState {
    int shift = 0;
    int ctrl = 0;
    int ext = 0;
};

/* Feeds one raw scancode byte through the shift/ctrl/extended-key
 * state machine. Returns -1 if this byte didn't complete a character
 * (a modifier press/release, the 0xE0 prefix, or a key release) —
 * the caller should keep reading more scancodes in that case — or the
 * decoded character (0-255) once one is ready. Shared by both the
 * blocking (getch_blocking) and non-blocking (poll_char) readers so
 * the decoding logic — and its quirks — stay in exactly one place. */
int decode_scancode(uint8_t s, DecoderState& st)
{
    if (s == 0xE0) {
        st.ext = 1;
        return -1;
    }

    /* Key release */
    if (s & 0x80) {
        uint8_t c = static_cast<uint8_t>(s & 0x7F);
        if (is_shift_sc(c))
            st.shift = 0;
        if (is_ctrl_sc(c))
            st.ctrl = 0;
        st.ext = 0;
        return -1;
    }

    if (is_shift_sc(s)) {
        st.shift = 1;
        return -1;
    }
    if (is_ctrl_sc(s) && !st.ext) {
        st.ctrl = 1;
        return -1;
    }

    if (st.ext) {
        st.ext = 0;
        /* The K_* pseudo-codes are >= 0x80, i.e. negative when stored
         * in a (signed) char. decode_scancode reports "no character
         * yet" as -1, so these must be widened through uint8_t —
         * returning them directly makes every arrow/Home/End/Delete
         * key look like "nothing decoded" and get silently dropped. */
        switch (s) {
        case 0x4B: return static_cast<uint8_t>(K_LEFT);
        case 0x4D: return static_cast<uint8_t>(K_RIGHT);
        case 0x48: return static_cast<uint8_t>(K_UP);
        case 0x50: return static_cast<uint8_t>(K_DOWN);
        case 0x47: return static_cast<uint8_t>(K_HOME);
        case 0x4F: return static_cast<uint8_t>(K_END);
        case 0x53: return static_cast<uint8_t>(K_DEL);
        default: return -1;
        }
    }

    if (s >= sizeof(map))
        return -1;

    if (st.ctrl) {
        char base = map[s];
        if (base >= 'a' && base <= 'z')
            return base - 'a' + 1;
        if (base >= 'A' && base <= 'Z')
            return base - 'A' + 1;
        return -1;
    }

    char ch = st.shift ? map_shift[s] : map[s];
    return ch ? static_cast<int>(static_cast<uint8_t>(ch)) : -1;
}

} // namespace

namespace vnu::kbd {

char getch_blocking()
{
    static DecoderState st;
    for (;;) {
        if (!(inb(0x64) & 1)) {
            asm volatile("pause");
            continue;
        }
        uint8_t s = inb(0x60);
        int ch = decode_scancode(s, st);
        if (ch >= 0)
            return static_cast<char>(ch);
    }
}

int poll_char()
{
    /* Separate decoder state from getch_blocking()'s — safe because
     * the two are never used in the same session (the GUI owns the
     * keyboard exclusively via poll_char() while it's running; every
     * other context uses the blocking reader). */
    static DecoderState st;
    if (!scancode_ready())
        return -1;
    uint8_t s = read_raw_scancode();
    return decode_scancode(s, st);
}

bool scancode_ready()
{
    /* Status bit0 = output buffer full, bit5 = byte came from the aux
     * (mouse) device rather than the keyboard. Must check both, or a
     * pending mouse packet byte gets stolen and dropped here before
     * vnu::mouse::poll() ever sees it. */
    uint8_t status = inb(0x64);
    return (status & 0x21) == 0x01;
}

void drain_excess()
{
    while (scancode_ready())
        (void)inb(0x60);
}

uint8_t read_raw_scancode()
{
    return inb(0x60);
}

} // namespace vnu::kbd
