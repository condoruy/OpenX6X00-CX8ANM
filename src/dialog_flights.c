/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CX8ANM: vuelos sobre una zona, tipo Flightradar. Un radar con los aviones alrededor del centro
 *  elegido (tu posición o un aeropuerto) y la lista con número de vuelo, tipo, altura, velocidad y
 *  distancia. Los datos los trae x6100-flights (redes ADS-B abiertas: airplanes.live, adsb.lol);
 *  se actualiza cada 15 s mientras la app está abierta.
 *
 *  La banda aérea, en la misma pantalla: "Escuchar torre" busca las emisoras de control de tránsito aéreo del
 *  aeropuerto elegido (x6100-iradio atc, las de la web primero) y las pasa por el parlante; en el X6200 sintonizado
 *  en 118-137 MHz se escucha la radio misma. Mientras se escucha, los aviones que con más probabilidad hablan con
 *  esa torre (cerca del aeropuerto, aproximando, despegando o en tierra) se resaltan con su matrícula, y el elegido
 *  tiene su ventana sobre el radar.
 */
#include "dialog_flights.h"

#include "buttons.h"
#include "cJSON.h"
#include "dialog.h"
#include "events.h"
#include "i18n.h"
#include "keyboard.h"
#include "meteo_alerts.h"
#include "radio.h"
#include "scheduler.h"
#include "styles.h"
#include "theme.h"

#include "audio.h"
#include "cfg/cfg_api.h"   /* cfg_fg_freq: la frecuencia, para el X6200 en la banda aérea */
#include "msg.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define HELPER     "/usr/sbin/x6100-flights"
#define DATA_FILE  "/tmp/x6100-flights.json"
#define REFRESH_MS 15000
#define LIST_ROWS  7
#define IRADIO     "/usr/sbin/x6100-iradio"
#define IR_STATUS  "/tmp/x6100-iradio.status"
#define MAX_TOWERS 12
#define NEAR_KM    30.0      /* a esta distancia del aeropuerto y bajos: hablan con la torre */
#define LOW_M      3000.0

typedef struct {
    const char *name;
    double      lat, lon;      /* 0, 0: tu posición (GPS o locator) */
} center_t;

static const center_t centers[] = {
    {"My position", 0, 0},
    {"SUMU Carrasco", -34.8384, -56.0308},
    {"SULS Laguna del Sauce", -34.8551, -55.0943},
    {"SUAA Melilla", -34.7892, -56.2647},
    {"SUDU Durazno", -33.3589, -56.4992},
    {"SURV Rivera", -30.9746, -55.4762},
    {"SUSO Salto", -31.4385, -57.9853},
    {"SABE Aeroparque", -34.5592, -58.4156},
    {"SAEZ Ezeiza", -34.8222, -58.5358},
    {"SBPA Porto Alegre", -29.9939, -51.1714},
};
#define N_CENTERS (int)(sizeof(centers) / sizeof(centers[0]))

static const int ranges_km[] = {25, 50, 100, 200, 400};
#define N_RANGES (int)(sizeof(ranges_km) / sizeof(ranges_km[0]))

/* Se recuerdan mientras la radio está prendida */
static int center_i = 0;
static int range_i  = 2;

static cJSON      *data;
static bool        updating;
static int         gen;
static char        upd_error[120];
static int         sel;            /* el avión elegido en la lista */
static int         top;            /* primera fila de la lista */
static lv_obj_t   *status_lbl, *radar, *list_lbl, *detail_lbl, *tower_lbl;
static lv_timer_t *refresh_timer;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void center_cb(button_data_t *b);
static void range_cb(button_data_t *b);
static void update_cb(button_data_t *b);

static void tower_cb(button_data_t *b);

static button_data_t btn_center = {.type = BTN_TEXT, .label = "Center", .press = center_cb};
static button_data_t btn_range  = {.type = BTN_TEXT, .label = "Range", .press = range_cb};
static button_data_t btn_tower  = {.type = BTN_TEXT, .label = "Listen\ntower", .press = tower_cb};
static button_data_t btn_update = {.type = BTN_TEXT, .label = "Update", .press = update_cb};

static buttons_page_t btn_page = {{&btn_center, &btn_range, &btn_tower, &btn_update}};

