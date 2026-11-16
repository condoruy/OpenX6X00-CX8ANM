/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CX8ANM: la voz de DMR a PCM (ver dmr_ambe.h). El orden de los bits de cada trama (el entrelazado del
 *  estándar, ETSI TS 102 361-1) es el de DSD (tablas rW rX rY rZ, licencia ISC).
 */
#include "dmr_ambe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mbelib.h"

static const int rW[36] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                           0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2};
static const int rX[36] = {23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2,
                           14, 1, 13, 0, 12, 10, 11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4};
static const int rY[36] = {0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3,
                           1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3};
static const int rZ[36] = {5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12, 22, 11, 21, 10, 20, 9,
                           19, 8, 18, 7, 17, 6, 16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0};

struct dmr_ambe {
    mbe_parms cur, prev, prev_enh;
    float     gain;      /* control automático de volumen */
};

dmr_ambe_t *dmr_ambe_new(void) {
    dmr_ambe_t *d = calloc(1, sizeof(*d));
    if (d) dmr_ambe_reset(d);
    return d;
}

void dmr_ambe_free(dmr_ambe_t *d) {
    free(d);
}

void dmr_ambe_reset(dmr_ambe_t *d) {
    mbe_initMbeParms(&d->cur, &d->prev, &d->prev_enh);
    d->gain = 7.0f;
}

static inline int bit_at(const uint8_t *b, int n) {
    return (b[n >> 3] >> (7 - (n & 7))) & 1;
}

/* 72 bits de la ráfaga (trama idx 0, 1 o 2; la segunda trama salta los 48 del medio) al cuadro de mbelib */
static void frame_bits(const uint8_t *burst, int idx, char fr[4][24]) {
    memset(fr, 0, 4 * 24);
    for (int i = 0; i < 36; i++) {
        int n = idx == 0 ? 2 * i : idx == 2 ? 192 + 2 * i : (i < 18 ? 72 + 2 * i : 156 + 2 * (i - 18));
        fr[rW[i]][rX[i]] = (char)bit_at(burst, n);
        fr[rY[i]][rZ[i]] = (char)bit_at(burst, n + 1);
    }
}

int dmr_ambe_burst(dmr_ambe_t *d, const uint8_t burst[DMR_BURST_BYTES], int16_t pcm[DMR_BURST_PCM]) {
    int   total = 0;
    float out[160];

    for (int f = 0; f < 3; f++) {
        char fr[4][24], ambe_d[49], err_str[64];
        int  errs = 0, errs2 = 0;

        frame_bits(burst, f, fr);
        mbe_processAmbe3600x2450Framef(out, &errs, &errs2, err_str, fr, ambe_d, &d->cur, &d->prev, &d->prev_enh, 3);
        total += errs2;

        /* El volumen: que el pico quede cerca de 1/3 de la escala, sin saltos (sube despacio, baja enseguida) */
        float peak = 0;
        for (int i = 0; i < 160; i++) {
            float a = fabsf(out[i]);
            if (a > peak) peak = a;
        }
        if (peak > 1.0f) {
            float want = 11000.0f / peak;
            if (want < d->gain) d->gain = want;
            else d->gain += (want - d->gain) * 0.05f;
        }
        if (d->gain > 50.0f) d->gain = 50.0f;
        for (int i = 0; i < 160; i++) {
            float s = out[i] * d->gain;
            pcm[f * 160 + i] = (int16_t)(s > 32000.f ? 32000 : s < -32000.f ? -32000 : s);
        }
    }
    return total;
}
