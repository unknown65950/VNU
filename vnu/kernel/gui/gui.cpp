#include <vnu/gui.h>
#include <vnu/vga_gfx.h>
#include <vnu/ps2mouse.h>
#include <vnu/kbd.h>
#include <vnu/apps.h>
#include <vnu/wintask.h>

namespace {

using vnu::wintask::MAX_TASKS;
using vnu::wintask::TaskHandle;
using vnu::wintask::NO_TASK;
using vnu::wintask::CON_COLS;
using vnu::wintask::CON_ROWS;
using vnu::wintask::SCROLL_ROWS;
using vnu::wintask::CELL_W;
using vnu::wintask::CELL_H;

constexpr int TITLE_H = 20;
constexpr int PANEL_H = 34;
constexpr int BTN_H = 26;
constexpr int BTN_PAD_Y = (PANEL_H - BTN_H) / 2;

constexpr int ICON_SIZE = 24;
constexpr int ICON_CELL_W = 80;
constexpr int ICON_CELL_H = 48;
constexpr int ICON_ORIGIN_X = 8;
/* Icons live below the taskbar now. */
constexpr int ICON_ORIGIN_Y = PANEL_H + 8;

constexpr int GFX_MIN_SCALE = 1;
constexpr int GFX_MAX_SCALE = 4;
constexpr int GFX_DEFAULT_SCALE = 2;

/* Resize grip band width, in pixels, along the window's right/bottom. */
constexpr int RESIZE_W = 12;

struct Window {
    int x, y, w, h;
};

enum class DragOp { None, Move, ResizeCorner, ResizeRight, ResizeBottom };

int gfx_scale_for(int w)
{
    int s = (w - 8) / vnu::wintask::GFX_W;
    if (s < GFX_MIN_SCALE)
        s = GFX_MIN_SCALE;
    if (s > GFX_MAX_SCALE)
        s = GFX_MAX_SCALE;
    return s;
}

void icon_rect(int index, int& x, int& y)
{
    int col = index % 4;
    int row = index / 4;
    x = ICON_ORIGIN_X + col * ICON_CELL_W;
    y = ICON_ORIGIN_Y + row * ICON_CELL_H;
}

void draw_icons(const vnu::apps::AppEntry* apps, int count)
{
    using namespace vnu::vgfx;
    for (int i = 0; i < count; ++i) {
        int x, y;
        icon_rect(i, x, y);
        /* NeXT-style beveled icon tile. */
        fill_rect(x, y, ICON_SIZE, ICON_SIZE, apps[i].icon_color);
        hline(x, y, ICON_SIZE, COLOR_BLACK);
        vline(x, y, ICON_SIZE, COLOR_BLACK);
        hline(x, y + ICON_SIZE - 1, ICON_SIZE, COLOR_BLACK);
        vline(x + ICON_SIZE - 1, y, ICON_SIZE, COLOR_BLACK);
        hline(x + 1, y + 1, ICON_SIZE - 2, COLOR_WHITE);
        vline(x + 1, y + 1, ICON_SIZE - 2, COLOR_WHITE);
        /* 8x16 letter vertically centered in the tile. */
        char letter[2] = {apps[i].name[0], 0};
        draw_char(x + (ICON_SIZE - 8) / 2, y + (ICON_SIZE - 16) / 2, letter[0], COLOR_BLACK);
        int label_w = text_width(apps[i].name);
        int label_x = x + (ICON_SIZE - label_w) / 2;
        if (label_x < 0)
            label_x = 0;
        draw_string(label_x, y + ICON_SIZE + 3, apps[i].name, COLOR_WHITE);
    }
}

int hit_test_icon(const vnu::apps::AppEntry* apps, int count, int mx, int my)
{
    (void)apps;
    for (int i = 0; i < count; ++i) {
        int x, y;
        icon_rect(i, x, y);
        if (mx >= x && mx < x + ICON_SIZE && my >= y && my < y + ICON_SIZE)
            return i;
    }
    return -1;
}

void clamp_to_desktop(Window& win)
{
    if (win.x < 0) win.x = 0;
    if (win.y < 0) win.y = 0;
    if (win.x + win.w > vnu::vgfx::WIDTH) win.x = vnu::vgfx::WIDTH - win.w;
    if (win.y + win.h > vnu::vgfx::HEIGHT) win.y = vnu::vgfx::HEIGHT - win.h;
}

void draw_chrome(const Window& win, bool active)
{
    using namespace vnu::vgfx;
    fill_rect(win.x, win.y, win.w, win.h, COLOR_LGRAY);
    rect(win.x, win.y, win.w, win.h, COLOR_BLACK);
    /* NeXT-style beveled window edge: light at top/left, dark bottom. */
    hline(win.x + 1, win.y + 1, win.w - 2, COLOR_WHITE);
    vline(win.x + 1, win.y + 2, win.h - 3, COLOR_WHITE);
    hline(win.x + 2, win.y + win.h - 2, win.w - 3, COLOR_DGRAY);
    vline(win.x + win.w - 2, win.y + 2, win.h - 3, COLOR_DGRAY);
    /* Active window gets a blue title bar, inactive ones a dark one. */
    fill_rect(win.x + 1, win.y + 1, win.w - 2, TITLE_H - 1,
              active ? COLOR_BLUE : COLOR_DGRAY);
    hline(win.x + 1, win.y + TITLE_H, win.w - 2, COLOR_BLACK);
}

/* Visible rows/columns that fit the window's client area. */
int fit_cols(const Window& win)
{
    int cols = (win.w - 4) / CELL_W;
    if (cols < 1)
        cols = 1;
    if (cols > CON_COLS)
        cols = CON_COLS;
    return cols;
}

int fit_rows(const Window& win)
{
    int rows = (win.h - (TITLE_H + 4)) / CELL_H;
    if (rows < 1)
        rows = 1;
    if (rows > CON_ROWS)
        rows = CON_ROWS;
    return rows;
}

/* Map a "total output row" (0 = oldest history row, hist_n + CON_ROWS -
 * 1 = live bottom) to a source row buffer. Returns nullptr for history
 * rows that fall outside the ring (shouldn't happen given the clamps). */
const char* output_row(const vnu::wintask::Console& con, int li, int* screen_row)
{
    if (li < con.hist_n) {
        int idx = con.hist_tail - con.hist_n + li;
        while (idx < 0)
            idx += SCROLL_ROWS;
        idx %= SCROLL_ROWS;
        return con.hist[idx];
    }
    *screen_row = li - con.hist_n;
    return con.cell[*screen_row];
}

void draw_console_window(const Window& win, const vnu::wintask::Console& con, bool active)
{
    draw_chrome(win, active);
    using namespace vnu::vgfx;
    draw_string(win.x + 4, win.y + 2, con.title, COLOR_WHITE);

    int cols = fit_cols(win);
    int rows = fit_rows(win);
    int total = con.hist_n + CON_ROWS;
    int scroll = con.scroll_off;
    if (scroll < 0)
        scroll = 0;
    if (scroll > con.hist_n)
        scroll = con.hist_n;
    /* First displayed total-output row. At scroll == 0 the live bottom
     * (hist_n + rows - 1 ... ) — i.e. the bottom of the cell screen plus
     * (rows - 1) pre-scroll rows — must line up with the window's bottom
     * row, so we anchor on the last screen line. */
    int first_li = total - rows - scroll;
    if (first_li < 0)
        first_li = 0;

    int cx = win.x + 2;
    int cy = win.y + TITLE_H + 2;
    /* TEMP-DIAGNOSE: red client == some console output written. */
    if (con.cur_row > 0 || con.cur_col > 0 || con.hist_n > 0)
        fill_rect(cx, cy, cols * CELL_W, rows * CELL_H, COLOR_RED);
    (void)rows;
    for (int v = 0; v < rows; ++v) {
        int li = first_li + v;
        int scr = 0;
        const char* row = output_row(con, li, &scr);
        for (int c = 0; c < cols; ++c) {
            char ch = row[c];
            if (ch != ' ' && ch != 0)
                draw_char(cx + c * CELL_W, cy + v * CELL_H, ch, COLOR_BLACK);
        }
    }

    /* Cursor (live bottom only). */
    if (con.scroll_off == 0 || con.scroll_off > con.hist_n) {
        int live_row = con.hist_n + con.cur_row;
        if (live_row >= first_li && live_row < first_li + rows && con.cur_col < cols) {
            int v = live_row - first_li;
            int ccx = cx + con.cur_col * CELL_W;
            int ccy = cy + v * CELL_H + CELL_H - 3;
            hline(ccx, ccy, CELL_W - 1, COLOR_BLACK);
        }
    }

    /* Scrollbar: only drawn when there is scrollback to view. */
    if (con.hist_n > 0 && rows < CON_ROWS) {
        int bx = win.x + win.w - 4 - 10;
        int top = win.y + TITLE_H + 2;
        int bh = win.h - (TITLE_H + 4);
        fill_rect(bx, top, 10, bh, COLOR_DGRAY);
        int maxoff = con.hist_n;
        int thumb_h = bh * rows / CON_ROWS;
        if (thumb_h < 10)
            thumb_h = 10;
        if (thumb_h > bh)
            thumb_h = bh;
        int thumb_y = top + ((bh - thumb_h) * scroll) / maxoff;
        fill_rect(bx, thumb_y, 10, thumb_h, COLOR_LGRAY);
        vline(bx, top, bh, COLOR_BLACK);
    }
}

void draw_gfx_window(const Window& win, const vnu::wintask::Console& con, bool active, int scale)
{
    draw_chrome(win, active);
    using namespace vnu::vgfx;
    draw_string(win.x + 4, win.y + 2, con.title, COLOR_WHITE);
    int origin_x = win.x + 2;
    int origin_y = win.y + TITLE_H + 2;
    blit_scaled(con.pixel, vnu::wintask::GFX_W, vnu::wintask::GFX_H,
                origin_x, origin_y, scale);
    rect(origin_x - 1, origin_y - 1, vnu::wintask::GFX_W * scale + 2,
         vnu::wintask::GFX_H * scale + 2, COLOR_BLACK);
}

/* Client-area size for a task's window: adapts to the console's mode
 * (text grid vs gfx framebuffer at `scale`). */
Window size_app_window(int x, int y, const vnu::wintask::Console* con, int gfx_scale)
{
    Window w;
    w.x = x;
    w.y = y;
    bool gfx = con && con->gfx;
    if (gfx) {
        w.w = vnu::wintask::GFX_W * gfx_scale + 8;
        w.h = TITLE_H + vnu::wintask::GFX_H * gfx_scale + 12;
    } else {
        w.w = CON_COLS * CELL_W + 8;
        w.h = TITLE_H + CON_ROWS * CELL_H + 12;
    }
    return w;
}

bool hit_test_titlebar(const Window& win, int mx, int my)
{
    return mx >= win.x && mx < win.x + win.w && my >= win.y && my < win.y + TITLE_H;
}

DragOp hit_test_resize(const Window& win, int mx, int my)
{
    if (mx >= win.x + win.w - RESIZE_W && my >= win.y + win.h - RESIZE_W)
        return DragOp::ResizeCorner;
    if (mx >= win.x + win.w - RESIZE_W && my >= win.y + TITLE_H)
        return DragOp::ResizeRight;
    if (my >= win.y + win.h - RESIZE_W && mx >= win.x)
        return DragOp::ResizeBottom;
    return DragOp::None;
}

bool hit_test_scrollbar(const Window& win, int mx, int my)
{
    int bx = win.x + win.w - 4 - 10;
    int top = win.y + TITLE_H + 2;
    int bh = win.h - (TITLE_H + 4);
    return mx >= bx && mx < bx + 10 && my >= top && my < top + bh;
}

/* --- Taskbar --- */

enum class PB : uint8_t { None = 0, Close, Min, Reboot, Exit, App, Clock };

struct PBtn {
    int x, y, w, h;
    PB kind;
    int handle;
};

int layout_panel(PBtn* out, int focused)
{
    using namespace vnu::vgfx;
    int n = 0;
    int y = BTN_PAD_Y;

    /* --- Left cluster --- */
    int x = 6;
    out[n++] = {x, y, 26, BTN_H, PB::Clock, -1}; /* analog clock */
    x += 26 + 4;

    const char* exit_label = "Exit";
    int w2 = text_width(exit_label) + 10;
    out[n++] = {x, y, w2, BTN_H, PB::Exit, -1};
    x += w2 + 4;

    const char* reb_label = "Reboot";
    w2 = text_width(reb_label) + 10;
    out[n++] = {x, y, w2, BTN_H, PB::Reboot, -1};
    x += w2 + 4;

    /* Running-app list: one button per open window ("active windows"). */
    for (int h = 0; h < MAX_TASKS && n < 24; ++h) {
        if (!vnu::wintask::has_window(h))
            continue;
        const char* title = vnu::wintask::console(h) ? vnu::wintask::console(h)->title : "";
        w2 = text_width(title) + 12;
        out[n++] = {x, y, w2, BTN_H, PB::App, h};
        x += w2 + 3;
    }

    /* --- Right cluster --- */
    x = WIDTH - 6;
    if (focused >= 0) {
        w2 = 22;
        x -= w2;
        out[n++] = {x, y, w2, BTN_H, PB::Close, focused};
        x -= 4;
        x -= w2;
        out[n++] = {x, y, w2, BTN_H, PB::Min, focused};
        x -= 4;

        const char* title =
            vnu::wintask::console(focused) ? vnu::wintask::console(focused)->title : "";
        w2 = text_width(title) + 8;
        x -= w2;
        out[n++] = {x, y, w2, BTN_H, PB::None, focused}; /* active window name */
    }
    return n;
}

int hit_panel(const PBtn* out, int n, int mx, int my)
{
    if (my >= PANEL_H)
        return -1;
    for (int i = 0; i < n; ++i) {
        const PBtn& b = out[i];
        if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h)
            return i;
    }
    return -1;
}

