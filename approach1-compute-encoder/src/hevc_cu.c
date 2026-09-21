/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_cu.c - the coding tree, Rec. ITU-T H.265 clauses 7.3.8.2 to 7.3.8.10.
 *
 * Where H.264 has a macroblock of a fixed sixteen samples, H.265 has a
 * coding tree unit of up to sixty-four that splits down a quadtree as far
 * as the picture needs, and then splits again - separately - for the
 * transform. So three trees are read here, nested: the coding quadtree,
 * the prediction units hanging off each of its leaves, and the transform
 * tree inside those.
 *
 * ⚠️ A syntax element that is not sent is not absent: it is inferred, and
 * the inferred value is usually not zero. split_transform_flag is one when
 * the block is bigger than the largest transform, cbf_cb is inherited from
 * the parent at the smallest chroma size, and an intra coding unit's
 * rqt_root_cbf is one without ever being coded. Reading those as zero
 * costs nothing at the time and desynchronises the slice later.
 */
#include "hevc_dec_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- neighbours */

/* Whether the block at these coordinates has already been decoded and
 * belongs to the same slice. 6.4.1, simplified: one slice, one tile. */
static bool disponibile(const hevcd_t *d, int x, int y)
{
    return x >= 0 && y >= 0 && x < d->sps->width && y < d->sps->height;
}

/* ------------------------------------------------------------------- SAO */

/* Clause 7.3.8.3. Read, not yet applied: the offsets it carries are a
 * filter over the finished picture, and nothing is finished yet. What
 * matters here is that every bin is consumed. */
static void leggi_sao(hevcd_t *d, int rx, int ry)
{
    hevcd_cabac_t *c = &d->cabac;
    const hevc_slice_t *sl = d->slice;

    const int passo_ctb = d->sps->ctb_width;
    hevcd_sao_t *mio = &d->sao[ry * passo_ctb + rx];
    memset(mio, 0, sizeof *mio);

    /* Merging is how the encoder says "the same as next door" in two
     * bins instead of thirty. */
    if (rx > 0 && hevcd_bin(c, HEVCD_CTX_SAO_MERGE_FLAG)) {
        *mio = d->sao[ry * passo_ctb + rx - 1];
        return;
    }
    if (ry > 0 && hevcd_bin(c, HEVCD_CTX_SAO_MERGE_FLAG)) {
        *mio = d->sao[(ry - 1) * passo_ctb + rx];
        return;
    }

    /* ⚠️ One type for luma and one for chroma, and the chroma one governs
     * both planes. The offsets are per plane all the same, and the edge
     * class is sent only with the plane that carried the type. */
    int tipo_croma = 0;
    for (int piano = 0; piano < 3; piano++) {
        if (piano == 0 && !sl->sao_luma) continue;
        if (piano > 0 && !sl->sao_chroma) continue;

        int tipo;
        if (piano == 2) {
            tipo = tipo_croma;
        } else {
            /* Truncated Rice, cMax 2: the first bin has a context, the
             * second does not. */
            tipo = 0;
            if (hevcd_bin(c, HEVCD_CTX_SAO_TYPE_IDX))
                tipo = hevcd_bypass(c) ? 2 : 1;
            if (piano == 1) tipo_croma = tipo;
        }
        if (tipo == 0) continue;

        int assoluti[4];
        for (int i = 0; i < 4; i++) {
            /* Truncated Rice, cMax 7 at eight bits, all in bypass. */
            int v = 0;
            while (v < 7 && hevcd_bypass(c)) v++;
            assoluti[i] = v;
        }
        mio->tipo[piano] = (uint8_t)tipo;
        if (tipo == 1) {
            for (int i = 0; i < 4; i++) {
                const int segno = (assoluti[i] && hevcd_bypass(c)) ? -1 : 1;
                mio->off[piano][i] = (int8_t)(segno * assoluti[i]);
            }
            mio->posizione[piano] = (uint8_t)hevcd_bypass_n(c, 5);
        } else {
            /* ⚠️ An edge offset carries no signs. The first two are
             * defined to be positive and the last two negative, because
             * the four cases they answer to are a valley, a step up, a
             * step down and a peak - and the filter only ever pushes a
             * sample back towards its neighbours. */
            for (int i = 0; i < 4; i++)
                mio->off[piano][i] = (int8_t)(i < 2 ? assoluti[i]
                                                    : -assoluti[i]);
            if (piano != 2) mio->classe[piano] = (uint8_t)hevcd_bypass_n(c, 2);
            else mio->classe[2] = mio->classe[1];
        }
    }
}

/* ------------------------------------------------- intra prediction modes */

/* Clause 8.4.2: the three most probable modes, from the units to the left
 * and above.
 *
 * ⚠️ The unit above counts only when it is inside this coding tree unit.
 * One row up and outside it, the mode is taken as DC however it was
 * actually coded - the standard will not let a prediction depend on a row
 * of the picture that a wavefront may not have finished. */
static void modi_probabili(const hevcd_t *d, int x, int y, int cand[3])
{
    const int passo = d->min_pu_width;
    int a, b;

    if (!disponibile(d, x - 1, y))
        a = HEVCD_INTRA_DC;
    else
        a = d->intra_mode[(y >> 2) * passo + ((x - 1) >> 2)];

    const int y_sopra = y - 1;
    if (!disponibile(d, x, y_sopra)
        || ((y_sopra >> d->sps->log2_ctb) != (y >> d->sps->log2_ctb)))
        b = HEVCD_INTRA_DC;
    else
        b = d->intra_mode[(y_sopra >> 2) * passo + (x >> 2)];

    if (a == b) {
        if (a < 2) {
            cand[0] = HEVCD_INTRA_PLANAR;
            cand[1] = HEVCD_INTRA_DC;
            cand[2] = HEVCD_INTRA_ANGULAR_26;
        } else {
            cand[0] = a;
            cand[1] = 2 + ((a + 29) % 32);
            cand[2] = 2 + ((a - 2 + 1) % 32);
        }
    } else {
        cand[0] = a;
        cand[1] = b;
        if (a != HEVCD_INTRA_PLANAR && b != HEVCD_INTRA_PLANAR)
            cand[2] = HEVCD_INTRA_PLANAR;
        else if (a != HEVCD_INTRA_DC && b != HEVCD_INTRA_DC)
            cand[2] = HEVCD_INTRA_DC;
        else
            cand[2] = HEVCD_INTRA_ANGULAR_26;
    }
}

