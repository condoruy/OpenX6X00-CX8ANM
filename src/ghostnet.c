/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CX8ANM: GhostNet (ver ghostnet.h). Los horarios son los de GhostNet versión 1.5 (s2underground/GhostNet),
 *  en UTC: la red semanal de cada región (los jueves a la noche de allá) y los puentes de datos de los sábados.
 */
#include "ghostnet.h"

#include "cJSON.h"
#include "params/params.h"
#include "app_window.h"
#include "radio_types.h"
#include "screen_main.h"
#include "cfg/cfg_api.h"
#include "cfg/computed_api.h"
#include "scheduler.h"
#include "styles.h"
#include "theme.h"
#include "i18n.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define TRANSLATE  "/usr/sbin/x6100-translate"
#define METEO_FILE "/tmp/x6100-meteo.json"
#define CHECK_MS   (20 * 1000)

/* ---------- los horarios (UTC; día 0 domingo) ---------- */

typedef struct {
    const char *net;
    int         wday, hh, mm, minutes, khz;
    const char *mode;
    const char *what;
} segment_t;

static const segment_t segments[] = {
    {"NA",     5,  1,  0, 30,  7107, "JS8",  "JS8 40 m"},
    {"NA",     5,  1, 30, 30,  7107, "VARA", "Winlink VARA (PC)"},
    {"NA",     5,  2,  0, 30,  7077, "RTTY", "RTTY 45.45 baud"},
    {"NA",     5,  2, 30, 30,  7190, "LSB",  "Voice, emergencies only"},
    {"EU",     4, 18,  0, 30,  7107, "JS8",  "JS8 40 m"},
    {"EU",     4, 18, 30, 30,  7107, "VARA", "Winlink VARA (PC)"},
    {"EU",     4, 19,  0, 30,  7077, "RTTY", "RTTY 45.45 baud"},
    {"EU",     4, 19, 30, 30,  7190, "LSB",  "Voice, emergencies only"},
    {"AU",     4,  7,  0, 30,  7107, "JS8",  "JS8 40 m"},
    {"AU",     4,  7, 30, 30,  7107, "VARA", "Winlink VARA (PC)"},
    {"AU",     4,  8,  0, 30,  7077, "RTTY", "RTTY 45.45 baud"},
    {"AU",     4,  8, 30, 30,  7190, "LSB",  "Voice, emergencies only"},
    {"B_NAEU", 6, 18,  0, 60, 14107, "JS8",  "JS8 20 m"},
    {"B_NAEU", 6, 19,  0, 60,     0, "ALE",  "Ion2G ALE (PC)"},
    {"B_NAAU", 6, 12,  0, 60, 14107, "JS8",  "JS8 20 m"},
    {"B_NAAU", 6, 13,  0, 30,     0, "ALE",  "Ion2G ALE (PC)"},
    {"B_NAAU", 6, 13, 30, 30,  3575, "JS8",  "JS8 80 m"},
    {"B_EUAU", 6, 20,  0, 60, 14107, "JS8",  "JS8 20 m"},
    {"B_EUAU", 6, 21,  0, 30,     0, "ALE",  "Ion2G ALE (PC)"},
    {"B_EUAU", 6, 21, 30, 30,  3575, "JS8",  "JS8 80 m"},
    {"B_AUSP", 6,  8,  0, 60, 14107, "JS8",  "JS8 20 m"},
    {"B_AUSP", 6,  9,  0, 30,  7107, "JS8",  "JS8 40 m"},
    {"B_AUSP", 6,  9, 30, 30,     0, "ALE",  "Ion2G ALE (PC)"},
};

static const char *net_name(const char *net) {
    if (!strcmp(net, "NA")) return tr("North America");
    if (!strcmp(net, "EU")) return tr("Europe");
    if (!strcmp(net, "AU")) return tr("Australia");
    if (!strcmp(net, "B_NAEU")) return tr("Bridge NA-Europe");
    if (!strcmp(net, "B_NAAU")) return tr("Bridge NA-Australia");
    if (!strcmp(net, "B_EUAU")) return tr("Bridge Europe-Australia");
    if (!strcmp(net, "B_AUSP")) return tr("Bridge Australia-Pacific");
    return net;
}

