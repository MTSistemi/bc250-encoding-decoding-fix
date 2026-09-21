/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_mc.c - inter prediction, Rec. ITU-T H.264 clause 8.4.2.
 *
 * The standard names the sixteen quarter-sample positions of a luma block
 * with letters, which is worth keeping because the derivation refers to them
 * constantly:
 *
 *        xFrac 0   1   2   3
 *  yFrac 0     G   a   b   c
 *        1     d   e   f   g
 *        2     h   i   j   k
 *        3     n   p   q   r
 *
 * G is the integer sample. b, h and j are the half-sample positions and are
 * the only ones actually filtered: b horizontally, h vertically, j both.
 * Every other position is the average of two of those, or of one of those and
 * an integer sample.
 *
 * ⚠️ j is NOT the six-tap filter applied to already-rounded half samples. It
 * is filtered from the intermediate values before their rounding, and shifted
 * once by ten at the end rather than twice by five. Rounding twice is off by
 * one on a large fraction of samples, which looks like a very slightly soft
 * picture on an I frame and accumulates into a smear over a GOP.
 */
#include "h264_mc.h"

#include <stddef.h>
#include <string.h>

static inline uint8_t clip_uint8(int v)
{
    return (uint8_t)(v & ~255 ? (-v) >> 31 : v);
}

/* The six-tap filter of 8.4.2.2.1, unrounded. */
#define TAP(a, b, c, d, e, f) ((a) - 5 * (b) + 20 * (c) + 20 * (d) - 5 * (e) + (f))

static inline int media(int a, int b) { return (a + b + 1) >> 1; }

/* -------------------------------------------------------------- fetching */

uint8_t *h264d_mc_fetch_luma(uint8_t *dst, const uint8_t *plane, int stride,
                             int plane_w, int plane_h,
                             int x, int y, int w, int h)
{
    const int pw = w + H264D_MC_PAD_BEFORE + H264D_MC_PAD_AFTER;
    const int ph = h + H264D_MC_PAD_BEFORE + H264D_MC_PAD_AFTER;
    for (int j = 0; j < ph; j++) {
        int sy = y - H264D_MC_PAD_BEFORE + j;
        sy = sy < 0 ? 0 : (sy >= plane_h ? plane_h - 1 : sy);
        const uint8_t *riga = plane + (size_t)sy * stride;
        uint8_t *out = dst + (size_t)j * pw;
        for (int i = 0; i < pw; i++) {
            int sx = x - H264D_MC_PAD_BEFORE + i;
            sx = sx < 0 ? 0 : (sx >= plane_w ? plane_w - 1 : sx);
            out[i] = riga[sx];
        }
    }
    return dst + H264D_MC_PAD_BEFORE * pw + H264D_MC_PAD_BEFORE;
}

uint8_t *h264d_mc_fetch_chroma(uint8_t *dst, const uint8_t *plane, int stride,
                               int plane_w, int plane_h,
                               int x, int y, int w, int h)
{
    const int pw = w + 1;
    for (int j = 0; j <= h; j++) {
        int sy = y + j;
        sy = sy < 0 ? 0 : (sy >= plane_h ? plane_h - 1 : sy);
        const uint8_t *riga = plane + (size_t)sy * stride;
        uint8_t *out = dst + (size_t)j * pw;
        for (int i = 0; i <= w; i++) {
            int sx = x + i;
            sx = sx < 0 ? 0 : (sx >= plane_w ? plane_w - 1 : sx);
            out[i] = riga[sx];
        }
    }
    return dst;
}

/* ------------------------------------------------------------------ luma */

/* The half-sample positions, over a (w+1) x (h+1) region: the quarter
 * positions on the right and bottom edges average with the half sample of
 * the next column or row, so one extra of each is needed.
 *
 * `b` is the horizontal half sample and `v` the vertical one, both clipped.
 * j is not built from either of them: it needs the unrounded intermediates
 * over rows -2..h+3, wider than this region, so it is filtered from the
 * source directly.
 */
#define MAXW 17
#define MAXH 17

static void mezzi_campioni(const uint8_t *src, int ss, int w, int h,
                           uint8_t b[MAXH][MAXW], uint8_t v[MAXH][MAXW],
                           int serve_b, int serve_v)
{
    if (serve_b) {
        for (int y = 0; y <= h; y++) {
            const uint8_t *r = src + (size_t)y * ss;
            for (int x = 0; x <= w; x++)
                b[y][x] = clip_uint8((TAP(r[x - 2], r[x - 1], r[x],
                                          r[x + 1], r[x + 2], r[x + 3]) + 16) >> 5);
        }
    }
    if (serve_v) {
        for (int y = 0; y <= h; y++) {
            const uint8_t *r = src + (size_t)y * ss;
            for (int x = 0; x <= w; x++) {
                int t = TAP(r[x - 2 * ss], r[x - ss], r[x], r[x + ss],
                            r[x + 2 * ss], r[x + 3 * ss]);
                v[y][x] = clip_uint8((t + 16) >> 5);
            }
        }
    }
}