static void scrivi_modo(hevcd_t *d, int x, int y, int lato, int modo)
{
    const int passo = d->min_pu_width;
    for (int j = 0; j < lato; j += 4)
        for (int i = 0; i < lato; i += 4) {
            const int px = (x + i) >> 2, py = (y + j) >> 2;
            if (px < d->min_pu_width && py < d->min_pu_height)
                d->intra_mode[py * passo + px] = (uint8_t)modo;
        }
}

/* Table 8-3: the chroma mode, from an index and the luma mode beside it. */
static int modo_croma(int idx, int luma)
{
    static const int base[4] = { HEVCD_INTRA_PLANAR, HEVCD_INTRA_ANGULAR_26,
                                 HEVCD_INTRA_ANGULAR_10, HEVCD_INTRA_DC };
    if (idx == 4) return luma;
    return (base[idx] == luma) ? 34 : base[idx];
}

/* ⚠️ Rows are decoded at the same time under wavefront parallelism, and
 * a block's bottom edge lands in a cell the row below is also writing to.
 * One byte, two writers, and a plain read-modify-write loses whichever
 * bit arrived first. */
#define METTI_BORDO(i, bit) \
    __atomic_fetch_or(&d->bordi[i], (uint8_t)(bit), __ATOMIC_RELAXED)

/* 8.7.2.2: this block's left and top edges are block boundaries, and the
 * deblocking filter is allowed to cross them.
 *
 * ⚠️ Only edges that fall on the eight-sample grid count. A transform
 * block boundary four samples in is a real boundary and the filter still
 * ignores it: filtering on a four grid would leave no unfiltered sample
 * anywhere, since the filter reaches four samples each way.
 */
static void segna_bordi(hevcd_t *d, int x0, int y0, int log2_size,
                        bool trasformata)
{
    if (!d->bordi) return;
    const int lato = 1 << log2_size;
    const int passo = d->bordi_passo;
    /* Bits two and three say the same edge is also a transform block's,
     * which is a different question from whether it may be filtered: a
     * coded residual on either side of a transform edge is worth
     * smoothing, the same residual in the middle of one is not. */
    const int v = trasformata ? 1 | 4 : 1;
    const int o = trasformata ? 2 | 8 : 2;

    if ((x0 & 7) == 0)
        for (int j = 0; j < lato; j += 8)
            METTI_BORDO(((y0 + j) >> 3) * passo + (x0 >> 3), v);
    if ((y0 & 7) == 0)
        for (int i = 0; i < lato; i += 8)
            METTI_BORDO((y0 >> 3) * passo + ((x0 + i) >> 3), o);

    /* ⚠️ And the far side too. A block's right edge is its neighbour's
     * left one and the neighbour marks it - unless the neighbour has no
     * transform blocks of its own, which a skipped coding unit does not.
     * Marking only the near edges loses every transform boundary that
     * happens to have a skipped unit on the other side of it. */
    if (!trasformata) return;
    const int xf = x0 + lato, yf = y0 + lato;
    if ((xf & 7) == 0 && xf < d->sps->width)
        for (int j = 0; j < lato; j += 8)
            METTI_BORDO(((y0 + j) >> 3) * passo + (xf >> 3), v);
    if ((yf & 7) == 0 && yf < d->sps->height)
        for (int i = 0; i < lato; i += 8)
            METTI_BORDO((yf >> 3) * passo + ((x0 + i) >> 3), o);
}

/* The same, for a rectangle: a prediction unit's own edges. 8.7.2.2
 * filters transform block edges and prediction block edges alike, and an
 * asymmetric partition puts one where no transform ever will.
 *
 * ⚠️ Not a transform boundary, so a coded residual either side of it does
 * not raise the strength. Only the motion does. */
static void segna_bordi_rett(hevcd_t *d, int x0, int y0, int w, int h)
{
    if (!d->bordi) return;
    const int passo = d->bordi_passo;
    if ((x0 & 7) == 0)
        for (int j = 0; j < h; j += 8)
            METTI_BORDO(((y0 + j) >> 3) * passo + (x0 >> 3), 1);
    if ((y0 & 7) == 0)
        for (int i = 0; i < w; i += 8)
            METTI_BORDO((y0 >> 3) * passo + ((x0 + i) >> 3), 2);
}

/* Which smallest transform blocks carry a luma residual. */
static void segna_cbf(hevcd_t *d, int x0, int y0, int log2_size)
{
    if (!d->cbf_map) return;
    const int lato = 1 << log2_size;
    for (int j = 0; j < lato; j += 4)
        for (int i = 0; i < lato; i += 4) {
            const int px = (x0 + i) >> 2, py = (y0 + j) >> 2;
            if (px < d->min_pu_width && py < d->min_pu_height)
                d->cbf_map[py * d->min_pu_width + px] = 1;
        }
}


/* ------------------------------------------------ inter prediction units */

/* Where the prediction units of each partition mode sit, Table 7-10 and
 * figure 7-2. Written as a table of quarters so the asymmetric modes are
 * not four more special cases. */
int hevcd_quante_pu(int part_mode)
{
    if (part_mode == HEVCD_PART_2Nx2N) return 1;
    if (part_mode == HEVCD_PART_NxN) return 4;
    return 2;
}