/* ---------- la configuración ---------- */

static ghostnet_conf_t conf;
static time_t          conf_mtime = -1;

static void conf_defaults(ghostnet_conf_t *c) {
    memset(c, 0, sizeof(*c));
    c->remind     = true;
    c->remind_min = 10;
    snprintf(c->nets, sizeof(c->nets), "NA,B_NAEU,ES");   /* las de horario cómodo desde Sudamérica */
    snprintf(c->es_name, sizeof(c->es_name), "%s", tr("Spanish net"));
    c->es_day = 4;      /* jueves */
    c->es_hh  = 21;
    c->es_min = 30;
    c->es_khz = 7107;
    snprintf(c->es_mode, sizeof(c->es_mode), "JS8");
    c->translate_rx = true;
    c->translate_tx = false;
}

static void conf_load(void) {
    struct stat st;
    FILE       *f;
    char        line[160];

    if (stat(GHOSTNET_CONF, &st) != 0) {
        if (conf_mtime != 0) conf_defaults(&conf);
        conf_mtime = 0;
        return;
    }
    if (st.st_mtime == conf_mtime) return;
    conf_mtime = st.st_mtime;
    conf_defaults(&conf);
    f = fopen(GHOSTNET_CONF, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '='), *v;
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        v   = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        int n = atoi(v);
        if (!strcmp(line, "remind")) conf.remind = n != 0;
        else if (!strcmp(line, "remind_min")) conf.remind_min = n >= 1 && n <= 120 ? n : 10;
        else if (!strcmp(line, "nets")) snprintf(conf.nets, sizeof(conf.nets), "%s", v);
        else if (!strcmp(line, "es_on")) conf.es_on = n != 0;
        else if (!strcmp(line, "es_name")) snprintf(conf.es_name, sizeof(conf.es_name), "%s", v);
        else if (!strcmp(line, "es_day")) conf.es_day = n >= 0 && n <= 6 ? n : 4;
        else if (!strcmp(line, "es_time")) sscanf(v, "%d:%d", &conf.es_hh, &conf.es_mm);
        else if (!strcmp(line, "es_min")) conf.es_min = n >= 5 && n <= 240 ? n : 30;
        else if (!strcmp(line, "es_khz")) conf.es_khz = n >= 1800 && n <= 54000 ? n : 7107;
        else if (!strcmp(line, "es_mode")) snprintf(conf.es_mode, sizeof(conf.es_mode), "%s", v);
        else if (!strcmp(line, "translate_rx")) conf.translate_rx = n != 0;
        else if (!strcmp(line, "translate_tx")) conf.translate_tx = n != 0;
        else if (!strcmp(line, "region")) snprintf(conf.region, sizeof(conf.region), "%s", v);
    }
    fclose(f);
    if (conf.es_hh < 0 || conf.es_hh > 23) conf.es_hh = 21;
    if (conf.es_mm < 0 || conf.es_mm > 59) conf.es_mm = 0;
}

const ghostnet_conf_t *ghostnet_conf(void) {
    conf_load();
    return &conf;
}

void ghostnet_conf_save(const ghostnet_conf_t *c) {
    FILE *f = fopen(GHOSTNET_CONF ".tmp", "w");
    if (!f) return;
    fprintf(f, "# GhostNet (CX8ANM): lo cambian la app JS8 y la página web GhostNet\n");
    fprintf(f, "remind=%d\nremind_min=%d\nnets=%s\n", c->remind, c->remind_min, c->nets);
    fprintf(f, "es_on=%d\nes_name=%s\nes_day=%d\nes_time=%02d:%02d\nes_min=%d\nes_khz=%d\nes_mode=%s\n", c->es_on,
            c->es_name, c->es_day, c->es_hh, c->es_mm, c->es_min, c->es_khz, c->es_mode);
    fprintf(f, "translate_rx=%d\ntranslate_tx=%d\nregion=%s\n", c->translate_rx, c->translate_tx, c->region);
    fclose(f);
    rename(GHOSTNET_CONF ".tmp", GHOSTNET_CONF);
    conf       = *c;
    conf_mtime = -1;    /* se relee: la hora del archivo nuevo */
    conf_load();
}

