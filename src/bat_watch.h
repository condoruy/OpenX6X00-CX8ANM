/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the base's supply readings as radio.c takes them, about every 0.4 s: whether they
 *  are the battery's yet, external power, and the low battery that starts the 30 s power-off
 *  (screen_main.c).
 *
 *  Seen on one X6100 on battery (#17), by its owner: after power-on its base sent about 3 s of
 *  nothing measured (0 V on both), then a % that climbed to the battery's over about 7 s more.
 *  Other bases, the X6200's among them, have not been watched doing it, and none of it was logged.
 *  Such readings are not the battery's. The top bar, the battery log with the time left, its
 *  10 % warning and the low battery take readings from BAT_SETTLE_MS after the first one with a
 *  voltage, which covers that sequence and costs a base that reports at once only the wait.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Voltages in 0.1 V; LOW + HYSTERESIS = HIGH (upstream 21c2b93) */
#define BAT_LOW         BOARD_BAT_LOW   /* the power-off: 6.0 V, the X6200 6.8 V (board.h) */
#define BAT_HYSTERESIS  2
#define BAT_HIGH        (BAT_LOW + BAT_HYSTERESIS)
#define BAT_LOW_READS   3       /* in a row, about 1.2 s */
#define BAT_NONE        30      /* under 3.0 V: no battery, none fitted or nothing measured yet (clock.c) */
#define BAT_EXT         90      /* on the jack: external power whatever the flags say */
#define BAT_SETTLE_MS   15000   /* after the first reading with a voltage: twice the climb #17 saw */
#define BAT_HELD_MS     5000    /* the X6200: DC IN at BOARD_BAT_EXT_MIN so long holds the battery up (bat_watch.c) */

typedef struct {
    uint64_t settle_at;     /* the readings count from then on; 0 before the first with a voltage */
    uint8_t  low_reads;     /* readings in a row of a battery at BAT_LOW or under */
    bool     low;           /* low battery: the power-off runs */
    bool     counts;        /* the last reading is the battery's and the supply's */
    bool     ext;           /* the last reading: on external power */
    bool     by_volts;      /* the % shown is the voltage's, not the base's (bat_watch_cap) */
    uint8_t  cap_vbat;      /* the voltage the voltage's % is of (bat_watch_cap); 0 none yet */
    uint8_t  up_reads;      /* readings in a row over cap_vbat */
    bool     held;          /* the X6200: on a supply that holds the battery up (bat_watch.c) */
    uint64_t over_since;    /* the X6200: the first of the readings in a row at BOARD_BAT_EXT_MIN and more; 0 none */
    uint8_t  under_reads;   /* the X6200: receive readings in a row under BOARD_BAT_EXT_MIN */
    bool     has_shown;     /* a % was shown on battery (bat_watch_shown) */
    uint8_t  shown;         /* the last one */
    uint64_t ext_since;     /* the first reading of this stretch on external power (bat_watch_ext_cap); 0 none */
    uint8_t  ext_base;      /* the base's % at its start, the jump of its first BAT_EXT_JUMP_MS taken in */
} bat_watch_t;

/* One reading; bat_missing: the base found no battery (the X6200's says so), tx: the base transmits. True when low
 * changed */
bool bat_watch_reading(bat_watch_t *w, uint8_t vbat, uint8_t vext, bool vext_flag, bool charging, bool bat_missing,
                       bool tx, uint64_t now_ms);

/* The % of a pack at vbat (0.1 V) on battery and receive: what an X6100 base (1.1.8) reports there,
 * from its discharge 8.3 to 6.4 V on the bench */
uint8_t bat_cap_of_volts(uint8_t vbat);

/* The % to show on battery, on receive: the base's, and the voltage's while the two are more than
 * BAT_CAP_APART apart, until they meet within BAT_CAP_MET with the voltage's at BAT_CAP_STEEP or more.
 * The base counts the charge it sees: a pack charged with the radio off stayed near empty at 8.2 V,
 * in Xiegu's firmware too (#49). From 7.3 V down the voltage's falls through 24, 9 and 3 to 0, where
 * it meets a base stuck at 0 as well as one that counts right: there it does not go back to the base's.
 * Nor is a stuck base 25 apart there: the voltage's is also taken when the base says 0 % with the
 * voltage's at BAT_CAP_AT_ZERO or more (7.2 V and up), where one that counts right still had some */
#define BAT_CAP_APART   25
#define BAT_CAP_MET     10
#define BAT_CAP_STEEP   25
#define BAT_CAP_AT_ZERO 9
/* CX8ANM: the voltage's only when it is the higher (a base that lost count down, #49); a base higher than the voltage
 * is a worn pack's sag under load and is shown, but no more than BAT_CAP_END_MAX % from BAT_CAP_END_V down and
 * BAT_CAP_LAST_MAX % from BAT_CAP_LAST_V down: the worn pack went from 6.5 V to the base's cut at 6.1 V in 18 min */
#define BAT_CAP_END_V    65
#define BAT_CAP_END_MAX  10
#define BAT_CAP_LAST_V   63
#define BAT_CAP_LAST_MAX 3

/* The voltage's % follows a voltage down at once, back up 0.1 V only after BAT_CAP_UP_READS readings in a row over
 * it (about 1 min), 0.2 V or more at once: on a 0.1 V step the readings go back and forth, and the % with them,
 * 24 and 9 % in turn at 7.3 and 7.2 V (#160). From the first reading again after external power */
#define BAT_CAP_UP_READS 150
uint8_t bat_watch_cap(bat_watch_t *w, uint8_t vbat, uint8_t batcap);

/* CX8ANM: the % shown on external power. The base's jumps 15-20 points up the moment the charger comes in (the
 * charging voltage) and a pack shown at 1 % on battery came up at 58 %; with the voltage's shown on battery (a base
 * that has lost count, #49) the two were further apart still. So on external power the % goes on from the last one
 * shown on battery and follows the base's from where it stood at the start (its jump of the first BAT_EXT_JUMP_MS
 * taken in), stretched to reach 100 when the base's reaches BAT_EXT_FULL, where the charger stops. With none shown on
 * battery since the start, the base's. bat_watch_shown: the % shown on battery, each reading */
#define BAT_EXT_JUMP_MS 30000
#define BAT_EXT_FULL    97
void    bat_watch_shown(bat_watch_t *w, uint8_t cap);
uint8_t bat_watch_ext_cap(bat_watch_t *w, uint8_t batcap, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
