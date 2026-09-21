/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_mb_cabac.c - the macroblock layer read with CABAC, Rec. ITU-T H.264
 * clause 7.3.5 with the context derivations of 9.3.3.1.
 *
 * Every syntax element here is a handful of bins, and almost every one of
 * them picks its context from the macroblocks above and to the left. Getting
 * a context wrong does not desynchronise the bitstream straight away - CABAC
 * keeps decoding, just with the wrong probabilities - so the damage shows up
 * a few hundred bins later as a macroblock with an impossible type. That is
 * why the neighbour lookups are written once, at the top, rather than
 * open-coded at each use.
 *
 * ⚠️ A neighbour in a different slice is not available. A slice has to
 * decode on its own, which is what slices are for, and h264d_mb_left() and
 * friends return NULL across a slice boundary. The deblocking filter is the
 * only part that may look across, and it decides for itself.
 */
#include "h264_dec_internal.h"

#include <string.h>

#include "h264_dec_tables.h"
#include "h264_pred.h"

/* Decoding order of the sixteen 4x4 luma blocks, as raster positions inside
 * the macroblock. The standard walks them in the Z order of 6.4.3; the
 * decoder stores everything in raster order, because every neighbour lookup
 * and the deblocking filter want raster. */
static const uint8_t zscan[16] = {
    0, 1, 4, 5,  2, 3, 6, 7,  8, 9, 12, 13,  10, 11, 14, 15
};

/* ---------------------------------------------------------- neighbours */

/* The 4x4 block to the left of raster block `b`, and the macroblock it is
 * in. Returns NULL when there is none. */
static const h264d_mb_t *sinistra4(const h264_decoder_t *d, int b, int *out)
{
    if (b & 3) { *out = b - 1; return &d->mbs[d->mb_idx]; }
    const h264d_mb_t *m = h264d_mb_left(d);
    if (!m) return NULL;
    *out = b + 3;
    return m;
}

static const h264d_mb_t *sopra4(const h264_decoder_t *d, int b, int *out)
{
    if (b >= 4) { *out = b - 4; return &d->mbs[d->mb_idx]; }
    const h264d_mb_t *m = h264d_mb_top(d);
    if (!m) return NULL;
    *out = b + 12;
    return m;
}

/* The same for a chroma 4x4 block, which is one of a 2x2 grid. */
static const h264d_mb_t *sinistra_croma(const h264_decoder_t *d, int b, int *out)
{
    if (b & 1) { *out = b - 1; return &d->mbs[d->mb_idx]; }
    const h264d_mb_t *m = h264d_mb_left(d);
    if (!m) return NULL;
    *out = b + 1;
    return m;
}

static const h264d_mb_t *sopra_croma(const h264_decoder_t *d, int b, int *out)
{
    if (b >= 2) { *out = b - 2; return &d->mbs[d->mb_idx]; }
    const h264d_mb_t *m = h264d_mb_top(d);
    if (!m) return NULL;
    *out = b + 2;
    return m;
}

/* ------------------------------------------------------ syntax elements */

static inline int decidi(h264_decoder_t *d, int ctx)
{
    return h264d_cabac_decision(&d->cabac, ctx);
}

/* 9.3.3.1.1.1 */
static int leggi_mb_skip(h264_decoder_t *d, bool bslice)
{
    const h264d_mb_t *a = h264d_mb_left(d);
    const h264d_mb_t *b = h264d_mb_top(d);
    int inc = 0;
    if (a && a->type != H264D_MB_P_SKIP && a->type != H264D_MB_B_SKIP) inc++;
    if (b && b->type != H264D_MB_P_SKIP && b->type != H264D_MB_B_SKIP) inc++;
    return decidi(d, (bslice ? CTX_MB_SKIP_B : CTX_MB_SKIP_P) + inc);
}

/* 9.3.2.5 and Table 9-36: the intra part of mb_type, shared by the I-slice
 * form and the intra escape of a P or B slice. Returns 0 for I_NxN, 25 for
 * I_PCM, 1..24 for the Intra_16x16 variants. */