bool ghostnet_net_reminded(const char *net) {
    const ghostnet_conf_t *c = ghostnet_conf();
    char                   list[sizeof(c->nets) + 2];
    char                   want[16];

    snprintf(list, sizeof(list), ",%s,", c->nets);
    snprintf(want, sizeof(want), ",%s,", net);
    return strstr(list, want) != NULL;
}

/* ---------- las próximas ventanas ---------- */

/* La próxima vez (o la que está pasando) del día de semana y hora UTC */
static time_t next_utc(time_t now, int wday, int hh, int mm, int minutes) {
    struct tm t;
    gmtime_r(&now, &t);
    time_t day0  = now - (t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
    int    ahead = (wday - t.tm_wday + 7) % 7;
    time_t s     = day0 + ahead * 86400 + hh * 3600 + mm * 60;
    if (s + minutes * 60 <= now) s += 7 * 86400;
    if (s - 7 * 86400 + minutes * 60 > now) s -= 7 * 86400;   /* empezó la semana pasada a esta hora y sigue */
    return s;
}

static time_t next_local(time_t now, int wday, int hh, int mm, int minutes) {
    struct tm t;
    localtime_r(&now, &t);
    int ahead = (wday - t.tm_wday + 7) % 7;
    for (int week = -1; week <= 1; week++) {
        struct tm x = t;
        x.tm_mday += ahead + week * 7;
        x.tm_hour  = hh;
        x.tm_min   = mm;
        x.tm_sec   = 0;
        x.tm_isdst = -1;
        time_t s   = mktime(&x);
        if (s + minutes * 60 > now) return s;
    }
    return now + 7 * 86400;
}

static int slot_cmp(const void *a, const void *b) {
    time_t x = ((const ghostnet_slot_t *)a)->start, y = ((const ghostnet_slot_t *)b)->start;
    return x < y ? -1 : x > y;
}

int ghostnet_upcoming(ghostnet_slot_t *out, int max, int hours, bool only_reminded) {
    const ghostnet_conf_t *c   = ghostnet_conf();
    time_t                 now = time(NULL);
    ghostnet_slot_t        all[sizeof(segments) / sizeof(segments[0]) + 1];
    int                    n = 0;

    for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); i++) {
        const segment_t *s = &segments[i];
        if (only_reminded && !ghostnet_net_reminded(s->net)) continue;
        ghostnet_slot_t *o = &all[n];
        o->start   = next_utc(now, s->wday, s->hh, s->mm, s->minutes);
        if (o->start > now + hours * 3600) continue;
        o->minutes = s->minutes;
        o->khz     = s->khz;
        o->net     = s->net;
        snprintf(o->name, sizeof(o->name), "%s", net_name(s->net));
        snprintf(o->what, sizeof(o->what), "%s", tr(s->what));
        snprintf(o->mode, sizeof(o->mode), "%s", s->mode);
        n++;
    }
    if (c->es_on && (!only_reminded || ghostnet_net_reminded("ES"))) {
        ghostnet_slot_t *o = &all[n];
        o->start = next_local(now, c->es_day, c->es_hh, c->es_mm, c->es_min);
        if (o->start <= now + hours * 3600) {
            o->minutes = c->es_min;
            o->khz     = c->es_khz;
            o->net     = "ES";
            snprintf(o->name, sizeof(o->name), "%s", c->es_name[0] ? c->es_name : tr("Spanish net"));
            snprintf(o->what, sizeof(o->what), tr("%s in Spanish"), c->es_mode);
            snprintf(o->mode, sizeof(o->mode), "%s", c->es_mode);
            n++;
        }
    }
    qsort(all, n, sizeof(all[0]), slot_cmp);
    if (n > max) n = max;
    memcpy(out, all, n * sizeof(all[0]));
    return n;
}

