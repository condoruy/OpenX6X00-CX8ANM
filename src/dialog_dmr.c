/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CX8ANM: escucha de DMR por Internet. x6100-dmr entra a la red como un hotspot (solo recepción) y entrega
 *  las ráfagas de voz; acá se decodifican (AMBE+2, mbelib: dmr_ambe.c), se pasan de 8 a 44,1 kHz y van al
 *  parlante o a los auriculares Bluetooth, como Zello y la radio por Internet. Nunca se transmite.
 *
 *  Red, DMR ID, clave del hotspot y grupos de conversación (TG): web de la radio, página DMR (/mnt/dmr.conf).
 */
#include "dialog_dmr.h"

#include "audio.h"
#include "board.h"
#include "buttons.h"
#include "dialog.h"
#include "dmr_ambe.h"
#include "events.h"
#include "i18n.h"
#include "keyboard.h"
#include "msg.h"
#include "radio.h"
#include "scheduler.h"
#include "styles.h"
#include "theme.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CONF_FILE   BOARD_DATA "/dmr.conf"   /* /mnt/dmr.conf en la radio */
#define STATUS_FILE "/tmp/x6100-dmr.status"
#define HEARD_FILE  "/tmp/x6100-dmr.heard"
#define HELPER      "/usr/sbin/x6100-dmr"
#define MAX_TG      30
#define HEARD_ROWS  12

typedef struct {
    int  tg;
    char name[48];
} tg_t;

static tg_t tgs[MAX_TG];
static int  tgs_n;
static int  cur_tg;
static char network[16];      /* "bm", "freedmr", "other" */
static bool have_apikey;
static char dmr_id[16];

static volatile bool  run_on;
static volatile bool  run_alive;
static volatile pid_t run_pid;
static bool           restart_pending;
static volatile bool  auto_restart;
static int            auto_tries;

static lv_obj_t   *title_lbl, *state_lbl, *talk_lbl, *table, *heard_tbl;
static lv_timer_t *status_timer;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void power_cb(button_data_t *b);
static void reload_cb(button_data_t *b);
static void help_cb(button_data_t *b);

static button_data_t btn_power  = {.type = BTN_TEXT, .label = "Connect /\nDisconnect", .press = power_cb};
static button_data_t btn_reload = {.type = BTN_TEXT, .label = "Reload\ntalkgroups", .press = reload_cb};
static button_data_t btn_help   = {.type = BTN_TEXT, .label = "Help", .press = help_cb};

static buttons_page_t btn_page = {{&btn_power, &btn_reload, &btn_help}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .btn_page     = &btn_page,
    .key_cb       = NULL,
};

dialog_t *dialog_dmr = &dialog;

static void dlog(const char *what) {
    FILE *f = fopen("/mnt/app_logs/dmr.txt", "a");
    if (!f) return;
    time_t    t = time(NULL);
    struct tm tm;
    char      ts[32];
    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s GUI: %s\n", ts, what);
    fclose(f);
}

/* ---------- configuración ---------- */

static void unquote(char *v) {
    size_t n = strlen(v);
    if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
        memmove(v, v + 1, n - 2);
        v[n - 2] = 0;
    }
}

static void load_conf(void) {
    FILE *f = fopen(CONF_FILE, "r");
    char  line[2048];
    int   saved = 0;

    tgs_n       = 0;
    have_apikey = false;
    dmr_id[0]   = 0;
    snprintf(network, sizeof(network), "bm");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq     = 0;
        char *v = eq + 1;
        unquote(v);
        if (!strcmp(line, "NETWORK")) {
            snprintf(network, sizeof(network), "%s", v);
        } else if (!strcmp(line, "DMR_ID")) {
            snprintf(dmr_id, sizeof(dmr_id), "%s", v);
        } else if (!strcmp(line, "APIKEY")) {
            have_apikey = v[0] != 0;
        } else if (!strcmp(line, "TG")) {
            saved = atoi(v);
        } else if (!strcmp(line, "TALKGROUPS")) {
            for (char *p = strtok(v, "|"); p && tgs_n < MAX_TG; p = strtok(NULL, "|")) {
                while (*p == ' ') p++;
                int tg = atoi(p);
                if (tg <= 0) continue;
                while (*p >= '0' && *p <= '9') p++;
                while (*p == ' ') p++;
                tgs[tgs_n].tg = tg;
                snprintf(tgs[tgs_n].name, sizeof(tgs[0].name), "%s", p);
                tgs_n++;
            }
        }
    }
    fclose(f);
    if (!cur_tg) cur_tg = saved ? saved : (tgs_n ? tgs[0].tg : 0);
}

