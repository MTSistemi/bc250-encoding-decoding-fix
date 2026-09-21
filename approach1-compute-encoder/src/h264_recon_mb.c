/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_recon_mb.c - turning one macroblock's decoded syntax into pixels.
 *
 * The entropy layer has already filled in the macroblock's type, prediction
 * modes, motion and coefficients; this is everything that happens after.
 *
 * ⚠️ Intra prediction reads its neighbours UNFILTERED, and that is why the
 * deblocking filter runs as a separate pass at the end of the picture rather
 * than macroblock by macroblock. While decoding, the frame buffer holds
 * exactly what intra prediction is supposed to see. A decoder that filters as
 * it goes has to keep a second copy of the unfiltered edges; here the
 * ordering does it.
 */
#include "h264_dec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A newline that survives being written from a script. */
#define NEWLINE "\n"

#include "h264_dec_tables.h"
#include "h264_mc.h"
#include "h264_parts.h"
#include "h264_pred.h"

static const uint8_t zscan[16] = {
    0, 1, 4, 5,  2, 3, 6, 7,  8, 9, 12, 13,  10, 11, 14, 15
};

/* ------------------------------------------------------ neighbourhood */

/* Whether the macroblock that block (x4, y4) of the current macroblock would
 * reach is there AND may be predicted from. Under constrained intra
 * prediction an inter neighbour counts as absent, which is the whole point
 * of the flag: it stops intra macroblocks depending on inter ones, so that a
 * lost inter macroblock cannot drag the intra ones down with it. */
static bool intra_usabile(const h264_decoder_t *d, const h264d_mb_t *m)
{
    if (!m) return false;
    if (d->pic.constrained_intra_pred && !m->intra) return false;
    return true;
}

/* The sample at (x, y) of the current macroblock's neighbourhood, in frame
 * coordinates. Only ever called for positions already reconstructed. */
static inline uint8_t campione(const uint8_t *piano, int stride, int x, int y)
{
    return piano[(size_t)y * stride + x];
}

/* ------------------------------------------------------------ intra */

/* Reference samples for one 4x4 luma block, clause 8.3.1.2 with the
 * substitution of 8.3.1.2.1. */
static void riferimenti4(const h264_decoder_t *d, const uint8_t *y, int sy,
                         int b, uint8_t top[8], uint8_t left[4], uint8_t *corner,
                         bool *at, bool *al)
{
    const int x4 = b & 3, y4 = b >> 2;
    const int px = d->mb_x * 16 + x4 * 4;
    const int py = d->mb_y * 16 + y4 * 4;

    const h264d_mb_t *mia = &d->mbs[d->mb_idx];
    const h264d_mb_t *sx = (x4 == 0) ? h264d_mb_left(d) : mia;
    const h264d_mb_t *su = (y4 == 0) ? h264d_mb_top(d) : mia;

    *al = intra_usabile(d, sx);
    *at = intra_usabile(d, su);

    if (*at)
        for (int i = 0; i < 4; i++)
            top[i] = campione(y, sy, px + i, py - 1);
    else
        memset(top, 0, 4);

    if (*al)
        for (int i = 0; i < 4; i++)
            left[i] = campione(y, sy, px - 1, py + i);
    else
        memset(left, 0, 4);

    /* The four samples above and to the right. They exist only when the 4x4
     * block up there has already been decoded, which for the blocks on the
     * right edge of a macroblock means looking into the macroblock above
     * and to the right. Where they do not exist the standard repeats
     * p[3,-1], which is why this runs after `top` is filled. */
    bool atr = false;
    if (*at) {
        if (x4 == 3) {
            atr = (y4 == 0) && intra_usabile(d, h264d_mb_top_right(d));
        } else if (y4 == 0) {
            atr = true;                      /* still in the macroblock above */
        } else {
            /* inside this macroblock: only if that block came earlier */
            const int r = (y4 - 1) * 4 + x4 + 1;
            atr = zscan[r] < zscan[b];
        }
    }
    if (atr)
        for (int i = 0; i < 4; i++)
            top[4 + i] = campione(y, sy, px + 4 + i, py - 1);
    else
        memset(top + 4, *at ? top[3] : 0, 4);

    const h264d_mb_t *ang;
    if (x4 == 0 && y4 == 0)      ang = h264d_mb_top_left(d);
    else if (x4 == 0)            ang = h264d_mb_left(d);
    else if (y4 == 0)            ang = h264d_mb_top(d);
    else                         ang = mia;
    *corner = intra_usabile(d, ang) ? campione(y, sy, px - 1, py - 1) : 0;
}

