/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CX8ANM: alertas meteorológicas de INUMET (x6100-meteo) en pantalla
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void        meteo_alerts_init(void);
void        meteo_alerts_now(void);              /* la próxima vuelta consulta enseguida */
void        meteo_alerts_check_file(void);       /* las alertas de /tmp/x6100-meteo-alerts.json */
bool        meteo_position(double *lat, double *lon);   /* GPS si hay, si no el locator QTH, si no la capital del país */
const char *meteo_position_src(double *lat, double *lon);   /* lo mismo y de dónde: gps, qth, country, default */
void        meteo_helper_cmd(char *cmd, size_t size, const char *what);   /* "x6100-meteo update|alerts LAT LON PAIS ORIGEN" */
uint32_t    meteo_level_color(int level);
const char *meteo_level_name(int level, const char *kind);
void        meteo_short_time(const char *in, char *out, size_t size);

#ifdef __cplusplus
}
#endif