void hevcd_rettangolo_pu(int part_mode, int k, int lato,
                         int *x, int *y, int *w, int *h)
{
    const int mezzo = lato / 2, quarto = lato / 4;

    switch (part_mode) {
    case HEVCD_PART_2NxN:
        *x = 0; *y = k * mezzo; *w = lato; *h = mezzo; return;
    case HEVCD_PART_Nx2N:
        *x = k * mezzo; *y = 0; *w = mezzo; *h = lato; return;
    case HEVCD_PART_NxN:
        *x = (k & 1) * mezzo; *y = (k >> 1) * mezzo;
        *w = mezzo; *h = mezzo; return;
    case HEVCD_PART_2NxnU:
        *x = 0; *y = k ? quarto : 0; *w = lato;
        *h = k ? lato - quarto : quarto; return;
    case HEVCD_PART_2NxnD:
        *x = 0; *y = k ? lato - quarto : 0; *w = lato;
        *h = k ? quarto : lato - quarto; return;
    case HEVCD_PART_nLx2N:
        *x = k ? quarto : 0; *y = 0;
        *w = k ? lato - quarto : quarto; *h = lato; return;
    case HEVCD_PART_nRx2N:
        *x = k ? lato - quarto : 0; *y = 0;
        *w = k ? quarto : lato - quarto; *h = lato; return;
    default:
        *x = 0; *y = 0; *w = lato; *h = lato; return;
    }
}

/* What this prediction unit's motion was, written over every smallest
 * block it covers. The neighbours that come after read it there. */
static void scrivi_campo(hevcd_t *d, int x0, int y0, int w, int h,
                         const hevcd_mvf_t *m)
{
    if (!d->mvf) return;
    for (int j = 0; j < h; j += 4)
        for (int i = 0; i < w; i += 4) {
            const int px = (x0 + i) >> 2, py = (y0 + j) >> 2;
            if (px < d->min_pu_width && py < d->min_pu_height)
                d->mvf[py * d->min_pu_width + px] = *m;
        }
}

/* An intra coding unit has no motion, and saying so is not the same as
 * leaving whatever the last picture put there: the neighbours ask. */
static void campo_intra(hevcd_t *d, int x0, int y0, int lato)
{
    hevcd_mvf_t vuoto;
    memset(&vuoto, 0, sizeof vuoto);
    vuoto.ref_idx[0] = vuoto.ref_idx[1] = -1;
    scrivi_campo(d, x0, y0, lato, lato, &vuoto);
}

/* 9.3.3.3: exp-Golomb of order k, entirely in bypass. */
static unsigned golomb(hevcd_cabac_t *c, int k)
{
    int q = 0;
    while (q < 24 && hevcd_bypass(c)) q++;
    unsigned v = q ? (((1u << q) - 1) << k) : 0;
    const int quanti = q + k;
    if (quanti) v += hevcd_bypass_n(c, quanti);
    return v;
}

/* 9.3.3.7. Four different binarisations wearing one name: which one
 * applies depends on the block size and on whether the sequence allows
 * asymmetric partitions.
 *
 * ⚠️ The context of the third bin is not the same in the two cases. At
 * the smallest coding block it is index two and it is deciding between
 * Nx2N and NxN; above it, index three, deciding between a symmetric
 * partition and an asymmetric one. */
static int leggi_part_mode(hevcd_t *d, int log2_size)
{
    hevcd_cabac_t *c = &d->cabac;
    const hevc_sps_t *sps = d->sps;

    if (hevcd_bin(c, HEVCD_CTX_PART_MODE + 0)) return HEVCD_PART_2Nx2N;
    const bool orizzontale = hevcd_bin(c, HEVCD_CTX_PART_MODE + 1) != 0;

    if (log2_size > sps->log2_min_cb) {
        if (!sps->amp_enabled
            || hevcd_bin(c, HEVCD_CTX_PART_MODE + 3))
            return orizzontale ? HEVCD_PART_2NxN : HEVCD_PART_Nx2N;
        const bool seconda = hevcd_bypass(c) != 0;
        if (orizzontale)
            return seconda ? HEVCD_PART_2NxnD : HEVCD_PART_2NxnU;
        return seconda ? HEVCD_PART_nRx2N : HEVCD_PART_nLx2N;
    }

    if (orizzontale) return HEVCD_PART_2NxN;
    /* ⚠️ An inter 4x4 does not exist, so at an eight-sample coding block
     * the vertical branch has nowhere left to go and the bin is not
     * sent. */
    if (log2_size == 3) return HEVCD_PART_Nx2N;
    return hevcd_bin(c, HEVCD_CTX_PART_MODE + 2) ? HEVCD_PART_Nx2N
                                                 : HEVCD_PART_NxN;
}

/* Truncated Rice with the first two bins in context and the rest in
 * bypass, for both lists. */
static int leggi_ref_idx(hevcd_t *d, int quante)
{
    hevcd_cabac_t *c = &d->cabac;
    const int massimo = quante - 1;
    const int con_contesto = massimo < 2 ? massimo : 2;

    int i = 0;
    while (i < con_contesto && hevcd_bin(c, HEVCD_CTX_REF_IDX_L0 + i)) i++;
    if (i == 2)
        while (i < massimo && hevcd_bypass(c)) i++;
    return i;
}

static int leggi_merge_idx(hevcd_t *d, int quanti)
{
    hevcd_cabac_t *c = &d->cabac;
    if (quanti <= 1) return 0;
    if (!hevcd_bin(c, HEVCD_CTX_MERGE_IDX)) return 0;
    int i = 1;
    while (i < quanti - 1 && hevcd_bypass(c)) i++;
    return i;
}

/* 7.3.8.9. The two components are interleaved rather than sent one after
 * the other, which lets the two greater-than-zero flags share a context
 * without either of them waiting for the other's remainder. */
