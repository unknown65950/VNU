#pragma once
#include <stdint.h>
#include <vnu/abi.h>

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
constexpr int INPUT_QUEUE_CAP = 128;
constexpr int TITLE_CAP = 24;
constexpr int MAX_TASKS = 6;
using TaskHandle = int;
constexpr TaskHandle NO_TASK = -1;

/* Graphics window size in pixels (the window's client area). A windowed
 * task can flip its console into a "gfx" surface by asking for it
 * (task_gfx_surface): the GUI then renders that buffer 1:1 instead of
 * the character grid, and mouse clicks over the client area get
 * delivered to the app as escape-sequence events on stdin.
 *
 * The canvas is a native 8x16 two-column pitch at its starting size, so
 * a gfx app's text renders at the same physical size as every console
 * window. That start is not a fixed size: the canvas follows the window
 * (see task_gfx_canvas), which is why the size is per-task state and
 * these two are only where a window *begins*. The GUI blits it into the
 * client area at the same scale, so a gfx window behaves like any other
 * one - and resizing the window now really does resize the drawing,
 * instead of scaling a 480x340 bitmap up. */
constexpr int GFX_W = 480;
constexpr int GFX_H = 340;

/* Where the shared surface sits in a windowed task's address space, and
 * how many frames it takes. It is *shared*: the same physical pages are
 * mapped here in the task and identity-mapped in every page directory
 * (they come from the PMM pool, which starts just above the kernel's
 * .bss), so the app writes pixels
 * at GFX_SURFACE_VA while the compositor reads the very same RAM through
 * the physical address. No copy, no syscall per frame.
 *
 * 0x500000 sits in the same 4 MiB window as the app image (0x400000),
 * so the task needs no extra private page table for it, and clear of
 * both the image and the per-task heap (0x700000). */
constexpr uint32_t GFX_SURFACE_VA = 0x00500000;

/* Pages a canvas of w x h takes at a given depth: the starting
 * 480x340 is 40 at 8bpp and 160 at 32bpp, and a canvas follows its
 * window, so this is asked with the size in hand rather than the
 * constants. The canvas is in the depth of the display the app is
 * drawing on (vgfx::bpp()), so a program writes the same pixels into it
 * that the frame itself holds - which is what lets a true-colour pixel,
 * and its alpha, reach the screen without a palette in the way.
 * A macro, not a function: this header is read by C too. */
#define GFX_SURFACE_PAGES(w, h, bpp) \
    ((uint32_t)((w) * (h) * ((bpp) / 8) + 0xFFFu) / 0x1000u)

/* The canvas is allocated in whole pages and mapped at GFX_SURFACE_VA,
 * so growing it means unmapping, reallocating and mapping again - which
 * would drop whatever was drawn. Instead the grant is rounded up to
 * this granularity: a resize inside one granule reuses the same pages
 * and costs nothing, and the app keeps its pixels across the drag. */
constexpr int GFX_CANVAS_GRANULE = 64;

/* VNU mouse protocol (delivered over the stdin escape stream, one event
 * per message so a partially-queued press can't corrupt the next):
 *   ESC '[' 'M' <button> <xl> <xh> <yl> <yh>
 * where <button> is 1 (left pressed) / 2 (left released) / 3 (Esc key)
 * / 4 (drag cancelled: a click became a drag, no release will follow)
 * and the coordinates are little-endian 16-bit client-area pixels (a
 * 480-wide canvas no longer fits in one byte).
 *
 * Drag-and-drop: the GUI delivers the dropped item's path as
 *   ESC '[' 'D' <len_lo> <len_hi> <path...>
 * (little-endian 16-bit length; the payload is the full VFS path). */
constexpr char MOUSE_ESC = 0x1B;
constexpr int MOUSE_MSG_LEN = 8;

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
    /* Gfx mode state: asking for the surface (task_gfx_surface) flips
     * this task to a pixel framebuffer window. `pixel` is then the
     * identity-mapped view of the pages the app writes at
     * GFX_SURFACE_VA - the compositor blits straight out of them, and
     * stays null for a text window, which never asks.
     *
     * `pix_w`/`pix_h` are the canvas the app draws *at*, and they are
     * per-task because the canvas follows the window: the window starts
     * at GFX_W x GFX_H and the app asks for something else with
     * task_gfx_canvas. `pix_pages` is what the canvas actually occupies
     * in the page pool, rounded up to GFX_CANVAS_GRANULE, so the window
     * can be dragged a little either way without a reallocation.
     * `winch` is set when the size changed under the app, which is what
     * raises SIGWINCH. */
    uint8_t* pixel;
    bool gfx;
    int pix_w;
    int pix_h;
    int pix_pages;
    bool winch;
    /* Signals pending for this windowed task: bitmask by signal number.
     * SIGWINCH is raised when the canvas changes size. The delivery
     * path is different from classic processes, but the handler and mask
     * are the same struct shape. */
    uint32_t sig_pending;
    uint32_t sig_mask;
    struct vnu_sigaction sig_action[VNU_NSIG];
    uint32_t sig_saved_mask;
    uint32_t sig_frame;
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