void h264d_mc_luma(uint8_t *dst, int ds, const uint8_t *src, int ss,
                   int w, int h, int xfrac, int yfrac)
{
    if (!xfrac && !yfrac) {
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * ds, src + (size_t)y * ss, (size_t)w);
        return;
    }

    uint8_t b[MAXH][MAXW], v[MAXH][MAXW], j[MAXH][MAXW];
    const int serve_j = (xfrac != 0 && yfrac != 0);

    mezzi_campioni(src, ss, w, h, b, v, xfrac != 0, yfrac != 0);

    if (serve_j) {
        /* Filtered from the unrounded horizontal intermediates, then shifted
         * once by ten - never from the rounded `b` values, which would round
         * twice and come out one low on a large share of samples. */
        for (int y = 0; y <= h; y++) {
            for (int x = 0; x <= w; x++) {
                const uint8_t *r = src + (size_t)y * ss + x;
                int32_t c[6];
                for (int k = -2; k <= 3; k++) {
                    const uint8_t *rr = r + (ptrdiff_t)k * ss;
                    c[k + 2] = TAP(rr[-2], rr[-1], rr[0], rr[1], rr[2], rr[3]);
                }
                j[y][x] = clip_uint8((TAP(c[0], c[1], c[2], c[3], c[4], c[5])
                                      + 512) >> 10);
            }
        }
    }

    for (int y = 0; y < h; y++) {
        uint8_t *o = dst + (size_t)y * ds;
        const uint8_t *g = src + (size_t)y * ss;
        for (int x = 0; x < w; x++) {
            int val;
            switch (yfrac * 4 + xfrac) {
            case  1: val = media(g[x], b[y][x]);                 break; /* a */
            case  2: val = b[y][x];                              break; /* b */
            case  3: val = media(g[x + 1], b[y][x]);             break; /* c */
            case  4: val = media(g[x], v[y][x]);                 break; /* d */
            case  5: val = media(b[y][x], v[y][x]);              break; /* e */
            case  6: val = media(b[y][x], j[y][x]);              break; /* f */
            case  7: val = media(b[y][x], v[y][x + 1]);          break; /* g */
            case  8: val = v[y][x];                              break; /* h */
            case  9: val = media(v[y][x], j[y][x]);              break; /* i */
            case 10: val = j[y][x];                              break; /* j */
            case 11: val = media(v[y][x + 1], j[y][x]);          break; /* k */
            case 12: val = media(g[x + ss], v[y][x]);            break; /* n */
            case 13: val = media(v[y][x], b[y + 1][x]);          break; /* p */
            case 14: val = media(j[y][x], b[y + 1][x]);          break; /* q */
            case 15: val = media(v[y][x + 1], b[y + 1][x]);      break; /* r */
            default: val = g[x];                                 break;
            }
            o[x] = (uint8_t)val;
        }
    }
}

/* ---------------------------------------------------------------- chroma */

void h264d_mc_chroma(uint8_t *dst, int ds, const uint8_t *src, int ss,
                     int w, int h, int xfrac, int yfrac)
{
    const int a = (8 - xfrac) * (8 - yfrac);
    const int b = xfrac * (8 - yfrac);
    const int c = (8 - xfrac) * yfrac;
    const int d = xfrac * yfrac;

    for (int y = 0; y < h; y++) {
        const uint8_t *r0 = src + (size_t)y * ss;
        const uint8_t *r1 = r0 + ss;
        uint8_t *o = dst + (size_t)y * ds;
        for (int x = 0; x < w; x++)
            o[x] = (uint8_t)((a * r0[x] + b * r0[x + 1]
                            + c * r1[x] + d * r1[x + 1] + 32) >> 6);
    }
}

/* ----------------------------------------------------------- combination */

void h264d_mc_average(uint8_t *dst, int ds, const uint8_t *a, int as,
                      const uint8_t *b, int bs, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *ra = a + (size_t)y * as;
        const uint8_t *rb = b + (size_t)y * bs;
        uint8_t *o = dst + (size_t)y * ds;
        for (int x = 0; x < w; x++)
            o[x] = (uint8_t)((ra[x] + rb[x] + 1) >> 1);
    }
}

void h264d_mc_weight(uint8_t *dst, int ds, const uint8_t *src, int ss,
                     int w, int h, int log2_denom, int weight, int offset)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *r = src + (size_t)y * ss;
        uint8_t *o = dst + (size_t)y * ds;
        if (log2_denom)
            for (int x = 0; x < w; x++)
                o[x] = clip_uint8(((r[x] * weight + (1 << (log2_denom - 1)))
                                   >> log2_denom) + offset);
        else
            for (int x = 0; x < w; x++)
                o[x] = clip_uint8(r[x] * weight + offset);
    }
}

void h264d_mc_weight_bi(uint8_t *dst, int ds,
                        const uint8_t *a, int as, const uint8_t *b, int bs,
                        int w, int h, int log2_denom, int w0, int o0, int w1, int o1)
{
    const int shift = log2_denom + 1;
    const int round = ((o0 + o1 + 1) << log2_denom);
    for (int y = 0; y < h; y++) {
        const uint8_t *ra = a + (size_t)y * as;
        const uint8_t *rb = b + (size_t)y * bs;
        uint8_t *o = dst + (size_t)y * ds;
        for (int x = 0; x < w; x++)
            o[x] = clip_uint8((ra[x] * w0 + rb[x] * w1 + round) >> shift);
    }
}
