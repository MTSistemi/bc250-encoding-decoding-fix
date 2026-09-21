/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_mb_motion.c - motion vector prediction and the motion part of the
 * macroblock layer, Rec. ITU-T H.264 clause 8.4.1.
 *
 * Three neighbours drive every predicted vector: A to the left, B above and
 * C above-right, with D above-left standing in when C does not exist. The
 * prediction is their median, except in the handful of cases where the
 * standard says to take one of them outright.
 *
 * ⚠️ "Does not exist" is not only about the edge of the picture. A neighbour
 * inside the current macroblock counts only if it has already been decoded,
 * which clause 6.4.11.7 defines by the Z-scan order, not by raster order. C
 * is the one this bites: the block above-right of a partition is often a
 * block the decoder has not reached yet, and treating it as available reads
 * a vector that is still zero.
 */
#include "h264_dec_internal.h"

#include "h264_parts.h"

#include <stdlib.h>
#include <string.h>

/* Raster position of each 4x4 block in decoding (Z-scan) order.
 *
 * It is also its own inverse - the permutation only ever swaps 2 with 4, 3
 * with 5, 10 with 12 and 11 with 13 - so the same table reads both ways, and
 * zscan[raster] is the Z-scan index that clause 6.4.11.7 compares to decide
 * whether a neighbour has been decoded yet. */
static const uint8_t zscan[16] = {
    0, 1, 4, 5,  2, 3, 6, 7,  8, 9, 12, 13,  10, 11, 14, 15
};

/* One neighbouring 4x4 block: which macroblock it is in, and where. */
typedef struct {
    const h264d_mb_t *mb;
    int blocco;             /* raster index inside that macroblock */
    bool c_e;               /* whether it exists at all */
} vicino_t;

/* Clause 6.4.11.4 and 6.4.11.7. (x4, y4) may run from -1 to 4. */
static vicino_t vicino(const h264_decoder_t *d, int x4, int y4, int raster_cur)
{
    vicino_t v = { NULL, 0, false };
    const h264d_mb_t *m;

    if (x4 < 0 && y4 < 0) {
        m = h264d_mb_top_left(d);
        if (!m) return v;
        v.mb = m; v.blocco = 15;
    } else if (x4 < 0) {
        if (y4 > 3) return v;
        m = h264d_mb_left(d);
        if (!m) return v;
        v.mb = m; v.blocco = y4 * 4 + 3;
    } else if (y4 < 0) {
        if (x4 > 3) {
            m = h264d_mb_top_right(d);
            if (!m) return v;
            v.mb = m; v.blocco = 12;
        } else {
            m = h264d_mb_top(d);
            if (!m) return v;
            v.mb = m; v.blocco = 12 + x4;
        }
    } else {
        if (x4 > 3 || y4 > 3) return v;
        const int r = y4 * 4 + x4;
        /* Inside the current macroblock: only if already decoded. */
        if (zscan[r] >= zscan[raster_cur]) return v;
        v.mb = &d->mbs[d->mb_idx];
        v.blocco = r;
    }
    v.c_e = true;
    return v;
}

/* The reference and vector a neighbour contributes. An intra neighbour, or
 * one that does not use this list, contributes reference -1 and a zero
 * vector, which is what makes it lose every comparison below. */
static void da_vicino(const vicino_t *v, int lista, int *ref, int16_t mv[2])
{
    if (!v->c_e || !v->mb || v->mb->intra) {
        *ref = -1;
        mv[0] = mv[1] = 0;
        return;
    }
    const int p = ((v->blocco >> 2) & 2) | ((v->blocco >> 1) & 1);
    *ref = v->mb->ref_idx[lista][p];   /* the index, per 8.4.1.3 */
    mv[0] = v->mb->mv[lista][v->blocco][0];
    mv[1] = v->mb->mv[lista][v->blocco][1];
}

static inline int16_t mediana(int a, int b, int c)
{
    const int massimo = a > b ? (a > c ? a : c) : (b > c ? b : c);
    const int minimo = a < b ? (a < c ? a : c) : (b < c ? b : c);
    return (int16_t)(a + b + c - massimo - minimo);
}

/* Clause 8.4.1.3. `blk` is the raster index of the partition's top-left 4x4
 * block, `w4` and `h4` its size in 4x4 units. */
