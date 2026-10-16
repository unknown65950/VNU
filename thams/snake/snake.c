/*
 * snake - the classic snake game, in a VNU window.
 *
 * A grid of cells with a snake in it that grows every time it eats and
 * dies on a wall or on itself; the arrow keys steer it. It needs a beat
 * faster than the one-per-second heartbeat every window is given, so it
 * is the first user of the kernel's per-window animation tick
 * (vgfx_anim): the kernel then wakes this window - and no other - every
 * step, vgfx_poll() reports each wake as VGFX_EV_ANIM, and the snake
 * advances one cell. Input and the tick arrive through the same poll, so
 * a turn pressed mid-step simply waits for the next one.
 *
 * The board is a fixed COLS x ROWS grid of CELL-pixel cells with a
 * status line under it, and the window opens at exactly that size.
 *
 * Controls: arrow keys steer, `p` pauses, Esc closes the window, and
 * Space starts a new game once the snake is dead.
 */
#include <vlibc/vgfx.h>
#include <vlibc/keys.h>
#include <vlibc/time.h>
#include <vlibc/unistd.h>

#define CELL     16           /* pixels per board cell (the font is 8) */
#define COLS     30           /* 480 / CELL */
#define ROWS     20           /* board rows above the status line */
#define STATUS_H 20
#define MAX_LEN  (COLS * ROWS)

/* The snake, head first: segment 0 is where the head is, the rest of the
 * body trailing behind it in the order the cells were vacated. */
static int seg_x[MAX_LEN];
static int seg_y[MAX_LEN];
static int len;

/* The heading in effect, and the one the next step will take. Only one
 * turn is remembered between steps, so two keys pressed in the same gap
 * cannot fold the snake back into its own neck. */
static int dir_x, dir_y;
static int turn_x, turn_y;

static int food_x, food_y;
static int score, best;
static int alive;
static int paused;

/* The step period asked of the kernel, shortened as the snake grows. */
static int step_ms = 140;

static unsigned rng_state;

/* A tiny LCG: the game wants a meal somewhere unexpected, not
 * cryptography, and freestanding vlibc has no rand(). */
static unsigned rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFFu;
}

static int occupied(int x, int y)
{
    for (int i = 0; i < len; ++i)
        if (seg_x[i] == x && seg_y[i] == y)
            return 1;
    return 0;
}

/* Places the next meal on a free cell: a random try first, which is the
 * common case and keeps the food from marching along the board, then a
 * scan so a board with almost no room still gets one. If there is
 * genuinely none, food_x goes negative, and that is a win. */
static void place_food(void)
{
    for (int t = 0; t < 200; ++t) {
        int x = (int)(rng() % COLS);
        int y = (int)(rng() % ROWS);
        if (!occupied(x, y)) {
            food_x = x;
            food_y = y;
            return;
        }
    }
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x)
            if (!occupied(x, y)) {
                food_x = x;
                food_y = y;
                return;
            }
    food_x = -1;
    food_y = -1;
}

static void reset(void)
{
    const int cx = COLS / 2, cy = ROWS / 2;
    len = 3;
    seg_x[0] = cx;     seg_y[0] = cy;
    seg_x[1] = cx - 1; seg_y[1] = cy;
    seg_x[2] = cx - 2; seg_y[2] = cy;
    dir_x = 1;
    dir_y = 0;
    turn_x = 1;
    turn_y = 0;
    score = 0;
    alive = 1;
    paused = 0;
    place_food();
}

/* One step: the queued heading takes effect, the head moves and the body
 * follows. The walls and the snake's own body are fatal, except the tail
 * - it is about to move out of the way. */
static void step(void)
{
    dir_x = turn_x;
    dir_y = turn_y;

    const int nx = seg_x[0] + dir_x;
    const int ny = seg_y[0] + dir_y;
    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) {
        alive = 0;
        return;
    }
    for (int i = 0; i < len - 1; ++i)
        if (seg_x[i] == nx && seg_y[i] == ny) {
            alive = 0;
            return;
        }

    const int ate = (nx == food_x && ny == food_y);
    if (ate && len < MAX_LEN)
        ++len;
    for (int i = len - 1; i > 0; --i) {
        seg_x[i] = seg_x[i - 1];
        seg_y[i] = seg_y[i - 1];
    }
    seg_x[0] = nx;
    seg_y[0] = ny;

    if (!ate)
        return;

    score += 10;
    if (score > best)
        best = score;
    /* A little faster with every meal, down to a floor the eye can still
     * follow. The kernel grants what it can, and says how much. */
    if (step_ms > 60) {
        step_ms -= 4;
        (void)vgfx_anim(step_ms);
    }
    place_food();
    if (food_x < 0)
        alive = 0; /* the board is full: nothing left to eat */
}

