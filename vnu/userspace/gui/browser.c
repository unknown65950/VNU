/*
 * browser — a small web browser for the VNU desktop.
 *
 * It fetches one address over the socket syscalls and shows what came
 * back as plain text: every <a href="..."> in the body becomes both a run
 * of text on the page and a link that can be followed. That is the whole
 * of what it does — no HTML layout, no CSS, no cookies, no history and no
 * TLS. The point is that a desktop program has a window, a keyboard and
 * a network here, and can use all three at once.
 *
 * Two places for the focus, because that is what an address needs: the
 * bar at the top, and the page below. Tab moves between them, Enter on a
 * link follows it, and Esc goes back to the bar rather than quitting, so
 * a typo in the address is one key from being fixed; a second Esc from
 * the bar closes the window.
 *
 * The bar shows the address being typed and the page shows the address
 * last fetched, and they are kept apart for that reason: an edit that has
 * not been fetched yet is visibly still a proposal. A failed fetch leaves
 * the page where it was and puts the reason above it, rather than
 * blanking the window.
 *
 * Input comes from vgfx_poll(), which is how a windowed program is
 * delivered keys at all. read(0, ...) is the console's byte stream, a
 * windowed task has none of it, and the kernel answers that call with 0 —
 * which is what the first version of this program did, so it drew a
 * window and then sat in it for ever with no way out.
 *
 * Keys:
 *   printable     edit the address     Backspace  delete before the caret
 *   Left Right    move the caret        Delete     delete at the caret
 *   Home End      either end of the address
 *   Tab           bar <-> the page      Enter      fetch / follow a link
 *   Up Down       choose a link
 *   Esc           back to the bar, then close the window
 *   F5            fetch the last address again
 *   Ctrl+C        copy the address being shown
 *   Ctrl+V        paste the clipboard into the address bar
 */
#include <stdint.h>

#include <vlibc/vgfx.h>
#include <vlibc/keys.h>
#include <vlibc/string.h>
#include <vlibc/stdio.h>
#include <vlibc/socket.h>
#include <vlibc/netdb.h>
#include <vlibc/clipboard.h>

#define BAR_H 20
#define PAGE_TOP (BAR_H + 4)
#define LINE_H 16
#define CHAR_W 8

/* How much of one answer is kept. A page this window can show is far
 * smaller; the rest of the transfer is read and dropped so the server
 * sees a clean request that ended rather than a reset connection. */
#define BUF_SZ 65536

/* One syscall's patience. A host that is not answering should not hang
 * the window, and the answer shows as a reason on screen rather than as
 * a program that stopped drawing. */
#define NET_TIMEOUT_MS 5000

/* A page is a flat list of rows of characters plus the links among them,
 * which is all "renders the body as text" needs. Making it a list of
 * rows rather than drawing straight into the window is what lets a
 * selection survive a redraw and lets a resize change nothing. */
#define MAX_ROWS 256
/* Wide enough for the widest window this is opened at, with the NUL:
 * 1280 px at 8 px a character is 160 columns. */
#define ROW_LEN 256

#define MAX_LINKS 128
#define URL_CAP 256

/* Where a link's text is on the page, in rows and columns rather than
 * pixels, plus its address. The position is kept in characters because
 * that is what layout() works in and what survives a resize: the
 * rectangle is worked out at draw time from the rows the window actually
 * has, so a window that is shorter or narrower does not leave every box
 * pointing at the wrong place. */
typedef struct {
    int row, col, len;
    char url[URL_CAP];
} Link;

static char g_url[URL_CAP] = "http://example.com/";
static int  g_caret = (int)sizeof("http://example.com/") - 1;

/* The URL that is actually loaded. Kept apart from g_url so editing the
 * bar does not pretend to have moved: until Enter is pressed the bar is
 * a proposal, and the page below is the one that was fetched. */
static char g_loaded[URL_CAP];

static char g_body[BUF_SZ];
static int  g_body_len;

/* Why there is no page, or nothing. A failure, and it is drawn over
 * what is on screen rather than instead of it. */
static char g_error[128];