static void leggi_mvd(hevcd_t *d, int16_t mvd[2])
{
    hevcd_cabac_t *c = &d->cabac;
    bool sopra_zero[2], sopra_uno[2] = { false, false };

    for (int i = 0; i < 2; i++)
        sopra_zero[i] = hevcd_bin(c, HEVCD_CTX_ABS_MVD_GREATER0_FLAG) != 0;
    for (int i = 0; i < 2; i++)
        if (sopra_zero[i])
            sopra_uno[i] = hevcd_bin(c, HEVCD_CTX_ABS_MVD_GREATER1_FLAG + 1) != 0;

    for (int i = 0; i < 2; i++) {
        mvd[i] = 0;
        if (!sopra_zero[i]) continue;
        int v = 1;
        if (sopra_uno[i]) v = 2 + (int)golomb(c, 1);
        mvd[i] = (int16_t)(hevcd_bypass(c) ? -v : v);
    }
}

/* 7.3.8.6. Everything a prediction unit says about where it copies from:
 * which lists, which pictures in them, and how far off the prediction
 * the true motion was. */
static bool leggi_pu(hevcd_t *d, int x0, int y0, int w, int h,
                     int part_idx, bool salta)
{
    hevcd_cabac_t *c = &d->cabac;
    const hevc_slice_t *sl = d->slice;
    const int quanti_merge = 5 - sl->five_minus_max_num_merge_cand;
    hevcd_mvf_t m;

    memset(&m, 0, sizeof m);
    m.ref_idx[0] = m.ref_idx[1] = -1;

    if (salta || hevcd_bin(c, HEVCD_CTX_MERGE_FLAG)) {
        const int idx = leggi_merge_idx(d, quanti_merge);
        hevcd_merge(d, x0, y0, w, h, part_idx, idx, &m);
        /* ⚠️ A merged unit copies its neighbour's index, and the
         * neighbour's index was resolved against the same lists, so it
         * still means the same picture. Its recorded picture may not be:
         * a temporal candidate has no index of its own. */
        for (int l = 0; l < 2; l++)
            if ((m.pred_flag & (1 << l)) && m.ref_idx[l] >= 0
                && m.ref_idx[l] < d->n_rif[l])
                m.ref_poc[l] = d->rif[l][m.ref_idx[l]]->poc;
        scrivi_campo(d, x0, y0, w, h, &m);
        hevcd_predici_inter(d, x0, y0, w, h, &m);
        return true;
    }

    /* Which lists this unit predicts from. In a P slice there is only
     * one and nothing is sent. */
    int liste = 1;                          /* bit 0 list 0, bit 1 list 1 */
    if (sl->type == 0) {
        /* ⚠️ An 8x4 or a 4x8 may not be bi-predicted: two of those in a
         * row would fetch more samples than the level allows, so the
         * first bin is not even sent for them. */
        if (w + h != 12
            && hevcd_bin(c, HEVCD_CTX_INTER_PRED_IDC + d->cu.depth))
            liste = 3;
        else
            liste = hevcd_bin(c, HEVCD_CTX_INTER_PRED_IDC + 4) ? 2 : 1;
    }

    m.pred_flag = (uint8_t)liste;
    for (int l = 0; l < 2; l++) {
        if (!(liste & (1 << l))) continue;
        m.ref_idx[l] = (int8_t)(sl->num_ref_idx[l] > 1
                                ? leggi_ref_idx(d, sl->num_ref_idx[l]) : 0);
        if (m.ref_idx[l] < d->n_rif[l])
            m.ref_poc[l] = d->rif[l][m.ref_idx[l]]->poc;
        int16_t mvd[2] = { 0, 0 };
        if (l == 1 && sl->mvd_l1_zero && liste == 3) {
            /* Sent as nothing at all: the slice header promised it. */
        } else {
            leggi_mvd(d, mvd);
        }
        const int mvp = hevcd_bin(c, HEVCD_CTX_MVP_LX_FLAG);
        hevcd_amvp(d, x0, y0, w, h, l, mvp, &m);
        /* ⚠️ The difference is added after the predictor is derived and
         * wraps at sixteen bits: the standard says the sum is taken
         * modulo 2^16, so a vector near the limit comes back round rather
         * than being clipped. */
        m.mv[l][0] = (int16_t)(m.mv[l][0] + mvd[0]);
        m.mv[l][1] = (int16_t)(m.mv[l][1] + mvd[1]);
    }
    scrivi_campo(d, x0, y0, w, h, &m);
    hevcd_predici_inter(d, x0, y0, w, h, &m);
    return false;
}

/* cu_skip_flag's context asks how many of the two neighbours were skipped
 * themselves, which is the cheapest possible guess at whether this part
 * of the picture is standing still. */
static int contesto_salto(const hevcd_t *d, int x0, int y0)
{
    const hevc_sps_t *sps = d->sps;
    const int passo = sps->min_cb_width;
    const int l = sps->log2_min_cb;
    int n = 0;

    if (x0 > 0 && d->skip[(y0 >> l) * passo + ((x0 - 1) >> l)]) n++;
    if (y0 > 0 && d->skip[((y0 - 1) >> l) * passo + (x0 >> l)]) n++;
    return n;
}

static void segna_salto(hevcd_t *d, int x0, int y0, int log2_size, bool salta)
{
    const hevc_sps_t *sps = d->sps;
    const int passo = sps->min_cb_width;
    const int n = 1 << (log2_size - sps->log2_min_cb);

    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            const int px = (x0 >> sps->log2_min_cb) + i;
            const int py = (y0 >> sps->log2_min_cb) + j;
            if (px < sps->min_cb_width && py < sps->min_cb_height)
                d->skip[py * passo + px] = salta ? 1 : 0;
        }
}

/* ------------------------------------------------------- transform units */

