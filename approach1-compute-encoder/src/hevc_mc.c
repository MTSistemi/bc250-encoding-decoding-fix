/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_mc.c - fetching the samples a motion vector points at,
 * Rec. ITU-T H.265 clause 8.5.3.3.
 *
 * A motion vector points a quarter of a sample at a time for luma and an
 * eighth for chroma, so most of the time it points between samples and the
 * ones in between have to be made up. H.265 makes them with an eight-tap
 * filter where H.264 used six, which is most of the reason its motion
 * compensation is sharper and most of the reason it is slower.
 *
 * ⚠️ Everything in here works at fourteen bits and clips to eight only at
 * the very end. Two predictions averaged after each has been rounded to
 * eight bits are wrong by half a level per sample, everywhere, for ever.
 */
#include "hevc_dec_internal.h"

#include <string.h>

#define LATO_MAX 64

static inline uint8_t ritaglia8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* ⚠️ A motion vector may point off the edge of the reference picture, and
 * legitimately: an object entering the frame was not there before. The
 * edge sample is repeated outwards rather than the fetch being refused. */
static inline int campione(const uint8_t *p, int passo, int w, int h,
                           int x, int y)
{
    x = x < 0 ? 0 : (x >= w ? w - 1 : x);
    y = y < 0 ? 0 : (y >= h ? h - 1 : y);
    return p[(size_t)y * passo + x];
}

/* One rectangle of one plane, at a fractional position, into fourteen-bit
 * intermediate values.
 *
 * `prima` is how far back the filter reaches: three samples for the eight
 * taps of luma, one for the four of chroma. */
static void interpola(const uint8_t *rif, int passo, int w_pic, int h_pic,
                      int x, int y, int w, int h, int fx, int fy,
                      const int8_t *filtro, int quanti, int prima,
                      int16_t *fuori, int passo_fuori)
{
    if (!fx && !fy) {
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++)
                fuori[r * passo_fuori + c] = (int16_t)
                    (campione(rif, passo, w_pic, h_pic, x + c, y + r) << 6);
        return;
    }

    const int8_t *fh = filtro + (size_t)fx * quanti;
    const int8_t *fv = filtro + (size_t)fy * quanti;

    if (!fy) {
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++) {
                int s = 0;
                for (int k = 0; k < quanti; k++)
                    s += fh[k] * campione(rif, passo, w_pic, h_pic,
                                          x + c - prima + k, y + r);
                fuori[r * passo_fuori + c] = (int16_t)s;
            }
        return;
    }

    if (!fx) {
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++) {
                int s = 0;
                for (int k = 0; k < quanti; k++)
                    s += fv[k] * campione(rif, passo, w_pic, h_pic,
                                          x + c, y + r - prima + k);
                fuori[r * passo_fuori + c] = (int16_t)s;
            }
        return;
    }

    /* Both: horizontally first, over enough extra rows above and below for
     * the vertical pass to have something to stand on. */
    int16_t mezzo[(LATO_MAX + 7) * LATO_MAX];
    const int alte = h + quanti - 1;
    for (int r = 0; r < alte; r++)
        for (int c = 0; c < w; c++) {
            int s = 0;
            for (int k = 0; k < quanti; k++)
                s += fh[k] * campione(rif, passo, w_pic, h_pic,
                                      x + c - prima + k, y + r - prima);
            mezzo[r * LATO_MAX + c] = (int16_t)s;
        }

    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            int s = 0;
            for (int k = 0; k < quanti; k++)
                s += fv[k] * mezzo[(r + k) * LATO_MAX + c];
            fuori[r * passo_fuori + c] = (int16_t)(s >> 6);
        }
}

/* ------------------------------------- fourteen bits back down to eight */

static void uno(uint8_t *dst, int passo, int w, int h,
                const int16_t *a, int passo_a)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            dst[r * passo + c] = ritaglia8((a[r * passo_a + c] + 32) >> 6);
}

static void due(uint8_t *dst, int passo, int w, int h,
                const int16_t *a, const int16_t *b, int passo_p)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            dst[r * passo + c] =
                ritaglia8((a[r * passo_p + c] + b[r * passo_p + c] + 64) >> 7);
}

/* 8.5.3.3.4.3. ⚠️ Used whenever the slice carries a weight table, even
 * where the weight happens to be neutral: for a neutral weight this is
 * the same arithmetic as the plain path, so there is nothing to gain by
 * deciding per block and something to lose by getting the decision
 * wrong. */