/* ---------- la torre ---------- */

typedef struct {
    char name[64];
    char url[512];
} tower_t;

static tower_t        towers[MAX_TOWERS];
static int            towers_n, tower_i = -1;   /* -1: no se escucha */
static bool           tower_searching;
static int            tower_gen;
static volatile bool  play_on, play_alive;
static volatile pid_t play_pid;
static bool           play_next_pending;
static char           tower_state[120];

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .btn_page     = &btn_page,
    .key_cb       = key_cb,
};

dialog_t *dialog_flights = &dialog;

/* ---------- valores ---------- */

static cJSON *obj(cJSON *o, const char *k) {
    return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
}

static bool numv(cJSON *o, const char *k, double *v) {
    cJSON *j = obj(o, k);
    if (!cJSON_IsNumber(j)) return false;
    *v = j->valuedouble;
    return true;
}

static const char *strv(cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(obj(o, k));
    return s ? s : "";
}

static cJSON *aircraft(void) {
    return obj(data, "ac");
}

static int n_aircraft(void) {
    return cJSON_GetArraySize(aircraft());
}

/* Número de vuelo, o matrícula, o el código del transpondedor */
static const char *ident(cJSON *a) {
    const char *s = strv(a, "call");
    if (!s[0]) s = strv(a, "reg");
    if (!s[0]) s = strv(a, "hex");
    return s;
}

static void center_pos(double *lat, double *lon) {
    if (centers[center_i].lat == 0 && centers[center_i].lon == 0) {
        meteo_position(lat, lon);
    } else {
        *lat = centers[center_i].lat;
        *lon = centers[center_i].lon;
    }
}