/* The same for an 8x8 luma block, clause 8.3.2.2. */
static void riferimenti8(const h264_decoder_t *d, const uint8_t *y, int sy,
                         int b8, uint8_t top[16], uint8_t left[8], uint8_t *corner,
                         bool *at, bool *al, bool *ac, bool *atr)
{
    const int bx = b8 & 1, by = b8 >> 1;
    const int px = d->mb_x * 16 + bx * 8;
    const int py = d->mb_y * 16 + by * 8;

    const h264d_mb_t *mia = &d->mbs[d->mb_idx];
    const h264d_mb_t *sx = (bx == 0) ? h264d_mb_left(d) : mia;
    const h264d_mb_t *su = (by == 0) ? h264d_mb_top(d) : mia;

    *al = intra_usabile(d, sx);
    *at = intra_usabile(d, su);

    if (*at)
        for (int i = 0; i < 8; i++)
            top[i] = campione(y, sy, px + i, py - 1);
    else
        memset(top, 0, 8);

    if (*al)
        for (int i = 0; i < 8; i++)
            left[i] = campione(y, sy, px - 1, py + i);
    else
        memset(left, 0, 8);

    /* Above-right: for the two left-hand 8x8 blocks it is the other half of
     * the row above; for the top-right block it is the macroblock above and
     * to the right; for the bottom-right block there is nothing. */
    *atr = false;
    if (*at) {
        if (b8 == 0)      *atr = true;
        else if (b8 == 1) *atr = intra_usabile(d, h264d_mb_top_right(d));
        else if (b8 == 2) *atr = true;
        else              *atr = false;
    }
    if (*atr)
        for (int i = 0; i < 8; i++)
            top[8 + i] = campione(y, sy, px + 8 + i, py - 1);
    else
        memset(top + 8, *at ? top[7] : 0, 8);

    const h264d_mb_t *ang;
    if (bx == 0 && by == 0)      ang = h264d_mb_top_left(d);
    else if (bx == 0)            ang = h264d_mb_left(d);
    else if (by == 0)            ang = h264d_mb_top(d);
    else                         ang = mia;
    *ac = intra_usabile(d, ang);
    *corner = *ac ? campione(y, sy, px - 1, py - 1) : 0;
}

static void riferimenti16(const h264_decoder_t *d, const uint8_t *piano, int s,
                          int px, int py, int n,
                          uint8_t *top, uint8_t *left, uint8_t *corner,
                          bool *at, bool *al)
{
    *al = intra_usabile(d, h264d_mb_left(d));
    *at = intra_usabile(d, h264d_mb_top(d));
    if (*at) for (int i = 0; i < n; i++) top[i] = campione(piano, s, px + i, py - 1);
    else     memset(top, 0, (size_t)n);
    if (*al) for (int i = 0; i < n; i++) left[i] = campione(piano, s, px - 1, py + i);
    else     memset(left, 0, (size_t)n);
    *corner = intra_usabile(d, h264d_mb_top_left(d))
            ? campione(piano, s, px - 1, py - 1) : 0;
}


/* âš ï¸ Looked up once. These are asked per macroblock, and getenv walks the
 * environment every time: at 1080p that was just under 1% of the decode. */
static const char *ambiente(const char *nome, int quale)
{
    static const char *valore[3];
    static bool letto[3];
    if (!letto[quale]) {
        valore[quale] = getenv(nome);
        letto[quale] = true;
    }
    return valore[quale];
}

