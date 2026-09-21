/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_mv.c - where a prediction unit's motion comes from,
 * Rec. ITU-T H.265 clause 8.5.3.2.
 *
 * Almost no motion vector is sent. What the bitstream carries is either a
 * merge index - "the same motion as that neighbour" - or a difference from
 * a predictor built out of the same neighbours. Both need the decoder to
 * rebuild the candidate list in exactly the encoder's order, from blocks
 * that have already been decoded, and a list that differs by one entry
 * decodes the whole picture somewhere else.
 *
 * ⚠️ Long-term references are refused at the parameter set, so every
 * comparison of "are these both short-term" in the standard is true here
 * and is left out. The scaling by picture order count below is the
 * short-term path only.
 */
#include "hevc_dec_internal.h"

#include <stdlib.h>
#include <string.h>

static inline int ritaglia(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The motion field of the picture being decoded, at a sample position. */
static const hevcd_mvf_t *campo(const hevcd_t *d, int x, int y)
{
    return &d->mvf[(y >> 2) * d->min_pu_width + (x >> 2)];
}

/* 6.4.1: has the block covering (xn, yn) been decoded before the one
 * covering (xc, yc).
 *
 * The z-scan address of every smallest transform block was worked out once
 * for the picture, so the question is two array reads and a comparison
 * rather than a walk back up the quadtree. */
static bool gia_fatto(const hevcd_t *d, int xc, int yc, int xn, int yn)
{
    const hevc_sps_t *sps = d->sps;
    if (xn < 0 || yn < 0 || xn >= sps->width || yn >= sps->height)
        return false;
    const int w = sps->width >> sps->log2_min_tb;
    const int32_t a = d->min_tb_addr_zs[(yc >> sps->log2_min_tb) * w
                                        + (xc >> sps->log2_min_tb)];
    const int32_t b = d->min_tb_addr_zs[(yn >> sps->log2_min_tb) * w
                                        + (xn >> sps->log2_min_tb)];
    return b <= a;
}

/* 7.4.3.3.1: two positions inside one merge estimation region cannot be
 * each other's candidates, which is what lets an encoder work out several
 * units' merge lists at the same time. At the default level of four
 * samples the region is a single smallest block and this never fires. */
static bool stessa_regione(const hevcd_t *d, int xn, int yn, int xp, int yp)
{
    const int l = d->pps->log2_parallel_merge_level;
    return (xn >> l) == (xp >> l) && (yn >> l) == (yp >> l);
}

/* Which neighbours of this prediction unit exist at all, before anything
 * is asked about what they contain. Whether a coding tree block to the
 * left or above exists is a different question from whether a position
 * inside this one has been decoded, and the standard asks both. */
typedef struct {
    bool sinistra, sopra, sopra_sinistra, sopra_destra, sotto_sinistra;
} vicini_t;

static vicini_t vicini(const hevcd_t *d, int x0, int y0, int w, int h)
{
    const hevc_sps_t *sps = d->sps;
    const int maschera = (1 << sps->log2_ctb) - 1;
    const int xb = x0 & maschera, yb = y0 & maschera;
    const int cx = x0 >> sps->log2_ctb, cy = y0 >> sps->log2_ctb;

    /* One slice and one tile: a coding tree block exists if it is inside
     * the picture and comes earlier in raster order. */
    const bool ctb_sinistra = cx > 0;
    const bool ctb_sopra = cy > 0;
    const bool ctb_sopra_sinistra = cx > 0 && cy > 0;
    const bool ctb_sopra_destra = cy > 0 && cx + 1 < sps->ctb_width;

    vicini_t v;
    v.sinistra = ctb_sinistra || xb;
    v.sopra = ctb_sopra || yb;
    v.sopra_sinistra = (xb || yb) ? (v.sinistra && v.sopra) : ctb_sopra_sinistra;
    /* ⚠️ Above right is the one that changes at the edge of a coding tree
     * block: inside it, it is simply "is there anything above"; at the
     * right edge it becomes a question about the next block along, which
     * under wavefront parallelism may not be finished. */
    v.sopra_destra = (xb + w == (1 << sps->log2_ctb))
        ? (ctb_sopra_destra && !yb) : v.sopra;
    v.sotto_sinistra = (y0 + h >= sps->height) ? false : v.sinistra;
    return v;
}

static bool inter(const hevcd_t *d, int x, int y)
{
    return campo(d, x, y)->pred_flag != 0;
}

static bool stessa_roba(const hevcd_mvf_t *a, const hevcd_mvf_t *b)
{
    if (a->pred_flag != b->pred_flag) return false;
    if (a->pred_flag & HEVCD_PF_L0) {
        if (a->ref_idx[0] != b->ref_idx[0]) return false;
        if (a->mv[0][0] != b->mv[0][0] || a->mv[0][1] != b->mv[0][1])
            return false;
    }
    if (a->pred_flag & HEVCD_PF_L1) {
        if (a->ref_idx[1] != b->ref_idx[1]) return false;
        if (a->mv[1][0] != b->mv[1][0] || a->mv[1][1] != b->mv[1][1])
            return false;
    }
    return true;
}

/* 8.5.3.2.8: a vector that pointed across td pictures, pointed across tb
 * instead. The reciprocal is computed once at fourteen bits rather than
 * dividing twice, which is what makes this exact on every decoder. */
static void scala(int16_t *mv, int td, int tb)
{
    td = ritaglia(td, -128, 127);
    tb = ritaglia(tb, -128, 127);
    const int tx = (0x4000 + abs(td / 2)) / td;
    const int f = ritaglia((tb * tx + 32) >> 6, -4096, 4095);
    for (int i = 0; i < 2; i++) {
        const int v = f * mv[i];
        mv[i] = (int16_t)ritaglia((v + 127 + (v < 0)) >> 8, -32768, 32767);
    }
}

/* ------------------------------------------------ the temporal candidate */

/* One list of the collocated picture, as picture order counts. */
static bool prendi_col(const hevcd_t *d, const hevcd_mvf_t *col, int lista_col,
                       int lista, int ref_idx, int16_t fuori[2])
{
    const hevcd_img_t *c = d->col;
    const int r = col->ref_idx[lista_col];
    if (r < 0 || r >= c->n_lista[lista_col]) return false;

    const int diff_col = c->poc - c->poc_lista[lista_col][r];
    const int diff_ora = d->corrente->poc - d->rif[lista][ref_idx]->poc;

    fuori[0] = col->mv[lista_col][0];
    fuori[1] = col->mv[lista_col][1];
    if (diff_col != diff_ora && diff_col)
        scala(fuori, diff_col, diff_ora);
    return true;
}

static bool da_collocata(const hevcd_t *d, const hevcd_mvf_t *col,
                         int lista, int ref_idx, int16_t fuori[2])
{
    if (!col->pred_flag) return false;                  /* an intra block */

    if (!(col->pred_flag & HEVCD_PF_L0))
        return prendi_col(d, col, 1, lista, ref_idx, fuori);
    if (col->pred_flag == HEVCD_PF_L0)
        return prendi_col(d, col, 0, lista, ref_idx, fuori);

    /* ⚠️ Both lists. Which one to believe depends on whether anything in
     * this picture's own lists comes after it: when nothing does, every
     * reference is in the past and the list with the same index is the
     * one that points the same way. When something does, the standard
     * takes the list the collocated picture was NOT taken from. */
    bool c_e_futuro = false;
    for (int l = 0; l < 2 && !c_e_futuro; l++)
        for (int i = 0; i < d->n_rif[l]; i++)
            if (d->rif[l][i]->poc > d->corrente->poc) { c_e_futuro = true; break; }

    const int quale = c_e_futuro ? (d->slice->collocated_from_l0 ? 1 : 0)
                                 : lista;
    return prendi_col(d, col, quale, lista, ref_idx, fuori);
}

/* 8.5.3.2.9: the block at the bottom right of this one in the collocated
 * picture, and failing that the one at its centre.
 *
 * ⚠️ Read at sixteen samples, not four. The motion of a finished picture
 * is kept coarser on purpose - it is the one thing a decoder must hold on
 * to for a whole picture - and the standard rounds the position down to
 * match. Reading the finer grid would read motion the encoder never
 * looked at. */
static bool temporale(const hevcd_t *d, int x0, int y0, int w, int h,
                      int lista, int ref_idx, int16_t fuori[2])
{
    const hevc_sps_t *sps = d->sps;
    if (!d->col || !d->col->mvf) return false;
    if (ref_idx < 0 || ref_idx >= d->n_rif[lista]) return false;

    int x = x0 + w, y = y0 + h;
    if ((y0 >> sps->log2_ctb) == (y >> sps->log2_ctb)
        && y < sps->height && x < sps->width) {
        x &= ~15; y &= ~15;
        if (da_collocata(d, &d->col->mvf[(y >> 2) * d->min_pu_width + (x >> 2)],
                         lista, ref_idx, fuori))
            return true;
    }

    x = (x0 + (w >> 1)) & ~15;
    y = (y0 + (h >> 1)) & ~15;
    return da_collocata(d, &d->col->mvf[(y >> 2) * d->min_pu_width + (x >> 2)],
                        lista, ref_idx, fuori);
}

/* ------------------------------------------------------------- merge mode */

/* Table 8-6: which two candidates each combined bi-predictive one is made
 * of, in the order they are tried. */
static const uint8_t coppie[12][2] = {
    { 0, 1 }, { 1, 0 }, { 0, 2 }, { 2, 0 }, { 1, 2 }, { 2, 1 },
    { 0, 3 }, { 3, 0 }, { 1, 3 }, { 3, 1 }, { 2, 3 }, { 3, 2 },
};

static void spaziali(hevcd_t *d, int x0, int y0, int w, int h,
                     bool lista_unica, int part_idx, int merge_idx,
                     hevcd_mvf_t *lista)
{
    const hevc_sps_t *sps = d->sps;
    const int quanti_max = 5 - d->slice->five_minus_max_num_merge_cand;
    const vicini_t v = vicini(d, x0, y0, w, h);
    const int part = d->cu.part_mode;

    const int xa1 = x0 - 1,     ya1 = y0 + h - 1;
    const int xb1 = x0 + w - 1, yb1 = y0 - 1;
    const int xb0 = x0 + w,     yb0 = y0 - 1;
    const int xa0 = x0 - 1,     ya0 = y0 + h;
    const int xb2 = x0 - 1,     yb2 = y0 - 1;

    const int quanti_rif = (d->slice->type == 1)
        ? d->n_rif[0]
        : (d->n_rif[0] < d->n_rif[1] ? d->n_rif[0] : d->n_rif[1]);

    int n = 0, zero = 0;
    bool ha_a1 = false, ha_b1 = false;

#define METTI(mvf) do {                                 \
        lista[n] = (mvf);                               \
        if (merge_idx == n) return;                     \
        n++;                                            \
    } while (0)

    /* ⚠️ The second unit of a vertically split coding unit may not merge
     * with the first: the two would then be one unit, and the encoder
     * would have sent it as one. The same rule turns up again for the
     * horizontal split and the candidate above. */
    if (!(!lista_unica && part_idx == 1
          && (part == HEVCD_PART_Nx2N || part == HEVCD_PART_nLx2N
              || part == HEVCD_PART_nRx2N))
        && !stessa_regione(d, xa1, ya1, x0, y0)
        && v.sinistra && inter(d, xa1, ya1)) {
        ha_a1 = true;
        METTI(*campo(d, xa1, ya1));
    }

    if (!(!lista_unica && part_idx == 1
          && (part == HEVCD_PART_2NxN || part == HEVCD_PART_2NxnU
              || part == HEVCD_PART_2NxnD))
        && !stessa_regione(d, xb1, yb1, x0, y0)
        && v.sopra && inter(d, xb1, yb1)) {
        ha_b1 = true;
        if (!(ha_a1 && stessa_roba(campo(d, xb1, yb1), campo(d, xa1, ya1))))
            METTI(*campo(d, xb1, yb1));
    }

    if (v.sopra_destra && xb0 < sps->width && inter(d, xb0, yb0)
        && gia_fatto(d, x0, y0, xb0, yb0)
        && !stessa_regione(d, xb0, yb0, x0, y0)
        && !(ha_b1 && stessa_roba(campo(d, xb0, yb0), campo(d, xb1, yb1))))
        METTI(*campo(d, xb0, yb0));

    if (v.sotto_sinistra && ya0 < sps->height && inter(d, xa0, ya0)
        && gia_fatto(d, x0, y0, xa0, ya0)
        && !stessa_regione(d, xa0, ya0, x0, y0)
        && !(ha_a1 && stessa_roba(campo(d, xa0, ya0), campo(d, xa1, ya1))))
        METTI(*campo(d, xa0, ya0));

    if (v.sopra_sinistra && inter(d, xb2, yb2)
        && !stessa_regione(d, xb2, yb2, x0, y0)
        && !(ha_a1 && stessa_roba(campo(d, xb2, yb2), campo(d, xa1, ya1)))
        && !(ha_b1 && stessa_roba(campo(d, xb2, yb2), campo(d, xb1, yb1)))
        && n != 4)
        METTI(*campo(d, xb2, yb2));

    if (d->slice->temporal_mvp_enabled && n < quanti_max) {
        hevcd_mvf_t t;
        memset(&t, 0, sizeof t);
        const bool l0 = temporale(d, x0, y0, w, h, 0, 0, t.mv[0]);
        const bool l1 = (d->slice->type == 0)
            && temporale(d, x0, y0, w, h, 1, 0, t.mv[1]);
        if (l0 || l1) {
            t.pred_flag = (uint8_t)((l0 ? HEVCD_PF_L0 : 0)
                                    | (l1 ? HEVCD_PF_L1 : 0));
            METTI(t);
        }
    }

    /* 8.5.3.2.4: a B slice can make new candidates by taking one list from
     * one candidate and the other list from another. */
    const int n_veri = n;
    if (d->slice->type == 0 && n_veri > 1 && n_veri < quanti_max) {
        for (int k = 0; n < quanti_max && k < n_veri * (n_veri - 1); k++) {
            const hevcd_mvf_t *a = &lista[coppie[k][0]];
            const hevcd_mvf_t *b = &lista[coppie[k][1]];
            if (!(a->pred_flag & HEVCD_PF_L0)) continue;
            if (!(b->pred_flag & HEVCD_PF_L1)) continue;
            if (d->rif[0][a->ref_idx[0]]->poc == d->rif[1][b->ref_idx[1]]->poc
                && a->mv[0][0] == b->mv[1][0] && a->mv[0][1] == b->mv[1][1])
                continue;
            hevcd_mvf_t c;
            memset(&c, 0, sizeof c);
            c.pred_flag = HEVCD_PF_BI;
            c.ref_idx[0] = a->ref_idx[0];
            c.ref_idx[1] = b->ref_idx[1];
            c.mv[0][0] = a->mv[0][0]; c.mv[0][1] = a->mv[0][1];
            c.mv[1][0] = b->mv[1][0]; c.mv[1][1] = b->mv[1][1];
            METTI(c);
        }
    }

    /* And when even that is not enough, zero vectors pointing at each
     * reference in turn. */
    while (n < quanti_max) {
        hevcd_mvf_t z;
        memset(&z, 0, sizeof z);
        z.pred_flag = (uint8_t)(HEVCD_PF_L0
                                | ((d->slice->type == 0) ? HEVCD_PF_L1 : 0));
        z.ref_idx[0] = z.ref_idx[1] = (int8_t)(zero < quanti_rif ? zero : 0);
        METTI(z);
        zero++;
    }
#undef METTI
}