static double dist_km(double lat1, double lon1, double lat2, double lon2) {
    double p = M_PI / 180, a = sin((lat2 - lat1) * p / 2), b = sin((lon2 - lon1) * p / 2);
    a = a * a + cos(lat1 * p) * cos(lat2 * p) * b * b;
    return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

/* El aeropuerto: el elegido, o con "Mi posición" el de la lista más cercano */
static int airport_i(void) {
    if (center_i > 0) return center_i;
    double lat, lon, best = 1e9;
    int    bi = 1;
    meteo_position(&lat, &lon);
    for (int i = 1; i < N_CENTERS; i++) {
        double d = dist_km(lat, lon, centers[i].lat, centers[i].lon);
        if (d < best) {
            best = d;
            bi   = i;
        }
    }
    return bi;
}

/* El X6200 sintonizado en la banda aérea: se escucha la radio misma */
static bool radio_airband(void) {
#ifdef BOARD_X6200
    int32_t f = cparam_i_get(cfg_fg_freq);
    return f >= 118000000 && f <= 137000000;
#else
    return false;
#endif
}

static bool listening(void) {
    return tower_i >= 0 || radio_airband();
}

/* ¿habla con la torre? cerca del aeropuerto y bajo (aproximando, despegando) o en tierra */
static bool likely(cJSON *a) {
    double lat, lon, alt;
    if (!listening() || !numv(a, "lat", &lat) || !numv(a, "lon", &lon)) return false;
    const center_t *ap = &centers[airport_i()];
    if (dist_km(lat, lon, ap->lat, ap->lon) > NEAR_KM) return false;
    return cJSON_IsTrue(obj(a, "ground")) || (numv(a, "alt_m", &alt) && alt < LOW_M);
}

static const char *phase(cJSON *a) {
    double vs;
    if (cJSON_IsTrue(obj(a, "ground"))) return tr("on the ground");
    if (numv(a, "vs", &vs) && vs <= -120) return tr("approaching");
    if (numv(a, "vs", &vs) && vs >= 120) return tr("departing");
    return tr("near the airport");
}

/* Mientras se escucha, primero los que probablemente hablan con la torre (el orden de cercanía entre ellos) */
static void sort_likely(void) {
    cJSON *ac = aircraft();
    if (!ac || !listening()) return;
    cJSON *first = cJSON_CreateArray(), *rest = cJSON_CreateArray();
    while (cJSON_GetArraySize(ac) > 0) {
        cJSON *a = cJSON_DetachItemFromArray(ac, 0);
        cJSON_AddItemToArray(likely(a) ? first : rest, a);
    }
    while (cJSON_GetArraySize(first) > 0) cJSON_AddItemToArray(ac, cJSON_DetachItemFromArray(first, 0));
    while (cJSON_GetArraySize(rest) > 0) cJSON_AddItemToArray(ac, cJSON_DetachItemFromArray(rest, 0));
    cJSON_Delete(first);
    cJSON_Delete(rest);
}

static void fmt_alt(char *b, size_t n, cJSON *a);

/* ---------- radar ---------- */

static void radar_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t     *ctx = lv_event_get_draw_ctx(e);
    lv_obj_t          *o   = lv_event_get_target(e);
    lv_coord_t         w   = lv_obj_get_width(o);
    lv_point_t         c   = {o->coords.x1 + w / 2, o->coords.y1 + w / 2};
    lv_coord_t         r   = w / 2 - 14;
    lv_draw_arc_dsc_t  arc;
    lv_draw_line_dsc_t line;
    lv_draw_label_dsc_t lbl;
    lv_draw_rect_dsc_t dot;
    lv_area_t          a;
    char               txt[32];
    int                km = ranges_km[range_i];

    /* anillos a 1/3, 2/3 y el borde, con su distancia; la N arriba */
    lv_draw_arc_dsc_init(&arc);
    arc.color = TC(text_dim);
    arc.width = 1;
    arc.opa   = LV_OPA_60;
    lv_draw_label_dsc_init(&lbl);
    lbl.font  = &sony_14;
    lbl.color = TC(text_dim);
    for (int k = 1; k <= 3; k++) {
        lv_draw_arc(ctx, &arc, &c, r * k / 3, 0, 360);
        snprintf(txt, sizeof(txt), "%d", km * k / 3);
        a.x1 = c.x + 3;
        a.y1 = c.y - r * k / 3 + 1;
        a.x2 = a.x1 + 40;
        a.y2 = a.y1 + 16;
        lv_draw_label(ctx, &lbl, &a, txt, NULL);
    }
    lv_draw_line_dsc_init(&line);
    line.color = TC(text_dim);
    line.width = 1;
    line.opa   = LV_OPA_40;
    lv_point_t p1 = {c.x - r, c.y}, p2 = {c.x + r, c.y};
    lv_draw_line(ctx, &line, &p1, &p2);
    p1 = (lv_point_t){c.x, c.y - r};
    p2 = (lv_point_t){c.x, c.y + r};
    lv_draw_line(ctx, &line, &p1, &p2);
    lbl.color = TC(accent);
    lbl.font  = &sony_16;
    a.x1 = c.x - 5;
    a.y1 = c.y - r - 15;
    a.x2 = a.x1 + 14;
    a.y2 = a.y1 + 16;
    lv_draw_label(ctx, &lbl, &a, "N", NULL);

    /* los aviones: un punto, una raya hacia donde va y el nombre si no son demasiados */
    cJSON *ac;
    int    i = 0, n = n_aircraft();
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    lbl.font   = &sony_14;
    line.width = 2;
    line.opa   = LV_OPA_COVER;
    cJSON_ArrayForEach(ac, aircraft()) {
        double d = 0, b = 0, trk;
        if (!numv(ac, "dist_km", &d) || !numv(ac, "brg", &b) || d > km) {
            i++;
            continue;
        }
        lv_coord_t x = c.x + (lv_coord_t)(r * d / km * sin(b * M_PI / 180));
        lv_coord_t y = c.y - (lv_coord_t)(r * d / km * cos(b * M_PI / 180));
        bool       is_sel = i == sel, emerg = cJSON_IsTrue(obj(ac, "emerg")), ground = cJSON_IsTrue(obj(ac, "ground"));
        bool       hl  = likely(ac);
        lv_color_t col = emerg ? TC(err) : is_sel ? TC(warn) : hl ? TC(accent) : ground ? TC(text_dim) : TC(ok);
        if (hl) {   /* un anillo alrededor de los que hablan con la torre */
            lv_draw_arc_dsc_t ring;
            lv_draw_arc_dsc_init(&ring);
            ring.color = col;
            ring.width = 2;
            lv_point_t pc = {x, y};
            lv_draw_arc(ctx, &ring, &pc, is_sel ? 10 : 8, 0, 360);
        }

        if (numv(ac, "trk", &trk) && !ground) {
            line.color = col;
            p1 = (lv_point_t){x, y};
            p2 = (lv_point_t){x + (lv_coord_t)(14 * sin(trk * M_PI / 180)), y - (lv_coord_t)(14 * cos(trk * M_PI / 180))};
            lv_draw_line(ctx, &line, &p1, &p2);
        }
        dot.bg_color = col;
        int s = is_sel ? 5 : 3;
        a.x1 = x - s;
        a.y1 = y - s;
        a.x2 = x + s;
        a.y2 = y + s;
        lv_draw_rect(ctx, &dot, &a);
        if (is_sel || hl || n <= 20) {
            const char *reg = strv(ac, "reg");
            lbl.color = col;
            a.x1 = x + (hl ? 11 : 6);
            a.y1 = y - 8;
            a.x2 = a.x1 + 100;
            a.y2 = a.y1 + 16;
            lv_draw_label(ctx, &lbl, &a, hl && reg[0] ? reg : ident(ac), NULL);   /* la matrícula si habla */
        }
        i++;
    }

    /* La ventana del elegido, sobre el radar (arriba a la izquierda) mientras se escucha la torre */
    cJSON *s = listening() ? cJSON_GetArrayItem(aircraft(), sel) : NULL;
    if (s) {
        char   l1[64], l2[64], l3[64], alt[20];
        double kmh = -1;
        fmt_alt(alt, sizeof(alt), s);
        numv(s, "kmh", &kmh);
        snprintf(l1, sizeof(l1), "%s", strv(s, "reg")[0] ? strv(s, "reg") : ident(s));
        snprintf(l2, sizeof(l2), "%s  %s", strv(s, "call"), strv(s, "type"));
        if (kmh >= 0) snprintf(l3, sizeof(l3), "%s  %d km/h  %s", alt, (int)kmh, likely(s) ? phase(s) : "");
        else snprintf(l3, sizeof(l3), "%s  %s", alt, likely(s) ? phase(s) : "");
        lv_draw_rect_dsc_t box;
        lv_draw_rect_dsc_init(&box);
        box.bg_color     = TC(panel);
        box.bg_opa       = LV_OPA_80;
        box.border_color = likely(s) ? TC(accent) : TC(text_dim);
        box.border_width = 1;
        box.radius       = 4;
        a.x1 = o->coords.x1 + 2;
        a.y1 = o->coords.y1 + 2;
        a.x2 = a.x1 + 176;
        a.y2 = a.y1 + 58;
        lv_draw_rect(ctx, &box, &a);
        lbl.font  = &sony_18;
        lbl.color = likely(s) ? TC(accent) : TC(text_head);
        a.x1 += 6;
        a.y1 += 2;
        lv_draw_label(ctx, &lbl, &a, l1, NULL);
        lbl.font  = &sony_14;
        lbl.color = TC(text);
        a.y1 += 20;
        lv_draw_label(ctx, &lbl, &a, l2, NULL);
        a.y1 += 16;
        lv_draw_label(ctx, &lbl, &a, l3, NULL);
    }
}