/* ------------------------------------------------------ inter */

/* Whether two 4x4 blocks of this macroblock move together: same reference
 * picture, same index into the list - the weights are chosen by index - and
 * the same vector in every list that is used. */
static inline bool stesso_moto(const h264d_mb_t *m, int a, int b)
{
    const int pa = h264d_part8(a), pb = h264d_part8(b);
    for (int l = 0; l < 2; l++) {
        if (m->ref[l][pa] != m->ref[l][pb]) return false;
        if (m->ref_idx[l][pa] != m->ref_idx[l][pb]) return false;
        if (m->ref[l][pa] < 0) continue;
        if (m->mv[l][a][0] != m->mv[l][b][0]) return false;
        if (m->mv[l][a][1] != m->mv[l][b][1]) return false;
    }
    return true;
}

/* A w4 x h4 rectangle of 4x4 blocks, in 4x4 units, predicted from one list
 * into tightly packed scratch blocks. `b` is its top-left block. */
static void predici_lista(h264_decoder_t *d, int b, int w4, int h4, int lista,
                          uint8_t *yb, uint8_t *cbb, uint8_t *crb)
{
    const h264d_mb_t *m = &d->mbs[d->mb_idx];
    const int slot = m->ref[lista][h264d_part8(b)];
    const h264d_frame_t *rif = &d->dpb[slot];
    const int16_t *mv = m->mv[lista][b];
    const int bx = b & 3, by = b >> 2;
    const int w = w4 * 4, h = h4 * 4;

    const int px = d->mb_x * 16 + bx * 4;
    const int py = d->mb_y * 16 + by * 4;

    uint8_t pad[(16 + H264D_MC_PAD_BEFORE + H264D_MC_PAD_AFTER)
                * (16 + H264D_MC_PAD_BEFORE + H264D_MC_PAD_AFTER)];
    int ps;
    const uint8_t *src = h264d_mc_fetch_luma(pad, &ps, rif->y, rif->stride_y,
                                             d->width, d->height,
                                             px + (mv[0] >> 2), py + (mv[1] >> 2),
                                             w, h);
    h264d_mc_luma(yb, w, src, ps, w, h, mv[0] & 3, mv[1] & 3);

    /* 4:2:0 chroma: the vector is the luma one, read at eighth-sample
     * accuracy over a plane at half the resolution. */
    const int cw = w4 * 2, ch = h4 * 2;
    const int cx = d->mb_x * 8 + bx * 2;
    const int cy = d->mb_y * 8 + by * 2;
    uint8_t cpad[(8 + 1) * (8 + 1)];
    const uint8_t *cs;
    int cst;
    cs = h264d_mc_fetch_chroma(cpad, &cst, rif->cb, rif->stride_c,
                               d->width / 2, d->height / 2,
                               cx + (mv[0] >> 3), cy + (mv[1] >> 3), cw, ch);
    h264d_mc_chroma(cbb, cw, cs, cst, cw, ch, mv[0] & 7, mv[1] & 7);
    cs = h264d_mc_fetch_chroma(cpad, &cst, rif->cr, rif->stride_c,
                               d->width / 2, d->height / 2,
                               cx + (mv[0] >> 3), cy + (mv[1] >> 3), cw, ch);
    h264d_mc_chroma(crb, cw, cs, cst, cw, ch, mv[0] & 7, mv[1] & 7);
}

/* Clause 8.4.2.3.1, the implicit weights: they come from how far the two
 * references sit from this picture in display order, so a B picture nearer
 * one of them leans on it more. Returns false when the standard says to fall
 * back to the plain average. */