static int leggi_mb_type_intra(h264_decoder_t *d, int base, bool in_i_slice)
{
    int stato = base;
    if (in_i_slice) {
        const h264d_mb_t *a = h264d_mb_left(d);
        const h264d_mb_t *b = h264d_mb_top(d);
        int inc = 0;
        if (a && a->type != H264D_MB_I_NxN) inc++;
        if (b && b->type != H264D_MB_I_NxN) inc++;
        if (!decidi(d, base + inc))
            return 0;
        stato = base + 2;
    } else {
        if (!decidi(d, base))
            return 0;
    }

    /* ⚠️ The second bin is the terminate decision, not an ordinary one. It
     * is the only place outside end_of_slice_flag that uses it, and reading
     * it as a normal bin leaves codIRange two too high from here on. */
    if (h264d_cabac_terminate(&d->cabac))
        return 25;                      /* I_PCM */

    int t = 1;
    t += 12 * decidi(d, stato + 1);     /* cbp_luma: 0 or 15 */
    if (decidi(d, stato + 2))           /* cbp_chroma */
        t += 4 + 4 * decidi(d, stato + 2 + (in_i_slice ? 1 : 0));
    t += 2 * decidi(d, stato + 3 + (in_i_slice ? 1 : 0));
    t += 1 * decidi(d, stato + 3 + (in_i_slice ? 2 : 0));
    return t;
}

/* Table 9-37, P slices. Returns the standard's mb_type, with the intra
 * types offset by 5 as the standard numbers them. */
static int leggi_mb_type_p(h264_decoder_t *d)
{
    if (!decidi(d, CTX_MB_TYPE_P)) {
        if (!decidi(d, CTX_MB_TYPE_P + 1))
            return 3 * decidi(d, CTX_MB_TYPE_P + 2);      /* 16x16 or 8x8 */
        return 2 - decidi(d, CTX_MB_TYPE_P + 3);          /* 16x8 or 8x16 */
    }
    return leggi_mb_type_intra(d, CTX_MB_TYPE_P + 3, false) + 5;
}

/* Table 9-37, B slices. */
static int leggi_mb_type_b(h264_decoder_t *d)
{
    const h264d_mb_t *a = h264d_mb_left(d);
    const h264d_mb_t *b = h264d_mb_top(d);
    int inc = 0;
    if (a && a->type != H264D_MB_B_SKIP && a->type != H264D_MB_B_DIRECT) inc++;
    if (b && b->type != H264D_MB_B_SKIP && b->type != H264D_MB_B_DIRECT) inc++;

    if (!decidi(d, CTX_MB_TYPE_B + inc))
        return 0;                                   /* B_Direct_16x16 */
    if (!decidi(d, CTX_MB_TYPE_B + 3))
        return 1 + decidi(d, CTX_MB_TYPE_B + 5);    /* B_L0_16x16, B_L1_16x16 */

    int bits = decidi(d, CTX_MB_TYPE_B + 4) << 3;
    bits |= decidi(d, CTX_MB_TYPE_B + 5) << 2;
    bits |= decidi(d, CTX_MB_TYPE_B + 5) << 1;
    bits |= decidi(d, CTX_MB_TYPE_B + 5);
    if (bits < 8)
        return bits + 3;
    if (bits == 13)
        return leggi_mb_type_intra(d, CTX_MB_TYPE_B + 5, false) + 23;
    if (bits == 14)
        return 11;                                  /* B_L1_L0_8x16 */
    if (bits == 15)
        return 22;                                  /* B_8x8 */
    bits = (bits << 1) | decidi(d, CTX_MB_TYPE_B + 5);
    return bits - 4;
}

static int leggi_sub_mb_type_p(h264_decoder_t *d)
{
    if (decidi(d, CTX_SUB_MB_TYPE_P))     return 0;   /* 8x8 */
    if (!decidi(d, CTX_SUB_MB_TYPE_P + 1)) return 1;  /* 8x4 */
    if (decidi(d, CTX_SUB_MB_TYPE_P + 2)) return 2;   /* 4x8 */
    return 3;                                         /* 4x4 */
}

static int leggi_sub_mb_type_b(h264_decoder_t *d)
{
    if (!decidi(d, CTX_SUB_MB_TYPE_B))
        return 0;                                     /* B_Direct_8x8 */
    if (!decidi(d, CTX_SUB_MB_TYPE_B + 1))
        return 1 + decidi(d, CTX_SUB_MB_TYPE_B + 3);
    int t = 3;
    if (decidi(d, CTX_SUB_MB_TYPE_B + 2)) {
        if (decidi(d, CTX_SUB_MB_TYPE_B + 3))
            return 11 + decidi(d, CTX_SUB_MB_TYPE_B + 3);
        t += 4;
    }
    t += 2 * decidi(d, CTX_SUB_MB_TYPE_B + 3);
    t += decidi(d, CTX_SUB_MB_TYPE_B + 3);
    return t;
}