void ghostnet_local_when(time_t t, char *out, size_t size) {
    static const char *days[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    struct tm          x;
    localtime_r(&t, &x);
    snprintf(out, size, "%s %02d:%02d", tr(days[x.tm_wday]), x.tm_hour, x.tm_min);
}

/* ---------- el grupo regional ---------- */

static const struct {
    char a2[3], a3[4];
} iso3[] = {
#include "ghostnet_iso3.inc"
};

void ghostnet_region_tag(char *out, size_t size) {
    const ghostnet_conf_t *c = ghostnet_conf();
    const char            *a3 = NULL;
    char                   state[4] = "";

    if (c->region[0]) {
        snprintf(out, size, "%s%s", c->region[0] == '@' ? "" : "@", c->region);
        for (char *p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
        return;
    }
    for (size_t i = 0; i < sizeof(iso3) / sizeof(iso3[0]); i++)
        if (!strncasecmp(iso3[i].a2, params.country.x, 2)) a3 = iso3[i].a3;
    if (!a3) {
        *out = 0;
        return;
    }
    /* El estado o departamento: el que encontró Meteo para la posición ("UY-MO"), si es del mismo país */
    FILE *f = fopen(METEO_FILE, "r");
    if (f) {
        char  *text = malloc(256 * 1024);
        size_t len  = text ? fread(text, 1, 256 * 1024 - 1, f) : 0;
        fclose(f);
        if (text) {
            text[len]    = 0;
            cJSON *root  = cJSON_Parse(text);
            const char *r = cJSON_GetStringValue(cJSON_GetObjectItem(root, "region"));
            if (r && strlen(r) >= 4 && r[2] == '-' && !strncasecmp(r, params.country.x, 2)) {
                int k = 0;
                for (const char *p = r + 3; *p && k < 3; p++)
                    if (isalnum((unsigned char)*p)) state[k++] = (char)toupper((unsigned char)*p);
                state[k] = 0;
            }
            cJSON_Delete(root);
            free(text);
        }
    }
    snprintf(out, size, "@GN%s%s", a3, state);
}

static bool has_group(const char *groups, const char *g) {
    size_t      n = strlen(g);
    const char *p = groups;
    while ((p = strcasestr(p, g)) != NULL) {
        if ((p == groups || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
        p += n;
    }
    return false;
}

void ghostnet_add_groups(char *groups, size_t size) {
    char        region[16];
    const char *add[3] = {"@GHOSTNET", "@GSTFLASH", region};

    ghostnet_region_tag(region, sizeof(region));
    for (int i = 0; i < 3; i++) {
        if (!add[i][0] || has_group(groups, add[i])) continue;
        size_t len = strlen(groups);
        if (len + strlen(add[i]) + 2 > size) break;
        snprintf(groups + len, size - len, "%s%s", len ? " " : "", add[i]);
    }
}

/* ---------- el registro para la web ---------- */

bool ghostnet_is_group(const char *to) {
    return to && (!strcasecmp(to, "@GHOSTNET") || !strcasecmp(to, "@GSTFLASH") || !strncasecmp(to, "@GN", 3));
}

void ghostnet_log(char kind, uint32_t id, const char *from, const char *to, int snr, int32_t dial_hz, const char *text) {
    struct stat st;
    if (stat(GHOSTNET_LOG, &st) == 0 && st.st_size > 256 * 1024) rename(GHOSTNET_LOG, GHOSTNET_LOG ".1");
    FILE *f = fopen(GHOSTNET_LOG, "a");
    if (!f) return;
    char clean[700];
    snprintf(clean, sizeof(clean), "%s", text ? text : "");
    for (char *p = clean; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    fprintf(f, "%c\t%ld\t%u\t%s\t%s\t%d\t%d\t%s\n", kind, (long)time(NULL), id, from ? from : "", to ? to : "", snr,
            (int)dial_hz, clean);
    fclose(f);
}

/* ---------- los recuadros ---------- */

static lv_obj_t   *popup;
static lv_timer_t *popup_timer;

static void popup_timer_cb(lv_timer_t *t) {
    (void)t;
    popup_timer = NULL;
    if (popup) {
        lv_obj_del(popup);
        popup = NULL;
    }
}

static void popup_show(uint32_t bg, uint32_t fg, const char *title, const char *line1, const char *line2, int ms) {
    if (popup_timer) {
        lv_timer_del(popup_timer);
        popup_timer = NULL;
    }
    if (popup) lv_obj_del(popup);
    popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(popup);
    lv_obj_set_pos(popup, 50, 140);
    lv_obj_set_size(popup, 700, 150);
    lv_obj_set_style_bg_color(popup, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(popup, 245, 0);
    lv_obj_set_style_border_color(popup, lv_color_hex(fg), 0);
    lv_obj_set_style_border_width(popup, 3, 0);
    lv_obj_set_style_radius(popup, theme_shape.radius_popup, 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(popup);
    lv_obj_set_style_text_font(l, &sony_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_label_set_text(l, title);
    lv_obj_set_pos(l, 14, 8);

    l = lv_label_create(popup);
    lv_obj_set_style_text_font(l, &sony_22, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 672);
    lv_label_set_text(l, line1);
    lv_obj_set_pos(l, 14, 46);

    l = lv_label_create(popup);
    lv_obj_set_style_text_font(l, &sony_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 672);
    lv_label_set_text(l, line2);
    lv_obj_set_pos(l, 14, 116);

    popup_timer = lv_timer_create(popup_timer_cb, ms, NULL);
    lv_timer_set_repeat_count(popup_timer, 1);
}

void ghostnet_flash_show(const char *text) {
    popup_show(0xd00000, 0xffffff, tr("EMERGENCY  -  @GSTFLASH"), text,
               tr("GhostNet: pass it on at once to whoever can help, by any means"), 5 * 60 * 1000);
}

/* ---------- el recordatorio ---------- */

static char last_reminded[48];

static void remind_cb(lv_timer_t *t) {
    (void)t;
    const ghostnet_conf_t *c = ghostnet_conf();
    ghostnet_slot_t        s[8];
    time_t                 now = time(NULL);

    if (!c->remind) return;
    int n = ghostnet_upcoming(s, 8, 3, true);
    for (int i = 0; i < n; i++) {
        long before = (long)(s[i].start - now);
        if (before > c->remind_min * 60 || before < c->remind_min * 60 - 90) continue;
        if (strcmp(s[i].net, "ES") != 0 && strcmp(s[i].mode, "JS8") != 0) continue;   /* al empezar la red: su JS8 */
        char key[48];
        snprintf(key, sizeof(key), "%s %ld", s[i].net, (long)s[i].start);
        if (!strcmp(key, last_reminded)) continue;
        snprintf(last_reminded, sizeof(last_reminded), "%s", key);

        char when[24], title[80], line1[160], line2[120];
        ghostnet_local_when(s[i].start, when, sizeof(when));
        snprintf(title, sizeof(title), "GhostNet: %s", s[i].name);
        snprintf(line1, sizeof(line1), tr("Starts in %d min (%s): %s on %d.%03d MHz"), c->remind_min, when, s[i].what,
                 s[i].khz / 1000, s[i].khz % 1000);
        snprintf(line2, sizeof(line2), "%s", tr("APP > Modes > JS8 > Freq > GhostNet mode  -  groups @GHOSTNET @GSTFLASH"));
        popup_show(0x0d3b66, 0xffffff, title, line1, line2, 90 * 1000);
        break;
    }
}

/* ---------- la traducción ---------- */

#define TR_QUEUE 16

typedef struct {
    uint32_t       id;
    bool           to_english;
    ghostnet_tr_cb done;
    char           text[256];
} tr_job_t;

typedef struct {
    uint32_t       id;
    bool           ok;
    ghostnet_tr_cb done;
    char           text[320];
} tr_result_t;

static tr_job_t        jobs[TR_QUEUE];
static int             job_head, job_count;
static bool            worker_on;
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  job_cond = PTHREAD_COND_INITIALIZER;

static void tr_deliver(void *arg) {
    tr_result_t *r = arg;
    if (r->done) r->done(r->id, r->ok, r->text);
}

static void tr_run(const tr_job_t *j) {
    char        cmd[700], *o = cmd;
    tr_result_t r = {.id = j->id, .ok = false, .done = j->done};

    o += snprintf(cmd, sizeof(cmd), TRANSLATE " %s '", j->to_english ? "tx" : "rx");
    for (const char *p = j->text; *p && o < cmd + sizeof(cmd) - 8; p++) {
        if (*p == '\'') {
            memcpy(o, "'\\''", 4);
            o += 4;
        } else {
            *o++ = *p;
        }
    }
    snprintf(o, cmd + sizeof(cmd) - o, "' 2>/dev/null");

    FILE *p = popen(cmd, "r");
    char  line[400] = "";
    if (p) {
        if (!fgets(line, sizeof(line), p)) line[0] = 0;
        pclose(p);
    }
    line[strcspn(line, "\n")] = 0;
    if (!strncmp(line, "ok ", 3)) {
        r.ok = true;
        snprintf(r.text, sizeof(r.text), "%s", line + 3);
    } else if (!strncmp(line, "error ", 6)) {
        snprintf(r.text, sizeof(r.text), "%s", line + 6);
    } else if (strcmp(line, "skip") != 0) {
        snprintf(r.text, sizeof(r.text), "%s", tr("no translator (is the radio on the internet?)"));
    }
    scheduler_put(tr_deliver, &r, sizeof(r));
}

static void *tr_worker(void *arg) {
    (void)arg;
    for (;;) {
        tr_job_t j;
        pthread_mutex_lock(&job_lock);
        while (!job_count) pthread_cond_wait(&job_cond, &job_lock);
        j         = jobs[job_head];
        job_head  = (job_head + 1) % TR_QUEUE;
        job_count--;
        pthread_mutex_unlock(&job_lock);
        tr_run(&j);
    }
    return NULL;
}

bool ghostnet_translate(bool to_english, const char *text, uint32_t id, ghostnet_tr_cb done) {
    bool queued = false;

    pthread_mutex_lock(&job_lock);
    if (!worker_on) {
        pthread_t th;
        if (pthread_create(&th, NULL, tr_worker, NULL) == 0) {
            pthread_detach(th);
            worker_on = true;
        }
    }
    if (worker_on && job_count < TR_QUEUE) {
        tr_job_t *j   = &jobs[(job_head + job_count) % TR_QUEUE];
        j->id         = id;
        j->to_english = to_english;
        j->done       = done;
        snprintf(j->text, sizeof(j->text), "%s", text);
        job_count++;
        queued = true;
        pthread_cond_signal(&job_cond);
    }
    pthread_mutex_unlock(&job_lock);
    return queued;
}

/* ---------- RTTY de GhostNet: 7.077 MHz, 45,45 baudios, 170 Hz ---------- */

static void rtty_go(void *arg) {
    (void)arg;
    params_lock();
    params.rtty_rate  = 4545;
    params.rtty_shift = 170;
    params_unlock(&params.dirty.rtty_rate);
    params_lock();
    params_unlock(&params.dirty.rtty_shift);
    main_screen_set_freq(7077000);
    cparam_i_set(cfg_cur_mode, radio_mode_usb);
    app_mem_save("RTTY");               /* la app RTTY abre donde quedó: acá */
    main_screen_action(ACTION_APP_RTTY);
}

void ghostnet_rtty_open(void) {
    lv_async_call(rtty_go, NULL);       /* después de que cierre la app que lo pidió */
}

void ghostnet_init(void) {
    conf_load();
    lv_timer_create(remind_cb, CHECK_MS, NULL);
}
