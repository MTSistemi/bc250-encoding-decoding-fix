/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_pred.c - intra prediction, Rec. ITU-T H.265 clause 8.4.4.2.
 *
 * Thirty-five modes over blocks from 4x4 to 32x32: planar, DC, and
 * thirty-three angles. All of them read one row above the block and one
 * column to its left, twice as long as the block in each direction, and
 * all of them are built on the same three steps - gather the reference
 * samples, substitute the ones that are not there, filter them, predict.
 *
 * ⚠️ The substitution is not padding. A sample that has not been decoded
 * yet takes the value of the last one that has, walking anticlockwise from
 * the bottom left; a block at the top left corner of a picture, with
 * nothing around it at all, takes mid-grey everywhere. Getting that wrong
 * shows up only at the edges of things, which is where it is least
 * visible and most annoying to find.
 */
#include "hevc_dec_internal.h"

#include <stdlib.h>
#include <string.h>

static inline uint8_t ritaglia8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* The reference array, laid out around the corner: indice 0 is the corner
 * sample p[-1][-1], 1..2N is the row above, and -1..-2N the column to the
 * left. One array instead of two, so the angular modes that reach across
 * the corner can walk straight through it. */
#define RIF(r, i) ((r)[64 + (i)])

/* Clause 6.4.1 through the z-scan order: whether the block at (x, y) has
 * already been decoded.
 *
 * ⚠️ Not "is it above or to the left". A coding tree unit is walked as a
 * quadtree, so the block above right of a transform block may or may not
 * have been decoded depending on where both sit in the tree. The z-scan
 * address is what answers that, and comparing coordinates instead gets the
 * top-right reference samples wrong for exactly the blocks where they
 * matter. */
static bool gia_decodificato(const hevcd_t *d, int x, int y, int x_cur, int y_cur)
{
    const hevc_sps_t *sps = d->sps;
    if (x < 0 || y < 0 || x >= sps->width || y >= sps->height)
        return false;
    const int passo = sps->width >> sps->log2_min_tb;
    const int a = d->min_tb_addr_zs[(y >> sps->log2_min_tb) * passo
                                    + (x >> sps->log2_min_tb)];
    const int b = d->min_tb_addr_zs[(y_cur >> sps->log2_min_tb) * passo
                                    + (x_cur >> sps->log2_min_tb)];
    return a < b;
}

/* 8.4.4.2.2: gather, then substitute. */
static void riferimenti(const hevcd_t *d, int c_idx, int x0, int y0, int n,
                        uint8_t *r)
{
    const uint8_t *piano = d->piano[c_idx];
    const int passo = d->passo[c_idx];
    const int scala = c_idx ? 1 : 0;          /* chroma is half resolution */
    const int lx = x0 << scala, ly = y0 << scala;   /* in luma coordinates */
    const int unita = 1 << scala;             /* luma samples per sample */

    bool c_e[4 * 64 + 1];
    memset(c_e, 0, sizeof(c_e));
    bool qualcosa = false;

    /* The column to the left, from the bottom up, then the corner, then
     * the row above from left to right: the order the substitution walks. */
    for (int i = 0; i < 2 * n; i++) {
        const int y = y0 + 2 * n - 1 - i;
        const int ok = gia_decodificato(d, lx - unita, ly + ((2 * n - 1 - i) << scala),
                                        lx, ly);
        if (ok && y < (d->sps->height >> scala)) {
            RIF(r, -(2 * n - i)) = piano[y * passo + x0 - 1];
            c_e[64 - (2 * n - i)] = true;
            qualcosa = true;
        }
    }
    {
        const int ok = gia_decodificato(d, lx - unita, ly - unita, lx, ly);
        if (ok) {
            RIF(r, 0) = piano[(y0 - 1) * passo + x0 - 1];
            c_e[64] = true;
            qualcosa = true;
        }
    }
    for (int i = 0; i < 2 * n; i++) {
        const int x = x0 + i;
        const int ok = gia_decodificato(d, lx + (i << scala), ly - unita, lx, ly);
        if (ok && x < (d->sps->width >> scala)) {
            RIF(r, i + 1) = piano[(y0 - 1) * passo + x];
            c_e[64 + i + 1] = true;
            qualcosa = true;
        }
    }

    if (!qualcosa) {
        memset(r, 128, 4 * 64 + 1);           /* 1 << (bitDepth - 1) */
        return;
    }

    /* Walk from the bottom left, anticlockwise: each hole takes the value
     * of the one before it. The first hole, if it is at the very start,
     * takes the first sample that does exist. */
    if (!c_e[64 - 2 * n]) {
        int k = 64 - 2 * n;
        while (k <= 64 + 2 * n && !c_e[k]) k++;
        r[64 - 2 * n] = r[k];
        c_e[64 - 2 * n] = true;
    }
    for (int k = 64 - 2 * n + 1; k <= 64 + 2 * n; k++)
        if (!c_e[k]) { r[k] = r[k - 1]; c_e[k] = true; }
}