/* 9.3.3.1.1.6 */
static int leggi_ref_idx(h264_decoder_t *d, int lista, int blk8, int cap)
{
    if (cap <= 1) return 0;

    const h264d_mb_t *mb = &d->mbs[d->mb_idx];
    int inc = 0;
    /* The neighbouring 8x8 partitions: to the left and above this one. */
    const int bx = blk8 & 1, by = blk8 >> 1;
    const h264d_mb_t *a;
    int pa;
    if (bx) { a = mb; pa = blk8 - 1; }
    else    { a = h264d_mb_left(d); pa = blk8 + 1; }
    const h264d_mb_t *b;
    int pb;
    if (by) { b = mb; pb = blk8 - 2; }
    else    { b = h264d_mb_top(d); pb = blk8 + 2; }

    if (a && !a->intra && a->ref[lista][pa] > 0) inc += 1;
    if (b && !b->intra && b->ref[lista][pb] > 0) inc += 2;

    if (!decidi(d, CTX_REF_IDX + inc))
        return 0;
    if (!decidi(d, CTX_REF_IDX + 4))
        return 1;
    int v = 2;
    while (v < cap - 1 && decidi(d, CTX_REF_IDX + 5))
        v++;
    return v;
}

/* 9.3.3.1.1.7, then the UEG3 binarization of 9.3.2.3. */
static int leggi_mvd(h264_decoder_t *d, int comp, int somma_vicini)
{
    const int base = comp ? CTX_MVD_Y : CTX_MVD_X;
    int inc = somma_vicini < 3 ? 0 : (somma_vicini > 32 ? 2 : 1);

    if (!decidi(d, base + inc))
        return 0;

    int v = 1;
    while (v < 9 && decidi(d, base + 2 + (v < 4 ? v + 1 : 6)))
        v++;

    if (v == 9)
        v += (int)h264d_cabac_eg_bypass(&d->cabac, 3);

    return h264d_cabac_bypass(&d->cabac) ? -v : v;
}

/* 9.3.3.1.1.4. ⚠️ The condition is inverted with respect to every other
 * neighbour test here: the context counts 8x8 blocks that are NOT coded. */
static int leggi_cbp(h264_decoder_t *d)
{
    const h264d_mb_t *mb = &d->mbs[d->mb_idx];
    int cbp = 0;

    for (int i = 0; i < 4; i++) {
        const int bx = i & 1, by = i >> 1;
        int va, vb;
        if (bx) va = (cbp >> (i - 1)) & 1;
        else {
            const h264d_mb_t *a = h264d_mb_left(d);
            va = a ? (a->cbp >> (i + 1)) & 1 : 1;
        }
        if (by) vb = (cbp >> (i - 2)) & 1;
        else {
            const h264d_mb_t *b = h264d_mb_top(d);
            vb = b ? (b->cbp >> (i + 2)) & 1 : 1;
        }
        const int inc = (va ? 0 : 1) + (vb ? 0 : 2);
        cbp |= decidi(d, CTX_CBP_LUMA + inc) << i;
    }
    (void)mb;

    /* 9.3.3.1.1.4 for chroma: the first bin asks whether either neighbour
     * has any chroma coefficients, the second whether either has AC ones. */
    const h264d_mb_t *a = h264d_mb_left(d);
    const h264d_mb_t *b = h264d_mb_top(d);
    int ca = a ? (a->cbp >> 4) : 0;
    int cb = b ? (b->cbp >> 4) : 0;
    int inc = (ca ? 1 : 0) + (cb ? 2 : 0);
    if (decidi(d, CTX_CBP_CHROMA + inc)) {
        inc = 4 + (ca == 2 ? 1 : 0) + (cb == 2 ? 2 : 0);
        cbp |= (decidi(d, CTX_CBP_CHROMA + inc) ? 2 : 1) << 4;
    }
    return cbp;
}

