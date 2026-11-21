/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CX8ANM: a world map with live points (the DMR app's world activity). The coastlines of world_map.h
 *  and the projection of map_geo.c, as js8_map.c draws them; one map at a time.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LIVE_MAP_IDLE, LIVE_MAP_ACTIVE, LIVE_MAP_HIGHLIGHT };

typedef struct {
    float   lat, lon;
    uint8_t kind;        /* LIVE_MAP_*: a faint dot, a bright dot, a ring with its label */
    char    label[16];
} live_map_pt_t;

/* The canvas (w x h); deleting it frees the map */
lv_obj_t *live_map_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

/* Draw the points: zoom 1 the world (centred on lon of the highlighted point, or clon), 2, 4, 8 around
 * clat/clon. box: the floating window (lines separated by \n, the first one bold), NULL none; it goes to the
 * corner away from the highlighted point. footer: a line at the bottom left, NULL none */
void live_map_draw(const live_map_pt_t *pts, int n, int zoom, double clat, double clon, const char *box,
                   const char *footer);

#ifdef __cplusplus
}
#endif