static void leggi_qp_delta(hevcd_t *d)
{
    hevcd_cabac_t *c = &d->cabac;
    if (!d->pps->cu_qp_delta_enabled || d->cu_qp_delta_coded) return;
    d->cu_qp_delta_coded = true;

    /* 9.3.3.10: a truncated Rice prefix of at most five, then an
     * exp-Golomb suffix in bypass. */
    int prefisso = 0;
    while (prefisso < 5
           && hevcd_bin(c, HEVCD_CTX_CU_QP_DELTA + (prefisso ? 1 : 0)))
        prefisso++;

    int valore = prefisso;
    if (prefisso == 5) {
        int k = 0;
        while (k < 32 && hevcd_bypass(c)) k++;
        valore = 5 + (int)((1u << k) - 1) + (int)(k ? hevcd_bypass_n(c, k) : 0);
    }
    if (valore && hevcd_bypass(c))
        valore = -valore;
    d->cu_qp_delta = valore;
    d->qp_y = ((d->qp_y_pred + valore + 52) % 52 + 52) % 52;
}

/* 8.6.1: the luma quantisation parameter's prediction, worked out once
 * per quantisation group.
 *
 * The prediction is the average of the group to the left and the group
 * above, and neither of them counts unless it sits inside the same coding
 * tree block. A neighbour one block over has certainly been decoded, and
 * the standard refuses it anyway, so that a decoder never has to keep a
 * whole row of parameters alive to decode the next one. What is left when
 * both are refused is qPY_PREV: the parameter of the last coding unit
 * decoded before this group.
 *
 * ⚠️ This runs for every group and not only for the ones that carry a
 * delta. A group with no delta still takes the prediction as its
 * parameter, so a decoder that simply lets QpY stand is right exactly as
 * long as the previous group happened to be the left neighbour - which at
 * the start of a row it is not.
 */
static void inizia_qg(hevcd_t *d, int x0, int y0)
{
    const int log2_ctb = d->sps->log2_ctb;
    const int log2_cb = d->sps->log2_min_cb;
    const int passo = d->sps->min_cb_width;

    d->qg_x = x0;
    d->qg_y = y0;

    /* ⚠️ Not cleared here. This runs once per quadtree node down to the
     * group's own size, and only the last of those calls is the group's;
     * clearing on the first one hands the group the previous group's
     * parameter at the exact point where the standard says not to. It is
     * cleared once a coding unit has been read. */
    const int prima = d->qg_riparte ? d->slice->qp : d->qp_y_prev;

    int a = prima, b = prima;
    if (x0 > 0 && ((x0 - 1) >> log2_ctb) == (x0 >> log2_ctb))
        a = d->qp_y_map[(y0 >> log2_cb) * passo + ((x0 - 1) >> log2_cb)];
    if (y0 > 0 && ((y0 - 1) >> log2_ctb) == (y0 >> log2_ctb))
        b = d->qp_y_map[((y0 - 1) >> log2_cb) * passo + (x0 >> log2_cb)];

    d->qp_y_pred = (a + b + 1) >> 1;
    d->qp_y = d->qp_y_pred;
}

/* Table 8-10: the chroma quantisation parameter, which is not the luma one
 * even before the offsets. Above 29 it stops following, because chroma
 * tolerates coarser quantisation than luma does and the standard says so
 * in a table rather than a formula. */
static int qp_croma(int qp_i)
{
    if (qp_i < 30) return qp_i < 0 ? 0 : qp_i;
    if (qp_i > 43) return qp_i - 6;
    return hevcd_qp_c[qp_i - 30];
}

/* 8.6.1, for the coding unit being reconstructed. */
static int qp_del_blocco(const hevcd_t *d, int c_idx)
{
    if (c_idx == 0) return d->qp_y;
    const int off = (c_idx == 1)
        ? d->pps->cb_qp_offset + d->slice->cb_qp_offset
        : d->pps->cr_qp_offset + d->slice->cr_qp_offset;
    int q = d->qp_y + off;
    if (q < 0) q = 0;
    if (q > 57) q = 57;
    return qp_croma(q);
}

/* One transform block: predict it, then add whatever residual it has.
 *
 * ⚠️ In that order and one block at a time. The next block predicts from
 * this one's reconstructed samples, so a version that read all the
 * residuals first and reconstructed afterwards would predict from
 * whatever was in the picture before. */
static void ricostruisci_tb(hevcd_t *d, int c_idx, int x, int y,
                            int log2_size, bool ha_residuo)
{
    /* The luma mode is per prediction block; chroma has one per coding
     * unit. For a chroma block the coordinates are halved, so the mode is
     * looked up at the luma position it covers. */
    const int lx = c_idx ? x * 2 : x, ly = c_idx ? y * 2 : y;
    const int modo = (c_idx == 0)
        ? d->intra_mode[(ly >> 2) * d->min_pu_width + (lx >> 2)]
        : d->cu.intra_mode_c;

    /* ⚠️ Only an intra block predicts from the samples beside it. An
     * inter one predicts from another picture, which is not written yet:
     * until it is, the residual lands on whatever is there. */
    if (d->cu.pred_mode == HEVCD_MODE_INTRA)
        hevcd_predici_intra(d, c_idx, x, y, log2_size, modo);
    if (!ha_residuo) return;

    /* 8.6.2: with the bypass the coefficients are the residual already.
     * Everything below this point - the scaling, the two transform stages,
     * the rounding - exists to undo a quantisation that never happened. */
    if (d->cu.transquant_bypass) {
        hevcd_aggiungi(d->piano[c_idx] + (size_t)y * d->passo[c_idx] + x,
                       d->passo[c_idx], d->coeff, log2_size);
        return;
    }

    hevcd_dequantizza(d->coeff, log2_size, qp_del_blocco(d, c_idx));
    if (d->transform_skip)
        hevcd_salta_trasformata(d->coeff, log2_size);
    else
        hevcd_trasforma(d->coeff, log2_size,
                        c_idx == 0 && log2_size == 2
                        && d->cu.pred_mode == HEVCD_MODE_INTRA);
    hevcd_aggiungi(d->piano[c_idx] + (size_t)y * d->passo[c_idx] + x,
                   d->passo[c_idx], d->coeff, log2_size);
}

