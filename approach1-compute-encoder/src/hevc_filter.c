/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_filter.c - the deblocking filter, Rec. ITU-T H.265 clause 8.7.2.
 *
 * Block transforms leave steps at the block edges, and the eye finds a
 * straight edge that is not in the picture far more readily than it finds
 * the error that produced it. So the standard smooths them - and does it
 * normatively, in the decoding loop, because the filtered picture is what
 * the next one predicts from. A decoder that skips it does not merely look
 * worse: it drifts.
 *
 * ⚠️ Every vertical edge in the picture is filtered before any horizontal
 * one, and the horizontal pass reads what the vertical pass wrote. Doing
 * it edge by edge, both directions at once, gives a different picture.
 * Two passes over the whole picture is the simplest way to be sure, and
 * the passes are independent inside themselves: the edges of one direction
 * are eight samples apart and reach four, so no two of them touch.
 */
#include "hevc_dec_internal.h"

#include <stdlib.h>
#include <string.h>

static inline int ritaglia(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint8_t ritaglia8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* The luma parameter of the coding unit covering a sample. */
static int qp_di(const hevcd_t *d, int x, int y)
{
    const int passo = d->sps->min_cb_width;
    const int l = d->sps->log2_min_cb;
    return d->qp_y_map[(y >> l) * passo + (x >> l)];
}

/* A coding unit coded losslessly keeps its samples exactly as they are:
 * filtering them would be the one thing that made the stream lossy. */
static bool intoccabile(const hevcd_t *d, int x, int y)
{
    if (!d->no_filtro) return false;
    const int passo = d->sps->min_cb_width;
    const int l = d->sps->log2_min_cb;
    return d->no_filtro[(y >> l) * passo + (x >> l)] != 0;
}

/* One four-line segment of a luma edge.
 *
 * `base` points at q0 of the first line, `avanti` steps from p to q and
 * `giu` steps from one line to the next: for a vertical edge those are one
 * sample and one row, for a horizontal edge the other way round. Writing
 * it once in these two steps is what keeps the two directions from
 * drifting apart, which is where a hand-unrolled deblocking filter usually
 * goes wrong.
 */
static void filtra_luma(uint8_t *base, int avanti, int giu,
                        int beta, int tc, bool tieni_p, bool tieni_q)
{
#define P(k, i) ((int)base[(i) * giu - ((k) + 1) * avanti])
#define Q(k, i) ((int)base[(i) * giu + (k) * avanti])
#define SCRIVI_P(k, i, v) \
    do { if (!tieni_p) base[(i) * giu - ((k) + 1) * avanti] = (v); } while (0)
#define SCRIVI_Q(k, i, v) \
    do { if (!tieni_q) base[(i) * giu + (k) * avanti] = (v); } while (0)

    /* 8.7.2.5.3. The decision looks at the first and the last line of the
     * four and at nothing in between: four lines of an eight-sample block
     * edge are alike enough that two of them decide for all four, and
     * halving the work of the decision was worth it to the committee. */
    const int dp0 = abs(P(2, 0) - 2 * P(1, 0) + P(0, 0));
    const int dp3 = abs(P(2, 3) - 2 * P(1, 3) + P(0, 3));
    const int dq0 = abs(Q(2, 0) - 2 * Q(1, 0) + Q(0, 0));
    const int dq3 = abs(Q(2, 3) - 2 * Q(1, 3) + Q(0, 3));
    const int dpq0 = dp0 + dq0, dpq3 = dp3 + dq3;

    if (dpq0 + dpq3 >= beta) return;

    const int dp = dp0 + dp3, dq = dq0 + dq3;
    const int soglia = (5 * tc + 1) >> 1;

    /* 8.7.2.5.6: strong only when the step really is a step - flat on both
     * sides, and the jump across small enough to be an artefact rather
     * than an edge that belongs to the picture. */
    bool forte = true;
    for (int i = 0; i < 4 && forte; i += 3) {
        const int dpq = 2 * (i == 0 ? dpq0 : dpq3);
        forte = dpq < (beta >> 2)
             && abs(P(3, i) - P(0, i)) + abs(Q(0, i) - Q(3, i)) < (beta >> 3)
             && abs(P(0, i) - Q(0, i)) < soglia;
    }

    if (forte) {
        /* 8.7.2.5.7. Three samples each side, every one of them held
         * within two tC of where it started: the filter may smooth a step
         * but it may not invent one. */
        for (int i = 0; i < 4; i++) {
            const int p0 = P(0, i), p1 = P(1, i), p2 = P(2, i), p3 = P(3, i);
            const int q0 = Q(0, i), q1 = Q(1, i), q2 = Q(2, i), q3 = Q(3, i);
            SCRIVI_P(0, i, (uint8_t)ritaglia((p2 + 2 * p1 + 2 * p0 + 2 * q0
                                              + q1 + 4) >> 3,
                                             p0 - 2 * tc, p0 + 2 * tc));
            SCRIVI_P(1, i, (uint8_t)ritaglia((p2 + p1 + p0 + q0 + 2) >> 2,
                                             p1 - 2 * tc, p1 + 2 * tc));
            SCRIVI_P(2, i, (uint8_t)ritaglia((2 * p3 + 3 * p2 + p1 + p0 + q0
                                              + 4) >> 3,
                                             p2 - 2 * tc, p2 + 2 * tc));
            SCRIVI_Q(0, i, (uint8_t)ritaglia((p1 + 2 * p0 + 2 * q0 + 2 * q1
                                              + q2 + 4) >> 3,
                                             q0 - 2 * tc, q0 + 2 * tc));
            SCRIVI_Q(1, i, (uint8_t)ritaglia((p0 + q0 + q1 + q2 + 2) >> 2,
                                             q1 - 2 * tc, q1 + 2 * tc));
            SCRIVI_Q(2, i, (uint8_t)ritaglia((p0 + q0 + q1 + 3 * q2 + 2 * q3
                                              + 4) >> 3,
                                             q2 - 2 * tc, q2 + 2 * tc));
        }
        return;
    }

    /* 8.7.2.5.7, the weak filter. One sample each side always, a second
     * one only on whichever side was flat enough to deserve it. */
    const bool tocca_p1 = dp < ((beta + (beta >> 1)) >> 3);
    const bool tocca_q1 = dq < ((beta + (beta >> 1)) >> 3);

    for (int i = 0; i < 4; i++) {
        const int p0 = P(0, i), p1 = P(1, i), p2 = P(2, i);
        const int q0 = Q(0, i), q1 = Q(1, i), q2 = Q(2, i);
        int delta = (9 * (q0 - p0) - 3 * (q1 - p1) + 8) >> 4;
        if (abs(delta) >= tc * 10) continue;
        delta = ritaglia(delta, -tc, tc);
        SCRIVI_P(0, i, ritaglia8(p0 + delta));
        SCRIVI_Q(0, i, ritaglia8(q0 - delta));
        if (tocca_p1) {
            const int dp1 = ritaglia((((p2 + p0 + 1) >> 1) - p1 + delta) >> 1,
                                     -(tc >> 1), tc >> 1);
            SCRIVI_P(1, i, ritaglia8(p1 + dp1));
        }
        if (tocca_q1) {
            const int dq1 = ritaglia((((q2 + q0 + 1) >> 1) - q1 - delta) >> 1,
                                     -(tc >> 1), tc >> 1);
            SCRIVI_Q(1, i, ritaglia8(q1 + dq1));
        }
    }
#undef P
#undef Q
#undef SCRIVI_P
#undef SCRIVI_Q
}

/* 8.7.2.5.5. Chroma gets one sample each side and no decision at all: it
 * is filtered where the boundary strength is two and nowhere else, which
 * in an intra picture means every edge. */
static void filtra_croma(uint8_t *base, int avanti, int giu, int tc,
                         bool tieni_p, bool tieni_q)
{
    for (int i = 0; i < 4; i++) {
        uint8_t *p1 = base + i * giu - 2 * avanti;
        uint8_t *p0 = base + i * giu - avanti;
        uint8_t *q0 = base + i * giu;
        uint8_t *q1 = base + i * giu + avanti;
        const int delta = ritaglia(((((int)*q0 - *p0) * 4) + *p1 - *q1 + 4) >> 3,
                                   -tc, tc);
        if (!tieni_p) *p0 = ritaglia8(*p0 + delta);
        if (!tieni_q) *q0 = ritaglia8(*q0 - delta);
    }
}

/* Table 8-10 again, as 8.7.2.5.5 asks for it. */
static int qp_croma(int qp_i)
{
    if (qp_i < 30) return qp_i < 0 ? 0 : qp_i;
    if (qp_i > 43) return qp_i - 6;
    return hevcd_qp_c[qp_i - 30];
}

/* beta and tC for one edge, 8.7.2.5.3. The boundary strength only ever
 * reaches the tables through tC, and only by two quantiser steps. */
static int beta_di(const hevcd_t *d, int qp)
{
    const int q = ritaglia(qp + d->slice->beta_offset, 0, 51);
    return hevcd_beta[q];
}

static int tc_di(const hevcd_t *d, int qp, int bs)
{
    const int q = ritaglia(qp + 2 * (bs - 1) + d->slice->tc_offset, 0, 53);
    return hevcd_tc[q];
}

/* Is the edge on this side of an 8x8 cell one the filter may cross. */
static bool bordo(const hevcd_t *d, int x, int y, int quale)
{
    return (d->bordi[(y >> 3) * d->bordi_passo + (x >> 3)] & quale) != 0;
}

/* 8.7.2.4. Two, one, or nothing at all.
 *
 * Two means an intra block is involved and the step across the edge is
 * whatever the prediction could not reach; that is worth the strong
 * filter and it is the only case where chroma is touched at all. One
 * means two inter blocks that disagree - a coded residual at a transform
 * edge, different reference pictures, or motion a quarter sample apart.
 * Nothing means two blocks that were predicted the same way from the same
 * place, where any step across the edge would be something the filter
 * invented.
 */
static int forza(const hevcd_t *d, int xp, int yp, int xq, int yq,
                 bool bordo_trasformata)
{
    const int passo = d->min_pu_width;
    const hevcd_mvf_t *p = &d->mvf[(yp >> 2) * passo + (xp >> 2)];
    const hevcd_mvf_t *q = &d->mvf[(yq >> 2) * passo + (xq >> 2)];

    if (!p->pred_flag || !q->pred_flag) return 2;

    if (bordo_trasformata && d->cbf_map
        && (d->cbf_map[(yp >> 2) * passo + (xp >> 2)]
            || d->cbf_map[(yq >> 2) * passo + (xq >> 2)]))
        return 1;

    const int np = (p->pred_flag == HEVCD_PF_BI) ? 2 : 1;
    const int nq = (q->pred_flag == HEVCD_PF_BI) ? 2 : 1;
    if (np != nq) return 1;

#define LONTANI(a, la, b, lb)                                   \
    (abs((a)->mv[la][0] - (b)->mv[lb][0]) >= 4                  \
     || abs((a)->mv[la][1] - (b)->mv[lb][1]) >= 4)

    if (np == 1) {
        const int lp = (p->pred_flag & HEVCD_PF_L0) ? 0 : 1;
        const int lq = (q->pred_flag & HEVCD_PF_L0) ? 0 : 1;
        if (p->ref_poc[lp] != q->ref_poc[lq]) return 1;
        return LONTANI(p, lp, q, lq) ? 1 : 0;
    }

    /* Both predict from two places. They agree only if the two pairs of
     * pictures are the same pair - in either order - and the vectors that
     * go with them are close enough. */
    const bool stesse = (p->ref_poc[0] == q->ref_poc[0]
                         && p->ref_poc[1] == q->ref_poc[1]);
    const bool incrociate = (p->ref_poc[0] == q->ref_poc[1]
                             && p->ref_poc[1] == q->ref_poc[0]);
    if (!stesse && !incrociate) return 1;

    if (p->ref_poc[0] != p->ref_poc[1]) {
        /* ⚠️ Two different pictures: there is only one way to pair them
         * up, and it is whichever way makes the pictures match. */
        if (stesse)
            return (LONTANI(p, 0, q, 0) || LONTANI(p, 1, q, 1)) ? 1 : 0;
        return (LONTANI(p, 0, q, 1) || LONTANI(p, 1, q, 0)) ? 1 : 0;
    }

    /* The same picture twice: either pairing will do, and the edge is
     * quiet if either one is close enough. */
    const bool dritte = !(LONTANI(p, 0, q, 0) || LONTANI(p, 1, q, 1));
    const bool storte = !(LONTANI(p, 0, q, 1) || LONTANI(p, 1, q, 0));
    return (dritte || storte) ? 0 : 1;
#undef LONTANI
}

/* One direction over the whole picture. `verticale` says which edges are
 * looked at, not which way the filter reads: a vertical edge is filtered
 * along x and stepped along y. */
static void una_direzione(hevcd_t *d, bool verticale)
{
    const hevc_sps_t *sps = d->sps;
    const int avanti_l = verticale ? 1 : d->passo[0];
    const int giu_l = verticale ? d->passo[0] : 1;
    const int quale = verticale ? 1 : 2;

    for (int y = 0; y < sps->height; y += verticale ? 4 : 8)
        for (int x = 0; x < sps->width; x += verticale ? 8 : 4) {
            /* The picture's own border is never an edge, and neither is a
             * position the coding tree never put a block boundary at. */
            if (verticale ? x == 0 : y == 0) continue;
            if (!bordo(d, x, y, quale)) continue;

            const int xp = verticale ? x - 1 : x;
            const int yp = verticale ? y : y - 1;
            const int bs = d->mvf
                ? forza(d, xp, yp, x, y,
                        (d->bordi[(y >> 3) * d->bordi_passo + (x >> 3)]
                         & (verticale ? 4 : 8)) != 0)
                : 2;
            if (!bs) continue;
            const int qp = (qp_di(d, x, y) + qp_di(d, xp, yp) + 1) >> 1;
            const bool tieni_p = intoccabile(d, xp, yp);
            const bool tieni_q = intoccabile(d, x, y);
            if (tieni_p && tieni_q) continue;

            filtra_luma(d->piano[0] + (size_t)y * d->passo[0] + x,
                        avanti_l, giu_l, beta_di(d, qp), tc_di(d, qp, bs),
                        tieni_p, tieni_q);

            /* ⚠️ Chroma is filtered on its own grid, which is eight chroma
             * samples and therefore sixteen luma ones. Filtering it
             * wherever luma is filtered doubles the edges it touches and
             * softens the picture in a way no reference decoder does. */
            if (bs != 2) continue;
            if (verticale ? (x & 15) : (y & 15)) continue;
            if (verticale ? (y & 7) : (x & 7)) continue;

            for (int c = 1; c < 3; c++) {
                const int off = c == 1 ? d->pps->cb_qp_offset
                                       : d->pps->cr_qp_offset;
                const int tc = hevcd_tc[ritaglia(qp_croma(ritaglia(qp + off,
                                                                   0, 57))
                                                 + 2 + d->slice->tc_offset,
                                                 0, 53)];
                if (!tc) continue;
                const int avanti_c = verticale ? 1 : d->passo[c];
                const int giu_c = verticale ? d->passo[c] : 1;
                filtra_croma(d->piano[c] + (size_t)(y / 2) * d->passo[c]
                             + x / 2, avanti_c, giu_c, tc,
                             tieni_p, tieni_q);
            }
        }
}

/* 8.7.2 over the finished picture.
 *
 * ⚠️ The offsets come from the slice, and one picture may carry several
 * slices with different ones. With a single slice per picture - which is
 * what the harness feeds it - this is exact; with more it would need the
 * offsets kept per coding tree block, the same way the quantisation
 * parameter already is.
 */
void hevcd_deblocca(hevcd_t *d)
{
    if (!d->bordi || d->slice->deblocking_filter_disabled) return;
    una_direzione(d, true);
    una_direzione(d, false);
}

/* ------------------------------------------------- sample adaptive offset */

/* 8.7.3. The last thing in the decoding loop, and the only part of it the
 * encoder steers directly: four offsets per coding tree block per plane,
 * chosen by measuring the error that everything upstream left behind.
 *
 * ⚠️ It reads the picture the deblocking filter produced and writes a
 * different one. An implementation that reads and writes the same plane
 * gets the band offset right and the edge offset wrong, in a way that is
 * worth a handful of sample values and shows up only where the offsets
 * are large - which is exactly where nobody looks first.
 */
static int segno(int v)
{
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

/* Which two neighbours each edge class compares against: horizontal,
 * vertical, and the two diagonals. */
static const int8_t sao_dx[4][2] = { { -1, 1 }, { 0, 0 }, { -1, 1 }, { 1, -1 } };
static const int8_t sao_dy[4][2] = { { 0, 0 }, { -1, 1 }, { -1, 1 }, { -1, 1 } };

static void sao_blocco(hevcd_t *d, int c, int rx, int ry,
                       const hevcd_sao_t *s)
{
    if (!s->tipo[c]) return;

    const int giu = c ? 1 : 0;
    const int log2 = d->sps->log2_ctb - giu;
    const int w = d->sps->width >> giu, h = d->sps->height >> giu;
    const int x0 = rx << log2, y0 = ry << log2;
    const int lato = 1 << log2;
    const int x1 = x0 + lato < w ? x0 + lato : w;
    const int y1 = y0 + lato < h ? y0 + lato : h;
    const int passo = d->passo[c];
    uint8_t *piano = d->piano[c];
    const uint8_t *prima = d->copia[c];

    if (s->tipo[c] == 1) {
        /* By band: the range of a sample is cut into thirty-two bands and
         * four consecutive ones get an offset each. An encoder reaches for
         * this where the error is a shift of level rather than a step -
         * a flat area that came out slightly too dark, say. */
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                if (intoccabile(d, x << giu, y << giu)) continue;
                uint8_t *p = piano + (size_t)y * passo + x;
                const int k = ((*p >> 3) - s->posizione[c]) & 31;
                if (k < 4) *p = ritaglia8(*p + s->off[c][k]);
            }
        return;
    }

    /* By edge: each sample is compared with two neighbours along one of
     * four directions, which sorts it into a valley, a step, or a peak,
     * and each of those gets its own offset. */
    const int cl = s->classe[c];
    const int ax = sao_dx[cl][0], ay = sao_dy[cl][0];
    const int bx = sao_dx[cl][1], by = sao_dy[cl][1];

    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            /* A sample whose neighbour would be outside the picture is
             * left alone: there is nothing to compare it with. */
            if (x + ax < 0 || x + ax >= w || x + bx < 0 || x + bx >= w) continue;
            if (y + ay < 0 || y + ay >= h || y + by < 0 || y + by >= h) continue;
            if (intoccabile(d, x << giu, y << giu)) continue;

            const int v = prima[(size_t)y * passo + x];
            int idx = 2 + segno(v - prima[(size_t)(y + ay) * passo + x + ax])
                        + segno(v - prima[(size_t)(y + by) * passo + x + bx]);
            /* Table 8-x: the five cases are renumbered so that "neither up
             * nor down" lands on zero, which is the one with no offset. */
            if (idx <= 2) idx = (idx == 2) ? 0 : idx + 1;
            if (!idx) continue;
            piano[(size_t)y * passo + x] = ritaglia8(v + s->off[c][idx - 1]);
        }
}