/* Something worth saying about a page that did arrive - an https
 * address answered in the clear, which is not what was asked for. It is
 * a note, not a failure, so it does not move the page or claim the last
 * page is all there is. */
static char g_note[128];

static char g_rows[MAX_ROWS][ROW_LEN];
static int  g_nrows;

static Link g_links[MAX_LINKS];
static int  g_nlinks;
/* Index into g_links, or -1 when there is nothing to choose. */
static int  g_sel = -1;
/* 0 = the page, 1 = the bar. The bar has it to start with: there is no
 * page to read yet, and an address is the only thing a new window can
 * be given. */
static int  g_focus = 1;

/* ------------------------------------------------------------------ */
/* URL handling                                                        */
/* ------------------------------------------------------------------ */

/* Split `url` into host, port and path. The scheme is optional, and
 * whether it is http or https makes no difference here: there is no TLS,
 * so both are fetched over a plain connection and fetch() says so on
 * screen when it was https. */
static int parse_url(const char* url, char* host, int hcap, unsigned short* port,
                     char* path, int pcap)
{
    const char* p = url;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    else if (strncmp(p, "https://", 8) == 0)
        p += 8;

    int i = 0;
    while (*p && *p != ':' && *p != '/' && *p != '?' && i < hcap - 1)
        host[i++] = *p++;
    host[i] = 0;
    if (i == 0)
        return -1;

    *port = 80;
    if (*p == ':') {
        ++p;
        int pr = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && pr < 65536) {
            pr = pr * 10 + (*p - '0');
            ++p;
            ++digits;
        }
        /* A port that is not a number at all leaves the default in
         * place rather than becoming port 0, which every connect would
         * fail on and which would be a far less useful message. The
         * characters after the colon are still skipped to the start of
         * the path: leaving `p` on them meant the path came out as "/" and
         * the real one was lost, so "example.com:8080/index.html" with
         * any typo in the port asked the server for the root. */
        if (digits > 0)
            *port = (unsigned short)pr;
        while (*p && *p != '/' && *p != '?')
            ++p;
    }

    i = 0;
    if (*p == '/' || *p == '?') {
        while (*p && i < pcap - 1)
            path[i++] = *p++;
    } else {
        path[i++] = '/';
    }
    path[i] = 0;
    return 0;
}

/* Fetch `url` and leave the answer in g_body. Returns 0, or -1 with a
 * reason in g_error.
 *
 * The previous page survives a failure. g_body is not touched until the
 * request has actually been sent, so a host that refuses, a name that
 * does not resolve or an address with no host in it all leave what was
 * on screen where it is, and only the reason is added to it. Clearing
 * the buffer up front - as the first version did - meant every failure
 * also cost the page, which is the one thing a browser must not do: a
 * mistyped address should not cost you the page you were reading.
 */
