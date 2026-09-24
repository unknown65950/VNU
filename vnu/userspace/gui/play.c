/*
 * play — WAV sound player for the VNU desktop.
 *
 * A GUI app (like calc/picview/clock): the desktop spawns it from the
 * "play" tile, the window is pre-created at 480x340 and the app just
 * draws, flushes and services events through the vgfx library.
 *
 * It lists the clips in /sounds (the kernel mounts three demo WAVs
 * there at boot, one VFS node per file), decodes the selected clip
 * with the self-contained libwav parser (vnu/userspace/libwav/) and
 * streams the raw PCM through the audio_* syscalls (49..56) into the
 * AC'97 DMA ring. audio_write is non-blocking, so the player tops the
 * ring up on every event/heartbeat and never stalls the desktop's
 * cooperative scheduler; the progress bar is driven by audio_pending()
 * (bytes still queued), so position and end-of-track tracking work
 * even though the whole clip is fed to the kernel up front.
 *
 * Session model:
 *   play   — open/arm the engine, reset the ring and feed the clip
 *   pause  — halt the engine where it is (audio_pause)
 *   resume — audio_reset + feed again from the saved position
 *   stop   — close the session entirely (audio_close) and rewind
 * The DMA ring is ~1.5 s of audio, so pressing space stops sound
 * almost immediately; resuming after a pause restarts at the same
 * position (queued-but-unplayed audio is dropped by the reset).
 *
 * Keys: ↑/↓ select clip, Space play/pause, s stop, Esc close.
 * The per-second heartbeat (TICK_BYTE 0x06) keeps the progress bar
 * moving while the window idles.
 */
#include <vlibc/vgfx.h>
#include <vlibc/keys.h>
#include <vlibc/dirent.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/audio.h>
#include "../libwav/wav.h"

#define TICK_BYTE 0x06 /* kernel GUI heartbeat, see vnu/wintask.h */

#define MAX_SOUNDS 12
#define NAME_CAP 28
#define DATA_CAP 65536 /* matches the VFS per-file cap */

/* --- discovered clips -------------------------------------------------- */

static char g_names[MAX_SOUNDS][NAME_CAP];
static int g_n;
static int g_sel;

/* --- the loaded clip --------------------------------------------------- */

static uint8_t g_file[DATA_CAP];
static long g_file_len;
static wav_info g_wav;
static int g_loaded;

/* --- transport state --------------------------------------------------- */

static int g_playing; /* engine armed (playing or paused mid-track) */
static int g_paused;
static long g_fed;   /* file bytes handed to the kernel this session */
static long g_pos;   /* bytes actually played (for the progress bar) */
static int g_hw;     /* audio hardware present */

/* --- string helpers (header-light version) ----------------------------- */

static int s_len(const char* s)
{
    int n = 0;
    while (s && s[n])
        ++n;
    return n;
}

static void s_cat(char* dst, int cap, const char* a, const char* b)
{
    int i = 0;
    for (; a && a[i] && i < cap - 1; ++i)
        dst[i] = a[i];
    int n = i;
    for (; b && b[i - n] && i < cap - 1; ++i)
        dst[i] = b[i - n];
    dst[i] = 0;
}

static void fmt_time(char* out, int cap, long bytes, const wav_info* w)
{
    long rate = (long)w->sample_rate * w->channels * (w->bits_per_sample / 8);
    if (rate == 0) {
        out[0] = 0;
        return;
    }
    long tenths = bytes * 10L / rate;
    long m = tenths / 600, s = (tenths / 10) % 60, t = tenths % 10;
    int n = 0;
    if (m > 99)
        m = 99;
    if (m >= 10) {
        out[n++] = (char)('0' + m / 10);
        out[n++] = (char)('0' + m % 10);
    } else {
        out[n++] = (char)('0' + m);
    }
    out[n++] = ':';
    out[n++] = (char)('0' + s / 10);
    out[n++] = (char)('0' + s % 10);
    out[n++] = '.';
    out[n++] = (char)('0' + t);
    out[n] = 0;
    (void)cap;
}

