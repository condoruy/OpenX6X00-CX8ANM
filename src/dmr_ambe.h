/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CX8ANM: la voz de DMR (AMBE+2 3600x2450) a PCM, con mbelib (third-party/mbelib). Una ráfaga de voz DMR
 *  (33 bytes, 264 bits: 108 de voz, 48 de sincronismo o señalización, 108 de voz) lleva tres tramas de 20 ms;
 *  cada una sale como 160 muestras de 8 kHz.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMR_AMBE_RATE   8000
#define DMR_BURST_BYTES 33
#define DMR_BURST_PCM   480     /* tres tramas de 160 muestras */

typedef struct dmr_ambe dmr_ambe_t;

dmr_ambe_t *dmr_ambe_new(void);
void        dmr_ambe_free(dmr_ambe_t *d);
/* Al empezar otra transmisión: se olvida la anterior (parámetros de la voz y el control de volumen) */
void        dmr_ambe_reset(dmr_ambe_t *d);
/* Una ráfaga a 480 muestras; devuelve los errores corregidos (el total de las tres tramas) */
int         dmr_ambe_burst(dmr_ambe_t *d, const uint8_t burst[DMR_BURST_BYTES], int16_t pcm[DMR_BURST_PCM]);

#ifdef __cplusplus
}
#endif