/* 8.4.4.2.3: whether to smooth the references, and how much. */
static void filtra(const hevcd_t *d, int modo, int n, int c_idx, uint8_t *r)
{
    if (c_idx != 0 || n == 4 || modo == HEVCD_INTRA_DC)
        return;

    /* Table 8-3, by log2 of the block side: 8 has a threshold of seven,
     * 16 of one, 32 of none. A four is handled above, and there is no
     * entry for it here - which is what the two zeros are holding.
     *
     * ⚠️ Indexed by the logarithm, not by the size. The first version of
     * this was off by one place and read past the end for a 32, which is
     * a wrong smoothing decision on exactly the blocks where smoothing
     * matters most. */
    static const int soglia[6] = { 0, 0, 0, 7, 1, 0 };
    int lg = 0;
    while ((1 << lg) < n) lg++;
    /* For planar this comes out as ten, which is what the clause intends:
     * the mode is as far from horizontal and vertical as anything gets. */
    const int dv = abs(modo - 26), dh = abs(modo - 10);
    const int dist = dv < dh ? dv : dh;
    if (dist <= soglia[lg])
        return;

    uint8_t f[4 * 64 + 1];
    memcpy(f, r, sizeof(f));

    /* The strong smoothing of a 32x32 block, when both edges are close
     * enough to a straight line that a ramp will do: a gradient with no
     * steps at all, which is what a flat sky needs. */
    if (d->sps->strong_intra_smoothing && n == 32
        && abs(RIF(r, 0) + RIF(r, 2 * n) - 2 * RIF(r, n)) < 8
        && abs(RIF(r, 0) + RIF(r, -2 * n) - 2 * RIF(r, -n)) < 8) {
        for (int i = 1; i < 2 * n; i++) {
            RIF(f, i) = (uint8_t)(((64 - i) * RIF(r, 0)
                                   + i * RIF(r, 2 * n) + 32) >> 6);
            RIF(f, -i) = (uint8_t)(((64 - i) * RIF(r, 0)
                                    + i * RIF(r, -2 * n) + 32) >> 6);
        }
    } else {
        RIF(f, 0) = (uint8_t)((RIF(r, -1) + 2 * RIF(r, 0) + RIF(r, 1) + 2) >> 2);
        for (int i = 1; i < 2 * n; i++) {
            RIF(f, i) = (uint8_t)((RIF(r, i - 1) + 2 * RIF(r, i)
                                   + RIF(r, i + 1) + 2) >> 2);
            RIF(f, -i) = (uint8_t)((RIF(r, -(i - 1)) + 2 * RIF(r, -i)
                                    + RIF(r, -(i + 1)) + 2) >> 2);
        }
    }
    memcpy(r, f, sizeof(f));
}

/* 8.4.4.2.5, planar: a bilinear ramp between the four edges. */
static void planare(const uint8_t *r, int n, int lg, uint8_t *dst, int passo)
{
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            dst[y * passo + x] = (uint8_t)
                (((n - 1 - x) * RIF(r, -(y + 1)) + (x + 1) * RIF(r, n + 1)
                  + (n - 1 - y) * RIF(r, x + 1) + (y + 1) * RIF(r, -(n + 1))
                  + n) >> (lg + 1));
}

/* 8.4.4.2.5, DC: the average, with the two edges smoothed into it on small
 * luma blocks so the join does not show. */