static bool pesi_impliciti(const h264_decoder_t *d, int ref0, int ref1,
                           int *w0, int *w1)
{
    const int s0 = d->slice.ref_list[0][ref0];
    const int s1 = d->slice.ref_list[1][ref1];
    if (s0 < 0 || s1 < 0 || s0 >= H264D_DPB_SIZE || s1 >= H264D_DPB_SIZE)
        return false;
    if (d->dpb[s0].is_long_term || d->dpb[s1].is_long_term)
        return false;

    const int poc = d->dpb[d->cur].poc;
    int tb = poc - d->dpb[s0].poc;
    int td = d->dpb[s1].poc - d->dpb[s0].poc;
    if (td == 0)
        return false;
    tb = tb < -128 ? -128 : (tb > 127 ? 127 : tb);
    td = td < -128 ? -128 : (td > 127 ? 127 : td);

    const int tx = (16384 + abs(td / 2)) / td;
    int dsf = (tb * tx + 32) >> 6;
    dsf = dsf < -1024 ? -1024 : (dsf > 1023 ? 1023 : dsf);

    const int q = dsf >> 2;
    if (q < -64 || q > 128)
        return false;               /* too far apart: average them instead */
    *w1 = q;
    *w0 = 64 - q;
    return true;
}

/* One rectangle of an inter macroblock, however it is predicted. */
static void compensa_blocco(h264_decoder_t *d, int b, int w4, int h4,
                            uint8_t *dy, int sdy,
                            uint8_t *dcb, uint8_t *dcr, int sdc)
{
    const h264d_mb_t *m = &d->mbs[d->mb_idx];
    const int p8 = h264d_part8(b);
    const int r0 = m->ref_idx[0][p8], r1 = m->ref_idx[1][p8];
    const h264d_slice_t *s = &d->slice;
    const int w = w4 * 4, h = h4 * 4;
    const int cw = w4 * 2, ch = h4 * 2;

    uint8_t y0[256], cb0[64], cr0[64];
    uint8_t y1[256], cb1[64], cr1[64];

    const bool usa0 = r0 >= 0 && m->ref[0][p8] >= 0;
    const bool usa1 = r1 >= 0 && m->ref[1][p8] >= 0;
    if (!usa0 && !usa1)
        return;

    if (usa0) predici_lista(d, b, w4, h4, 0, y0, cb0, cr0);
    if (usa1) predici_lista(d, b, w4, h4, 1, y1, cb1, cr1);

    if (usa0 && usa1) {
        /* Clause 8.4.2.3: explicit weights when the picture parameter set
         * says 1, weights derived from the picture order counts when it says
         * 2, and otherwise the plain average. */
        int w0 = 32, w1 = 32, o0 = 0, o1 = 0, denom = 5;
        bool pesata = false;
        if (d->pic.weighted_bipred_idc == 1) {
            pesata = true;
            denom = s->luma_log2_weight_denom;
            w0 = s->luma_weight[0][r0];  o0 = s->luma_offset[0][r0];
            w1 = s->luma_weight[1][r1];  o1 = s->luma_offset[1][r1];
        } else if (d->pic.weighted_bipred_idc == 2) {
            pesata = pesi_impliciti(d, r0, r1, &w0, &w1);
            denom = 5;
            o0 = o1 = 0;
        }

        if (!pesata) {
            h264d_mc_average(dy, sdy, y0, w, y1, w, w, h);
            h264d_mc_average(dcb, sdc, cb0, cw, cb1, cw, cw, ch);
            h264d_mc_average(dcr, sdc, cr0, cw, cr1, cw, cw, ch);
            return;
        }

        h264d_mc_weight_bi(dy, sdy, y0, w, y1, w, w, h, denom, w0, o0, w1, o1);
        if (d->pic.weighted_bipred_idc == 1) {
            denom = s->chroma_log2_weight_denom;
            h264d_mc_weight_bi(dcb, sdc, cb0, cw, cb1, cw, cw, ch, denom,
                               s->chroma_weight[0][r0][0], s->chroma_offset[0][r0][0],
                               s->chroma_weight[1][r1][0], s->chroma_offset[1][r1][0]);
            h264d_mc_weight_bi(dcr, sdc, cr0, cw, cr1, cw, cw, ch, denom,
                               s->chroma_weight[0][r0][1], s->chroma_offset[0][r0][1],
                               s->chroma_weight[1][r1][1], s->chroma_offset[1][r1][1]);
        } else {
            h264d_mc_weight_bi(dcb, sdc, cb0, cw, cb1, cw, cw, ch, 5, w0, 0, w1, 0);
            h264d_mc_weight_bi(dcr, sdc, cr0, cw, cr1, cw, cw, ch, 5, w0, 0, w1, 0);
        }
        return;
    }

    /* One list only. */
    const int lista = usa0 ? 0 : 1;
    const int r = usa0 ? r0 : r1;
    const uint8_t *py = usa0 ? y0 : y1;
    const uint8_t *pcb = usa0 ? cb0 : cb1;
    const uint8_t *pcr = usa0 ? cr0 : cr1;

    const bool pesata = (d->pic.weighted_pred && s->type == 0)
                     || (d->pic.weighted_bipred_idc == 1 && s->type == 1);
    if (!pesata) {
        for (int yy = 0; yy < h; yy++)
            memcpy(dy + (size_t)yy * sdy, py + yy * w, (size_t)w);
        for (int yy = 0; yy < ch; yy++) {
            memcpy(dcb + (size_t)yy * sdc, pcb + yy * cw, (size_t)cw);
            memcpy(dcr + (size_t)yy * sdc, pcr + yy * cw, (size_t)cw);
        }
        return;
    }

    h264d_mc_weight(dy, sdy, py, w, w, h, s->luma_log2_weight_denom,
                    s->luma_weight[lista][r], s->luma_offset[lista][r]);
    h264d_mc_weight(dcb, sdc, pcb, cw, cw, ch, s->chroma_log2_weight_denom,
                    s->chroma_weight[lista][r][0], s->chroma_offset[lista][r][0]);
    h264d_mc_weight(dcr, sdc, pcr, cw, cw, ch, s->chroma_log2_weight_denom,
                    s->chroma_weight[lista][r][1], s->chroma_offset[lista][r][1]);
}