void draw_clock(int cx, int cy, int r);
void draw_panel(const PBtn* out, int n)
{
    using namespace vnu::vgfx;
    fill_rect(0, 0, WIDTH, PANEL_H, COLOR_LGRAY);
    hline(0, 0, WIDTH, COLOR_WHITE);
    hline(0, PANEL_H - 1, WIDTH, COLOR_BLACK);

    for (int i = 0; i < n; ++i) {
        const PBtn& b = out[i];
        switch (b.kind) {
        case PB::Min:
            fill_rect(b.x, b.y, b.w, b.h, COLOR_DGRAY);
            hline(b.x, b.y, b.w, COLOR_WHITE);
            vline(b.x, b.y, b.h, COLOR_WHITE);
            hline(b.x, b.y + b.h - 1, b.w, COLOR_BLACK);
            vline(b.x + b.w - 1, b.y, b.h, COLOR_BLACK);
            hline(b.x + 5, b.y + b.h / 2, b.w - 10, COLOR_WHITE);
            break;
        case PB::Close:
            fill_rect(b.x, b.y, b.w, b.h, COLOR_DGRAY);
            hline(b.x, b.y, b.w, COLOR_WHITE);
            vline(b.x, b.y, b.h, COLOR_WHITE);
            hline(b.x, b.y + b.h - 1, b.w, COLOR_BLACK);
            vline(b.x + b.w - 1, b.y, b.h, COLOR_BLACK);
            line(b.x + 4, b.y + 4, b.x + b.w - 5, b.y + b.h - 5, COLOR_WHITE);
            line(b.x + b.w - 5, b.y + 4, b.x + 4, b.y + b.h - 5, COLOR_WHITE);
            break;
        case PB::Reboot:
        case PB::Exit:
            fill_rect(b.x, b.y, b.w, b.h, COLOR_DGRAY);
            hline(b.x, b.y, b.w, COLOR_WHITE);
            vline(b.x, b.y, b.h, COLOR_WHITE);
            hline(b.x, b.y + b.h - 1, b.w, COLOR_BLACK);
            vline(b.x + b.w - 1, b.y, b.h, COLOR_BLACK);
            draw_string(b.x + 5, b.y + 5, b.kind == PB::Reboot ? "Reboot" : "Exit",
                        COLOR_WHITE);
            break;
        case PB::App:
            fill_rect(b.x, b.y, b.w, b.h, COLOR_DGRAY);
            hline(b.x, b.y, b.w, COLOR_WHITE);
            vline(b.x, b.y, b.h, COLOR_WHITE);
            hline(b.x, b.y + b.h - 1, b.w, COLOR_BLACK);
            vline(b.x + b.w - 1, b.y, b.h, COLOR_BLACK);
            draw_string(b.x + 5, b.y + 5,
                        vnu::wintask::console(b.handle) ? vnu::wintask::console(b.handle)->title : "",
                        COLOR_WHITE);
            break;
        case PB::None:
            draw_string(b.x + 4, b.y + 5,
                        vnu::wintask::console(b.handle) ? vnu::wintask::console(b.handle)->title : "",
                        COLOR_BLACK);
            break;
        case PB::Clock:
            draw_clock(b.x + b.w / 2, b.y + b.h / 2, 10);
            break;
        default:
            break;
        }
    }
}

