#pragma once
#include <stdint.h>

// Lets MULTIPLE launched apps run *inside VibeGraphics windows*,
// concurrently with the GUI's own event loop and with each other.
//
// Each windowed task gets its own page directory (see
// vnu::paging::create_address_space): the shared low ~32 MiB identity
// map keeps the kernel/VFS/LFB visible to every task, while the app
// region (0x400000), a per-task stack (0x900000) and a per-task heap
// (0x700000-0x800000) are backed by freshly allocated, private physical
// frames — so every app can still be linked at the same fixed 0x400000
// without relocation. Control switches between the GUI and a task (and
// between tasks) cooperatively with vnu_swtch_pd, which swaps the
// page directory at the exact point the context switch happens.
//
// Switching is cooperative, at two points only — a blocking stdin read,
// or exit() — so there's exactly one task executing between those
// points, and no locks are needed to touch shared kernel state.

namespace vnu::wintask {

constexpr int CON_COLS = 80;
constexpr int CON_ROWS = 24;
constexpr int SCROLL_ROWS = 96; /* scrollback ring depth */
constexpr int CELL_W = 8;       /* text cell pitch in pixels */
constexpr int CELL_H = 16;      /* text cell height (8x16 VGA font) */
constexpr int INPUT_QUEUE_CAP = 64;
constexpr int TITLE_CAP = 24;
constexpr int MAX_TASKS = 6;
using TaskHandle = int;
constexpr TaskHandle NO_TASK = -1;

/* Graphics window size in pixels (the window's client area). A windowed
 * task can flip its console into a "gfx" surface by writing pixels to
 * fd 3 (see task_gfx_write): the GUI then renders this buffer 1:1
 * instead of the character grid, and mouse clicks over the client area
 * get delivered to the app as escape-sequence events on stdin. */
constexpr int GFX_W = 240;
constexpr int GFX_H = 170;

/* VNU mouse protocol (delivered over the stdin escape stream, one event
 * per message so a partially-queued press can't corrupt the next):
 *   ESC '[' 'M' <button> <x> <y>
 * where <button> is 1 (left pressed) / 2 (left released) and x/y are the
 * click position in client-area pixels. */
constexpr char MOUSE_ESC = 0x1B;
constexpr int MOUSE_MSG_LEN = 6;

struct Console {
    /* Text-mode screen: `cell` holds the visible CON_ROWS screen. When a
     * row scrolls off the top it is parked in the `hist` ring (a simple
     * scrollback), and `scroll_off` is how many rows above the bottom the
     * GUI is currently showing (0 = live bottom). */
    char cell[CON_ROWS][CON_COLS];
    char hist[SCROLL_ROWS][CON_COLS];
    int hist_tail;  /* next free slot in `hist` */
    int hist_n;     /* how many history rows are valid */
    int scroll_off; /* 0 = bottom (live); > 0 = scrolled up */
    int cur_col;
    int cur_row;
    char input[INPUT_QUEUE_CAP];
    int in_head;
    int in_tail;
    char title[TITLE_CAP];
    /* Gfx mode state: writing to fd 3 flips this task to a pixel
     * framebuffer window. `gfx_cursor` is the current fill offset for
     * subsequent fd 3 writes (lseek(3) positions it, like /dev/fb). */
    uint8_t pixel[GFX_W * GFX_H];
    uint32_t gfx_cursor;
    bool gfx;
};

// (Re)initialises the multi-slot task table. Must be called once from
// gui::run() after paging is up, before any spawn().
void init();

// Starts a new windowed slot running `data`/`size` as an ELF image
// already in memory (e.g. read out of a VFS file). Returns a task
// handle for future calls, or NO_TASK on failure (slot table full, no
// address space, bad ELF, ...). The image is copied into the task's
// own storage, so `data` no longer needs to stay valid after returning.
// `title` is copied into the console for the GUI to render.
TaskHandle spawn(const char* argv0_path, const uint8_t* data, uint32_t size, const char* title);

// Tears down a task that is parked (blocked/waiting): frees its page
// directory, root-image copy and slot. Must NOT be called for the task
// currently executing (the GUI never does — it only closes windows in
// its own event loop).
void close_task(TaskHandle h);

bool is_running(TaskHandle h);
bool has_window(TaskHandle h); // true while there's content worth showing
Console* console(TaskHandle h);

// Counts live tasks (any state != Unused), for the panel.
int live_count();

// Gives one task a time slice: switches into it (swapping page
// directories) and returns once it either blocks on empty input or
// exits. No-op for a task that isn't runnable.
void run_slice(TaskHandle h);

// Runs one slice of every live runnable task, low-to-high by handle.
void run_all_slices();

// Delivers one keystroke to a task's stdin queue.
void feed_input(TaskHandle h, char ch);

// Delivers a mouse event (button and pixel coordinates) to a task's
// stdin queue as a VNU mouse protocol message (MOUSE_MSG_LEN bytes).
// All six bytes are enqueued atomically: if the queue doesn't have room
// for the whole message, the event is silently dropped.
// Button values: 1 = left press, 2 = left released.
void feed_mouse(TaskHandle h, int button, int px, int py);

// --- Gfx surface for fd 3 write/lseek (pixels → pixel buffer) ---
// (operate on the currently executing task, see current_is_task())

uint32_t task_gfx_write(const char* buf, uint32_t n);
int task_gfx_seek(int32_t offset, int whence);

// Per-task heap break. The task's heap is pre-mapped (BRK_MIN..BRK_MAX),
// so this is just bookkeeping for vlibc's malloc(). Called from the
// SYS_brk handler while a windowed task is executing.
uint32_t task_brk(uint32_t req);

// --- Syscall-facing hooks (called only from syscall.cpp's write/read/
// exit/brk handling, guarded by current_is_task()) ---

bool current_is_task();
void task_write(const char* buf, uint32_t n);
char task_getch_blocking();
[[noreturn]] void task_exit();
bool exit_respawns_root(uint32_t& out_eip, uint32_t& out_esp);
bool exec_current(const char* path, char* const* argv, uint32_t& out_eip, uint32_t& out_esp);

} // namespace vnu::wintask