/* --------------------------------------------------------- residual */

static void aggiungi_luma(h264_decoder_t *d, h264d_mb_t *m, uint8_t *y, int sy)
{
    const int lista4 = m->intra ? 0 : 3;     /* scaling list, Table 7-2 */
    const int lista8 = m->intra ? 0 : 1;

    if (m->transform8x8) {
        for (int b8 = 0; b8 < 4; b8++) {
            if (!((m->cbp >> b8) & 1)) continue;
            uint8_t *dst = y + (size_t)((b8 >> 1) * 8) * sy + (b8 & 1) * 8;
            h264d_idct8_add(dst, sy, d->coeff8[b8], d->dequant.d8[lista8], m->qpy);
        }
        return;
    }

    for (int k = 0; k < 16; k++) {
        const int b = zscan[k];
        uint8_t *dst = y + (size_t)((b >> 2) * 4) * sy + (b & 3) * 4;
        const bool i16 = (m->type == H264D_MB_I_16x16);
        if (!i16 && !((m->cbp >> h264d_part8(b)) & 1))
            continue;
        /* An all-zero block adds nothing. nnz counts the coefficients the
         * entropy decoder actually read; for Intra_16x16 the DC arrives
         * separately and is not in that count, so it is tested on its own. */
        if (m->nnz[0][b] == 0 && (!i16 || d->coeff[0][b][0] == 0))
            continue;
        h264d_idct4_add(dst, sy, d->coeff[0][b], d->dequant.d4[lista4],
                        m->qpy, i16);
    }
}

/* ------------------------------------------------------------- driver */