void h264d_predict_mv(h264_decoder_t *d, int lista, int blk, int w4, int h4,
                      int ref_idx, int16_t out[2])
{
    const int x4 = blk & 3, y4 = blk >> 2;

    vicino_t va = vicino(d, x4 - 1, y4, blk);
    vicino_t vb = vicino(d, x4, y4 - 1, blk);
    vicino_t vc = vicino(d, x4 + w4, y4 - 1, blk);
    if (!vc.c_e)
        vc = vicino(d, x4 - 1, y4 - 1, blk);      /* D stands in for C */

    int ra, rb, rc;
    int16_t ma[2], mb[2], mc[2];
    da_vicino(&va, lista, &ra, ma);
    da_vicino(&vb, lista, &rb, mb);
    da_vicino(&vc, lista, &rc, mc);

    /* Clause 8.4.1.3.1: when neither B nor C exists but A does, all three
     * take A's values. Without this the median of (A, 0, 0) would win and
     * the vector would collapse towards zero at the top edge of every
     * picture. */
    if (!vb.c_e && !vc.c_e && va.c_e) {
        rb = rc = ra;
        mb[0] = mc[0] = ma[0];
        mb[1] = mc[1] = ma[1];
    }

    /* The directional shortcuts of 8.4.1.3, for the two-partition shapes. */
    if (w4 == 4 && h4 == 2) {               /* 16x8 */
        if (y4 == 0 && rb == ref_idx) { out[0] = mb[0]; out[1] = mb[1]; return; }
        if (y4 != 0 && ra == ref_idx) { out[0] = ma[0]; out[1] = ma[1]; return; }
    } else if (w4 == 2 && h4 == 4) {        /* 8x16 */
        if (x4 == 0 && ra == ref_idx) { out[0] = ma[0]; out[1] = ma[1]; return; }
        if (x4 != 0 && rc == ref_idx) { out[0] = mc[0]; out[1] = mc[1]; return; }
    }

    /* Exactly one neighbour on the same reference picture wins outright. */
    const int quanti = (ra == ref_idx) + (rb == ref_idx) + (rc == ref_idx);
    if (quanti == 1) {
        if (ra == ref_idx)      { out[0] = ma[0]; out[1] = ma[1]; }
        else if (rb == ref_idx) { out[0] = mb[0]; out[1] = mb[1]; }
        else                    { out[0] = mc[0]; out[1] = mc[1]; }
        return;
    }

    out[0] = mediana(ma[0], mb[0], mc[0]);
    out[1] = mediana(ma[1], mb[1], mc[1]);
}

/* Clause 8.4.1.1: the vector of a skipped P macroblock. It is the ordinary
 * 16x16 prediction, except that it collapses to zero when the macroblock is
 * at the top-left, or when either of the two immediate neighbours is itself
 * on reference 0 with a zero vector. */
void h264d_skip_mv_p(h264_decoder_t *d, int16_t out[2])
{
    vicino_t va = vicino(d, -1, 0, 0);
    vicino_t vb = vicino(d, 0, -1, 0);

    int ra, rb;
    int16_t ma[2], mb[2];
    da_vicino(&va, 0, &ra, ma);
    da_vicino(&vb, 0, &rb, mb);

    if (!va.c_e || !vb.c_e
        || (ra == 0 && ma[0] == 0 && ma[1] == 0)
        || (rb == 0 && mb[0] == 0 && mb[1] == 0)) {
        out[0] = out[1] = 0;
        return;
    }
    h264d_predict_mv(d, 0, 0, 4, 4, 0, out);
}

/* ------------------------------------------------------ spatial direct */

static inline int min_positivo(int a, int b)
{
    if (a >= 0 && b >= 0) return a < b ? a : b;
    return a > b ? a : b;
}

/* Clause 8.4.1.2.2. Fills in both lists for the whole macroblock. Returns
 * non-zero when the derivation cannot be done. */