/* 9.3.3.1.1.5, then the mapping of 9.3.2.7. */
static int leggi_qp_delta(h264_decoder_t *d)
{
    int inc = d->last_qp_delta_nonzero ? 1 : 0;
    if (!decidi(d, CTX_MB_QP_DELTA + inc)) {
        d->last_qp_delta_nonzero = 0;
        return 0;
    }
    int k = 1;
    if (decidi(d, CTX_MB_QP_DELTA + 2)) {
        k = 2;
        while (k < 96 && decidi(d, CTX_MB_QP_DELTA + 3))
            k++;
    }
    d->last_qp_delta_nonzero = 1;
    /* value k maps to (-1)^(k+1) * Ceil(k / 2) */
    return (k & 1) ? (k + 1) / 2 : -(k / 2);
}

/* 9.3.3.1.1.8 */
static int leggi_chroma_pred_mode(h264_decoder_t *d)
{
    const h264d_mb_t *a = h264d_mb_left(d);
    const h264d_mb_t *b = h264d_mb_top(d);
    int inc = 0;
    if (a && a->intra && a->type != H264D_MB_I_PCM && a->chroma_pred_mode != 0) inc++;
    if (b && b->intra && b->type != H264D_MB_I_PCM && b->chroma_pred_mode != 0) inc++;

    if (!decidi(d, CTX_INTRA_CHROMA_PRED + inc))
        return 0;
    if (!decidi(d, CTX_INTRA_CHROMA_PRED + 3))
        return 1;
    return decidi(d, CTX_INTRA_CHROMA_PRED + 3) ? 3 : 2;
}

static int leggi_transform8x8(h264_decoder_t *d)
{
    const h264d_mb_t *a = h264d_mb_left(d);
    const h264d_mb_t *b = h264d_mb_top(d);
    int inc = (a && a->transform8x8) + (b && b->transform8x8);
    return decidi(d, CTX_TRANSFORM_8X8 + inc);
}

/* ------------------------------------------------------------- residual */

/* 9.3.3.1.1.9, condTermFlagN for coded_block_flag. */
static int cbf_vicino(const h264_decoder_t *d, const h264d_mb_t *n, int blocco,
                      int piano, bool dc, bool corrente_intra)
{
    if (!n)
        return corrente_intra ? 1 : 0;
    if (n->type == H264D_MB_I_PCM)
        return 1;
    if (n->type == H264D_MB_P_SKIP || n->type == H264D_MB_B_SKIP)
        return 0;
    if (dc)
        return n->cbf_dc[piano] ? 1 : 0;
    return n->nnz[piano][blocco] ? 1 : 0;
    (void)d;
}

/* Clause 7.3.5.3.3 with the contexts of 9.3.3.1.3.
 *
 * `cat` is the block category of Table 9-42, `n` the number of coefficients
 * the block can hold, and `out` receives them in the scan order the caller
 * asked for - the caller un-scans them, because the 4x4 and 8x8 scans differ
 * and the DC blocks use a third one.
 *
 * Returns how many coefficients are non-zero, which is what the CAVLC
 * context of the next macroblock and the deblocking filter both want.
 */
static int leggi_residuo(h264_decoder_t *d, int cat, int n, int16_t *out,
                         int cbf_inc, bool leggi_cbf)
{
    memset(out, 0, (size_t)n * sizeof(int16_t));

    if (leggi_cbf) {
        const int ctx = CTX_CBF + 4 * cat + cbf_inc;
        if (!decidi(d, ctx))
            return 0;
    }

    const int campo = 0;    /* progressive only */
    const int base_sig  = h264d_sig_coeff_offset[campo][cat];
    const int base_last = h264d_last_coeff_offset[campo][cat];
    const int base_abs  = h264d_abs_level_offset[cat];
    const bool otto = (cat == CAT_LUMA_8X8);

    uint8_t significativo[64];
    memset(significativo, 0, sizeof(significativo));

    int ultimo = n - 1;
    int i = 0;
    while (i < n - 1) {
        int inc_sig, inc_last;
        if (otto) {
            inc_sig = h264d_sig_coeff_offset_8x8[campo][i];
            inc_last = h264d_last_coeff_offset_8x8[i];
        } else if (cat == CAT_CHROMA_DC) {
            /* NumC8x8 is 1 at 4:2:0, so the index is just i capped at 2. */
            inc_sig = inc_last = i < 2 ? i : 2;
        } else {
            inc_sig = inc_last = i;
        }
        if (decidi(d, base_sig + inc_sig)) {
            significativo[i] = 1;
            if (decidi(d, base_last + inc_last)) {
                ultimo = i;
                break;
            }
        }
        i++;
    }
    if (i == n - 1)
        significativo[i] = 1;      /* ran to the end: the last one must be */
    else
        significativo[ultimo] = 1;

    /* Levels, read backwards from the last significant coefficient. */
    int quanti_uno = 0, quanti_maggiori = 0, nonzero = 0;
    for (int k = ultimo; k >= 0; k--) {
        if (!significativo[k])
            continue;

        int inc = quanti_maggiori ? 0
                                  : (1 + quanti_uno < 4 ? 1 + quanti_uno : 4);
        int livello = 1;
        if (decidi(d, base_abs + inc)) {
            /* The tail uses a second context group, capped one lower for
             * chroma DC because that block has fewer coefficients. */
            const int cappa = (cat == CAT_CHROMA_DC) ? 3 : 4;
            int m = 1;
            while (m < 13) {
                inc = 5 + (quanti_maggiori < cappa ? quanti_maggiori : cappa);
                if (!decidi(d, base_abs + inc))
                    break;
                m++;
            }
            livello = 1 + m;
            if (m == 13)
                livello += (int)h264d_cabac_eg_bypass(&d->cabac, 0);
            quanti_maggiori++;
        } else {
            quanti_uno++;
        }

        out[k] = (int16_t)(h264d_cabac_bypass(&d->cabac) ? -livello : livello);
        nonzero++;
    }
    return nonzero;
}