static void leggi_tu(hevcd_t *d, int x0, int y0, int x_base, int y_base,
                     int log2_size, int depth, int blk,
                     bool cbf_luma, bool cbf_cb, bool cbf_cr)
{
    if (cbf_luma || cbf_cb || cbf_cr)
        leggi_qp_delta(d);

    if (cbf_luma)
        hevcd_leggi_residuo(d, x0, y0, log2_size, 0);
    ricostruisci_tb(d, 0, x0, y0, log2_size, cbf_luma);

    if (log2_size > 2) {
        const int cx = x0 >> 1, cy = y0 >> 1;
        if (cbf_cb) hevcd_leggi_residuo(d, x0, y0, log2_size - 1, 1);
        ricostruisci_tb(d, 1, cx, cy, log2_size - 1, cbf_cb);
        if (cbf_cr) hevcd_leggi_residuo(d, x0, y0, log2_size - 1, 2);
        ricostruisci_tb(d, 2, cx, cy, log2_size - 1, cbf_cr);
    } else if (blk == 3) {
        /* ⚠️ At the smallest luma size the four 4x4 blocks share one 4x4
         * chroma block, which is read with the last of them and lives at
         * the parent's corner. */
        const int cx = x_base >> 1, cy = y_base >> 1;
        if (cbf_cb) hevcd_leggi_residuo(d, x_base, y_base, 2, 1);
        ricostruisci_tb(d, 1, cx, cy, 2, cbf_cb);
        if (cbf_cr) hevcd_leggi_residuo(d, x_base, y_base, 2, 2);
        ricostruisci_tb(d, 2, cx, cy, 2, cbf_cr);
    }
    (void)depth;
}

static void leggi_albero_trasformate(hevcd_t *d, int x0, int y0,
                                     int x_base, int y_base, int log2_size,
                                     int depth, int blk,
                                     bool cbf_cb_padre, bool cbf_cr_padre)
{
    hevcd_cabac_t *c = &d->cabac;
    const hevc_sps_t *sps = d->sps;

    const int max_depth = d->cu.pred_mode == HEVCD_MODE_INTRA
        ? sps->max_transform_hierarchy_depth_intra + (d->cu.intra_split ? 1 : 0)
        : sps->max_transform_hierarchy_depth_inter;

    bool dividi;
    if (log2_size <= sps->log2_max_tb && log2_size > sps->log2_min_tb
        && depth < max_depth && !(d->cu.intra_split && depth == 0)) {
        dividi = hevcd_bin(c, HEVCD_CTX_SPLIT_TRANSFORM_FLAG + 5 - log2_size) != 0;
    } else {
        /* Inferred, and rarely zero: too big for the largest transform, an
         * intra unit split into four prediction blocks, or an inter unit
         * whose partitions the transform is not allowed to straddle.
         *
         * ⚠️ That last one only bites when the sequence allows no
         * transform depth at all for inter: with a depth to spare the
         * flag is read instead, and the condition above already says so. */
        const bool inter_diviso = sps->max_transform_hierarchy_depth_inter == 0
            && d->cu.pred_mode == HEVCD_MODE_INTER
            && d->cu.part_mode != HEVCD_PART_2Nx2N && depth == 0;
        dividi = (log2_size > sps->log2_max_tb)
              || (d->cu.intra_split && depth == 0) || inter_diviso;
    }

    bool cbf_cb = false, cbf_cr = false;
    if (log2_size > 2) {
        if (depth == 0 || cbf_cb_padre)
            cbf_cb = hevcd_bin(c, HEVCD_CTX_CBF_CB_CR + depth) != 0;
        if (depth == 0 || cbf_cr_padre)
            cbf_cr = hevcd_bin(c, HEVCD_CTX_CBF_CB_CR + depth) != 0;
    } else {
        /* At 4x4 luma there is no chroma of its own: it belongs to the
         * parent and is inherited unread. */
        cbf_cb = cbf_cb_padre;
        cbf_cr = cbf_cr_padre;
    }

    if (dividi) {
        const int mezzo = 1 << (log2_size - 1);
        leggi_albero_trasformate(d, x0, y0, x0, y0, log2_size - 1,
                                 depth + 1, 0, cbf_cb, cbf_cr);
        leggi_albero_trasformate(d, x0 + mezzo, y0, x0, y0, log2_size - 1,
                                 depth + 1, 1, cbf_cb, cbf_cr);
        leggi_albero_trasformate(d, x0, y0 + mezzo, x0, y0, log2_size - 1,
                                 depth + 1, 2, cbf_cb, cbf_cr);
        leggi_albero_trasformate(d, x0 + mezzo, y0 + mezzo, x0, y0,
                                 log2_size - 1, depth + 1, 3, cbf_cb, cbf_cr);
        return;
    }

    bool cbf_luma = true;
    if (d->cu.pred_mode == HEVCD_MODE_INTRA || depth != 0 || cbf_cb || cbf_cr)
        cbf_luma = hevcd_bin(c, HEVCD_CTX_CBF_LUMA + (depth == 0 ? 1 : 0)) != 0;

    segna_bordi(d, x0, y0, log2_size, true);
    if (cbf_luma) segna_cbf(d, x0, y0, log2_size);
    leggi_tu(d, x0, y0, x_base, y_base, log2_size, depth, blk,
             cbf_luma, cbf_cb, cbf_cr);
}

/* ---------------------------------------------------------- coding units */