/* ---------- lista y detalle ---------- */

static void fmt_alt(char *b, size_t n, cJSON *a) {
    double m;
    if (cJSON_IsTrue(obj(a, "ground")))
        snprintf(b, n, "%s", tr("ground"));
    else if (numv(a, "alt_m", &m))
        snprintf(b, n, "%d m", (int)m);
    else
        snprintf(b, n, "--");
}

static void draw_list(void) {
    static char txt[2048];
    int         n = n_aircraft(), len = 0;

    if (!list_lbl) return;
    if (!data) {
        lv_label_set_text(list_lbl, updating ? tr("Getting the flights...") : upd_error[0] ? upd_error : tr("No data: Update (needs WiFi)"));
        lv_label_set_text(detail_lbl, "");
        return;
    }
    if (n == 0) {
        lv_label_set_text_fmt(list_lbl, "%s", tr("No aircraft seen in this zone right now"));
        lv_label_set_text(detail_lbl, tr("Only aircraft picked up by a volunteer receiver nearby are shown"));
        return;
    }
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;

    for (int i = top; i < n && i < top + LIST_ROWS && len < (int)sizeof(txt) - 160; i++) {
        cJSON *a = cJSON_GetArrayItem(aircraft(), i);
        char   alt[20];
        double d = 0, kmh = -1;
        numv(a, "dist_km", &d);
        numv(a, "kmh", &kmh);
        fmt_alt(alt, sizeof(alt), a);
        char sp[8];
        if (kmh >= 0) snprintf(sp, sizeof(sp), "%d", (int)kmh); else snprintf(sp, sizeof(sp), "--");
        uint32_t c = cJSON_IsTrue(obj(a, "emerg")) ? theme_hex(TC_err) : i == sel ? theme_hex(TC_warn)
                   : likely(a) ? theme_hex(TC_accent) : theme_hex(TC_text);
        len += snprintf(txt + len, sizeof(txt) - len, "#%06x %s %-8.8s %-4.4s %7s %4s km/h %5.1f km#\n", c,
                        i == sel ? ">" : "  ", ident(a), strv(a, "type"), alt, sp, d);
    }
    lv_label_set_text(list_lbl, txt);

    /* el elegido, con todo */
    cJSON *a = cJSON_GetArrayItem(aircraft(), sel);
    char   alt[20];
    double kmh, trk, vs, d, b;
    fmt_alt(alt, sizeof(alt), a);
    char line2[96] = "";
    int  l2 = 0;
    if (numv(a, "kmh", &kmh)) l2 += snprintf(line2 + l2, sizeof(line2) - l2, "%d km/h  ", (int)kmh);
    if (numv(a, "trk", &trk)) l2 += snprintf(line2 + l2, sizeof(line2) - l2, "%s %03d°  ", tr("heading"), (int)trk);
    if (numv(a, "vs", &vs) && fabs(vs) >= 60)
        l2 += snprintf(line2 + l2, sizeof(line2) - l2, "%s %d m/min  ", vs > 0 ? tr("climbing") : tr("descending"), (int)fabs(vs));
    numv(a, "dist_km", &d);
    numv(a, "brg", &b);
    lv_label_set_text_fmt(detail_lbl, "#%06x %s#  %s %s  %s\n%s  %s\n%.1f km %s %03d°   %s %s%s%s",
                          theme_hex(TC_text_head), ident(a), strv(a, "reg"), strv(a, "type"), strv(a, "desc"),
                          alt, line2, d, tr("at"), (int)b, tr("squawk"), strv(a, "squawk"),
                          cJSON_IsTrue(obj(a, "emerg")) ? "  " : "", cJSON_IsTrue(obj(a, "emerg")) ? tr("EMERGENCY") : "");
}