static int fetch(const char* url)
{
    char host[128], path[512];
    unsigned short port = 80;
    g_error[0] = 0;
    g_note[0] = 0;

    if (parse_url(url, host, (int)sizeof(host), &port, path, (int)sizeof(path)) != 0) {
        snprintf(g_error, sizeof(g_error), "no host in that address");
        return -1;
    }

    /* An https address is fetched over a plain connection - there is no
     * TLS here - so a server that insists on one will refuse. Saying so
     * first is more use than a bare "connection refused" from a host that
     * is working perfectly well, so the fetch still goes ahead: some
     * servers answer plain requests to an https port. */
    int plain_note = strncmp(url, "https://", 8) == 0;

    uint32_t ip = 0;
    if (gethostbyname(host, &ip) < 0) {
        snprintf(g_error, sizeof(g_error), "cannot find %s", host);
        return -1;
    }

    int s = vnu_socket(VNU_AF_INET, VNU_SOCK_STREAM);
    if (s < 0) {
        snprintf(g_error, sizeof(g_error), "no socket");
        return -1;
    }
    if (vnu_connect(s, ip, port, NET_TIMEOUT_MS) < 0) {
        vnu_netclose(s);
        snprintf(g_error, sizeof(g_error), "%s:%u refused", host, (unsigned)port);
        return -1;
    }

    char req[1024];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\n"
                      "User-Agent: vnu-browser\r\nConnection: close\r\n\r\n",
                      path, host);
    int sent = 0;
    while (sent < rl) {
        int w = vnu_send(s, req + sent, (uint32_t)(rl - sent), NET_TIMEOUT_MS);
        if (w <= 0)
            break;
        sent += w;
    }
    if (sent < rl) {
        vnu_netclose(s);
        snprintf(g_error, sizeof(g_error), "could not send the request");
        return -1;
    }

    /* Read to the buffer's end and no further. A page bigger than the
     * window shows as much as fits, which is what any browser does;
     * what it must not do is walk off the end of the buffer, so the
     * room for the NUL is reserved here rather than at the end. */
    g_body_len = 0;
    while (g_body_len < BUF_SZ - 1) {
        int r = vnu_recv(s, g_body + g_body_len,
                         (uint32_t)(BUF_SZ - 1 - g_body_len), NET_TIMEOUT_MS);
        if (r <= 0)
            break;
        g_body_len += r;
    }
    g_body[g_body_len] = 0;
    vnu_netclose(s);

    if (g_body_len == 0) {
        snprintf(g_error, sizeof(g_error), "no answer from %s", host);
        return -1;
    }
    if (plain_note)
        snprintf(g_note, sizeof(g_note),
                 "no TLS here: %s answered over a plain connection", host);
    else
        g_note[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Turning the body into rows and links                                */
/* ------------------------------------------------------------------ */

/* The body starts after the header block; find the blank line that ends
 * it. A response with no blank line is returned whole, because a server
 * that answers with only a body is still showing something. */
static char* skip_headers(char* s)
{
    for (char* p = s; *p; ++p) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
            return p + 4;
        if (p[0] == '\n' && p[1] == '\n')
            return p + 2;
    }
    return s;
}

/* Find `needle` in `s`, NUL-safe at both ends. */
static char* find_str(char* s, const char* needle)
{
    unsigned long nl = strlen(needle);
    if (!nl)
        return 0;
    for (char* p = s; *p; ++p) {
        unsigned long i = 0;
        while (i < nl && p[i] && p[i] == needle[i])
            ++i;
        if (i == nl)
            return p;
    }
    return 0;
}

static char* find_char(char* s, char c)
{
    for (; *s; ++s)
        if (*s == c)
            return s;
    return 0;
}

/* Lay the body out into g_rows, recording a link for every <a href>.
 *
 * The markup is handled rather than parsed. A tag is skipped whole, from
 * '<' to '>' - skipping only the two angle brackets would leave every
 * tag's name on the page as text, which is what the first version of
 * this did and why "<h1>Title</h1>" came out as "h1Title/h1". The
 * entities a text page uses become their one character, and what is left
 * between the tags lands in the current row. That is the whole grammar
 * of the pages this shows, and anything richer would be a lie about
 * what it does.
 *
 * Every row is NUL-terminated as it is written. Terminating only the
 * last one leaves every other row unterminated, and drawing one of them
 * then runs off the end of the row into whatever follows.
 */