void h264d_reconstruct_mb(h264_decoder_t *d)
{
    h264d_mb_t *m = &d->mbs[d->mb_idx];
    h264d_frame_t *f = &d->dpb[d->cur];

    uint8_t *y = f->y + (size_t)(d->mb_y * 16) * f->stride_y + d->mb_x * 16;
    uint8_t *cb = f->cb + (size_t)(d->mb_y * 8) * f->stride_c + d->mb_x * 8;
    uint8_t *cr = f->cr + (size_t)(d->mb_y * 8) * f->stride_c + d->mb_x * 8;
    const int sy = f->stride_y, sc = f->stride_c;

    if (d->dequant.qp != m->qpy || !d->dequant.valid)
        h264d_dequant_build(&d->dequant, m->qpy, d->pic.scaling4, d->pic.scaling8);

    if (m->intra) {
        if (m->type == H264D_MB_I_16x16) {
            uint8_t top[16], left[16], ang;
            bool at, al;
            riferimenti16(d, f->y, sy, d->mb_x * 16, d->mb_y * 16, 16,
                          top, left, &ang, &at, &al);
            h264d_pred16x16(y, sy, m->ipred[0], top, left, ang, at, al);

            /* The DC coefficients go through their own transform first, and
             * the result becomes each 4x4 block's DC.
             *
             * The 4x4 array they form is RASTER, not the Z order the blocks
             * are decoded in. It is a spatial downsample of the macroblock,
             * which is the whole point of the second Hadamard - it
             * decorrelates DCs that are next to each other. Handing it out
             * in Z order swaps the two blocks of each 8x8 diagonal. */
            const char *d16 = ambiente("BC250_H264_DUMP16", 0);
            const int m16 = d16 ? atoi(d16) : -1;
            if (m16 == d->mb_idx) {
                fprintf(stderr, "mb %d I16 modo %d cbp %02x qp %d%s",
                        d->mb_idx, m->ipred[0], m->cbp, m->qpy, NEWLINE);
                fprintf(stderr, "  previsione (riga 0 e colonna 0):%s", NEWLINE);
                fprintf(stderr, "   ");
                for (int xx = 0; xx < 16; xx++) fprintf(stderr, " %3d", y[xx]);
                fprintf(stderr, "%s", NEWLINE);
                fprintf(stderr, "  DC prima della trasformata:%s", NEWLINE);
                for (int yy = 0; yy < 4; yy++) {
                    fprintf(stderr, "   ");
                    for (int xx = 0; xx < 4; xx++)
                        fprintf(stderr, " %6d", d->dc_luma[yy * 4 + xx]);
                    fprintf(stderr, "%s", NEWLINE);
                }
            }

            h264d_luma_dc_transform(d->dc_luma, m->qpy, d->dequant.d4[0][0]);
            for (int k = 0; k < 16; k++)
                d->coeff[0][k][0] = d->dc_luma[k];

            if (m16 == d->mb_idx) {
                fprintf(stderr, "  DC dopo la trasformata:%s", NEWLINE);
                for (int yy = 0; yy < 4; yy++) {
                    fprintf(stderr, "   ");
                    for (int xx = 0; xx < 4; xx++)
                        fprintf(stderr, " %6d", d->dc_luma[yy * 4 + xx]);
                    fprintf(stderr, "%s", NEWLINE);
                }
                fprintf(stderr, "  AC del blocco raster 0:%s", NEWLINE);
                fprintf(stderr, "   ");
                for (int xx = 1; xx < 16; xx++)
                    fprintf(stderr, " %4d", d->coeff[0][0][xx]);
                fprintf(stderr, "%s", NEWLINE);
            }
        } else if (m->transform8x8) {
            for (int b8 = 0; b8 < 4; b8++) {
                uint8_t top[16], left[8], ang;
                bool at, al, ac, atr;
                riferimenti8(d, f->y, sy, b8, top, left, &ang, &at, &al, &ac, &atr);
                uint8_t *dst = y + (size_t)((b8 >> 1) * 8) * sy + (b8 & 1) * 8;
                const int modo = m->ipred[(b8 >> 1) * 8 + (b8 & 1) * 2];
                h264d_pred8x8_luma(dst, sy, modo, top, left, ang, at, al, ac, atr);
                if ((m->cbp >> b8) & 1)
                    h264d_idct8_add(dst, sy, d->coeff8[b8], d->dequant.d8[0], m->qpy);
            }
        } else {
            /* Intra_4x4 predicts and reconstructs one block at a time: the
             * next block's references are this block's output. */
            for (int k = 0; k < 16; k++) {
                const int b = zscan[k];
                uint8_t top[8], left[4], ang;
                bool at, al;
                riferimenti4(d, f->y, sy, b, top, left, &ang, &at, &al);
                uint8_t *dst = y + (size_t)((b >> 2) * 4) * sy + (b & 3) * 4;
                h264d_pred4x4(dst, sy, m->ipred[b], top, left, ang, at, al);

                const char *dump = ambiente("BC250_H264_DUMP", 1);
                int dmb = -1, dbl = -1;
                if (dump) sscanf(dump, "%d,%d", &dmb, &dbl);
                if (dmb == d->mb_idx && dbl == b) {
                    fprintf(stderr, "mb %d blocco %d modo %d top=%d left=%d qp %d%s",
                            d->mb_idx, b, m->ipred[b], at, al, m->qpy, NEWLINE);
                    fprintf(stderr, "  previsione:%s", NEWLINE);
                    for (int yy = 0; yy < 4; yy++) {
                        fprintf(stderr, "   ");
                        for (int xx = 0; xx < 4; xx++)
                            fprintf(stderr, " %3d", dst[yy * sy + xx]);
                        fprintf(stderr, "%s", NEWLINE);
                    }
                    fprintf(stderr, "  coefficienti (raster):%s", NEWLINE);
                    for (int yy = 0; yy < 4; yy++) {
                        fprintf(stderr, "   ");
                        for (int xx = 0; xx < 4; xx++)
                            fprintf(stderr, " %5d", d->coeff[0][b][yy * 4 + xx]);
                        fprintf(stderr, "%s", NEWLINE);
                    }
                }

                if ((m->cbp >> h264d_part8(b)) & 1)
                    h264d_idct4_add(dst, sy, d->coeff[0][b],
                                    d->dequant.d4[0], m->qpy, false);

                if (dmb == d->mb_idx && dbl == b) {
                    fprintf(stderr, "  ricostruito:%s", NEWLINE);
                    for (int yy = 0; yy < 4; yy++) {
                        fprintf(stderr, "   ");
                        for (int xx = 0; xx < 4; xx++)
                            fprintf(stderr, " %3d", dst[yy * sy + xx]);
                        fprintf(stderr, "%s", NEWLINE);
                    }
                }
            }
        }

        if (m->type == H264D_MB_I_16x16)
            aggiungi_luma(d, m, y, sy);

        /* Chroma, both planes. */
        uint8_t top[8], left[8], ang;
        bool at, al;
        riferimenti16(d, f->cb, sc, d->mb_x * 8, d->mb_y * 8, 8,
                      top, left, &ang, &at, &al);
        h264d_pred_chroma(cb, sc, m->chroma_pred_mode, top, left, ang, at, al);
        riferimenti16(d, f->cr, sc, d->mb_x * 8, d->mb_y * 8, 8,
                      top, left, &ang, &at, &al);
        h264d_pred_chroma(cr, sc, m->chroma_pred_mode, top, left, ang, at, al);
    } else {
        /* One list only for now; the bi-predicted case averages two of
         * these into a scratch block, which the B stage adds. */
        /* Greedy rectangles of identical motion: as wide as the row
         * allows, then as tall as that width allows. A 16x16 partition
         * comes out as one call instead of sixteen, and a macroblock whose
         * four 8x8s move differently still costs four. */
        bool fatto[16] = { false };
        for (int b = 0; b < 16; b++) {
            if (fatto[b]) continue;
            const int bx = b & 3, by = b >> 2;
            int w4 = 1;
            while (bx + w4 < 4 && !fatto[b + w4] && stesso_moto(m, b, b + w4))
                w4++;
            int h4 = 1;
            while (by + h4 < 4) {
                bool uguale = true;
                for (int k = 0; k < w4 && uguale; k++)
                    uguale = !fatto[b + h4 * 4 + k]
                          && stesso_moto(m, b, b + h4 * 4 + k);
                if (!uguale) break;
                h4++;
            }

            uint8_t *dy = y + (size_t)(by * 4) * sy + bx * 4;
            uint8_t *dcb = cb + (size_t)(by * 2) * sc + bx * 2;
            uint8_t *dcr = cr + (size_t)(by * 2) * sc + bx * 2;
            compensa_blocco(d, b, w4, h4, dy, sy, dcb, dcr, sc);

            for (int j = 0; j < h4; j++)
                for (int k = 0; k < w4; k++)
                    fatto[b + j * 4 + k] = true;
        }

        const char *di = ambiente("BC250_H264_DUMPI", 2);
        const int mi = di ? atoi(di) : -1;
        if (mi == d->mb_idx) {
            fprintf(stderr, "mb %d inter t8 %d cbp %02x qp %d mv %d,%d rif %d%s",
                    d->mb_idx, m->transform8x8, m->cbp, m->qpy,
                    m->mv[0][0][0], m->mv[0][0][1], m->ref_idx[0][0], NEWLINE);
            fprintf(stderr, "  previsione, prime due righe:%s", NEWLINE);
            for (int yy = 0; yy < 2; yy++) {
                fprintf(stderr, "   ");
                for (int xx = 0; xx < 8; xx++)
                    fprintf(stderr, " %3d", y[yy * sy + xx]);
                fprintf(stderr, "%s", NEWLINE);
            }
            if (m->transform8x8) {
                fprintf(stderr, "  coefficienti 8x8 del blocco 0, prime 16:%s",
                        NEWLINE);
                fprintf(stderr, "   ");
                for (int k = 0; k < 16; k++)
                    fprintf(stderr, " %5d", d->coeff8[0][k]);
                fprintf(stderr, "%s", NEWLINE);
            }
        }

        aggiungi_luma(d, m, y, sy);

        if (mi == d->mb_idx) {
            fprintf(stderr, "  ricostruito, prime due righe:%s", NEWLINE);
            for (int yy = 0; yy < 2; yy++) {
                fprintf(stderr, "   ");
                for (int xx = 0; xx < 8; xx++)
                    fprintf(stderr, " %3d", y[yy * sy + xx]);
                fprintf(stderr, "%s", NEWLINE);
            }
        }
    }

    /* Chroma residual, the same for intra and inter: the DC coefficients of
     * each plane go through their own 2x2 transform, then each 4x4 block. */
    for (int p = 0; p < 2; p++) {
        uint8_t *piano = p ? cr : cb;
        /* Table 7-2: scaling lists 1 and 2 are intra Cb and Cr, 4 and 5 the
         * inter ones. Both planes used to share one index, which quietly
         * applied Cb's matrix to Cr. */
        const int lista_c = (m->intra ? 1 : 4) + p;
        const int off = p ? d->pic.second_chroma_qp_index_offset
                          : d->pic.chroma_qp_index_offset;
        int qpi = m->qpy + off;
        qpi = qpi < 0 ? 0 : (qpi > 51 ? 51 : qpi);
        const int qpc = h264d_chroma_qp[qpi];

        h264d_dequant_t *dqc = &d->dequant_c[p];
        if (!dqc->valid || dqc->qp != qpc)
            h264d_dequant_build(dqc, qpc, d->pic.scaling4, d->pic.scaling8);

        if ((m->cbp >> 4) != 0)
            h264d_chroma_dc_transform(d->dc_chroma[p], qpc, dqc->d4[lista_c][0]);

        /* A block whose DC and AC are both zero adds nothing, and a
         * macroblock with no chroma residual at all is the common case:
         * without this every one of them ran eight inverse transforms over
         * zeros. */
        for (int b = 0; b < 4; b++) {
            d->coeff[p + 1][b][0] = d->dc_chroma[p][b];
            if (m->nnz[p + 1][b] == 0 && d->dc_chroma[p][b] == 0)
                continue;
            uint8_t *dst = piano + (size_t)((b >> 1) * 4) * sc + (b & 1) * 4;
            h264d_idct4_add(dst, sc, d->coeff[p + 1][b], dqc->d4[lista_c],
                            qpc, true);
        }
    }
}