/* El TG elegido queda en /mnt/dmr.conf (TG=), para la próxima vez */
static void save_tg(int tg) {
    FILE *f = fopen(CONF_FILE, "r");
    if (!f) return;
    char  *buf = NULL;
    size_t len = 0;
    FILE  *m   = open_memstream(&buf, &len);
    char   line[2048];
    bool   done = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "TG=", 3)) {
            fprintf(m, "TG=%d\n", tg);
            done = true;
        } else {
            fputs(line, m);
        }
    }
    if (!done) fprintf(m, "TG=%d\n", tg);
    fclose(f);
    fclose(m);
    FILE *o = fopen(CONF_FILE ".tmp", "w");
    if (o) {
        fwrite(buf, 1, len, o);
        fclose(o);
        rename(CONF_FILE ".tmp", CONF_FILE);
    }
    free(buf);
}

/* ---------- la voz: ráfagas -> 8 kHz -> 44,1 kHz ---------- */

static void end_child(pid_t pid) {
    kill(-pid, SIGTERM);
    for (int i = 0; i < 40; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        usleep(50000);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static bool read_full(int fd, void *buf, size_t n) {
    size_t have = 0;
    while (have < n) {
        ssize_t r = read(fd, (char *)buf + have, n - have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        have += r;
    }
    return true;
}

static void run_ended(void *arg);

static void *run_fn(void *arg) {
    int  tg = (int)(intptr_t)arg;
    int  fds[2];
    char tgs_arg[16];

    snprintf(tgs_arg, sizeof(tgs_arg), "%d", tg);
    if (pipe(fds) != 0) {
        run_on = run_alive = false;
        scheduler_put_noargs(run_ended);
        return NULL;
    }
    unlink(STATUS_FILE);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) dup2(dn, 2);
        execl(HELPER, HELPER, "run", tgs_arg, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        run_on = run_alive = false;
        scheduler_put_noargs(run_ended);
        return NULL;
    }
    setpgid(pid, pid);
    run_pid = pid;

    bool bt = audio_play_bt_begin();
    if (!bt) radio_set_play_mode(AUDIO_PLAY_ON);

    dmr_ambe_t    *dec = dmr_ambe_new();
    static int16_t pcm[DMR_BURST_PCM];
    static int16_t out[DMR_BURST_PCM * 6 + 8];
    double         pos  = 0;
    int16_t        prev = 0;
    const double   step = (double)DMR_AMBE_RATE / AUDIO_PLAY_RATE;

    while (run_on && dec) {
        uint8_t type;
        if (!read_full(fds[0], &type, 1)) {
            if (run_on) dlog("x6100-dmr terminó");
            break;
        }
        if (type == 'E') {           /* fin de una transmisión */
            dmr_ambe_reset(dec);
            prev = 0;
            pos  = 0;
            continue;
        }
        if (type != 'V') continue;
        uint8_t burst[DMR_BURST_BYTES];
        if (!read_full(fds[0], burst, sizeof(burst))) break;
        dmr_ambe_burst(dec, burst, pcm);

        size_t o = 0;
        while (pos < DMR_BURST_PCM) {
            double  fl = floor(pos);
            int     i  = (int)fl;
            int16_t a  = i == 0 ? prev : pcm[i - 1];
            int16_t b  = pcm[i];
            out[o++]   = (int16_t)(a + (b - a) * (pos - fl));
            pos += step;
        }
        pos -= DMR_BURST_PCM;
        prev = pcm[DMR_BURST_PCM - 1];
        if (o && audio_play_until(out, o, &run_on, 3000) == -2) dlog("el parlante no tomó el audio en 3 s");
    }
    dmr_ambe_free(dec);
    run_pid      = 0;
    auto_restart = run_on;
    end_child(pid);
    close(fds[0]);

    audio_play_flush();
    if (bt) audio_play_bt_end();
    else radio_set_play_mode(AUDIO_PLAY_OFF);
    run_on    = false;
    run_alive = false;
    unlink(STATUS_FILE);      /* que la web no muestre un estado viejo con la app cerrada */
    scheduler_put_noargs(run_ended);
    return NULL;
}

static void launch(void);
static void show_all(void);

static void stop_run(void) {
    run_on    = false;
    pid_t pid = run_pid;
    if (pid > 0) kill(-pid, SIGTERM);
}

static void auto_relaunch(lv_timer_t *t) {
    (void)t;
    if (dialog.run && !run_alive) launch();
}

static void run_ended(void *arg) {
    (void)arg;
    if (restart_pending && !run_alive && dialog.run) {
        restart_pending = false;
        launch();
    } else if (auto_restart && !run_alive && dialog.run && auto_tries < 5) {
        auto_restart = false;
        auto_tries++;
        lv_timer_t *t = lv_timer_create(auto_relaunch, 5000, NULL);
        lv_timer_set_repeat_count(t, 1);
    }
    if (dialog.run) show_all();
}

static void launch(void) {
    pthread_t t;

    if (!dmr_id[0]) {
        msg_update_text_fmt("%s", tr("Set your DMR ID on the radio's web page (DMR page)"));
        return;
    }
    run_on    = true;
    run_alive = true;
    if (pthread_create(&t, NULL, run_fn, (void *)(intptr_t)cur_tg) == 0) {
        pthread_detach(t);
    } else {
        run_on = run_alive = false;
    }
}

static void connect_to(int tg) {
    cur_tg     = tg;
    auto_tries = 0;
    save_tg(tg);
    if (run_alive) {
        stop_run();
        restart_pending = true;
    } else {
        launch();
    }
    show_all();
}

/* ---------- pantalla ---------- */

static const char *net_name(void) {
    if (!strcmp(network, "bm")) return "BrandMeister";
    if (!strcmp(network, "freedmr")) return "FreeDMR";
    return tr("Other network");
}

/* ¿se puede elegir el TG desde la radio? En BrandMeister hace falta la llave de la API */
static bool tg_selectable(void) {
    return strcmp(network, "bm") || have_apikey;
}

static void show_all(void) {
    if (!table) return;
    lv_label_set_text_fmt(title_lbl, "#%06x DMR#  #%06x %s  %s%s#", theme_hex(TC_accent), theme_hex(TC_text_dim),
                          net_name(), dmr_id[0] ? "ID " : "", dmr_id[0] ? dmr_id : tr("no DMR ID"));
    lv_table_set_row_cnt(table, tgs_n ? tgs_n : 1);
    if (!tgs_n) {
        lv_table_set_cell_value(table, 0, 0, tr("No talkgroups: add them on the radio's web page (DMR page)"));
        lv_table_set_cell_value(table, 0, 1, "");
    }
    for (int i = 0; i < tgs_n; i++) {
        bool cur = run_on && tgs[i].tg == cur_tg;
        lv_table_set_cell_value_fmt(table, i, 0, "%s %d %s", cur ? ">" : " ", tgs[i].tg, tgs[i].name);
        lv_table_set_cell_value(table, i, 1, cur ? tr("listening") : "");
    }
}

static void heard_update(void) {
    if (!heard_tbl) return;
    FILE *f = fopen(HEARD_FILE, "r");
    int   n = 0;
    char  line[256];

    lv_table_set_cell_value(heard_tbl, 0, 0, tr("Last heard"));
    lv_table_set_cell_value(heard_tbl, 0, 1, "TG");
    lv_table_set_cell_value(heard_tbl, 0, 2, "");
    while (f && n < HEARD_ROWS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char *p[5] = {line};
        int   k    = 1;
        for (char *c = line; *c && k < 5; c++)
            if (*c == '\t') {
                *c     = 0;
                p[k++] = c + 1;
            }
        if (k < 5) continue;
        time_t    t = (time_t)atoll(p[0]);
        struct tm tm;
        char      hm[8];
        localtime_r(&t, &tm);
        strftime(hm, sizeof(hm), "%H:%M", &tm);
        n++;
        lv_table_set_cell_value(heard_tbl, n, 0, p[2]);
        lv_table_set_cell_value(heard_tbl, n, 1, p[3]);
        lv_table_set_cell_value_fmt(heard_tbl, n, 2, "%s %ss", hm, p[4]);
    }
    if (f) fclose(f);
    lv_table_set_row_cnt(heard_tbl, n + 1);
}

static void status_cb(lv_timer_t *t) {
    (void)t;
    char  st[200] = "", info[128] = "", talker[128] = "", note[160] = "";
    FILE *f = fopen(STATUS_FILE, "r");

    if (f) {
        if (fgets(st, sizeof(st), f)) st[strcspn(st, "\n")] = 0;
        if (fgets(info, sizeof(info), f)) info[strcspn(info, "\n")] = 0;
        if (fgets(talker, sizeof(talker), f)) talker[strcspn(talker, "\n")] = 0;
        if (fgets(note, sizeof(note), f)) note[strcspn(note, "\n")] = 0;
        fclose(f);
    }
    if (!state_lbl) return;
    if (!run_on) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              tg_selectable() ? tr("Disconnected: choose a talkgroup and press the MFK or Connect")
                                              : tr("Disconnected: press Connect (talkgroups set in BrandMeister SelfCare)"));
    } else if (!strncmp(st, "error", 5)) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_err), st[5] ? tr(st + 6) : tr("Error"));
    } else if (!strcmp(st, "online")) {
        if (cur_tg && tg_selectable())
            lv_label_set_text_fmt(state_lbl, "#%06x %s %s# TG %d%s%s", theme_hex(TC_accent), ">", tr("Listening"),
                                  cur_tg, note[0] ? "  " : "", note);
        else
            lv_label_set_text_fmt(state_lbl, "#%06x %s %s# %s", theme_hex(TC_accent), ">", tr("Listening"),
                                  tr("the talkgroups of your hotspot in BrandMeister SelfCare"));
    } else {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_text_dim), tr("Connecting..."));
    }
    char *tab = strchr(talker, '\t');
    if (run_on && talker[0] && tab) {
        *tab++    = 0;
        char *tg  = strchr(tab, '\t');
        lv_label_set_text_fmt(talk_lbl, "#%06x %s %s#  TG %s", theme_hex(TC_accent), ">", talker,
                              tg ? tg + 1 : "");
    } else {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              run_on ? tr("Listen only: the radio never transmits on DMR") : "");
    }
    heard_update();
}