static void show_status(void) {
    char extra[160] = "";

    if (!status_lbl) return;
    if (updating)
        snprintf(extra, sizeof(extra), "  -  %s", tr("updating..."));
    else if (upd_error[0])
        snprintf(extra, sizeof(extra), "  -  #%06x %s#", theme_hex(TC_err), upd_error);
    else if (data)
        snprintf(extra, sizeof(extra), "  -  %d %s  -  %s %s (%s)", n_aircraft(), tr("aircraft"), tr("updated"),
                 strv(data, "time"), strv(data, "src"));
    lv_label_set_text_fmt(status_lbl, "#%06x %s# #%06x %s - %d km#%s", theme_hex(TC_text_head), tr("FLIGHTS"),
                          theme_hex(TC_accent), tr(centers[center_i].name), ranges_km[range_i], extra);
    /* la torre, en su propia línea abajo a la derecha */
    if (!tower_lbl) return;
    if (radio_airband())
        lv_label_set_text_fmt(tower_lbl, "#%06x > %s %.3f MHz#", theme_hex(TC_accent), tr("Listening on the radio"),
                              cparam_i_get(cfg_fg_freq) / 1e6);
    else if (tower_state[0])
        lv_label_set_text_fmt(tower_lbl, "#%06x %s#", theme_hex(tower_state[0] == '!' ? TC_err : TC_accent),
                              tower_state[0] == '!' ? tower_state + 1 : tower_state);
    else
        lv_label_set_text_fmt(tower_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              tr("Listen tower: the air traffic control of this airport, over the internet"));
}