static void layout(void)
{
    for (int i = 0; i < MAX_ROWS; ++i)
        g_rows[i][0] = 0;
    g_nrows = 1;
    g_nlinks = 0;

    char* p = g_body_len ? skip_headers(g_body) : 0;
    if (!p)
        return;

    int row = 0, col = 0;
    int done = 0;

    /* Put `len` bytes of `text` in the current row, starting a new one
     * whenever the current one is full, and stop filling rows once there
     * are none left. The NUL at the end of each row is written as the
     * row is left, so every row is terminated even the ones the loop
     * ends in the middle of. */
    #define EMIT(text, len) do {                                             \
            const char* _t = (text);                                         \
            int _n = (len);                                                  \
            for (int _i = 0; _i < _n && !done; ++_i) {                       \
                if (col >= ROW_LEN - 1) {                                    \
                    g_rows[row][col] = 0;                                    \
                    if (++row >= MAX_ROWS) { done = 1; break; }              \
                    col = 0;                                                 \
                }                                                            \
                g_rows[row][col++] = _t[_i];                                 \
            }                                                                \
        } while (0)

    /* The end of a row, wherever the row happened to end. */
    #define END_ROW() do {                                                    \
            g_rows[row][col] = 0;                                            \
            if (++row >= MAX_ROWS) { done = 1; }                             \
            else { col = 0; }                                                \
        } while (0)

    while (*p && !done) {
        if (p[0] == '<') {
            /* An <a href="..."> is a link: its text goes on the page
             * like any other text and its address is remembered, so a
             * later Enter can be turned back into a fetch. */
            if ((p[1] == 'a' || p[1] == 'A') &&
                (p[2] == ' ' || p[2] == '\t' || p[2] == '>')) {
                char* href = find_str(p, "href=\"");
                if (href) {
                    href += 6;
                    char* q = find_char(href, '"');
                    /* The rest of the opening tag, up to its '>' - which
                     * has to be stepped over either way, or the '>' ends
                     * up on the page as text. */
                    char* gt = q ? find_char(q, '>') : 0;
                    if (q && gt) {
                        int hl = (int)(q - href);
                        char* close = find_str(gt, "</a>");
                        int tl = close ? (int)(close - (gt + 1)) : 0;
                        if (hl > 0 && hl < URL_CAP && tl > 0 && tl < ROW_LEN) {
                            if (g_nlinks < MAX_LINKS) {
                                memcpy(g_links[g_nlinks].url, href,
                                       (unsigned long)hl);
                                g_links[g_nlinks].url[hl] = 0;
                                g_links[g_nlinks].row = row;
                                g_links[g_nlinks].col = col;
                                g_links[g_nlinks].len = tl;
                                ++g_nlinks;
                            }
                            EMIT(gt + 1, tl);
                            p = close + 4;
                            continue;
                        }
                        /* A link this program cannot follow - no address,
                         * or an <a> whose </a> never comes. The opening
                         * tag is stepped over and what follows is laid out
                         * as ordinary text: the page stays readable and
                         * nothing claims to be selectable that is not
                         * there. */
                        p = gt + 1;
                        continue;
                    }
                }
            }

            /* Any other tag: skip to the '>' and take the whole thing
             * with it. A <br> is the one that means something - it is a
             * line break in every page there is. */
            char* gt = find_char(p, '>');
            if (!gt)
                break;      /* an unterminated tag: nothing after it is text */
            int is_br = (p[1] == 'b' || p[1] == 'B') &&
                        (p[2] == 'r' || p[2] == 'R');
            p = gt + 1;
            if (is_br)
                END_ROW();
            continue;
        }

        if (p[0] == '&') {
            /* The entities a text-only page actually uses. An
             * unrecognised one is dropped, since printing "frobnicate"
             * on the page would be a worse lie than a space. */
            char c = 0;
            if (strncmp(p, "&amp;", 5) == 0) { c = '&'; p += 5; }
            else if (strncmp(p, "&lt;", 4) == 0) { c = '<'; p += 4; }
            else if (strncmp(p, "&gt;", 4) == 0) { c = '>'; p += 4; }
            else if (strncmp(p, "&nbsp;", 6) == 0) { c = ' '; p += 6; }
            else {
                while (*p && *p != ';')
                    ++p;
                if (*p == ';')
                    ++p;
                continue;
            }
            EMIT(&c, 1);
            continue;
        }

        if (p[0] == '\r') {
            ++p;
            continue;
        }
        if (p[0] == '\n') {
            ++p;
            END_ROW();
            continue;
        }

        EMIT(p, 1);
        ++p;
    }

    /* The row the loop ended in is terminated here; the ones it ended
     * are were terminated as they were left. */
    if (!done)
        g_rows[row][col] = 0;
    g_nrows = row + 1;
    #undef EMIT
    #undef END_ROW
}

/* Is `url` absolute, or does it have to be resolved against the page
 * that is showing? */