/* --------------------------------------------------------- the macroblock */

static void azzera_mb(h264d_mb_t *m)
{
    memset(m, 0, sizeof(*m));
    memset(m->ref, -1, sizeof(m->ref));
}

/* The residual of one macroblock, clause 7.3.5.3. */
static void leggi_residuo_mb(h264_decoder_t *d, h264d_mb_t *m, bool i16)
{
    const bool intra = m->intra;

    if (i16) {
        const h264d_mb_t *a; int ba;
        const h264d_mb_t *b; int bb;
        a = h264d_mb_left(d); ba = 0;
        b = h264d_mb_top(d);  bb = 0;
        int inc = cbf_vicino(d, a, ba, 0, true, intra)
                + 2 * cbf_vicino(d, b, bb, 0, true, intra);
        int nz = leggi_residuo(d, CAT_I16_DC, 16, d->dc_luma, inc, true);
        m->cbf_dc[0] = nz ? 1 : 0;
    }

    /* Luma. */
    if (m->transform8x8) {
        for (int b8 = 0; b8 < 4; b8++) {
            if (!((m->cbp >> b8) & 1)) {
                memset(d->coeff8[b8], 0, sizeof(d->coeff8[b8]));
                continue;
            }
            /* No coded_block_flag for an 8x8 luma block at 4:2:0: the
             * coded block pattern has already said it is there. */
            int nz = leggi_residuo(d, CAT_LUMA_8X8, 64, d->coeff8[b8], 0, false);
            const int bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 2; x++)
                    m->nnz[0][(by + y) * 4 + bx + x] = (uint8_t)(nz ? 1 : 0);
        }
    } else {
        for (int k = 0; k < 16; k++) {
            const int b = zscan[k];
            const int b8 = ((b >> 3) << 1) | ((b >> 1) & 1);
            if (!((m->cbp >> b8) & 1)) {
                memset(d->coeff[0][b], 0, sizeof(d->coeff[0][b]));
                m->nnz[0][b] = 0;
                continue;
            }
            int pa, pb;
            const h264d_mb_t *a = sinistra4(d, b, &pa);
            const h264d_mb_t *bmb = sopra4(d, b, &pb);
            int inc = cbf_vicino(d, a, pa, 0, false, intra)
                    + 2 * cbf_vicino(d, bmb, pb, 0, false, intra);
            const int cat = i16 ? CAT_I16_AC : CAT_LUMA_4X4;
            const int n = i16 ? 15 : 16;
            int16_t tmp[16];
            int nz = leggi_residuo(d, cat, n, tmp, inc, true);
            /* An Intra_16x16 block codes only its fifteen AC coefficients;
             * the DC comes from the separate block above. */
            if (i16) {
                d->coeff[0][b][0] = 0;
                memcpy(d->coeff[0][b] + 1, tmp, 15 * sizeof(int16_t));
            } else {
                memcpy(d->coeff[0][b], tmp, 16 * sizeof(int16_t));
            }
            m->nnz[0][b] = (uint8_t)nz;
        }
    }

    /* Chroma DC, then chroma AC. */
    const int cbp_c = m->cbp >> 4;
    for (int p = 0; p < 2; p++) {
        if (cbp_c) {
            const h264d_mb_t *a = h264d_mb_left(d);
            const h264d_mb_t *b = h264d_mb_top(d);
            int inc = cbf_vicino(d, a, 0, p + 1, true, intra)
                    + 2 * cbf_vicino(d, b, 0, p + 1, true, intra);
            int nz = leggi_residuo(d, CAT_CHROMA_DC, 4, d->dc_chroma[p], inc, true);
            m->cbf_dc[p + 1] = nz ? 1 : 0;
        } else {
            memset(d->dc_chroma[p], 0, sizeof(d->dc_chroma[p]));
            m->cbf_dc[p + 1] = 0;
        }
    }
    for (int p = 0; p < 2; p++) {
        for (int b = 0; b < 4; b++) {
            if (cbp_c != 2) {
                memset(d->coeff[p + 1][b], 0, sizeof(d->coeff[p + 1][b]));
                m->nnz[p + 1][b] = 0;
                continue;
            }
            int pa, pb;
            const h264d_mb_t *a = sinistra_croma(d, b, &pa);
            const h264d_mb_t *bmb = sopra_croma(d, b, &pb);
            int inc = cbf_vicino(d, a, pa, p + 1, false, intra)
                    + 2 * cbf_vicino(d, bmb, pb, p + 1, false, intra);
            int16_t tmp[16];
            int nz = leggi_residuo(d, CAT_CHROMA_AC, 15, tmp, inc, true);
            d->coeff[p + 1][b][0] = 0;
            memcpy(d->coeff[p + 1][b] + 1, tmp, 15 * sizeof(int16_t));
            m->nnz[p + 1][b] = (uint8_t)nz;
        }
    }
}