static int selected(void) {
    uint16_t row = 0, col = 0;
    if (!table || !tgs_n) return -1;
    lv_table_get_selected_cell(table, &row, &col);
    return row == LV_TABLE_CELL_NONE || row >= tgs_n ? -1 : row;
}

static void power_cb(button_data_t *b) {
    (void)b;
    if (run_on || run_alive) {
        restart_pending = false;
        stop_run();
        show_all();
        return;
    }
    int i = selected();
    if (i >= 0 && tg_selectable()) cur_tg = tgs[i].tg;
    auto_tries = 0;
    launch();
    show_all();
}

static void reload_cb(button_data_t *b) {
    (void)b;
    load_conf();
    show_all();
    msg_update_text_fmt("%s: %d", tr("Talkgroups"), tgs_n);
}

static void help_cb(button_data_t *b) {
    (void)b;
    msg_update_text_fmt("%s", tr("DMR: set your DMR ID, hotspot password and talkgroups on the radio's web page (DMR page)"));
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_ENTER: {
            int i = selected();
            if (i < 0) break;
            if (run_on && tgs[i].tg == cur_tg) power_cb(NULL);
            else if (tg_selectable() || !run_on) connect_to(tgs[i].tg);
            else msg_update_text_fmt("%s", tr("BrandMeister: add your API key on the DMR web page to change talkgroups from the radio"));
            break;
        }
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

static lv_obj_t *make_table(int x, int w, int cols) {
    lv_obj_t *t = lv_table_create(dialog.obj);
    lv_obj_remove_style(t, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_pos(t, x, 100);
    lv_obj_set_size(t, w, DIALOG_H - 108);
    lv_table_set_col_cnt(t, cols);
    lv_obj_set_style_border_width(t, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(t, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_text_font(t, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(t, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(t, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(t, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(t, 0, LV_PART_ITEMS);
    return t;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    title_lbl = lv_label_create(dialog.obj);
    lv_label_set_recolor(title_lbl, true);
    lv_obj_set_style_text_font(title_lbl, &sony_22, 0);
    lv_obj_set_pos(title_lbl, 14, 6);

    state_lbl = lv_label_create(dialog.obj);
    lv_label_set_recolor(state_lbl, true);
    lv_label_set_long_mode(state_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(state_lbl, 770);
    lv_obj_set_style_text_font(state_lbl, &sony_22, 0);
    lv_obj_set_style_text_color(state_lbl, TC(text), 0);
    lv_obj_set_pos(state_lbl, 14, 38);

    talk_lbl = lv_label_create(dialog.obj);
    lv_label_set_recolor(talk_lbl, true);
    lv_label_set_long_mode(talk_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(talk_lbl, 770);
    lv_obj_set_style_text_font(talk_lbl, &sony_24, 0);
    lv_obj_set_style_text_color(talk_lbl, TC(text_value), 0);
    lv_obj_set_pos(talk_lbl, 14, 66);

    table = make_table(10, 400, 2);
    lv_table_set_col_width(table, 0, 290);
    lv_table_set_col_width(table, 1, 110);
    lv_obj_set_style_text_color(table, TC(focus_text), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_color(table, TC(focus), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(table, 128, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    heard_tbl = make_table(420, 370, 3);
    lv_table_set_col_width(heard_tbl, 0, 190);
    lv_table_set_col_width(heard_tbl, 1, 70);
    lv_table_set_col_width(heard_tbl, 2, 110);
    lv_obj_set_style_text_font(heard_tbl, &sony_20, LV_PART_ITEMS);
    lv_obj_set_style_text_color(heard_tbl, TC(text_dim), LV_PART_ITEMS);
    lv_obj_clear_flag(heard_tbl, LV_OBJ_FLAG_CLICKABLE);

    buttons_unload_page();
    buttons_load_page(&btn_page);

    load_conf();
    show_all();
    for (int i = 0; i < tgs_n; i++)
        if (tgs[i].tg == cur_tg) {
            lv_table_t *tt = (lv_table_t *)table;
            tt->row_act    = i;
            tt->col_act    = 0;
            lv_obj_invalidate(table);
        }

    status_timer = lv_timer_create(status_cb, 400, NULL);
    status_cb(NULL);
}

static void destruct_cb(void) {
    restart_pending = false;
    stop_run();
    if (status_timer) lv_timer_del(status_timer);
    status_timer = NULL;
    title_lbl = state_lbl = talk_lbl = table = heard_tbl = NULL;
}
