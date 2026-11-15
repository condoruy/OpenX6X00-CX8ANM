/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CX8ANM: GhostNet (s2underground, versión 1.5) en la radio. Los horarios de las redes y los puentes de datos
 *  con su recordatorio en cualquier pantalla, una red propia en español, el grupo regional (@GN + país + estado),
 *  el aviso rojo de @GSTFLASH y la traducción inglés <-> español de JS8 (x6100-translate, con WiFi).
 *  Lo que se elige queda en /mnt/ghostnet.conf, que también edita la página web GhostNet.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GHOSTNET_CONF BOARD_DATA "/ghostnet.conf"   /* /mnt/ghostnet.conf en la radio */

typedef struct {
    bool remind;          /* recordatorio antes de cada red elegida */
    int  remind_min;      /* cuántos minutos antes */
    char nets[96];        /* las redes con recordatorio: "NA,B_NAEU,ES" */
    bool es_on;           /* la red propia en español */
    char es_name[32];
    int  es_day;          /* día, en hora local: 0 domingo ... 6 sábado */
    int  es_hh, es_mm;    /* hora local */
    int  es_min;          /* duración */
    int  es_khz;
    char es_mode[8];      /* "JS8", "RTTY", "LSB", "USB" */
    bool translate_rx;    /* los mensajes en inglés llegan con su traducción */
    bool translate_tx;    /* lo que escribo sale traducido al inglés (se revisa antes de mandar) */
    char region[16];      /* el grupo regional elegido a mano; "" lo arma solo */
} ghostnet_conf_t;

/* Una ventana de una red: la próxima vez (o la que está pasando) */
typedef struct {
    time_t      start;    /* en segundos UTC */
    int         minutes;
    int         khz;
    const char *net;      /* "NA", "EU", ..., "ES" */
    char        name[40]; /* "Norteamérica", "Puente NA-Europa", "Red hispana" */
    char        what[40]; /* "JS8 40 m", "RTTY 45,45 baudios" */
    char        mode[8];  /* "JS8", "VARA", "RTTY", "LSB", "ALE" */
} ghostnet_slot_t;

void                   ghostnet_init(void);
const ghostnet_conf_t *ghostnet_conf(void);             /* la de /mnt/ghostnet.conf, releída si cambió */
void                   ghostnet_conf_save(const ghostnet_conf_t *c);
bool                   ghostnet_net_reminded(const char *net);   /* ¿esa red tiene recordatorio? */

/* Las ventanas de las redes en las próximas `hours` horas (las que están pasando también), por hora */
int ghostnet_upcoming(ghostnet_slot_t *out, int max, int hours, bool only_reminded);
/* "jue 22:00", en hora local */
void ghostnet_local_when(time_t t, char *out, size_t size);

/* El grupo regional: "@GNURYMO" (país de Ajustes + departamento de Meteo), o el elegido a mano */
void ghostnet_region_tag(char *out, size_t size);
/* Suma @GHOSTNET, @GSTFLASH y el grupo regional a los grupos (separados por espacios) que no los tengan */
void ghostnet_add_groups(char *groups, size_t size);

/* Abre la app RTTY en 7.077 MHz a 45,45 baudios (la ventana de RTTY de las redes), cerrada la app actual */
void ghostnet_rtty_open(void);

/* El registro de GhostNet para la página web (BOARD_DATA/ghostnet_log.txt, una línea por mensaje, separada por
 * tabulaciones: R recibido, X enviado, T traducción del recibido de igual id). to: callsign o @GRUPO */
#define GHOSTNET_LOG BOARD_DATA "/ghostnet_log.txt"
bool ghostnet_is_group(const char *to);   /* @GHOSTNET, @GSTFLASH o @GN... */
void ghostnet_log(char kind, uint32_t id, const char *from, const char *to, int snr, int32_t dial_hz, const char *text);

/* El recuadro rojo de un mensaje de emergencia a @GSTFLASH */
void ghostnet_flash_show(const char *text);

/* Traducción en otro hilo; `done` corre en el de la pantalla con ok y el texto o el motivo del error.
 * rx: del inglés al español (ok false y "" si no está en inglés); tx: al inglés, como JS8 lo puede mandar */
typedef void (*ghostnet_tr_cb)(uint32_t id, bool ok, const char *text);
bool ghostnet_translate(bool to_english, const char *text, uint32_t id, ghostnet_tr_cb done);

#ifdef __cplusplus
}
#endif