/* --- loading ----------------------------------------------------------- */

static int load_file(int idx)
{
    char path[64];
    s_cat(path, sizeof(path), "/sounds/", g_names[idx]);
    int fd = open(path, 0); /* O_RDONLY */
    if (fd < 0)
        return -1;
    long total = 0;
    for (;;) {
        long r = read(fd, g_file + total, (unsigned long)(DATA_CAP - (unsigned)total));
        if (r <= 0)
            break;
        total += r;
        if (total >= DATA_CAP)
            break;
    }
    close(fd);
    g_file_len = total;
    if (wav_parse(g_file, (size_t)g_file_len, &g_wav) != 0)
        return -1;
    g_loaded = 1;
    return 0;
}

/* --- transport --------------------------------------------------------- */

static void feed(void)
{
    while (g_fed < (long)g_wav.data_len) {
        long n = audio_write(g_wav.data + g_fed,
                             (uint32_t)((unsigned long)g_wav.data_len - (unsigned long)g_fed));
        if (n <= 0)
            break;
        g_fed += n;
    }
}

static void snd_start(void)
{
    if (g_loaded == 0)
        return;
    if (g_paused) {
        /* resume: drop whatever remained queued, top up from g_fed */
        audio_reset();
        feed();
        g_paused = 0;
        g_playing = 1;
        return;
    }
    if (g_playing)
        return;
    if (audio_open() != 0) {
        g_hw = 0;
        return;
    }
    if (audio_set_fmt(g_wav.sample_rate, g_wav.channels,
                      g_wav.bits_per_sample) != 0) {
        audio_close();
        return;
    }
    g_fed = 0;
    feed();
    g_playing = 1;
    g_paused = 0;
}

static void snd_pause(void)
{
    if (g_playing && !g_paused) {
        audio_pause();
        g_paused = 1;
    }
}

static void snd_stop(void)
{
    if (g_playing || g_paused) {
        audio_close();
        g_playing = 0;
        g_paused = 0;
    }
    g_fed = 0;
    g_pos = 0;
}

static long pending_user(void)
{
    long p = audio_pending();
    if (p < 0)
        return 0;
    /* p is 16-bit stereo output bytes; convert to source-file bytes */
    int in_sz = g_wav.channels * (g_wav.bits_per_sample / 8);
    return p / 4 * in_sz;
}

static void update_pos(void)
{
    if (!g_loaded) {
        g_pos = 0;
        return;
    }
    long pu = g_fed - pending_user();
    if (pu < 0)
        pu = 0;
    if (pu > (long)g_wav.data_len)
        pu = (long)g_wav.data_len;
    g_pos = pu;
    /* natural end of track: everything fed and the ring drained */
    if (g_playing && g_fed >= (long)g_wav.data_len && pending_user() <= 0) {
        g_playing = 0;
        g_paused = 0;
    }
}

/* --- drawing ----------------------------------------------------------- */

#define LIST_X 8
#define LIST_Y 36
#define LIST_W 200
#define LIST_H 236
#define ROW_H 18

#define BTN_X 224
#define BTN_Y 36
#define BTN_W 116
#define BTN_H 26
#define STOP_X 348

/* deterministic pseudo-waveform shown behind the progress bar */
static const uint8_t k_wave[24] = {
    6, 14, 9, 18, 5, 20, 12, 7, 16, 4, 19, 10,
    8, 17, 3, 15, 11, 6, 21, 13, 5, 18, 9, 7,
};