static void uno_pesato(uint8_t *dst, int passo, int w, int h,
                       const int16_t *a, int passo_a,
                       int peso, int off, int den)
{
    const int log2wd = den + 6;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            const int v = a[r * passo_a + c];
            dst[r * passo + c] = ritaglia8(
                log2wd >= 1 ? (((v * peso + (1 << (log2wd - 1))) >> log2wd) + off)
                            : (v * peso + off));
        }
}

static void due_pesate(uint8_t *dst, int passo, int w, int h,
                       const int16_t *a, const int16_t *b, int passo_p,
                       int pa, int pb, int oa, int ob, int den)
{
    const int log2wd = den + 6;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            dst[r * passo + c] = ritaglia8(
                (a[r * passo_p + c] * pa + b[r * passo_p + c] * pb
                 + ((oa + ob + 1) << log2wd)) >> (log2wd + 1));
}

/* Does this slice carry a weight table at all. */
static bool pesato(const hevcd_t *d)
{
    return (d->pps->weighted_pred && d->slice->type == 1)
        || (d->pps->weighted_bipred && d->slice->type == 0);
}

void hevcd_predici_inter(hevcd_t *d, int x0, int y0, int w, int h,
                         const hevcd_mvf_t *m)
{
    const hevc_sps_t *sps = d->sps;
    static int16_t p[2][LATO_MAX * LATO_MAX];
    const bool usa[2] = { (m->pred_flag & HEVCD_PF_L0) != 0,
                          (m->pred_flag & HEVCD_PF_L1) != 0 };
    const bool pesi = pesato(d);

    for (int piano = 0; piano < 3; piano++) {
        const int giu = piano ? 1 : 0;
        const int pw = w >> giu, ph = h >> giu;
        const int px = x0 >> giu, py = y0 >> giu;
        const int w_pic = sps->width >> giu, h_pic = sps->height >> giu;

        for (int l = 0; l < 2; l++) {
            if (!usa[l]) continue;
            const int i = m->ref_idx[l];
            if (i < 0 || i >= d->n_rif[l] || !d->rif[l][i]) return;
            const hevcd_img_t *r = d->rif[l][i];
            const int mvx = m->mv[l][0], mvy = m->mv[l][1];
            /* ⚠️ Luma counts quarters and chroma eighths. At 4:2:0 the
             * chroma plane is half the size, so the same vector lands on a
             * finer grid there, not a coarser one. */
            const int passi = piano ? 3 : 2;
            interpola(r->piano[piano], r->passo[piano], w_pic, h_pic,
                      px + (mvx >> passi), py + (mvy >> passi), pw, ph,
                      mvx & ((1 << passi) - 1), mvy & ((1 << passi) - 1),
                      piano ? &hevcd_epel[0][0] : &hevcd_qpel[0][0],
                      piano ? 4 : 8, piano ? 1 : 3,
                      p[l], LATO_MAX);
        }

        uint8_t *dst = d->piano[piano] + (size_t)py * d->passo[piano] + px;

        if (!pesi) {
            if (usa[0] && usa[1])
                due(dst, d->passo[piano], pw, ph, p[0], p[1], LATO_MAX);
            else
                uno(dst, d->passo[piano], pw, ph, p[usa[0] ? 0 : 1], LATO_MAX);
            continue;
        }

        const int den = piano ? d->slice->chroma_log2_weight_denom
                              : d->slice->luma_log2_weight_denom;
        int peso[2] = { 1 << den, 1 << den }, off[2] = { 0, 0 };
        for (int l = 0; l < 2; l++) {
            if (!usa[l]) continue;
            const int i = m->ref_idx[l];
            peso[l] = piano ? d->slice->chroma_weight[l][i][piano - 1]
                            : d->slice->luma_weight[l][i];
            off[l] = piano ? d->slice->chroma_offset[l][i][piano - 1]
                           : d->slice->luma_offset[l][i];
        }

        if (usa[0] && usa[1])
            due_pesate(dst, d->passo[piano], pw, ph, p[0], p[1], LATO_MAX,
                       peso[0], peso[1], off[0], off[1], den);
        else {
            const int l = usa[0] ? 0 : 1;
            uno_pesato(dst, d->passo[piano], pw, ph, p[l], LATO_MAX,
                       peso[l], off[l], den);
        }
    }
}