static int is_absolute(const char* url)
{
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

/* Resolve a link against the page it was found on, so a relative href on
 * a real page leads somewhere instead of failing to parse. Only what a
 * page's own address makes obvious is handled: a root-relative path
 * takes the scheme and host, and a bare name is taken relative to the
 * directory the current path names. */
static void resolve(const char* base, const char* href, char* out, int cap)
{
    if (is_absolute(href) || href[0] == 0) {
        snprintf(out, (unsigned long)cap, "%s", href);
        return;
    }

    /* parse_url() hands back the host, not the scheme, so the scheme is
     * taken off the front of the base by hand and the rest is parsed. */
    char scheme[16];
    const char* b = base;
    if (strncmp(b, "http://", 7) == 0) {
        snprintf(scheme, sizeof(scheme), "http");
        b += 7;
    } else if (strncmp(b, "https://", 8) == 0) {
        snprintf(scheme, sizeof(scheme), "https");
        b += 8;
    } else {
        snprintf(scheme, sizeof(scheme), "http");
    }

    char host[128], path[512];
    unsigned short port = 80;
    if (parse_url(b, host, (int)sizeof(host), &port, path, (int)sizeof(path)) != 0)
        return;

    if (href[0] == '/') {
        if (port == 80)
            snprintf(out, (unsigned long)cap, "%s://%s%s", scheme, host, href);
        else
            snprintf(out, (unsigned long)cap, "%s://%s:%u%s", scheme, host, (unsigned)port, href);
        return;
    }

    /* Relative: the new address is the directory the current path names
     * plus the href, so the last segment of the path is replaced. The
     * slash to cut at is the *last* one - cutting at the first would
     * make every link on a page under /a/b/ point at /other instead of
     * /a/b/other, which is where the first version of this went wrong.
     */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    for (int i = (int)strlen(dir) - 1; i >= 0; --i) {
        if (dir[i] == '/') {
            dir[i + 1] = 0;
            break;
        }
        if (i == 0)
            snprintf(dir, sizeof(dir), "/");
    }
    if (!dir[0])
        snprintf(dir, sizeof(dir), "/");
    if (port == 80)
        snprintf(out, (unsigned long)cap, "%s://%s%s%s", scheme, host, dir, href);
    else
        snprintf(out, (unsigned long)cap, "%s://%s:%u%s%s", scheme, host, (unsigned)port, dir, href);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static int page_top(void)
{
    /* Where row 0 of the page is drawn; every row is this plus
     * n * LINE_H. The links carry rows rather than pixels so that this
     * can move without every selection box moving the wrong way with it.
     *
     * Both messages are drawn at PAGE_TOP, so the page starts below
     * whichever is showing: two lines for a failure (the reason and what
     * is still on screen), one for a note. With no page there is nothing
     * to move out of the way. */
    int lines = g_error[0] ? 2 : (g_note[0] ? 1 : 0);
    return PAGE_TOP + (g_body_len ? lines * LINE_H : 0);
}

static void draw(void)
{
    /* The canvas, not the display: the browser is a window, and a window
     * that laid itself out from the screen's size would draw its bar
     * across the whole desktop and its page down past its own bottom. */
    int w = 0, h = 0;
    vgfx_size(&w, &h);

    vgfx_clear(VGFX_BLACK);
    vgfx_fill_rect(0, 0, w, BAR_H, VGFX_DGRAY);

    /* What the bar shows is the proposal, so an edit that has not been
     * fetched yet is visibly an edit. */
    char line[URL_CAP + 4];
    snprintf(line, sizeof(line), "> %s", g_url);
    vgfx_str(4, 2, line, VGFX_WHITE);
    if (g_focus == 1) {
        int cx = 4 + CHAR_W * g_caret;
        vgfx_rect(cx - 1, 1, 9, LINE_H + 1, VGFX_YELLOW);
    }

    /* A failure is drawn over the page, not instead of it: the reason on
     * top, the page still underneath. With nothing loaded there is no
     * page, so the reason is all there is to show.
     *
     * A note is one line, and does not move the page: it is about a page
     * that did arrive, so the page is what the window is showing. */
    if (g_error[0]) {
        vgfx_str(4, PAGE_TOP, g_error, VGFX_LRED);
        if (g_body_len) {
            char hint[URL_CAP + 4];
            snprintf(hint, sizeof(hint), "still showing %s",
                     g_loaded[0] ? g_loaded : "the last page");
            vgfx_str(4, PAGE_TOP + LINE_H, hint, VGFX_LGRAY);
        }
        /* Fall through: the page below is still what is on screen. */
    } else if (g_note[0]) {
        vgfx_str(4, PAGE_TOP, g_note, VGFX_LCYAN);
    }

    if (!g_body_len) {
        if (!g_error[0])
            vgfx_str(4, PAGE_TOP, "Type an address and press Enter.",
                     VGFX_LGRAY);
        return;
    }

    int top = page_top();
    int rows = (h - top) / LINE_H;
    if (rows < 0)
        rows = 0;             /* a window shorter than the bar alone */
    if (rows > g_nrows)
        rows = g_nrows;
    for (int r = 0; r < rows; ++r) {
        if (!g_rows[r][0])
            continue;
        vgfx_str(4, top + r * LINE_H, g_rows[r], VGFX_WHITE);
    }

    /* The selected link, so it is clear what Enter is about to fetch.
     * Its box is worked out from the row and column it was laid out at,
     * and is clipped to the window - a link whose text runs off the
     * right edge still gets a box around the part that is on screen,
     * and one below the fold gets none rather than one drawn over the
     * taskbar. */
    if (g_focus == 0 && g_sel >= 0 && g_sel < g_nlinks) {
        const Link* l = &g_links[g_sel];
        if (l->row < rows) {
            int x = 4 + CHAR_W * l->col;
            int avail = (w - x) / CHAR_W;
            int len = l->len < avail ? l->len : avail;
            if (len > 0)
                vgfx_rect(x - 2, top + l->row * LINE_H - 2,
                          len * CHAR_W + 4, LINE_H + 2, VGFX_YELLOW);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

/* Fetch `url` and lay out the answer.
 *
 * The bar is not touched: what is loaded and what is being typed are two
 * different things, and only the first one should jump because the
 * second one changed.
 *
 * g_loaded is recorded only once the page is really here. It is what the
 * window names when a fetch fails and the reason is drawn over the page
 * that stayed, so it has to keep naming the address that is actually on
 * screen - setting it first would leave the window saying "still showing
 * <the address that just failed>".
 */
static void go(const char* url)
{
    /* Copied out of `url` before the fetch, which may be reading from
     * g_loaded itself (F5 does exactly that), and written back into it
     * afterwards. */
    char loaded[URL_CAP];
    snprintf(loaded, sizeof(loaded), "%s", url);

    if (fetch(url) != 0)
        return;             /* the old page is still laid out; leave it */
    snprintf(g_loaded, sizeof(g_loaded), "%s", loaded);
    layout();
    g_sel = g_nlinks ? 0 : -1;
}

/* F5: fetch again what is on screen. With nothing loaded yet there is
 * nothing to fetch again, so it fetches what the bar is proposing. */
static void refetch(void)
{
    go(g_loaded[0] ? g_loaded : g_url);
}

/* ------------------------------------------------------------------ */
/* Keys                                                                */
/* ------------------------------------------------------------------ */

/* Everything the address bar is edited with, so the bounds live in one
 * place: a caret is a position in the string, which is never past its
 * end, and a character is only added when there is room for it and its
 * NUL. */
static void bar_insert(char c)
{
    int len = (int)strlen(g_url);
    if (len + 2 > (int)sizeof(g_url))
        return;                     /* full: leave it as it is */
    if (g_caret > len)
        g_caret = len;
    for (int i = len; i > g_caret; --i)
        g_url[i] = g_url[i - 1];
    g_url[g_caret++] = c;
    g_url[len + 1] = 0;
}

static void bar_erase(void)
{
    int len = (int)strlen(g_url);
    if (g_caret >= len)
        return;
    for (int i = g_caret; i < len; ++i)
        g_url[i] = g_url[i + 1];
    g_url[len] = 0;
}

/* One key with the focus on the address bar. */
static void bar_key(unsigned char k)
{
    switch (k) {
        case '\n':
        case '\r':
            go(g_url);
            return;
        case '\t':
            g_focus = 0;
            g_sel = g_nlinks ? 0 : -1;
            return;
        case VNU_KEY_LEFT:
            if (g_caret > 0)
                --g_caret;
            return;
        case VNU_KEY_RIGHT:
            if (g_caret < (int)strlen(g_url))
                ++g_caret;
            return;
        case VNU_KEY_HOME:
            g_caret = 0;
            return;
        case VNU_KEY_END:
            g_caret = (int)strlen(g_url);
            return;
        case VNU_KEY_DEL:
            bar_erase();
            return;
        case 8:                     /* Backspace */
            /* At the start of the string there is nothing before the
             * caret, so nothing is erased at all - not the first
             * character, which is what erasing the caret's own
             * character would do here. */
            if (g_caret == 0)
                return;
            --g_caret;
            bar_erase();
            return;
        default:
            break;
    }
    /* A printable character. Anything below a space is a control code
     * and nothing above DEL is a character, so neither is typed. */
    if (k >= 32 && k < 127)
        bar_insert((char)k);
}

/* One key with the focus on the page: moving between links, following
 * one, and getting back to the bar. */
static void page_key(unsigned char k)
{
    /* With no links there is nothing to move through, so every key that
     * would have moved the selection is a way back to the bar. */
    if (g_nlinks == 0) {
        if (k == '\t' || k == '\n' || k == ' ' || k == VNU_KEY_DOWN ||
            k == VNU_KEY_UP)
            g_focus = 1;
        return;
    }

    switch (k) {
        case '\t':
            g_focus = 1;
            return;
        case '\n':
            if (g_sel >= 0 && g_sel < g_nlinks) {
                char next[URL_CAP];
                resolve(g_loaded, g_links[g_sel].url, next, (int)sizeof(next));
                snprintf(g_url, sizeof(g_url), "%s", next);
                go(g_url);
            } else {
                g_focus = 1;
            }
            return;
        case VNU_KEY_DOWN:
            g_sel = (g_sel + 1) % g_nlinks;
            return;
        case VNU_KEY_UP:
            g_sel = (g_sel - 1 + g_nlinks) % g_nlinks;
            return;
        case VNU_KEY_HOME:
            g_sel = 0;
            return;
        case VNU_KEY_END:
            g_sel = g_nlinks - 1;
            return;
        default:
            return;
    }
}

int main(void)
{
    for (;;) {
        draw();
        vgfx_flush();

        vgfx_event_t ev;
        if (!vgfx_poll(&ev))
            continue;

        /* A resize needs no work of its own: the page is laid out in
         * characters, so only the number of rows the window can show
         * changes, and draw() reads that from the resolution each pass.
         * The link rectangles move with it because they were computed
         * from the same rows. */
        if (ev.type != VGFX_EV_KEY)
            continue;

        unsigned char k = ev.key;

        if (k == VNU_KEY_ESC) {
            if (g_focus == 0) {
                g_focus = 1;
                continue;
            }
            /* Esc from the bar leaves, which is the only way out: a
             * windowed program that has consumed Esc as "go back" has to
             * have a second one, or the window can never be closed. */
            return 0;
        }
        if (k == VNU_KEY_F5) {
            refetch();
            continue;
        }

        /* The clipboard, on the address: Ctrl+C copies what is shown,
         * Ctrl+V pastes into the bar and moves the focus there so the
         * pasted text is where it can be edited and fetched. Control
         * bytes never reach the bar as characters, so this is the only
         * way they can mean anything. */
        if (k == 0x03) {
            vclip_set(g_url, (unsigned long)strlen(g_url));
            snprintf(g_note, sizeof(g_note), "address copied");
            continue;
        }
        if (k == 0x16) {
            char buf[256];
            long n = vclip_get(buf, sizeof(buf));
            for (long i = 0; i < n; ++i)
                if (buf[i] >= 32 && buf[i] < 127)
                    bar_insert((char)buf[i]);
            g_focus = 1;
            continue;
        }

        if (g_focus == 1)
            bar_key(k);
        else
            page_key(k);
    }
    return 0;
}