static void draw(void) {
    draw_list();
    show_status();
    if (radar) lv_obj_invalidate(radar);
}

/* ---------- datos ---------- */

static void load_file(void) {
    FILE *f = fopen(DATA_FILE, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = len > 0 && len < 1024 * 1024 ? malloc(len + 1) : NULL;
    if (text) text[fread(text, 1, len, f)] = 0;
    fclose(f);
    if (!text) return;
    cJSON *j = cJSON_Parse(text);
    free(text);
    if (!j) return;
    cJSON_Delete(data);
    data = j;
    sort_likely();
}

typedef struct {
    int  gen;
    char err[120];
} upd_result_t;

typedef struct {
    int    gen;
    double lat, lon;
    int    km;
} upd_args_t;

static void update_done(void *arg) {
    upd_result_t *r = arg;
    if (r->gen != gen || !dialog.run) return;
    updating = false;
    snprintf(upd_error, sizeof(upd_error), "%s", r->err);
    if (!r->err[0]) load_file();
    draw();
}

static void *update_thread(void *arg) {
    upd_args_t   in = *(upd_args_t *)arg;
    upd_result_t r  = {.gen = in.gen};
    char         cmd[160], line[200] = "";

    free(arg);
    snprintf(cmd, sizeof(cmd), HELPER " update %.4f %.4f %d 2>/dev/null", in.lat, in.lon, in.km);
    FILE *p = popen(cmd, "r");
    if (p) {
        char l[200];
        while (fgets(l, sizeof(l), p)) snprintf(line, sizeof(line), "%s", l);
        pclose(p);
    }
    line[strcspn(line, "\n")] = 0;
    if (!strncmp(line, "error ", 6))
        snprintf(r.err, sizeof(r.err), "%s", line + 6);
    else if (strncmp(line, "ok", 2))
        snprintf(r.err, sizeof(r.err), "%s", tr("No data: is WiFi connected?"));
    scheduler_put(update_done, &r, sizeof(r));
    return NULL;
}

static void start_update(void) {
    pthread_t   t;
    upd_args_t *a;

    if (updating || !(a = malloc(sizeof(*a)))) return;
    a->gen = gen;
    a->km  = ranges_km[range_i];
    center_pos(&a->lat, &a->lon);
    updating     = true;
    upd_error[0] = 0;
    show_status();
    if (pthread_create(&t, NULL, update_thread, a) == 0) {
        pthread_detach(t);
    } else {
        free(a);
        updating = false;
    }
}

static void refresh_cb(lv_timer_t *t) {
    (void)t;
    start_update();
}

/* Otro centro u otro radio: lo de antes ya no sirve */
static void restart(void) {
    gen++;
    updating = false;
    cJSON_Delete(data);
    data = NULL;
    sel = top = 0;
    draw();
    start_update();
}

/* ---------- la torre: buscar sus emisoras y escucharlas ---------- */

static void tower_stop(void) {
    play_on   = false;
    pid_t pid = play_pid;
    if (pid > 0) kill(-pid, SIGTERM);
}

static void end_child(pid_t pid) {
    kill(-pid, SIGTERM);
    for (int i = 0; i < 40; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        usleep(50000);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void tower_launch(void);

static void play_ended(void *arg) {
    (void)arg;
    if (!dialog.run) return;
    if (play_next_pending) {
        play_next_pending = false;
        tower_launch();
    } else if (tower_i >= 0) {    /* se cortó sola: la emisora no anda */
        FILE *f = fopen(IR_STATUS, "r");
        char  st[160] = "";
        if (f) {
            if (fgets(st, sizeof(st), f)) st[strcspn(st, "\n")] = 0;
            fclose(f);
        }
        snprintf(tower_state, sizeof(tower_state), "!%s: %s", towers[tower_i].name,
                 !strncmp(st, "error ", 6) ? st + 6 : tr("the stream stopped"));
        tower_i = -1;
    }
    sort_likely();
    draw();
}

static void *play_fn(void *arg) {
    char *url = arg;
    int   fds[2];

    if (pipe(fds) != 0) {
        free(url);
        play_on = play_alive = false;
        scheduler_put_noargs(play_ended);
        return NULL;
    }
    unlink(IR_STATUS);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) dup2(dn, 2);
        execl(IRADIO, IRADIO, "play", url, (char *)NULL);
        _exit(127);
    }
    free(url);
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        play_on = play_alive = false;
        scheduler_put_noargs(play_ended);
        return NULL;
    }
    setpgid(pid, pid);
    play_pid = pid;

    bool bt = audio_play_bt_begin();
    if (!bt) radio_set_play_mode(AUDIO_PLAY_ON);
    static int16_t buf[2048];
    size_t         have = 0;
    while (play_on) {
        ssize_t r = read(fds[0], (char *)buf + have, sizeof(buf) - have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        have += r;
        size_t samples = have / 2;
        if (samples && audio_play_until(buf, samples, &play_on, 3000) == -2) break;
        if (have & 1) ((char *)buf)[0] = ((char *)buf)[have - 1];
        have &= 1;
    }
    play_pid = 0;
    end_child(pid);
    close(fds[0]);
    audio_play_flush();
    if (bt) audio_play_bt_end();
    else radio_set_play_mode(AUDIO_PLAY_OFF);
    play_on    = false;
    play_alive = false;
    scheduler_put_noargs(play_ended);
    return NULL;
}

static void tower_launch(void) {
    pthread_t t;
    if (tower_i < 0 || tower_i >= towers_n) return;
    if (radio_get_state() != RADIO_RX) {
        snprintf(tower_state, sizeof(tower_state), "!%s", tr("Not while transmitting"));
        tower_i = -1;
        return;
    }
    char *url = strdup(towers[tower_i].url);
    if (!url) return;
    play_on = play_alive = true;
    snprintf(tower_state, sizeof(tower_state), "> %s %d/%d: %s", tr("Tower"), tower_i + 1, towers_n, towers[tower_i].name);
    if (pthread_create(&t, NULL, play_fn, url) == 0) {
        pthread_detach(t);
    } else {
        free(url);
        play_on = play_alive = false;
    }
}

typedef struct {
    int  gen;
    int  n;
    char err[120];
} tw_result_t;

static void tower_found(void *arg) {
    tw_result_t *r = arg;
    if (r->gen != tower_gen || !dialog.run) return;
    tower_searching = false;
    towers_n        = r->n;
    if (!towers_n) {
        snprintf(tower_state, sizeof(tower_state), "!%s %s", r->err[0] ? r->err : tr("No tower stream found for"),
                 r->err[0] ? "" : centers[airport_i()].name);
        draw();
        return;
    }
    tower_i = 0;
    tower_launch();
    sort_likely();
    sel = top = 0;
    draw();
}

static void *tower_search_fn(void *arg) {
    int         ap = *(int *)arg;
    tw_result_t r  = {.gen = *((int *)arg + 1)};
    char        cmd[256], code[8] = "", rest[64] = "", line[1024];

    free(arg);
    /* "SUMU Carrasco" -> el código y el nombre */
    sscanf(centers[ap].name, "%7s %63[^\n]", code, rest);
    snprintf(cmd, sizeof(cmd), IRADIO " atc '%s' '%s' 2>/dev/null", code, rest);
    FILE *p = popen(cmd, "r");
    if (p) {
        while (fgets(line, sizeof(line), p) && r.n < MAX_TOWERS) {
            line[strcspn(line, "\n")] = 0;
            if (!strncmp(line, "error ", 6)) {
                if (strstr(line, "no se encontraron")) continue;
                snprintf(r.err, sizeof(r.err), "%s", line + 6);
                continue;
            }
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab      = 0;
            char *end = strchr(tab + 1, '\t');
            if (end) *end = 0;
            snprintf(towers[r.n].name, sizeof(towers[0].name), "%s", line);
            snprintf(towers[r.n].url, sizeof(towers[0].url), "%s", tab + 1);
            r.n++;
        }
        pclose(p);
    }
    scheduler_put(tower_found, &r, sizeof(r));
    return NULL;
}

/* Escuchar torre: la primera emisora; otra vez, la siguiente; después de la última, se deja de escuchar */
static void tower_cb(button_data_t *b) {
    (void)b;
    if (radio_airband()) {
        msg_update_text_fmt("%s %.3f MHz", tr("Listening on the radio"), cparam_i_get(cfg_fg_freq) / 1e6);
        return;
    }
    if (tower_searching) return;
    if (tower_i >= 0) {
        if (tower_i + 1 < towers_n) {
            tower_i++;
            if (play_alive) {
                play_next_pending = true;
                tower_stop();
            } else {
                tower_launch();
            }
        } else {
            tower_i        = -1;
            tower_state[0] = 0;
            tower_stop();
        }
        draw();
        return;
    }
    int *a = malloc(2 * sizeof(int));
    if (!a) return;
    a[0]            = airport_i();
    a[1]            = ++tower_gen;
    tower_searching = true;
    towers_n        = 0;
    snprintf(tower_state, sizeof(tower_state), "%s %s...", tr("Searching the tower of"), centers[a[0]].name);
    pthread_t t;
    if (pthread_create(&t, NULL, tower_search_fn, a) == 0) {
        pthread_detach(t);
    } else {
        free(a);
        tower_searching = false;
    }
    draw();
}

/* ---------- botones y teclas ---------- */

static void center_cb(button_data_t *b) {
    (void)b;
    center_i = (center_i + 1) % N_CENTERS;
    if (tower_i >= 0 || tower_searching) {   /* otro aeropuerto: su torre es otra */
        tower_gen++;
        tower_searching = false;
        tower_i         = -1;
        tower_state[0]  = 0;
        tower_stop();
    }
    restart();
}

static void range_cb(button_data_t *b) {
    (void)b;
    range_i = (range_i + 1) % N_RANGES;
    restart();
}

static void update_cb(button_data_t *b) {
    (void)b;
    start_update();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            if (sel > 0) sel--;
            draw();
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            if (sel < n_aircraft() - 1) sel++;
            draw();
            break;
        case LV_KEY_ENTER:
            center_cb(NULL);
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_coord_t x, lv_coord_t y, lv_coord_t w) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_label_set_recolor(l, true);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    lv_coord_t side = DIALOG_H - 34;

    dialog.obj = dialog_init(parent);
    gen++;

    status_lbl = lv_label_create(dialog.obj);
    lv_label_set_recolor(status_lbl, true);
    lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(status_lbl, 770);
    lv_obj_set_style_text_font(status_lbl, &sony_20, 0);
    lv_obj_set_style_text_color(status_lbl, TC(text_dim), 0);
    lv_obj_set_pos(status_lbl, 14, 4);

    radar = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(radar);
    lv_obj_set_pos(radar, 8, 30);
    lv_obj_set_size(radar, side, side);
    lv_obj_add_event_cb(radar, radar_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(radar, key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, radar);
    lv_group_set_editing(keyboard_group, true);

    lv_coord_t lx = side + 20, lw = 800 - lx - 10;
    list_lbl   = mk_label(dialog.obj, &sony_18, lx, 32, lw);
    lv_obj_set_style_text_font(list_lbl, &sony_18, 0);
    detail_lbl = mk_label(dialog.obj, &sony_18, lx, 32 + LIST_ROWS * 21 + 4, lw);
    lv_label_set_long_mode(detail_lbl, LV_LABEL_LONG_WRAP);
    tower_lbl = mk_label(dialog.obj, &sony_18, lx, DIALOG_H - 30, lw);
    lv_label_set_long_mode(tower_lbl, LV_LABEL_LONG_DOT);

    buttons_unload_page();
    buttons_load_page(&btn_page);

    sel = top = 0;
    draw();
    start_update();
    refresh_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}

static void destruct_cb(void) {
    gen++;
    tower_gen++;
    tower_searching   = false;
    tower_i           = -1;
    tower_state[0]    = 0;
    play_next_pending = false;
    tower_stop();
    updating = false;
    if (refresh_timer) lv_timer_del(refresh_timer);
    refresh_timer = NULL;
    status_lbl = radar = list_lbl = detail_lbl = tower_lbl = NULL;
    cJSON_Delete(data);
    data = NULL;
}
