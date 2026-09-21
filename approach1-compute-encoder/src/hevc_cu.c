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

    bool unisci = false;
    if (rx > 0)
        unisci = hevcd_bin(c, HEVCD_CTX_SAO_MERGE_FLAG) != 0;
    if (!unisci && ry > 0)
        unisci = hevcd_bin(c, HEVCD_CTX_SAO_MERGE_FLAG) != 0;
    if (unisci) return;

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
        if (tipo == 1) {
            for (int i = 0; i < 4; i++)
                if (assoluti[i]) hevcd_bypass(c);      /* the sign */
            hevcd_bypass_n(c, 5);                      /* band position */
        } else if (piano != 2) {
            hevcd_bypass_n(c, 2);                      /* edge class */
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

/* 8.7.2.2: this block's left and top edges are block boundaries, and the
 * deblocking filter is allowed to cross them.
 *
 * ⚠️ Only edges that fall on the eight-sample grid count. A transform
 * block boundary four samples in is a real boundary and the filter still
 * ignores it: filtering on a four grid would leave no unfiltered sample
 * anywhere, since the filter reaches four samples each way.
 */
static void segna_bordi(hevcd_t *d, int x0, int y0, int log2_size)
{
    if (!d->bordi) return;
    const int lato = 1 << log2_size;
    const int passo = d->bordi_passo;

    if ((x0 & 7) == 0)
        for (int j = 0; j < lato; j += 8)
            d->bordi[((y0 + j) >> 3) * passo + (x0 >> 3)] |= 1;
    if ((y0 & 7) == 0)
        for (int i = 0; i < lato; i += 8)
            d->bordi[(y0 >> 3) * passo + ((x0 + i) >> 3)] |= 2;
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
        /* Inferred, and rarely zero: too big for the largest transform, or
         * an intra unit split into four prediction blocks. */
        dividi = (log2_size > sps->log2_max_tb)
              || (d->cu.intra_split && depth == 0);
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

    segna_bordi(d, x0, y0, log2_size);
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

    segna_bordi(d, x0, y0, log2_size);
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