/* --- Analog clock (from the RTC) --- */

inline uint8_t rtc_read(uint8_t reg)
{
    asm volatile("outb %0, $0x70" : : "a"(reg));
    uint8_t v;
    asm volatile("inb $0x71, %0" : "=a"(v));
    return v;
}

void read_rtc_time(int& h, int& m, int& s)
{
    while (rtc_read(0x0A) & 0x80) {
    }
    uint8_t sb = rtc_read(0x0B);
    bool bcd = !(sb & 0x04);
    uint8_t sec = rtc_read(0x00);
    uint8_t min = rtc_read(0x02);
    uint8_t hour = rtc_read(0x04);
    auto norm = [bcd](uint8_t v) { return bcd ? ((v & 0x0F) + ((v >> 4) & 0x0F) * 10) : v; };
    s = norm(sec);
    m = norm(min);
    h = norm(hour);
}

int sine1000(int deg)
{
    static const int T[90] = {
        0,    17,   34,   52,   69,   87,   104,  121,  139,  156,
        173,  190,  207,  224,  241,  258,  275,  292,  309,  325,
        342,  358,  374,  390,  406,  422,  438,  453,  469,  484,
        499,  515,  529,  544,  559,  573,  587,  601,  615,  629,
        642,  656,  669,  681,  694,  707,  719,  731,  743,  754,
        766,  777,  788,  798,  809,  819,  829,  838,  848,  857,
        866,  875,  883,  891,  899,  906,  913,  920,  927,  933,
        939,  945,  950,  956,  961,  965,  970,  974,  978,  981,
        985,  988,  990,  993,  995,  997,  998,  999,  1000, 1000,
    };
    int q = deg % 360;
    if (q < 0)
        q += 360;
    int seg = q / 90;
    int r = q % 90;
    switch (seg) {
    case 0:
        return T[r];
    case 1:
        return T[89 - r];
    case 2:
        return -T[r];
    default:
        return -T[89 - r];
    }
}

