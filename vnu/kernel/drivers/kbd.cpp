#include <vnu/kbd.h>

namespace {

extern "C" void vnu_debug_putc(char c); /* serial (COM1), like every driver */

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
        /* The keypad's own Enter is E0 1C (the main block's Enter is a
         * plain 1C and decodes below). Without this the shell never sees
         * a newline from a numeric-keypad Enter. */
        case 0x1C: return '\n';
        default: return -1;
        }
    }

    /* Function keys, set 1 scancodes 0x3B..0x44 with F11/F12 at
     * 0x57/0x58. They report the same code whatever the modifiers do,
     * like the arrows do. Checked before the table lookup because 0x58
     * is one past its end. */
    switch (s) {
    case 0x3B: return vnu::kbd::K_F1;
    case 0x3C: return vnu::kbd::K_F2;
    case 0x3D: return vnu::kbd::K_F3;
    case 0x3E: return vnu::kbd::K_F4;
    case 0x3F: return vnu::kbd::K_F5;
    case 0x40: return vnu::kbd::K_F6;
    case 0x41: return vnu::kbd::K_F7;
    case 0x42: return vnu::kbd::K_F8;
    case 0x43: return vnu::kbd::K_F9;
    case 0x44: return vnu::kbd::K_F10;
    case 0x57: return vnu::kbd::K_F11;
    case 0x58: return vnu::kbd::K_F12;
    default: break;
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

/* --- The scancode queue --------------------------------------------
 * The scancodes the controller has handed over, in arrival order, and
 * the state of the one decoder that turns them into characters.
 *
 * Reading port 0x60 is destructive, and this machine has more than one
 * place that wants a key: the shell's read is a spin over poll_char()
 * that yields to the scheduler between characters, and a desktop that
 * is up has its own event loop doing the same. Two readers on one
 * destructive port are a byte gone from both - and worse, a *stale*
 * one: each reader checks the status register and then reads the data
 * register, and a timer interrupt between those two instructions lets
 * the other reader take the byte, so the second read returns whatever
 * the output register still holds. A stream read that way loses makes
 * and duplicates breaks, and the text typed comes out with holes in it:
 * a `cat /proc/gfx` sent at the shell arrives as `cat /procgfx`, one
 * make code gone and its break code read twice.
 *
 * So the controller is drained into this queue, with interrupts off -
 * the status/data pair has to be read as one - and every reader takes
 * from the queue. Each byte is read once, goes to exactly one reader,
 * and the decoder state is the device's rather than a reader's: which
 * modifier is held and whether an 0xE0 prefix is outstanding is a fact
 * about the keyboard, so one state per reader would be two half-true
 * answers to the same question, and a key the desktop read between two
 * shell reads leaves a prefix pending that turns the shell's next
 * character into an arrow key.
 *
 * Overflow drops the newest byte: the alternative is a reader that
 * never catches up, and a lost keypress is a smaller harm than a
 * keyboard stuck in one state. */
constexpr int SCAN_QUEUE = 256;
uint8_t scan_queue[SCAN_QUEUE];
int scan_head = 0;
int scan_tail = 0;
DecoderState decoder;

bool queue_empty()
{
    return scan_head == scan_tail;
}

void note_overflow()
{
    /* The queue holds 255 scancodes: a power-on self-test, a mouse
     * handshake, or a keypress away faster than anyone can type. It has
     * not happened in a boot, and one line about it is all it deserves. */
    static bool said = false;
    if (said)
        return;
    said = true;
    for (const char* s = "kbd: scancode queue overflow\n"; *s; ++s)
        vnu_debug_putc(*s);
}

/* Move everything the controller has into the queue. The cli is the
 * whole point of this: it runs from the shell's read loop (which yields
 * between characters) and from the desktop's event loop, and a
 * preemption between the status read and the data read is exactly what
 * produced the stale bytes. The interrupt flag is put back as it was
 * found, so this is also correct in a caller that runs with interrupts
 * already off. cli and not a spinlock, because the drain is a handful
 * of inb instructions and there is nothing here to block on. */
void fill_queue()
{
    uint32_t flags;
    asm volatile("pushfl; popl %0" : "=r"(flags));
    asm volatile("cli" ::: "memory");
    for (;;) {
        /* Status bit0 = output buffer full, bit5 = the byte came from
         * the aux (mouse) device rather than the keyboard. Both are
         * checked, or a pending mouse packet byte is stolen and
         * dropped here before vnu::mouse::poll() ever sees it. */
        if ((inb(0x64) & 0x21) != 0x01)
            break;
        uint8_t s = inb(0x60);
        int next = (scan_tail + 1) % SCAN_QUEUE;
        if (next == scan_head) {
            note_overflow();
            continue;
        }
        scan_queue[scan_tail] = s;
        scan_tail = next;
    }
    if (flags & 0x200u)
        asm volatile("sti" ::: "memory");
}

} // namespace

namespace vnu::kbd {

char getch_blocking()
{
    for (;;) {
        fill_queue();
        if (!queue_empty()) {
            uint8_t s = scan_queue[scan_head];
            scan_head = (scan_head + 1) % SCAN_QUEUE;
            int ch = decode_scancode(s, decoder);
            if (ch >= 0)
                return static_cast<char>(ch);
            continue;   /* a modifier or a prefix, not a character */
        }
        /* Nothing queued and the controller has nothing either. This is
         * the fallback reader for contexts with nothing else to run
         * (the native console); the shell's read goes through
         * poll_char() so the machine can run other processes. */
        asm volatile("pause");
    }
}

int poll_char()
{
    /* One scancode out of the queue, decoded with the device's own
     * state. -1 means either "nothing to read" or "that byte was not a
     * character" (a modifier, a release, a prefix); both are the same
     * to a caller that polls in a loop, and both leave the queue one
     * byte shorter. */
    fill_queue();
    if (queue_empty())
        return -1;
    uint8_t s = scan_queue[scan_head];
    scan_head = (scan_head + 1) % SCAN_QUEUE;
    return decode_scancode(s, decoder);
}

void drain_excess()
{
    /* Whatever the controller has, plus whatever the queue took on its
     * way in, and the decoder state that goes with it: the desktop does
     * this before it re-initialises the aux device, so a key pressed
     * earlier cannot be read as a mouse acknowledgement. */
    fill_queue();
    scan_head = scan_tail;
    decoder = DecoderState{};
}

} // namespace vnu::kbd