static void draw(void)
{
    vgfx_clear(VGFX_DGRAY);

    /* header */
    vgfx_str8(8, 6, "play - wav player", VGFX_WHITE);

    /* clip list */
    vgfx_str8(8, 26, "sounds", VGFX_LCYAN);
    vgfx_rect(LIST_X, LIST_Y, LIST_W, LIST_H, VGFX_BLACK);
    if (g_n == 0) {
        vgfx_str8(11, 44, "empty /sounds", VGFX_LRED);
    }
    for (int i = 0; i < g_n && i < 12; ++i) {
        int y = LIST_Y + 6 + i * ROW_H;
        if (i == g_sel) {
            vgfx_fill_rect(LIST_X + 2, y - 2, LIST_W - 4, ROW_H - 2, VGFX_LBLUE);
            vgfx_str8(LIST_X + 6, y + 2, g_names[i], VGFX_BLACK);
        } else {
            vgfx_str8(LIST_X + 6, y + 2, g_names[i], VGFX_WHITE);
        }
    }

    /* transport buttons */
    vgfx_rect(BTN_X, BTN_Y, BTN_W, BTN_H, VGFX_BLACK);
    vgfx_fill_rect(BTN_X + 1, BTN_Y + 1, BTN_W - 2, BTN_H - 2, VGFX_LGRAY);
    const char* plabel = g_paused ? "resume" : (g_playing ? "pause" : "play");
    vgfx_str8(BTN_X + (BTN_W - vgfx_text_width8(plabel)) / 2,
              BTN_Y + 6, plabel, VGFX_BLACK);
    vgfx_rect(STOP_X, BTN_Y, BTN_W, BTN_H, VGFX_BLACK);
    vgfx_fill_rect(STOP_X + 1, BTN_Y + 1, BTN_W - 2, BTN_H - 2, VGFX_LGRAY);
    vgfx_str8(STOP_X + (BTN_W - vgfx_text_width8("stop")) / 2,
              BTN_Y + 6, "stop", VGFX_BLACK);

    /* fake waveform visualiser: heights track the play position so the
     * window has some life while a clip plays */
    if (g_loaded) {
        long frac = (g_wav.data_len == 0) ? 0 : g_pos * 256L / g_wav.data_len;
        int base = LIST_Y + 70;
        for (int i = 0; i < 24; ++i) {
            int h = 4 + (int)(k_wave[i] * (32 + frac * 3L / 8)) / 8;
            vgfx_vline(224 + i * 10, base + 48 - h, h, VGFX_LBLUE);
        }
    }

    /* progress bar + times */
    vgfx_rect(224, 84, 240, 12, VGFX_BLACK);
    if (g_loaded) {
        long frac = (g_wav.data_len == 0) ? 0 : g_pos * 240L / g_wav.data_len;
        if (frac > 240)
            frac = 240;
        if (frac > 0)
            vgfx_fill_rect(225, 85, (int)frac, 10, VGFX_GREEN);
    }
    if (g_loaded) {
        char t[40], tt[20];
        fmt_time(t, sizeof(t), g_pos, &g_wav);
        fmt_time(tt, sizeof(tt), (long)g_wav.data_len, &g_wav);
        vgfx_str8(224, 100, t, VGFX_WHITE);
        vgfx_str8(224 + vgfx_text_width8(t) + 14, 100, "/", VGFX_LGRAY);
        vgfx_str8(224 + vgfx_text_width8(t) + 26, 100, tt, VGFX_WHITE);

        char info[48];
        int n = 0;
        /* "22050 Hz / 2ch / 16-bit" */
        long r = g_wav.sample_rate;
        char rb[8];
        int rn = 0;
        do {
            rb[rn++] = (char)('0' + r % 10);
            r /= 10;
        } while (r > 0);
        while (rn)
            info[n++] = rb[--rn];
        info[n++] = ' ';
        info[n++] = 'H';
        info[n++] = 'z';
        info[n++] = ' ';
        info[n++] = '/';
        info[n++] = ' ';
        info[n++] = (char)('0' + g_wav.channels);
        info[n++] = 'c';
        info[n++] = 'h';
        info[n++] = ' ';
        info[n++] = '/';
        info[n++] = ' ';
        char bb[4];
        int bnc = 0;
        long b = g_wav.bits_per_sample;
        do {
            bb[bnc++] = (char)('0' + b % 10);
            b /= 10;
        } while (b > 0);
        while (bnc)
            info[n++] = bb[--bnc];
        info[n++] = '-';
        info[n++] = 'b';
        info[n++] = 'i';
        info[n++] = 't';
        info[n] = 0;
        vgfx_str8(224, 116, info, VGFX_LCYAN);
    }
    if (g_loaded) {
        vgfx_str8(224, 132, g_names[g_sel], VGFX_WHITE);
    }

    /* status strip */
    vgfx_fill_rect(0, VGFX_H - 17, VGFX_W, 17, VGFX_BLACK);
    const char* st;
    if (!g_hw)
        st = "no audio hardware";
    else if (!g_loaded)
        st = "stopped";
    else if (g_paused)
        st = "paused";
    else if (g_playing)
        st = "playing";
    else if (g_pos >= (long)g_wav.data_len && g_wav.data_len > 0)
        st = "finished";
    else
        st = "stopped";
    vgfx_str8(6, VGFX_H - 13, st, VGFX_LGREEN);
    vgfx_str8(6 + vgfx_text_width8(st) + 18, VGFX_H - 13,
              (g_n > 0) ? "1 play  s stop  Esc close" : "no sound clips",
              VGFX_LGRAY);
}