int cosine1000(int deg)
{
    return sine1000(deg + 90);
}

void draw_clock(int cx, int cy, int r)
{
    using namespace vnu::vgfx;
    int h = 0, m = 0, s = 0;
    read_rtc_time(h, m, s);

    fill_circle(cx, cy, r, COLOR_WHITE);
    circle(cx, cy, r, COLOR_BLACK);
    circle(cx, cy, r - 2, COLOR_DGRAY);

    int sec_deg = s * 6;
    int min_deg = m * 6 + s / 10;
    int hr_deg = (h % 12) * 30 + m / 2;

    int hx = (cx * 1000 + (r - 6) * sine1000(hr_deg)) / 1000;
    int hy = (cy * 1000 - (r - 6) * cosine1000(hr_deg)) / 1000;
    line(cx, cy, hx, hy, COLOR_BLACK);
    int mx2 = (cx * 1000 + (r - 2) * sine1000(min_deg)) / 1000;
    int my2 = (cy * 1000 - (r - 2) * cosine1000(min_deg)) / 1000;
    line(cx, cy, mx2, my2, COLOR_BLACK);
    int sx2 = (cx * 1000 + (r - 1) * sine1000(sec_deg)) / 1000;
    int sy2 = (cy * 1000 - (r - 1) * cosine1000(sec_deg)) / 1000;
    line(cx, cy, sx2, sy2, COLOR_BLACK);
    put_pixel(cx, cy, COLOR_BLACK);
}