/* The Intra_16x16 mb_type encodes the prediction mode, the luma coded block
 * pattern and the chroma one in a single number, clause 7.4.5 Table 7-11. */
static void spacchetta_i16(int t, int *modo, int *cbp)
{
    const int k = t - 1;                 /* 0..23 */
    *modo = k & 3;
    *cbp = ((k >> 2) & 3) << 4;          /* chroma */
    if (k >= 12) *cbp |= 15;             /* luma all or nothing */
}

int h264d_decode_mb_cabac(h264_decoder_t *d)
{
    h264d_mb_t *m = &d->mbs[d->mb_idx];
    azzera_mb(m);

    const bool bslice = d->slice.type == 1;
    const bool islice = d->slice.type == 2;

    if (!islice) {
        if (leggi_mb_skip(d, bslice)) {
            m->type = (uint8_t)(bslice ? H264D_MB_B_SKIP : H264D_MB_P_SKIP);
            m->qpy = (int8_t)d->qpy;
            d->last_qp_delta_nonzero = 0;
            h264d_reconstruct_mb(d);
            return 0;
        }
    }

    int t = islice ? leggi_mb_type_intra(d, CTX_MB_TYPE_I, true)
                   : (bslice ? leggi_mb_type_b(d) : leggi_mb_type_p(d));

    /* Resolve the slice-relative numbering into our own. */
    int i16_modo = -1;
    if (islice || (!bslice && t >= 5) || (bslice && t >= 23)) {
        const int ti = islice ? t : (bslice ? t - 23 : t - 5);
        if (ti == 25) {
            m->type = H264D_MB_I_PCM;
            m->intra = 1;
        } else if (ti == 0) {
            m->type = H264D_MB_I_NxN;
            m->intra = 1;
        } else {
            m->type = H264D_MB_I_16x16;
            m->intra = 1;
            int cbp;
            spacchetta_i16(ti, &i16_modo, &cbp);
            m->cbp = (uint8_t)cbp;
        }
    } else if (bslice) {
        static const uint8_t mappa_b[23] = {
            H264D_MB_B_DIRECT, H264D_MB_B_16x16, H264D_MB_B_16x16,
            H264D_MB_B_16x16,
            H264D_MB_B_16x8, H264D_MB_B_8x16, H264D_MB_B_16x8, H264D_MB_B_8x16,
            H264D_MB_B_16x8, H264D_MB_B_8x16, H264D_MB_B_16x8, H264D_MB_B_8x16,
            H264D_MB_B_16x8, H264D_MB_B_8x16, H264D_MB_B_16x8, H264D_MB_B_8x16,
            H264D_MB_B_16x8, H264D_MB_B_8x16, H264D_MB_B_16x8, H264D_MB_B_8x16,
            H264D_MB_B_16x8, H264D_MB_B_8x16, H264D_MB_B_8x8,
        };
        m->type = mappa_b[t < 23 ? t : 22];
    } else {
        static const uint8_t mappa_p[4] = {
            H264D_MB_P_16x16, H264D_MB_P_16x8, H264D_MB_P_8x16, H264D_MB_P_8x8
        };
        m->type = mappa_p[t < 4 ? t : 3];
    }

    if (m->type == H264D_MB_I_PCM) {
        /* Clause 7.3.5: the engine is re-initialised after the raw samples,
         * which the caller does because it owns the byte pointer. */
        m->qpy = (int8_t)d->qpy;
        memset(m->nnz, 16, sizeof(m->nnz));
        return 2;   /* not supported yet: tell the caller to drop the slice */
    }

    /* --- intra modes ------------------------------------------------- */
    if (m->intra) {
        if (m->type == H264D_MB_I_NxN) {
            if (d->pic.transform_8x8_mode)
                m->transform8x8 = (uint8_t)leggi_transform8x8(d);

            const int passo = m->transform8x8 ? 4 : 1;
            for (int k = 0; k < 16; k += passo) {
                const int b = zscan[k];
                /* 8.3.1.1: the predicted mode is the smaller of the two
                 * neighbours' modes, and a neighbour that is not Intra_NxN
                 * counts as DC unless constrained intra prediction says it
                 * is unavailable altogether. */
                int pa, pb;
                const h264d_mb_t *a = sinistra4(d, b, &pa);
                const h264d_mb_t *bm = sopra4(d, b, &pb);
                int ma = (a && a->type == H264D_MB_I_NxN) ? a->ipred[pa]
                       : (a ? 2 : -1);
                int mb2 = (bm && bm->type == H264D_MB_I_NxN) ? bm->ipred[pb]
                        : (bm ? 2 : -1);
                int previsto = (ma < 0 || mb2 < 0) ? 2
                             : (ma < mb2 ? ma : mb2);

                int modo;
                if (decidi(d, CTX_PREV_INTRA4X4_FLAG)) {
                    modo = previsto;
                } else {
                    int r = decidi(d, CTX_REM_INTRA4X4);
                    r |= decidi(d, CTX_REM_INTRA4X4) << 1;
                    r |= decidi(d, CTX_REM_INTRA4X4) << 2;
                    modo = r < previsto ? r : r + 1;
                }
                if (passo == 1) {
                    m->ipred[b] = (int8_t)modo;
                } else {
                    /* An Intra_8x8 mode covers all four of its 4x4 slots, so
                     * that the neighbour derivation above finds it whichever
                     * one it asks about. */
                    const int bx = b & 3, by = b >> 2;
                    for (int y = 0; y < 2; y++)
                        for (int x = 0; x < 2; x++)
                            m->ipred[(by + y) * 4 + bx + x] = (int8_t)modo;
                }
            }
        }
        m->chroma_pred_mode = (int8_t)leggi_chroma_pred_mode(d);
    } else {
        /* --- motion ---------------------------------------------------- */
        if (h264d_read_motion_cabac(d, m, bslice) != 0)
            return 1;
    }

    /* --- coded block pattern and the residual ------------------------ */
    const bool i16 = (m->type == H264D_MB_I_16x16);
    if (!i16) {
        m->cbp = (uint8_t)leggi_cbp(d);
        if ((m->cbp & 15) && d->pic.transform_8x8_mode && !m->intra
            && m->type != H264D_MB_P_8x8 + 100 /* placeholder, see below */)
            m->transform8x8 = (uint8_t)leggi_transform8x8(d);
    }

    if (m->cbp || i16) {
        d->qpy += leggi_qp_delta(d);
        d->qpy = ((d->qpy + 52) % 52 + 52) % 52;
    } else {
        d->last_qp_delta_nonzero = 0;
    }
    m->qpy = (int8_t)d->qpy;

    leggi_residuo_mb(d, m, i16);
    if (i16)
        m->ipred[0] = (int8_t)i16_modo;

    h264d_reconstruct_mb(d);
    return 0;
}