static void leggi_cu(hevcd_t *d, int x0, int y0, int log2_size)
{
    hevcd_cabac_t *c = &d->cabac;
    const hevc_sps_t *sps = d->sps;
    const int lato = 1 << log2_size;

    d->cu.x = x0;
    d->cu.y = y0;
    d->cu.log2_size = log2_size;
    d->cu.pred_mode = HEVCD_MODE_INTRA;
    d->cu.part_mode = HEVCD_PART_2Nx2N;
    d->cu.transquant_bypass = false;
    d->cu.intra_split = false;

    if (d->pps->transquant_bypass_enabled)
        d->cu.transquant_bypass = hevcd_bin(c, HEVCD_CTX_CU_TRANSQUANT_BYPASS_FLAG) != 0;

    /* An I slice has neither cu_skip_flag nor pred_mode_flag: everything
     * in it is intra by definition. */
    d->cu.skip = false;
    if (d->slice->type != 2) {
        d->cu.skip = hevcd_bin(c, HEVCD_CTX_SKIP_FLAG
                               + contesto_salto(d, x0, y0)) != 0;
        segna_salto(d, x0, y0, log2_size, d->cu.skip);
        if (d->cu.skip) {
            /* A skipped unit is one merge index and nothing else: no
             * partition, no residual, not even a prediction mode. */
            d->cu.pred_mode = HEVCD_MODE_INTER;
            leggi_pu(d, x0, y0, lato, lato, 0, true);
            return;
        }
        if (!hevcd_bin(c, HEVCD_CTX_PRED_MODE_FLAG))
            d->cu.pred_mode = HEVCD_MODE_INTER;
    } else {
        segna_salto(d, x0, y0, log2_size, false);
    }
    campo_intra(d, x0, y0, lato);

    if (d->cu.pred_mode == HEVCD_MODE_INTER) {
        d->cu.part_mode = leggi_part_mode(d, log2_size);

        bool unito = false;
        const int quante = hevcd_quante_pu(d->cu.part_mode);
        for (int k = 0; k < quante; k++) {
            int px, py, pw, ph;
            hevcd_rettangolo_pu(d->cu.part_mode, k, lato, &px, &py, &pw, &ph);
            segna_bordi_rett(d, x0 + px, y0 + py, pw, ph);
            unito = leggi_pu(d, x0 + px, y0 + py, pw, ph, k, false);
        }

        /* 7.3.8.5: a whole-block merge says nothing about its residual,
         * because a skip would have been sent instead if there were
         * none. Every other inter unit says whether it has one. */
        bool ha_residuo = true;
        if (!(d->cu.part_mode == HEVCD_PART_2Nx2N && unito))
            ha_residuo = hevcd_bin(c, HEVCD_CTX_NO_RESIDUAL_DATA_FLAG) != 0;
        if (ha_residuo)
            leggi_albero_trasformate(d, x0, y0, x0, y0, log2_size, 0, 0,
                                     false, false);
        return;
    }

    if (log2_size == sps->log2_min_cb) {
        if (!hevcd_bin(c, HEVCD_CTX_PART_MODE)) {
            d->cu.part_mode = HEVCD_PART_NxN;
            d->cu.intra_split = true;
        }
    }

    if (sps->pcm_enabled && d->cu.part_mode == HEVCD_PART_2Nx2N
        && log2_size >= sps->log2_min_pcm_cb
        && log2_size <= sps->log2_max_pcm_cb) {
        /* pcm_flag is coded as a terminating bin. Refused rather than
         * decoded: the samples that follow are raw and byte aligned, and
         * nothing here restarts the arithmetic decoder afterwards. */
        if (hevcd_terminate(c)) {
            d->fine_slice = true;
            return;
        }
    }

    const int pb = (d->cu.part_mode == HEVCD_PART_NxN) ? lato / 2 : lato;
    const int quanti = (d->cu.part_mode == HEVCD_PART_NxN) ? 2 : 1;

    bool dal_probabile[2][2];
    for (int j = 0; j < quanti; j++)
        for (int i = 0; i < quanti; i++)
            dal_probabile[j][i] =
                hevcd_bin(c, HEVCD_CTX_PREV_INTRA_LUMA_PRED_FLAG) != 0;

    int modo_luma_0 = HEVCD_INTRA_DC;
    for (int j = 0; j < quanti; j++) {
        for (int i = 0; i < quanti; i++) {
            const int x = x0 + i * pb, y = y0 + j * pb;
            int cand[3];
            modi_probabili(d, x, y, cand);

            int modo;
            if (dal_probabile[j][i]) {
                /* mpm_idx: truncated Rice, cMax 2, all bypass. */
                int idx = 0;
                if (hevcd_bypass(c)) idx = hevcd_bypass(c) ? 2 : 1;
                modo = cand[idx];
            } else {
                /* The three candidates are taken out of the range, so what
                 * is sent is five bits into what is left. */
                if (cand[0] > cand[1]) { const int t = cand[0]; cand[0] = cand[1]; cand[1] = t; }
                if (cand[0] > cand[2]) { const int t = cand[0]; cand[0] = cand[2]; cand[2] = t; }
                if (cand[1] > cand[2]) { const int t = cand[1]; cand[1] = cand[2]; cand[2] = t; }
                modo = (int)hevcd_bypass_n(c, 5);
                for (int k = 0; k < 3; k++)
                    if (modo >= cand[k]) modo++;
            }
            scrivi_modo(d, x, y, pb, modo);
            if (i == 0 && j == 0) modo_luma_0 = modo;
        }
    }

    /* intra_chroma_pred_mode: one context-coded bin, then two in bypass
     * when it is set. At 4:2:0 there is one for the whole coding unit. */
    int idx_c = 4;
    if (hevcd_bin(c, HEVCD_CTX_INTRA_CHROMA_PRED_MODE))
        idx_c = (int)hevcd_bypass_n(c, 2);
    d->cu.intra_mode_c = modo_croma(idx_c, modo_luma_0);

    campo_intra(d, x0, y0, lato);

    /* An intra coding unit's rqt_root_cbf is not sent: it is one. */
    leggi_albero_trasformate(d, x0, y0, x0, y0, log2_size, 0, 0, false, false);
}