void reboot_now()
{
    asm volatile("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    for (;;) {
    }
}

} // namespace

namespace vnu::gui {

void run()
{
    vnu::vgfx::enter_gfx_mode();
    vnu::vgfx::draw_wallpaper();
    vnu::mouse::init();
    vnu::wintask::init();

    vnu::apps::AppEntry apps[vnu::apps::MAX_APPS];
    int app_count = vnu::apps::list(apps, vnu::apps::MAX_APPS);

    Window geom[MAX_TASKS];
    bool minimized[MAX_TASKS] = {};
    TaskHandle order[MAX_TASKS];
    int order_len = 0;
    TaskHandle focused = NO_TASK;
    int cascade = 0;
    int gfx_scale[MAX_TASKS] = {};
    bool was_gfx[MAX_TASKS] = {};
    for (int i = 0; i < MAX_TASKS; ++i)
        gfx_scale[i] = GFX_DEFAULT_SCALE;

    static uint8_t load_buf[20480];

    auto raise_window = [&](TaskHandle h) {
        int j = -1;
        for (int i = 0; i < order_len; ++i)
            if (order[i] == h) {
                j = i;
                break;
            }
        if (j >= 0) {
            for (int i = j; i < order_len - 1; ++i)
                order[i] = order[i + 1];
            --order_len;
        }
        order[order_len++] = h;
        focused = h;
    };

    auto focus_top = [&]() { focused = order_len > 0 ? order[order_len - 1] : NO_TASK; };

    auto window_by_title = [&](const char* name) -> TaskHandle {
        for (int i = 0; i < order_len; ++i) {
            vnu::wintask::Console* c = vnu::wintask::console(order[i]);
            if (c) {
                const char* t = c->title;
                int k = 0;
                while (name[k] && t[k] && name[k] == t[k])
                    ++k;
                if (name[k] == 0 && t[k] == 0)
                    return order[i];
            }
        }
        return NO_TASK;
    };

    auto window_exists = [&](TaskHandle h) { return vnu::wintask::has_window(h); };

    auto close_window = [&](TaskHandle h) {
        if (!window_exists(h)) {
            vnu::wintask::close_task(h);
            return;
        }
        int j = -1;
        for (int i = 0; i < order_len; ++i)
            if (order[i] == h) {
                j = i;
                break;
            }
        if (j >= 0) {
            for (int i = j; i < order_len - 1; ++i)
                order[i] = order[i + 1];
            --order_len;
        }
        vnu::wintask::close_task(h);
        minimized[h] = false;
        if (focused == h)
            focus_top();
    };

    auto sweep_finished = [&]() {
        for (int i = 0; i < order_len;) {
            TaskHandle h = order[i];
            if (!vnu::wintask::has_window(h)) {
                vnu::wintask::close_task(h);
                minimized[h] = false;
                for (int k = i; k < order_len - 1; ++k)
                    order[k] = order[k + 1];
                --order_len;
            } else {
                ++i;
            }
        }
        if (focused != NO_TASK && !vnu::wintask::has_window(focused))
            focus_top();
    };

    auto spawn_from_icon = [&](int app_idx) {
        const char* name = apps[app_idx].name;
        TaskHandle existing = window_by_title(name);
        if (existing != NO_TASK) {
            minimized[existing] = false;
            raise_window(existing);
            return;
        }
        uint32_t n = vnu::apps::read_bin(name, load_buf, sizeof(load_buf));
        if (n == 0)
            return;
        TaskHandle h = vnu::wintask::spawn(name, load_buf, n, name);
        if (h == NO_TASK)
            return;
        int cx = 36 + (cascade % 6) * 26;
        int cy = PANEL_H + 8 + (cascade % 6) * 22;
        ++cascade;
        gfx_scale[h] = GFX_DEFAULT_SCALE;
        geom[h] = size_app_window(cx, cy, vnu::wintask::console(h), gfx_scale[h]);
        clamp_to_desktop(geom[h]);
        minimized[h] = false;
        raise_window(h);
    };

    int mx = vnu::vgfx::WIDTH / 2;
    int my = vnu::vgfx::HEIGHT / 2;
    DragOp drag_op = DragOp::None;
    TaskHandle drag_h = NO_TASK;
    int drag_off_x = 0, drag_off_y = 0;
    int rs_orig_w = 0, rs_orig_h = 0, rs_mx = 0, rs_my = 0;
    bool left_was_down = false;
    TaskHandle gfx_press_h = NO_TASK;
    int gfx_press_px = 0, gfx_press_py = 0;
    bool have_gfx_press = false;

    PBtn panel_btns[24];
    int panel_n = 0;

    for (;;) {
        /* Give every live task a burst; each either blocks on empty
         * input or exits, so this always comes straight back. */
        vnu::wintask::run_all_slices();
        sweep_finished();

        int key = vnu::kbd::poll_char();
        if (key >= 0) {
            if (key == 27 && focused == NO_TASK) {
                /* Esc with no windows open leaves the desktop. */
                break;
            }
            if (focused != NO_TASK && vnu::wintask::is_running(focused)) {
                vnu::wintask::Console* con = vnu::wintask::console(focused);
                if (con) {
                    con->scroll_off = 0;
                    if (con->gfx && key == 27)
                        vnu::wintask::feed_mouse(focused, 3, 0, 0);
                    else
                        vnu::wintask::feed_input(focused, static_cast<char>(key));
                }
                vnu::wintask::run_slice(focused);
            }
        }

        int dx = 0, dy = 0;
        uint8_t buttons = 0;
        int8_t wheel = 0;
        if (vnu::mouse::poll(dx, dy, buttons, wheel)) {
            mx += dx;
            my += dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx >= vnu::vgfx::WIDTH) mx = vnu::vgfx::WIDTH - 1;
            if (my >= vnu::vgfx::HEIGHT) my = vnu::vgfx::HEIGHT - 1;

            bool left_down = (buttons & 0x01) != 0;
            bool just_pressed = left_down && !left_was_down;
            left_was_down = left_down;

            if (just_pressed) {
                panel_n = layout_panel(panel_btns, focused);
                int pb = hit_panel(panel_btns, panel_n, mx, my);
                if (pb >= 0) {
                    const PBtn& b = panel_btns[pb];
                    switch (b.kind) {
                    case PB::Close:
                        if (b.handle != NO_TASK)
                            close_window(b.handle);
                        break;
                    case PB::Min:
                        if (b.handle != NO_TASK)
                            minimized[b.handle] = true;
                        break;
                    case PB::Reboot:
                        reboot_now();
                        break;
                    case PB::Exit:
                        goto exit_gui;
                    case PB::App:
                        if (b.handle != NO_TASK) {
                            minimized[b.handle] = false;
                            raise_window(b.handle);
                        }
                        break;
                    default:
                        break;
                    }
                } else if (my >= PANEL_H) {
                    int hit = hit_test_icon(apps, app_count, mx, my);
                    if (hit >= 0) {
                        spawn_from_icon(hit);
                    } else {
                        /* Click in a window: topmost first. */
                        for (int i = order_len - 1; i >= 0; --i) {
                            TaskHandle h = order[i];
                            if (!window_exists(h) || minimized[h])
                                continue;
                            const Window& win = geom[h];
                            vnu::wintask::Console* con = vnu::wintask::console(h);
                            if (!win.w || !win.h)
                                continue;
                            int o0 = mx >= win.x && mx < win.x + win.w;
                            if (!o0 || my < win.y || my >= win.y + win.h)
                                continue;
                            raise_window(h);
                            DragOp op = hit_test_resize(win, mx, my);
                            if (op != DragOp::None) {
                                drag_op = op;
                                drag_h = h;
                                rs_orig_w = win.w;
                                rs_orig_h = win.h;
                                rs_mx = mx;
                                rs_my = my;
                            } else if (con && con->gfx && mx >= win.x + 2 &&
                                       mx < win.x + 2 + vnu::wintask::GFX_W * gfx_scale[h] &&
                                       my >= win.y + TITLE_H + 2 &&
                                       my < win.y + TITLE_H + 2 + vnu::wintask::GFX_H * gfx_scale[h]) {
                                int px = (mx - (win.x + 2)) / gfx_scale[h];
                                int py = (my - (win.y + TITLE_H + 2)) / gfx_scale[h];
                                vnu::wintask::feed_mouse(h, 1, px, py);
                                gfx_press_h = h;
                                gfx_press_px = px;
                                gfx_press_py = py;
                                have_gfx_press = true;
                            } else if (hit_test_scrollbar(win, mx, my)) {
                                if (con && !con->gfx && con->hist_n > 0) {
                                    int top = win.y + TITLE_H + 2;
                                    int bh = win.h - (TITLE_H + 4);
                                    int maxoff = con->hist_n;
                                    int off = ((my - top) * maxoff) / bh;
                                    if (off > maxoff)
                                        off = maxoff;
                                    con->scroll_off = off;
                                }
                            } else if (hit_test_titlebar(win, mx, my)) {
                                drag_op = DragOp::Move;
                                drag_h = h;
                                drag_off_x = mx - win.x;
                                drag_off_y = my - win.y;
                            }
                            break;
                        }
                    }
                }
            }

            if (!left_down) {
                drag_op = DragOp::None;
                drag_h = NO_TASK;
                if (have_gfx_press && gfx_press_h != NO_TASK &&
                    vnu::wintask::is_running(gfx_press_h)) {
                    vnu::wintask::Console* con = vnu::wintask::console(gfx_press_h);
                    if (con && con->gfx)
                        vnu::wintask::feed_mouse(gfx_press_h, 2, gfx_press_px, gfx_press_py);
                }
                have_gfx_press = false;
            }

            if (drag_op != DragOp::None && drag_h != NO_TASK) {
                Window& win = geom[drag_h];
                vnu::wintask::Console* con = vnu::wintask::console(drag_h);
                switch (drag_op) {
                case DragOp::Move:
                    win.x = mx - drag_off_x;
                    win.y = my - drag_off_y;
                    break;
                case DragOp::ResizeCorner:
                case DragOp::ResizeRight:
                    if (con && con->gfx) {
                        int w = win.w + (mx - rs_mx);
                        if (w < vnu::wintask::GFX_W * GFX_MIN_SCALE + 8)
                            w = vnu::wintask::GFX_W * GFX_MIN_SCALE + 8;
                        if (w > vnu::wintask::GFX_W * GFX_MAX_SCALE + 8)
                            w = vnu::wintask::GFX_W * GFX_MAX_SCALE + 8;
                        gfx_scale[drag_h] = gfx_scale_for(w);
                        win.w = vnu::wintask::GFX_W * gfx_scale[drag_h] + 8;
                    } else {
                        int w = rs_orig_w + (mx - rs_mx);
                        if (w < 20 * CELL_W + 8)
                            w = 20 * CELL_W + 8;
                        if (w > CON_COLS * CELL_W + 8)
                            w = CON_COLS * CELL_W + 8;
                        win.w = w;
                    }
                    if (drag_op == DragOp::ResizeCorner) {
                        if (con && con->gfx) {
                            win.h = vnu::wintask::GFX_H * gfx_scale[drag_h] + 12;
                        } else {
                            int hh = rs_orig_h + (my - rs_my);
                            if (hh < TITLE_H + 3 * CELL_H + 12)
                                hh = TITLE_H + 3 * CELL_H + 12;
                            if (hh > TITLE_H + CON_ROWS * CELL_H + 12)
                                hh = TITLE_H + CON_ROWS * CELL_H + 12;
                            win.h = hh;
                        }
                    }
                    break;
                case DragOp::ResizeBottom:
                    if (con && con->gfx) {
                        win.h = vnu::wintask::GFX_H * gfx_scale[drag_h] + 12;
                    } else {
                        int hh = rs_orig_h + (my - rs_my);
                        if (hh < TITLE_H + 3 * CELL_H + 12)
                            hh = TITLE_H + 3 * CELL_H + 12;
                        if (hh > TITLE_H + CON_ROWS * CELL_H + 12)
                            hh = TITLE_H + CON_ROWS * CELL_H + 12;
                        win.h = hh;
                    }
                    break;
                default:
                    break;
                }
                clamp_to_desktop(win);
            }

            /* Mouse-wheel: scroll the focused text window. */
            if (wheel != 0 && focused != NO_TASK) {
                vnu::wintask::Console* con = vnu::wintask::console(focused);
                const Window& win = geom[focused];
                if (con && !con->gfx && mx >= win.x && mx < win.x + win.w &&
                    my >= win.y && my < win.y + win.h) {
                    if (wheel > 0)
                        ++con->scroll_off;
                    else
                        --con->scroll_off;
                    if (con->scroll_off < 0)
                        con->scroll_off = 0;
                    if (con->scroll_off > con->hist_n)
                        con->scroll_off = con->hist_n;
                }
            }
        }

        /* Redraw. */
        vnu::vgfx::draw_wallpaper();
        draw_icons(apps, app_count);

        for (int i = 0; i < order_len; ++i) {
            TaskHandle h = order[i];
            if (minimized[h])
                continue;
            vnu::wintask::Console* con = vnu::wintask::console(h);
            if (!con)
                continue;
            /* If the app flipped between text and gfx mode, re-fit the
             * window to the new client area (e.g. a gfx app writing to
             * fd 3, or one that never did). */
            bool g = con->gfx;
            if (g != was_gfx[h]) {
                was_gfx[h] = g;
                Window sized = size_app_window(geom[h].x, geom[h].y, con, gfx_scale[h]);
                geom[h].w = sized.w;
                geom[h].h = sized.h;
                clamp_to_desktop(geom[h]);
            }
            const Window& win = geom[h];
            if (con->gfx)
                draw_gfx_window(win, *con, h == focused, gfx_scale[h]);
            else
                draw_console_window(win, *con, h == focused);
        }

        panel_n = layout_panel(panel_btns, focused);
        draw_panel(panel_btns, panel_n);

        vnu::vgfx::draw_cursor(mx, my, vnu::vgfx::COLOR_BLACK);
        vnu::vgfx::present();
    }

exit_gui:
    for (int i = 0; i < order_len; ++i)
        vnu::wintask::close_task(order[i]);
    vnu::vgfx::exit_to_text();
}

} // namespace vnu::gui