static void continuo(const uint8_t *r, int n, int lg, int c_idx,
                     uint8_t *dst, int passo)
{
    int somma = n;
    for (int i = 0; i < n; i++)
        somma += RIF(r, i + 1) + RIF(r, -(i + 1));
    const int dc = somma >> (lg + 1);

    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            dst[y * passo + x] = (uint8_t)dc;

    if (c_idx != 0 || n >= 32)
        return;
    dst[0] = (uint8_t)((RIF(r, -1) + 2 * dc + RIF(r, 1) + 2) >> 2);
    for (int x = 1; x < n; x++)
        dst[x] = (uint8_t)((RIF(r, x + 1) + 3 * dc + 2) >> 2);
    for (int y = 1; y < n; y++)
        dst[y * passo] = (uint8_t)((RIF(r, -(y + 1)) + 3 * dc + 2) >> 2);
}

/* 8.4.4.2.6, the thirty-three angles.
 *
 * ⚠️ A mode whose angle is negative reaches past the corner and needs the
 * other edge projected onto its own reference line - which is what
 * invAngle is for. Without it the samples past the corner are whatever was
 * left there, and the error is confined to one triangle of the block. */
static void angolare(const uint8_t *r, int modo, int n, int c_idx,
                     uint8_t *dst, int passo)
{
    const int ang = hevcd_intra_angle[modo - 2];
    const bool verticale = modo >= 18;

    /* One reference line, built in the direction the mode walks. */
    int16_t rif[3 * 64 + 1];
    int16_t *base = rif + 64;
    const int segno = verticale ? 1 : -1;

    (void)segno;
    /* Index 0 is the corner either way; after that the row above for a
     * vertical mode and the column to the left for a horizontal one. */
    base[0] = RIF(r, 0);
    for (int x = 1; x <= n; x++)
        base[x] = verticale ? RIF(r, x) : RIF(r, -x);

    if (ang < 0) {
        const int fino = (n * ang) >> 5;
        if (fino < -1) {
            const int inv = hevcd_inv_angle[modo - 11];
            for (int x = -1; x >= fino; x--) {
                const int k = ((x * inv + 128) >> 8);
                base[x] = verticale ? RIF(r, -k) : RIF(r, k);
            }
        }
    } else {
        for (int x = n + 1; x <= 2 * n; x++)
            base[x] = verticale ? RIF(r, x) : RIF(r, -x);
    }

    for (int y = 0; y < n; y++) {
        const int idx = ((y + 1) * ang) >> 5;
        const int fatt = ((y + 1) * ang) & 31;
        for (int x = 0; x < n; x++) {
            int v;
            if (fatt)
                v = ((32 - fatt) * base[x + idx + 1]
                     + fatt * base[x + idx + 2] + 16) >> 5;
            else
                v = base[x + idx + 1];
            if (verticale) dst[y * passo + x] = (uint8_t)v;
            else           dst[x * passo + y] = (uint8_t)v;
        }
    }

    /* The exactly vertical and exactly horizontal modes smooth their first
     * line against the other edge, on small luma blocks. */
    if (c_idx == 0 && n < 32) {
        if (modo == HEVCD_INTRA_ANGULAR_26) {
            for (int y = 0; y < n; y++)
                dst[y * passo] = ritaglia8(RIF(r, 1)
                                           + ((RIF(r, -(y + 1)) - RIF(r, 0)) >> 1));
        } else if (modo == HEVCD_INTRA_ANGULAR_10) {
            for (int x = 0; x < n; x++)
                dst[x] = ritaglia8(RIF(r, -1)
                                   + ((RIF(r, x + 1) - RIF(r, 0)) >> 1));
        }
    }
}

void hevcd_predici_intra(hevcd_t *d, int c_idx, int x0, int y0, int log2_size,
                         int modo)
{
    const int n = 1 << log2_size;
    uint8_t r[4 * 64 + 1];
    memset(r, 128, sizeof(r));

    riferimenti(d, c_idx, x0, y0, n, r);
    filtra(d, modo, n, c_idx, r);

    uint8_t *dst = d->piano[c_idx] + (size_t)y0 * d->passo[c_idx] + x0;
    const int passo = d->passo[c_idx];

    if (modo == HEVCD_INTRA_PLANAR)      planare(r, n, log2_size, dst, passo);
    else if (modo == HEVCD_INTRA_DC)     continuo(r, n, log2_size, c_idx, dst, passo);
    else                                 angolare(r, modo, n, c_idx, dst, passo);
}