void hevcd_merge(hevcd_t *d, int x0, int y0, int w, int h, int part_idx,
                 int merge_idx, hevcd_mvf_t *fuori)
{
    hevcd_mvf_t lista[8];
    const int lato = 1 << d->cu.log2_size;
    const int w0 = w, h0 = h;
    bool unica = false;

    memset(lista, 0, sizeof lista);

    /* 8.5.3.2.1: at the smallest coding unit, a merge level above four
     * samples makes every unit of it share the list of the whole block. */
    if (d->pps->log2_parallel_merge_level > 2 && lato == 8) {
        unica = true;
        x0 = d->cu.x; y0 = d->cu.y;
        w = lato; h = lato;
        part_idx = 0;
    }

    spaziali(d, x0, y0, w, h, unica, part_idx, merge_idx, lista);
    *fuori = lista[merge_idx];

    /* ⚠️ An 8x4 or a 4x8 may not be bi-predicted, and a merge candidate
     * does not know what shape it landed in. It is cut down to list zero
     * here rather than refused. */
    if (fuori->pred_flag == HEVCD_PF_BI && w0 + h0 == 12)
        fuori->pred_flag = HEVCD_PF_L0;
}

/* --------------------------------------------------------------- AMVP */

/* The neighbour points at the same picture: its vector is the predictor
 * as it stands. */