/* Writes `v` - never negative here - at (x, y) and returns the x past
 * it. */
static int num_at(int x, int y, int v, int color)
{
    char buf[12];
    int n = 0;
    if (v == 0)
        buf[n++] = '0';
    while (v > 0) {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    for (int i = 0; i < n; ++i)
        vgfx_char8(x + i * 8, y, buf[n - 1 - i], color);
    return x + n * 8;
}

static void draw(void)
{
    const int board_w = COLS * CELL;
    const int board_h = ROWS * CELL;

    vgfx_clear(VGFX_BLACK);

    /* The arena, so the wall the snake dies on is the wall it can see. */
    vgfx_rect(0, 0, board_w, board_h, VGFX_DGRAY);

    if (food_x >= 0)
        vgfx_fill_rect(food_x * CELL + 3, food_y * CELL + 3, CELL - 6,
                       CELL - 6, VGFX_LRED);

    for (int i = 0; i < len; ++i) {
        /* The head fills its cell, the body is inset a pixel, so the
         * snake reads as segments rather than as one long bar. */
        if (i == 0)
            vgfx_fill_rect(seg_x[i] * CELL + 1, seg_y[i] * CELL + 1,
                           CELL - 2, CELL - 2, VGFX_LGREEN);
        else
            vgfx_fill_rect(seg_x[i] * CELL + 2, seg_y[i] * CELL + 2,
                           CELL - 4, CELL - 4, VGFX_DGRAY);
    }

    /* The status line: score and best, the two numbers a player watches. */
    vgfx_hline(0, board_h, board_w, VGFX_DGRAY);
    int x = 6;
    vgfx_str8(x, board_h + 6, "SCORE", VGFX_LGRAY);
    x = num_at(x + vgfx_text_width8("SCORE") + 6, board_h + 6, score,
               VGFX_WHITE);
    x += 18;
    vgfx_str8(x, board_h + 6, "BEST", VGFX_LGRAY);
    num_at(x + vgfx_text_width8("BEST") + 6, board_h + 6, best, VGFX_WHITE);

    if (!alive) {
        /* A centered panel over the board, for the end of a game. */
        const char* line1 = (food_x < 0) ? "YOU WIN" : "GAME OVER";
        const char* line2 = "Space - play again";
        const int pw = 22 * 8 + 16;
        const int ph = 46;
        const int px = (board_w - pw) / 2;
        const int py = (board_h - ph) / 2;
        vgfx_fill_rect(px, py, pw, ph, VGFX_BLACK);
        vgfx_rect(px, py, pw, ph, VGFX_DGRAY);
        vgfx_str8(px + (pw - vgfx_text_width8(line1)) / 2, py + 9, line1,
                  VGFX_LRED);
        vgfx_str8(px + (pw - vgfx_text_width8(line2)) / 2, py + 27, line2,
                  VGFX_LGRAY);
    } else if (paused) {
        const char* msg = "PAUSED - p to resume";
        vgfx_str8((board_w - vgfx_text_width8(msg)) / 2, board_h / 2 - 4,
                  msg, VGFX_WHITE);
    }
}

/* A direction, unless it is the reverse of the one in effect: a snake
 * cannot turn back into its own neck. */
static void turn(int x, int y)
{
    if (x == -dir_x && y == -dir_y)
        return;
    turn_x = x;
    turn_y = y;
}

static void on_key(unsigned char k)
{
    if (k == VNU_KEY_ESC)
        exit(0);
    if (!alive) {
        if (k == ' ')
            reset();
        return;
    }
    if (k == 'p' || k == 'P') {
        paused = !paused;
        return;
    }
    if (k == VNU_KEY_LEFT)
        turn(-1, 0);
    else if (k == VNU_KEY_RIGHT)
        turn(1, 0);
    else if (k == VNU_KEY_UP)
        turn(0, -1);
    else if (k == VNU_KEY_DOWN)
        turn(0, 1);
}

int main(void)
{
    vgfx_canvas(COLS * CELL, ROWS * CELL + STATUS_H);

    /* Seed from the clock and the uptime: the first is constant within a
     * second, the second is not, so two games started in the same second
     * still differ. */
    rng_state = (unsigned)time(0) * 2654435761u + (unsigned)uptime();
    if (rng_state == 0)
        rng_state = 1;

    reset();
    (void)vgfx_anim(step_ms);

    for (;;) {
        vgfx_event_t ev;
        if (!vgfx_poll(&ev))
            continue;
        if (ev.type == VGFX_EV_ANIM) {
            if (alive && !paused)
                step();
        } else if (ev.type == VGFX_EV_KEY) {
            on_key(ev.key);
        }
        draw();
        vgfx_flush();
    }
}