/* --- input ------------------------------------------------------------- */

static void handle_press(int x, int y)
{
    if (x >= BTN_X && x < BTN_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H) {
        if (g_loaded == 0)
            load_file(g_sel);
        if (g_playing && !g_paused)
            snd_pause();
        else
            snd_start();
        return;
    }
    if (x >= STOP_X && x < STOP_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H) {
        snd_stop();
        return;
    }
    if (x >= LIST_X && x < LIST_X + LIST_W && y >= LIST_Y && y < LIST_Y + LIST_H) {
        int idx = (y - (LIST_Y + 6)) / ROW_H;
        if (idx >= 0 && idx < g_n) {
            g_sel = idx;
            g_loaded = 0;
            snd_stop();
            load_file(g_sel);
        }
        return;
    }
}

int main(void)
{
    /* discover /sounds */
    DIR* d = opendir("/sounds");
    if (d) {
        struct dirent* de;
        while ((de = readdir(d)) != 0 && g_n < MAX_SOUNDS) {
            if (de->d_type == 8) { /* regular file */
                int i = 0;
                while (de->d_name[i] && i < NAME_CAP - 1) {
                    g_names[g_n][i] = de->d_name[i];
                    ++i;
                }
                g_names[g_n][i] = 0;
                ++g_n;
            }
        }
        closedir(d);
    }

    /* bare open+close probes for the sound card without touching the
     * DMA engine */
    g_hw = (audio_open() == 0);
    if (g_hw)
        audio_close();

    if (g_n > 0) {
        g_sel = 0;
        load_file(0);
    }

    vgfx_event_t ev;
    for (;;) {
        draw();
        vgfx_flush();
        vgfx_poll(&ev);

        if (ev.type == VGFX_EV_KEY) {
            if (ev.key == VNU_KEY_ESC)
                return 0;
            if (ev.key == VNU_KEY_UP && g_sel > 0)
                --g_sel;
            if (ev.key == VNU_KEY_DOWN && g_sel < g_n - 1)
                ++g_sel;
            if (ev.key == ' ' && g_loaded) {
                if (g_playing && !g_paused)
                    snd_pause();
                else
                    snd_start();
            }
            if ((ev.key == 's' || ev.key == 'S') && g_loaded)
                snd_stop();
        } else if (ev.type == VGFX_EV_PRESS) {
            handle_press(ev.x, ev.y);
        }

        /* heartbeat or any input: top the ring back up and refresh
         * the play position */
        if (g_playing && !g_paused)
            feed();
        update_pos();
    }
}