int h264d_direct_spatial(h264_decoder_t *d, h264d_mb_t *m, int maschera)
{
    /* The three neighbours of the macroblock as a whole. */
    vicino_t va = vicino(d, -1, 0, 0);
    vicino_t vb = vicino(d, 0, -1, 0);
    vicino_t vc = vicino(d, 4, -1, 0);
    if (!vc.c_e)
        vc = vicino(d, -1, -1, 0);

    int ref[2];
    int16_t mvp[2][2];
    for (int l = 0; l < 2; l++) {
        int ra, rb, rc;
        int16_t ma[2], mb[2], mc[2];
        da_vicino(&va, l, &ra, ma);
        da_vicino(&vb, l, &rb, mb);
        da_vicino(&vc, l, &rc, mc);
        ref[l] = min_positivo(ra, min_positivo(rb, rc));
        (void)ma; (void)mb; (void)mc;
    }

    bool zero_forzato = false;
    if (ref[0] < 0 && ref[1] < 0) {
        ref[0] = ref[1] = 0;
        zero_forzato = true;
    }

    for (int l = 0; l < 2; l++) {
        if (ref[l] < 0 || zero_forzato) {
            mvp[l][0] = mvp[l][1] = 0;
        } else {
            /* The ordinary 16x16 prediction, for the reference just chosen.
             * It has to run before anything of this macroblock is written,
             * which is why both lists are worked out first. */
            h264d_predict_mv(d, l, 0, 4, 4, ref[l], mvp[l]);
        }
    }

    /* The co-located picture is RefPicList1[0]. A direct macroblock in a
     * slice without one is a malformed stream. */
    const int slot_col = d->slice.ref_list[1][0];
    if (slot_col < 0 || slot_col >= H264D_DPB_SIZE)
        return 1;
    const h264d_frame_t *col = &d->dpb[slot_col];
    if (!col->col_ref || !col->col_mv)
        return 1;
    const int8_t *cr8 = col->col_ref + (size_t)d->mb_idx * 8;
    const int16_t *cmv = col->col_mv + (size_t)d->mb_idx * 64;

    /* With direct_8x8_inference_flag the co-located block of each 8x8 is its
     * outer corner, and the answer covers the whole 8x8. Without it, every
     * 4x4 asks about itself. */
    static const uint8_t angolo[4] = { 0, 3, 12, 15 };

    for (int p8 = 0; p8 < 4; p8++) {
        /* A B_8x8 can be direct in some of its four 8x8s and explicit in the
         * others, and the explicit ones have already been read. The
         * derivation itself is per macroblock - it only ever looks at
         * neighbours outside it - so it is worked out once and written only
         * where it belongs. */
        if (!((maschera >> p8) & 1))
            continue;
        const int base = (p8 >> 1) * 8 + (p8 & 1) * 2;
        for (int k = 0; k < 4; k++) {
            const int b = base + (k >> 1) * 4 + (k & 1);
            const int bcol = d->pic.direct_8x8_inference ? angolo[p8] : b;

            /* 8.4.1.2.1: the co-located block speaks through list 0 when it
             * used it, otherwise through list 1; an intra one says nothing. */
            int ref_col = cr8[0 * 4 + h264d_part8(bcol)];
            const int16_t *mv_col = cmv + (0 * 16 + bcol) * 2;
            if (ref_col < 0) {
                ref_col = cr8[1 * 4 + h264d_part8(bcol)];
                mv_col = cmv + (1 * 16 + bcol) * 2;
            }

            /* ⚠️ Short-term only. The harness refuses long-term references,
             * so every picture here is short-term; a decoder that accepts
             * them has to check, because a long-term co-located picture
             * never sets this flag however still it is. */
            const bool col_fermo = (ref_col == 0)
                                && mv_col[0] >= -1 && mv_col[0] <= 1
                                && mv_col[1] >= -1 && mv_col[1] <= 1;

            for (int l = 0; l < 2; l++) {
                int16_t mx = 0, my = 0;
                if (!zero_forzato && ref[l] >= 0 && !(ref[l] == 0 && col_fermo)) {
                    mx = mvp[l][0];
                    my = mvp[l][1];
                }
                m->mv[l][b][0] = mx;
                m->mv[l][b][1] = my;
                m->mvd[l][b][0] = 0;
                m->mvd[l][b][1] = 0;
            }
        }
        for (int l = 0; l < 2; l++) {
            m->ref_idx[l][p8] = (int8_t)ref[l];
            m->ref[l][p8] = (ref[l] >= 0)
                          ? d->slice.ref_list[l][ref[l]] : -1;
        }
    }
    return 0;
}