// spawn() variant with one extra argv entry: the windowed app starts
// with argv[] = {argv0_path, arg1, NULL}. The GUI uses it to open a
// dropped file in an app (drop a picture on the picview icon → the
// viewer is launched with the file, exactly like the file manager's
// execve path). arg1 is copied into the app's argv frame.
TaskHandle spawn_argv(const char* argv0_path, const char* arg1, const uint8_t* data,
                      uint32_t size, const char* title);

// Handle of the currently executing windowed task, or NO_TASK when a
// syscall runs from the console (non-windowed) track.
int current_handle();

// Tears down a task that is parked (blocked/waiting): frees its page
// directory, root-image copy and slot. Must NOT be called for the task
// currently executing (the GUI never does — it only closes windows in
// its own event loop).
void close_task(TaskHandle h);

/* The same operations addressed by console rather than by handle, for
 * the signal path: raising SIGWINCH knows which window changed, not which
 * slot number it happens to live in.
 *
 * close_task_of() is the careful one - a task killed by a signal may be
 * the one executing right now, and its pages cannot be freed under its
 * own feet, so in that case it is only marked finished and the GUI's own
 * sweep closes the window from the GUI's stack. That is the same shape
 * exit() has. */
int handle_of(Console* c);
void wake_task_of(Console* c);
void close_task_of(Console* c);

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

/* Per-second heartbeat. The GUI feeds TICK_BYTE into every live gfx-mode
 * task's stdin queue once per RTC-second change, so animation apps (the
 * desktop clock) can advance their hands even while idle — the task
 * blocks in its next read, the byte unblocks it, and the app redraws.
 * It arrives as a plain KEY event to vgfx_poll(); apps that don't want
 * the tick simply ignore it. Switched off for text-console tasks. */
enum : char { TICK_BYTE = 0x06 };

// Feeds TICK_BYTE to every live gfx-mode task's input queue.
void heartbeat_gfx_tasks();

// Delivers a mouse event (button and pixel coordinates) to a task's
// stdin queue as a VNU mouse protocol message (MOUSE_MSG_LEN bytes).
// All six bytes are enqueued atomically: if the queue doesn't have room
// for the whole message, the event is silently dropped.
// Button values: 1 = left press, 2 = left released, 3 = Esc key,
// 4 = drag cancelled (the click turned into a drag; no release follows).
void feed_mouse(TaskHandle h, int button, int px, int py);

// Delivers a drag-and-drop "you received this file" notification to a
// task's stdin queue: ESC '[' 'D' <len_lo> <len_hi> <path...>, all
// enqueued atomically (dropped when the queue can't fit it). `path` is
// the full VFS path of the dropped item. The receiving app decides what
// to do with it (picview loads the file; files refreshes its listing).
void feed_drop(TaskHandle h, const char* path);

// --- Canvas negotiation, on any task's console ---

/* Resizes the canvas belonging to `target` to what `req` asks for (0 on
// either axis leaves that one alone) and writes the granted size and
// depth to `out`. Either pointer may be null to only request, resp. only
// ask. Changing the size reallocates the canvas, carries the drawn
// top-left corner over and sets the task's `winch`, which is what raises
// SIGWINCH - the resize and the notice of it are one event.
//
// Addressed by console rather than by "the current task" because a
// window can be resized from two sides: the app, through the syscall,
// while it is executing; and the GUI, which owns the geometry and is
// doing it because the user dragged the window's edge. Both end up here,
// so both get the same clamping and the same signal. */
int canvas_resize(Console& target, uint32_t pgdir, const struct vnu_gfx_canvas* req,
                  struct vnu_gfx_canvas* out);

/* The page directory a console's pages live in, or 0 if no task holds
 * it. The canvas is mapped into the owning task's address space, so
 * whoever resizes it needs that task's `pgdir` - and the GUI resizing
 * another task's window is exactly the case the task's own
 * current_is_task() would refuse. */
uint32_t pgdir_of(Console& target);

/* Raises SIGWINCH for every task whose canvas changed size and has not
 * been told yet.
 *
 * canvas_resize() only sets a flag: the app's own request is a syscall,
 * where a signal raised on the spot would want to be delivered before
 * the syscall even returns, while the GUI's (from a window drag) has no
 * task executing at all and the task is usually parked in a read. So the
 * GUI's pass over its tasks is where the flag becomes a signal, every
 * pass, which is the one place that needs no task to be running and
 * reaches a task asleep in its input read - the signal wakes it, the
 * read comes back EINTR and the handler runs on the way out. */
void raise_winch_signals();

// --- Gfx surface (operates on the currently executing task, see
// current_is_task()) ---

/* Gfx surface: maps the shared pages into the calling task (first call
 * only) and returns where to draw, i.e. GFX_SURFACE_VA. Asking is also
 * what puts the window into pixel mode. Returns 0 when the caller is
 * not a windowed task, or when the pages could not be mapped. */
uint32_t task_gfx_surface();

/* The calling task's console, valid only while current_is_task(). The
 * other half of every windowed syscall hook: this is *which* window the
 * request is about, where the other hooks answer on its behalf. */
Console* console_of_current();

/* Per-task heap break. The task's heap is pre-mapped (BRK_MIN..BRK_MAX),
 * so this is just bookkeeping for vlibc's malloc(). Called from the
 * SYS_brk handler while a windowed task is executing. */
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