void hevcd_sao(hevcd_t *d)
{
    if (!d->sao || !d->piano[0]) return;

    bool serve = false;
    for (int i = 0; i < d->sps->ctb_count && !serve; i++)
        serve = d->sao[i].tipo[0] || d->sao[i].tipo[1] || d->sao[i].tipo[2];
    if (!serve) return;

    const size_t misura[3] = { d->n_piano, d->n_piano / 4, d->n_piano / 4 };
    for (int c = 0; c < 3; c++) {
        if (!d->copia[c] || d->n_copia < d->n_piano) {
            free(d->copia[c]);
            d->copia[c] = malloc(misura[c]);
            if (!d->copia[c]) return;
        }
        memcpy(d->copia[c], d->piano[c], misura[c]);
    }
    d->n_copia = d->n_piano;

    for (int ry = 0; ry < d->sps->ctb_height; ry++)
        for (int rx = 0; rx < d->sps->ctb_width; rx++) {
            const hevcd_sao_t *s = &d->sao[ry * d->sps->ctb_width + rx];
            for (int c = 0; c < 3; c++) sao_blocco(d, c, rx, ry, s);
        }
}

void hevcd_libera_filtri(hevcd_t *d)
{
    free(d->sao);
    d->sao = NULL;
    d->n_sao = 0;
    for (int c = 0; c < 3; c++) { free(d->copia[c]); d->copia[c] = NULL; }
    d->n_copia = 0;
}
