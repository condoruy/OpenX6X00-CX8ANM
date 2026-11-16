/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00: boot animation and boot sound
 *
 *  The boot logo composes itself on the framebuffer while the radio boots.
 *  The kernel logo is the first frame (the plain background), then every
 *  logo pixel fades in from its own start time: the ring grows out from its
 *  centre, the signal runs through it left to right and out of the gap, the
 *  name sweeps in, the rule draws from the centre, the subtitle last.
 *  It ends on the full logo, which stays until the GUI takes the screen.
 *
 *  The owner's own picture (/mnt/bootlogo.png on the DATA partition, from the
 *  web configurator's Display page; updates leave it there) is shown in its
 *  place and fades in as a whole: the steps above are made for our logo. One
 *  that is missing, damaged or not 800x480 gives ours, never a dark start.
 *
 *  With Settings > Audio > Boot sound on (read from the GUI's params.db),
 *  a satellite link sound plays on the speaker with the animation (CX8ANM:
 *  hiss, data bursts, the lock sweep and the "connected" chime; was "CQ CQ").
 *
 *  CX8ANM: the station's callsign (Settings, params.db) shows at the top of
 *  the logo, the last thing to fade in; on the owner's own picture too.
 *
 *  Started in the background by /etc/init.d/S00bootanim. Any failure just
 *  skips that part: the boot never waits for it.
 */

#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/fb.h>
#include <alsa/asoundlib.h>
/* The base, through the control library of the radio this is built for (BOARD_X6200 from CMake) */
#ifdef BOARD_X6200
#include <aether_radio/x6200_control/control.h>
#define base_control_init   x6200_control_init
#define base_rxvol_set      x6200_control_rxvol_set
#define base_record_set     x6200_control_record_set
#else
#include <aether_radio/x6100_control/control.h>
#define base_control_init   x6100_control_init
#define base_rxvol_set      x6100_control_rxvol_set
#define base_record_set     x6100_control_record_set
#endif
#include <math.h>
#include <png.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "callfont.h"

#define W        800    /* the logo, landscape */
#define H        480
#define SLOW(ms) ((ms) * 28 / 19)   /* the 1.9 s of steps played over 2.8 s */
#define FADE_MS  SLOW(180)          /* each pixel fades in over this */
#define FRAME_MS 40     /* 25 frames per second */
#define CX       400    /* the ring's centre (tools/logo/mklogo.py) */
#define CY       143
#define GUI_UP   "/tmp/x6100_gui.drm"   /* written by the GUI once it owns the display */
#define OWN_LOGO "/mnt/bootlogo.png"    /* the owner's own (webserver apps.py, Boot picture) */
#define OUR_LOGO "/usr/share/x6100/bootlogo.png"   /* ours, boot/logo.png: every update puts it back */
#define OWN_FADE_MS SLOW(1900)          /* an own picture fades in as a whole, over our steps' 2.8 s */

static volatile sig_atomic_t stop;      /* SIGTERM from S49bootanim: the codec is needed, the sound stops */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* ---- Boot sound ----------------------------------------------------------- */

#ifndef PARAMS_DB
#define PARAMS_DB   "/mnt/params.db"
#endif
#define AUDIO_BUSY  "/tmp/x6100-bootanim.audio" /* while the codec is open: S49bootanim waits */
#define RATE        44100
#define TONE_HZ     600
#define DIT_MS      60        /* 20 WPM */
#define EDGE_MS     5         /* raised-cosine keying edges */
#define AMPL        20675     /* -4 dBFS */
#define LEAD_MS     1500      /* silence first: the base opens its speaker path late (400 ms still cut the first C) */
#define MIN_VOL     28        /* AF for the sound when the saved VOL (0..55) is lower */
#define VOL_DEFAULT 30        /* p_volume's default, when the DB has none */
#ifdef BOARD_X6200
#define VOL_MAX     100       /* the volume's range (board.h BOARD_VOL_MAX) */
#else
#define VOL_MAX     55
#endif
#define SOUND_MAX_MS 5700     /* the link sound is 4.7 s with the lead: the sound never runs longer than this */

/* The base unit, through the GUI's own control library: base_control_init()
 * sends every register with the library's defaults (RX, PTT off), as the GUI
 * does first thing at its start; voice_rec alone then makes the base play the
 * codec on the speaker. The GUI repeats base_control_init() when it starts */
static int user_vol;

/* The CW key line held up (1) while the base is ours, as the GUI sets it before
 * base_control_init() (radio.c). Only this line: the library's x6100_gpio_init()
 * would also drive the backlight, USB and WiFi lines. Released at the end, so the
 * GUI can claim it */
#define KEY_CHIP "gpiochip1"
#define KEY_LINE 203                  /* X6100_PIN_MORSE_KEY */
static struct gpiod_chip *key_chip;
static struct gpiod_line *key_line;

static void key_release(void) {
    if (key_line) gpiod_line_release(key_line);
    if (key_chip) gpiod_chip_close(key_chip);
    key_line = NULL;
    key_chip = NULL;
}

static int key_up(void) {
    struct gpiod_line *l;

    if (!(key_chip = gpiod_chip_open_by_name(KEY_CHIP))) return 0;
    if (!(l = gpiod_chip_get_line(key_chip, KEY_LINE)) || gpiod_line_request_output(l, "x6100-bootanim", 1) != 0) {
        key_release();
        return 0;
    }
    key_line = l;
    return 1;
}
static int16_t cq[RATE * 6];  /* the boot sound, lead included */
static int     cq_len;

/* A setting as saved by the GUI (def when it has none), -1 when the DB can't be read */
static int saved(const char *name, int def) {
    sqlite3      *db;
    sqlite3_stmt *st;
    int           val = -1;

    if (sqlite3_open_v2(PARAMS_DB, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db, "SELECT val FROM params WHERE name = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        val = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : def;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return val;
}

/* CX8ANM: a satellite link coming up, 3.2 s after the lead: the hiss of the downlink rising with a faint
 * carrier drifting (Doppler), the handshake's data bursts (two-tone FSK), the lock sweep and the chime */
static uint32_t rnd = 0x6100cafe;
static float noise(void) {
    rnd = rnd * 1664525u + 1013904223u;
    return (float)(int32_t)rnd / 2147483648.0f;
}

static float env_in_out(int i, int len, int edge) {
    if (i < edge) return 0.5f - 0.5f * cosf(M_PI * i / edge);
    if (i >= len - edge) return 0.5f - 0.5f * cosf(M_PI * (len - i) / edge);
    return 1.0f;
}

static void link_sound(void) {
    const int base = RATE * LEAD_MS / 1000, total = RATE * 3200 / 1000;
    static float mix[RATE * 4];
    float ph = 0, lp = 0;

    memset(mix, 0, sizeof(mix));
    /* 1. the hiss, filtered, up over 0.5 s and down at the end; a carrier drifting 1750 -> 1950 Hz under it */
    for (int i = 0; i < total; i++) {
        float t  = (float)i / RATE;
        float up = t < 0.5f ? t / 0.5f : 1.0f;
        float dn = t > 2.4f ? fmaxf(0.0f, 1.0f - (t - 2.4f) / 0.8f) : 1.0f;
        lp += 0.35f * (noise() - lp);
        mix[i] += 0.16f * lp * up * dn;
        if (t < 1.7f) {
            ph += 2.0f * M_PI * (1750.0f + 200.0f * t / 1.7f) / RATE;
            mix[i] += 0.10f * sinf(ph) * up * env_in_out(i, RATE * 17 / 10, RATE / 20);
        }
    }
    /* 2. the handshake: 0.55 .. 1.6 s, 26 bursts of 1200 / 2200 Hz, 28 ms each, in groups */
    static const char pattern[] = "1011 0110 1001 1101 0011 1010 01";
    int at = RATE * 55 / 100;
    for (const char *c = pattern; *c; c++) {
        int len = RATE * 28 / 1000;
        if (*c == ' ') {
            at += RATE * 35 / 1000;
            continue;
        }
        float f = *c == '1' ? 2200.0f : 1200.0f;
        for (int i = 0; i < len; i++)
            mix[at + i] += 0.42f * sinf(2.0f * M_PI * f * i / RATE) * env_in_out(i, len, RATE * 3 / 1000);
        at += len;
    }
    /* 3. the lock sweep: 1.65 .. 2.15 s, 500 -> 2600 Hz, rising */
    ph      = 0;
    at      = RATE * 165 / 100;
    int len = RATE / 2;
    for (int i = 0; i < len; i++) {
        float x = (float)i / len;
        ph += 2.0f * M_PI * (500.0f + 2100.0f * x * x) / RATE;
        mix[at + i] += 0.40f * sinf(ph) * env_in_out(i, len, RATE / 50);
    }
    /* 4. connected: two bell tones, 2.25 s and 2.45 s, 880 then 1320 Hz with a soft overtone, decaying */
    for (int k = 0; k < 2; k++) {
        float f = k ? 1320.0f : 880.0f;
        at      = RATE * (225 + 20 * k) / 100;
        len     = total - at;
        for (int i = 0; i < len; i++) {
            float t = (float)i / RATE;
            float e = expf(-t * 3.2f) * (i < RATE / 200 ? (float)i / (RATE / 200) : 1.0f);
            mix[at + i] += 0.48f * e * (sinf(2.0f * M_PI * f * t) + 0.25f * sinf(4.0f * M_PI * f * t));
        }
    }
    cq_len = base;
    memset(cq, 0, base * sizeof(cq[0]));
    for (int i = 0; i < total; i++) {
        float v = mix[i] * AMPL * 1.3f;
        cq[base + i] = (int16_t)fmaxf(-32000.0f, fminf(32000.0f, v));
    }
    cq_len = base + total + RATE * DIT_MS / 1000;   /* and one dit of silence, as the CW had */
}

/* The codec as PulseAudio runs it: playback and capture together, stereo. Opened
 * non-blocking: nothing here ever waits on it, and the whole sound has a deadline */
static snd_pcm_t *play_pcm, *rec_pcm;
static int64_t    deadline;

static int pcm_open(snd_pcm_t **p, snd_pcm_stream_t dir) {
    if (snd_pcm_open(p, "plughw:0,0", dir, SND_PCM_NONBLOCK) < 0) {
        *p = NULL;
        return 0;
    }
    return snd_pcm_set_params(*p, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, RATE, 1, 100000) == 0;
}

static void pcm_close(void) {
    if (play_pcm) { snd_pcm_drop(play_pcm); snd_pcm_close(play_pcm); }
    if (rec_pcm)  { snd_pcm_drop(rec_pcm);  snd_pcm_close(rec_pcm); }
    play_pcm = rec_pcm = NULL;
}

/* 100 ms of silence in: the codec runs when it has played 20 ms of it within 300 ms */
static int codec_runs(void) {
    static int16_t z[RATE / 10 * 2];
    snd_pcm_sframes_t d, k;

    snd_pcm_start(rec_pcm);
    if ((k = snd_pcm_writei(play_pcm, z, RATE / 10)) <= 0) return 0;
    for (int64_t until = now_ms() + 300; now_ms() < until; usleep(10000)) {
        snd_pcm_readi(rec_pcm, z, RATE / 100);          /* capture read and thrown away */
        if (snd_pcm_delay(play_pcm, &d) == 0 && d < k - RATE / 50) return 1;
    }
    return 0;
}

static void *sound_thread(void *arg) {
    static int16_t st[441 * 2], sink[441 * 2];
    snd_pcm_sframes_t d;
    int at = 0;

    (void)arg;
    while (at < cq_len && !stop && now_ms() < deadline && access(GUI_UP, F_OK) != 0) {
        int n = cq_len - at < 441 ? cq_len - at : 441;
        for (int i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = cq[at + i];
        snd_pcm_readi(rec_pcm, sink, 441);
        snd_pcm_sframes_t k = snd_pcm_writei(play_pcm, st, n);
        if (k > 0) at += k;
        else if (k == -EAGAIN) usleep(5000);
        else if (snd_pcm_recover(play_pcm, k, 1) < 0) break;
    }
    while (!stop && now_ms() < deadline && snd_pcm_delay(play_pcm, &d) == 0 && d > 0) {
        snd_pcm_readi(rec_pcm, sink, 441);
        usleep(10000);
    }
    pcm_close();
    return NULL;
}

/* True when the sound started: codec running, base in play mode */
static int sound_start(pthread_t *th) {
    if (saved("boot_sound", 0) != 1 || saved("speaker_off", 0) == 1) return 0;   /* Speaker always off: a remote station */
    user_vol = saved("vol", VOL_DEFAULT);
    if (user_vol < 0 || user_vol > VOL_MAX) return 0;
    close(open(AUDIO_BUSY, O_CREAT | O_WRONLY, 0644));
    /* The codec levels as the GUI sets them (audio.c audio_mixer_setup); ALSA restores the rest later */
    system("alsactl restore >/dev/null 2>&1; amixer -q sset 'Headphone',0 63,63 >/dev/null 2>&1; "
           "amixer -q sset 'AIF1 DA0',0 160,160 >/dev/null 2>&1");
    if (!pcm_open(&play_pcm, SND_PCM_STREAM_PLAYBACK) || !pcm_open(&rec_pcm, SND_PCM_STREAM_CAPTURE) ||
        !codec_runs()) {
        goto no_sound;                       /* the base is not touched */
    }
    if (access(GUI_UP, F_OK) == 0) goto no_sound;
    if (!key_up()) goto no_sound;
    if (!base_control_init()) goto no_sound;   /* once: no retries */
    base_rxvol_set(user_vol < MIN_VOL ? MIN_VOL : user_vol);
    base_record_set(true);
    link_sound();
    deadline = now_ms() + SOUND_MAX_MS;
    if (pthread_create(th, NULL, sound_thread, NULL) == 0) return 1;
    base_record_set(false);
    base_rxvol_set(user_vol);
no_sound:
    key_release();
    pcm_close();
    unlink(AUDIO_BUSY);
    return 0;
}

/* Codec closed (by the thread), play mode off, the user's AF back; nothing once the GUI is up */
static void sound_end(pthread_t th) {
    pthread_join(th, NULL);
    if (access(GUI_UP, F_OK) != 0) {
        base_record_set(false);
        base_rxvol_set(user_vol);
    }
    key_release();
    unlink(AUDIO_BUSY);
}

/* ---- The callsign (CX8ANM) ------------------------------------------------ */

#define CALL_Y 14       /* its top, over the ring (which starts at y 48) */

/* A text setting as saved by the GUI, "" when it has none */
static void saved_text(const char *name, char *out, size_t size) {
    sqlite3      *db;
    sqlite3_stmt *st;

    *out = 0;
    if (sqlite3_open_v2(PARAMS_DB, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db, "SELECT val FROM params WHERE name = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
            snprintf(out, size, "%s", (const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

static int glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    for (size_t i = 0; i < sizeof(callfont) / sizeof(callfont[0]); i++)
        if (callfont[i].c == c) return (int)i;
    return -1;
}

/* The callsign drawn into the logo, centred at the top, gold with a dark edge so it reads on any picture */
static void draw_call(uint8_t *logo) {
    char call[16];
    int  w = 0;

    saved_text("callsign", call, sizeof(call));
    for (char *c = call; *c; c++) {
        int g = glyph(*c);
        if (g >= 0) w += callfont[g].w + CALLFONT_SPACING;
    }
    if (w == 0 || w > W - 40) return;
    int x0 = (W - w) / 2;
    for (int pass = 0; pass < 2; pass++) {      /* the dark edge first, then the gold */
        int x = x0;
        for (char *c = call; *c; c++) {
            int g = glyph(*c);
            if (g < 0) continue;
            for (int y = 0; y < CALLFONT_H; y++)
                for (int i = 0; i < callfont[g].w; i++) {
                    int a = callfont[g].px[y * callfont[g].w + i];
                    if (!a) continue;
                    for (int d = pass ? 0 : 2; d >= (pass ? 0 : -2); d -= pass ? 1 : 2) {
                        int px = x + i + (pass ? 0 : d), py = CALL_Y + y + (pass ? 0 : 2);
                        if (px < 0 || px >= W || py >= H) continue;
                        uint8_t *p   = logo + (py * W + px) * 3;
                        const int col[3] = {229, 168, 35};
                        for (int k = 0; k < 3; k++)
                            p[k] = pass ? (uint8_t)((p[k] * (255 - a) + col[k] * a) / 255) : (uint8_t)(p[k] * (255 - a / 2) / 255);
                    }
                }
            x += callfont[g].w + CALLFONT_SPACING;
        }
    }
}

/* ---- Animation ------------------------------------------------------------ */

/* When a logo pixel starts to fade in, ms from the first frame. The mark is 190 px wide at the
 * top (y 48..238): its ring is gold, the signal through it blue (b > r); then the name (y 258..340),
 * the rule (y 352), the subtitle (y 372..400) and the maintainer's line at the foot (y 424..466). */
static int start_ms(int x, int y, const uint8_t *p) {
    if (y < CALL_Y + CALLFONT_H + 3) return 1820 + abs(x - 400) * 120 / 200;   /* the callsign, last, from the centre */
    if (y >= 415) return 1760 + (x - 230) * 240 / 340;               /* the foot, left to right */
    if (y < 250) {
        double r = hypot(x - CX, y - CY);

        if (p[2] > p[0]) return 560 + (x - 300) * 2;                 /* the signal, left to right */
        return 120 + (int)(r * 4);                                   /* the ring, from the centre out */
    }
    if (y < 350) return 820 + (x - 160) * 420 / 480 + (345 - y);     /* the name, a diagonal sweep */
    if (y < 362) return 1300 + abs(x - 400) * 260 / 60;              /* the rule, from the centre */
    return 1500 + (x - 85) * 220 / 630;                              /* the subtitle */
}

static void on_term(int sig) {
    (void)sig;
    stop = 1;
}

/* The PNG's interlace flag (IHDR byte 12), read straight from the file because png_image hides it: 1 (don't
 * decode it) when it is interlaced or can't be read. The radio's libpng (1.6.39) overran the 8-bit buffer
 * decoding a 16-bit interlaced picture (fixed upstream in 1.6.51), and this runs at boot as root; the web page
 * keeps such a file off /mnt, this guards one put there by hand over SSH */
static int png_interlaced(const char *path) {
    unsigned char head[29];                   /* the signature (8), the IHDR length and type (8), its 13 bytes */
    int           fd = open(path, O_RDONLY);

    if (fd < 0) return 1;
    ssize_t got = read(fd, head, sizeof head);
    close(fd);
    return got != (ssize_t)sizeof head || head[28] != 0;
}

/* The PNG at path into logo, true when it is one of the screen's size, not interlaced, and read whole. What is
 * transparent in it shows black (composed onto the buffer, cleared first). An interlaced one gives ours, as a
 * missing or damaged one does: it is never handed to png_image_finish_read */
static int logo_read(const char *path, uint8_t *logo) {
    png_image img = {.version = PNG_IMAGE_VERSION};

    if (!png_image_begin_read_from_file(&img, path)) return 0;
    img.format = PNG_FORMAT_RGB;
    if (img.width != W || img.height != H || png_interlaced(path)) {
        png_image_free(&img);
        return 0;
    }
    memset(logo, 0, W * H * 3);
    return png_image_finish_read(&img, NULL, logo, 0, NULL) != 0;
}

int main(int argc, char **argv) {
    static uint8_t logo[W * H * 3];

    if (access(GUI_UP, F_OK) == 0) return 0;
    signal(SIGTERM, on_term);
    /* The owner's picture (one named on the command line in its place, to try one by hand), else ours. Before,
     * a picture that could not be read gave no picture and no boot sound */
    int own = logo_read(argc > 1 ? argv[1] : OWN_LOGO, logo);
    if (!own && !logo_read(OUR_LOGO, logo)) return 1;
    int fade = own ? OWN_FADE_MS : FADE_MS;
    /* The plain background (the kernel logo's) before the callsign goes onto the picture */
    uint8_t bg0[3] = {logo[0], logo[1], logo[2]};
    draw_call(logo);

    /* The panel is 480x800 portrait: landscape (x, y) is fb (y, 799 - x), as the kernel logo */
    int fd = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f) ||
        v.xres != H || v.yres != W || v.bits_per_pixel != 32) return 1;
    uint8_t *fb = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) return 1;
    #define PIX(r, g, b) ((uint32_t)(r) << v.red.offset | (uint32_t)(g) << v.green.offset | (uint32_t)(b) << v.blue.offset)

    /* Only the pixels that differ from the background change */
    const uint8_t *bg = bg0;
    static uint32_t off[W * H];
    static int16_t  t0[W * H];
    static uint32_t src[W * H];
    int n = 0, end = 0;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t *p = logo + (y * W + x) * 3;
            uint32_t       o = (W - 1 - x) * f.line_length + y * 4;

            *(uint32_t *)(fb + o) = PIX(bg[0], bg[1], bg[2]);
            if (p[0] == bg[0] && p[1] == bg[1] && p[2] == bg[2]) continue;
            off[n] = o;
            t0[n]  = own ? 0 : SLOW(start_ms(x, y, p));
            src[n] = (y * W + x) * 3;
            if (t0[n] + fade > end) end = t0[n] + fade;
            n++;
        }
    }

    pthread_t th;
    int       sound = sound_start(&th);
    /* With the sound the animation starts later, so the last element ends on the full logo
     * (cq_len has one dit of silence at the end; 100 ms is the codec buffer) */
    int64_t   start = now_ms() + (sound ? (cq_len - RATE * DIT_MS / 1000) * 1000LL / RATE + 100 - end : 0);
    int64_t   wait0 = start - now_ms();
    if (wait0 > 0) usleep(wait0 * 1000);
    for (int t = 0;; t += FRAME_MS) {
        if (t > end) t = end;
        for (int i = 0; i < n; i++) {
            int a = (t - t0[i]) * 256 / fade;
            if (a <= 0) continue;
            if (a > 256) a = 256;
            const uint8_t *p = logo + src[i];
            *(uint32_t *)(fb + off[i]) = PIX(bg[0] + ((p[0] - bg[0]) * a >> 8),
                                             bg[1] + ((p[1] - bg[1]) * a >> 8),
                                             bg[2] + ((p[2] - bg[2]) * a >> 8));
        }
        if (t == end || access(GUI_UP, F_OK) == 0) break;
        int64_t wait = start + t + FRAME_MS - now_ms();
        if (wait > 0) usleep(wait * 1000);
    }
    if (sound) {
        if (access(GUI_UP, F_OK) == 0) stop = 1;   /* the GUI is up: it needs the codec and the base */
        sound_end(th);
    }
    return 0;
}