/* ------------------------------------------------------ the coding quadtree */

static void leggi_quadtree(hevcd_t *d, int x0, int y0, int log2_size, int depth)
{
    hevcd_cabac_t *c = &d->cabac;
    const hevc_sps_t *sps = d->sps;
    const int lato = 1 << log2_size;

    bool dividi;
    if (x0 + lato <= sps->width && y0 + lato <= sps->height
        && log2_size > sps->log2_min_cb) {
        /* 9.3.4.2.2: how deep the neighbours were split. */
        int ctx = 0;
        const int passo = sps->min_cb_width;
        if (disponibile(d, x0 - 1, y0)
            && d->ct_depth[(y0 >> sps->log2_min_cb) * passo
                           + ((x0 - 1) >> sps->log2_min_cb)] > depth)
            ctx++;
        if (disponibile(d, x0, y0 - 1)
            && d->ct_depth[((y0 - 1) >> sps->log2_min_cb) * passo
                           + (x0 >> sps->log2_min_cb)] > depth)
            ctx++;
        dividi = hevcd_bin(c, HEVCD_CTX_SPLIT_CODING_UNIT_FLAG + ctx) != 0;
    } else {
        /* A block that runs off the picture has to be split until it does
         * not, and one already at the smallest size cannot be. */
        dividi = log2_size > sps->log2_min_cb;
    }

    if (d->pps->cu_qp_delta_enabled
        && log2_size >= sps->log2_ctb - d->pps->diff_cu_qp_delta_depth) {
        d->cu_qp_delta_coded = false;
        d->cu_qp_delta = 0;
        inizia_qg(d, x0, y0);
    }

    if (dividi) {
        const int mezzo = lato >> 1;
        leggi_quadtree(d, x0, y0, log2_size - 1, depth + 1);
        if (x0 + mezzo < sps->width)
            leggi_quadtree(d, x0 + mezzo, y0, log2_size - 1, depth + 1);
        if (y0 + mezzo < sps->height)
            leggi_quadtree(d, x0, y0 + mezzo, log2_size - 1, depth + 1);
        if (x0 + mezzo < sps->width && y0 + mezzo < sps->height)
            leggi_quadtree(d, x0 + mezzo, y0 + mezzo, log2_size - 1, depth + 1);
        return;
    }

    /* Remember how deep this went, for the neighbours that come after. */
    const int passo = sps->min_cb_width;
    const int n = lato >> sps->log2_min_cb;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            const int px = (x0 >> sps->log2_min_cb) + i;
            const int py = (y0 >> sps->log2_min_cb) + j;
            if (px < sps->min_cb_width && py < sps->min_cb_height)
                d->ct_depth[py * passo + px] = (uint8_t)depth;
        }

    segna_bordi(d, x0, y0, log2_size, false);
    d->cu.depth = depth;
    leggi_cu(d, x0, y0, log2_size);

    if (d->no_filtro && d->cu.transquant_bypass)
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++) {
                const int px = (x0 >> sps->log2_min_cb) + i;
                const int py = (y0 >> sps->log2_min_cb) + j;
                if (px < sps->min_cb_width && py < sps->min_cb_height)
                    d->no_filtro[py * passo + px] = 1;
            }

    /* And what parameter it ended up with, for the groups that come after
     * and, later, for the deblocking filter. */
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            const int px = (x0 >> sps->log2_min_cb) + i;
            const int py = (y0 >> sps->log2_min_cb) + j;
            if (px < sps->min_cb_width && py < sps->min_cb_height)
                d->qp_y_map[py * passo + px] = (int8_t)d->qp_y;
        }
    d->qp_y_prev = d->qp_y;
    d->qg_riparte = false;
}


/* 6.5.2: the z-scan address of every smallest transform block, so that
 * "has this neighbour been decoded yet" can be answered by comparing two
 * numbers. With one tile and one slice the coding tree units are in raster
 * order, and inside each of them the address is the interleaving of the
 * bits of x and y - which is what a quadtree walk is. */
int hevcd_prepara_zscan(hevcd_t *d)
{
    const hevc_sps_t *sps = d->sps;
    const int w = sps->width >> sps->log2_min_tb;
    const int h = sps->height >> sps->log2_min_tb;
    const size_t serve = (size_t)w * h;

    if (!d->min_tb_addr_zs || d->n_zs < serve) {
        free(d->min_tb_addr_zs);
        d->min_tb_addr_zs = calloc(serve, sizeof(int32_t));
        d->n_zs = serve;
        if (!d->min_tb_addr_zs) return -1;
    }

    const int diff = sps->log2_ctb - sps->log2_min_tb;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int cx = (x << sps->log2_min_tb) >> sps->log2_ctb;
            const int cy = (y << sps->log2_min_tb) >> sps->log2_ctb;
            int32_t a = (int32_t)(sps->ctb_width * cy + cx) << (diff * 2);
            for (int i = 0; i < diff; i++) {
                const int m = 1 << i;
                a += ((m & x) ? m * m : 0) + ((m & y) ? 2 * m * m : 0);
            }
            d->min_tb_addr_zs[y * w + x] = a;
        }
    }
    return 0;
}

int hevcd_leggi_ctu(hevcd_t *d, int x0, int y0)
{
    const hevc_sps_t *sps = d->sps;
    const hevc_slice_t *sl = d->slice;

    if (sl->sao_luma || sl->sao_chroma)
        leggi_sao(d, x0 >> sps->log2_ctb, y0 >> sps->log2_ctb);

    leggi_quadtree(d, x0, y0, sps->log2_ctb, 0);
    return d->fine_slice ? 1 : 0;
}