static bool uguale(const hevcd_t *d, int x, int y, int l, int lista, int ref,
                   int16_t fuori[2])
{
    const hevcd_mvf_t *m = campo(d, x, y);
    if (!(m->pred_flag & (1 << l))) return false;
    if (d->rif[l][m->ref_idx[l]]->poc != d->rif[lista][ref]->poc) return false;
    fuori[0] = m->mv[l][0];
    fuori[1] = m->mv[l][1];
    return true;
}

/* It points somewhere else: the vector is stretched to reach where this
 * unit is pointing. */
static bool scalato(const hevcd_t *d, int x, int y, int l, int lista, int ref,
                    int16_t fuori[2])
{
    const hevcd_mvf_t *m = campo(d, x, y);
    if (!(m->pred_flag & (1 << l))) return false;
    fuori[0] = m->mv[l][0];
    fuori[1] = m->mv[l][1];

    const int poc_vicino = d->rif[l][m->ref_idx[l]]->poc;
    const int poc_mio = d->rif[lista][ref]->poc;
    if (poc_vicino != poc_mio) {
        int diff = d->corrente->poc - poc_vicino;
        if (!diff) diff = 1;
        scala(fuori, diff, d->corrente->poc - poc_mio);
    }
    return true;
}

void hevcd_amvp(hevcd_t *d, int x0, int y0, int w, int h, int lista,
                int mvp_flag, hevcd_mvf_t *mv)
{
    const hevc_sps_t *sps = d->sps;
    const vicini_t v = vicini(d, x0, y0, w, h);
    const int ref = mv->ref_idx[lista];
    const int l0 = lista, l1 = !lista;

    const int xa0 = x0 - 1,     ya0 = y0 + h;
    const int xa1 = x0 - 1,     ya1 = y0 + h - 1;
    const int xb0 = x0 + w,     yb0 = y0 - 1;
    const int xb1 = x0 + w - 1, yb1 = y0 - 1;
    const int xb2 = x0 - 1,     yb2 = y0 - 1;

    const bool ha_a0 = v.sotto_sinistra && ya0 < sps->height
        && inter(d, xa0, ya0) && gia_fatto(d, x0, y0, xa0, ya0);
    const bool ha_a1 = v.sinistra && inter(d, xa1, ya1);
    const bool ha_b0 = v.sopra_destra && xb0 < sps->width
        && inter(d, xb0, yb0) && gia_fatto(d, x0, y0, xb0, yb0);
    const bool ha_b1 = v.sopra && inter(d, xb1, yb1);
    const bool ha_b2 = v.sopra_sinistra && inter(d, xb2, yb2);

    int16_t ma[2] = { 0, 0 }, mb[2] = { 0, 0 };
    bool trovato_a = true, trovato_b = true;

    /* ⚠️ Whether anything exists below left or left at all decides, much
     * further down, whether the candidate from above is allowed to be
     * stretched. The standard calls it isScaledFlag and it is the one
     * piece of state that crosses between the two halves of this. */
    const bool si_scala = ha_a0 || ha_a1;

    if (!((ha_a0 && (uguale(d, xa0, ya0, l0, lista, ref, ma)
                     || uguale(d, xa0, ya0, l1, lista, ref, ma)))
          || (ha_a1 && (uguale(d, xa1, ya1, l0, lista, ref, ma)
                        || uguale(d, xa1, ya1, l1, lista, ref, ma)))
          || (ha_a0 && (scalato(d, xa0, ya0, l0, lista, ref, ma)
                        || scalato(d, xa0, ya0, l1, lista, ref, ma)))
          || (ha_a1 && (scalato(d, xa1, ya1, l0, lista, ref, ma)
                        || scalato(d, xa1, ya1, l1, lista, ref, ma)))))
        trovato_a = false;

    if (!((ha_b0 && (uguale(d, xb0, yb0, l0, lista, ref, mb)
                     || uguale(d, xb0, yb0, l1, lista, ref, mb)))
          || (ha_b1 && (uguale(d, xb1, yb1, l0, lista, ref, mb)
                        || uguale(d, xb1, yb1, l1, lista, ref, mb)))
          || (ha_b2 && (uguale(d, xb2, yb2, l0, lista, ref, mb)
                        || uguale(d, xb2, yb2, l1, lista, ref, mb)))))
        trovato_b = false;

    if (!si_scala) {
        /* Nothing to the left at all: what was found above becomes the
         * left candidate, and above is derived again with stretching
         * allowed. */
        if (trovato_b) {
            trovato_a = true;
            ma[0] = mb[0]; ma[1] = mb[1];
        }
        trovato_b = false;
        if (ha_b0)
            trovato_b = scalato(d, xb0, yb0, l0, lista, ref, mb)
                || scalato(d, xb0, yb0, l1, lista, ref, mb);
        if (ha_b1 && !trovato_b)
            trovato_b = scalato(d, xb1, yb1, l0, lista, ref, mb)
                || scalato(d, xb1, yb1, l1, lista, ref, mb);
        if (ha_b2 && !trovato_b)
            trovato_b = scalato(d, xb2, yb2, l0, lista, ref, mb)
                || scalato(d, xb2, yb2, l1, lista, ref, mb);
    }

    int16_t cand[2][2];
    int n = 0;
    if (trovato_a) { cand[n][0] = ma[0]; cand[n][1] = ma[1]; n++; }
    if (trovato_b && (!trovato_a || ma[0] != mb[0] || ma[1] != mb[1])) {
        cand[n][0] = mb[0]; cand[n][1] = mb[1]; n++;
    }

    if (n < 2 && d->slice->temporal_mvp_enabled && mvp_flag == n) {
        int16_t t[2] = { 0, 0 };
        if (temporale(d, x0, y0, w, h, lista, ref, t)) {
            cand[n][0] = t[0]; cand[n][1] = t[1]; n++;
        }
    }
    while (n < 2) { cand[n][0] = 0; cand[n][1] = 0; n++; }

    mv->mv[lista][0] = cand[mvp_flag][0];
    mv->mv[lista][1] = cand[mvp_flag][1];
}
