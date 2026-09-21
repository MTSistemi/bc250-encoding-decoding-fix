/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_mb_cavlc.c - the macroblock layer read with CAVLC, Rec. ITU-T H.264
 * clause 7.3.5 without the arithmetic coder.
 *
 * The same macroblocks, the same reconstruction, a different way of getting
 * the numbers off the wire. Where CABAC reads bins against a context, CAVLC
 * reads Exp-Golomb codes and fixed variable-length tables, and where CABAC
 * signals a skipped macroblock one at a time, CAVLC counts a run of them.
 */
#include "h264_dec_internal.h"

#include <stdlib.h>
#include <string.h>

#include "h264_dec_tables.h"
#include "h264_parts.h"
#include "h264_pred.h"

int h264d_cavlc_residuo(br_t *br, int nc, int n_max, int16_t *out);

static const uint8_t zscan[16] = {
    0, 1, 4, 5,  2, 3, 6, 7,  8, 9, 12, 13,  10, 11, 14, 15
};

/* ---------------------------------------------------------- neighbours */

static const h264d_mb_t *sinistra4(const h264_decoder_t *d, int b, int *out)
{
    *out = 0;
    if (b & 3) { *out = b - 1; return &d->mbs[d->mb_idx]; }
    const h264d_mb_t *m = h264d_mb_left(d);
    if (!m) return NULL;
    *out = b + 3;
    return m;
}

static const h264d_mb_t *sopra4(const h264_decoder_t *d, int b, int *out)
{
    *out = 0;
    if (b >= 4) { *out = b - 4; return &d->mbs[d->mb_idx]; }
    const h264d_mb_t *m = h264d_mb_top(d);
    if (!m) return NULL;
    *out = b + 12;
    return m;
}

static const h264d_mb_t *sinistra_croma(const h264_decoder_t *d, int b, int *out)
{
    *out = 0;
    if (b & 1) { *out = b - 1; return &d->mbs[d->mb_idx]; }
    const h264d_mb_t *m = h264d_mb_left(d);
    if (!m) return NULL;
    *out = b + 1;
    return m;
}

static const h264d_mb_t *sopra_croma(const h264_decoder_t *d, int b, int *out)
{
    *out = 0;
    if (b >= 2) { *out = b - 2; return &d->mbs[d->mb_idx]; }
    const h264d_mb_t *m = h264d_mb_top(d);
    if (!m) return NULL;
    *out = b + 2;
    return m;
}

/* Clause 9.2.1: which coeff_token table to read the next block with.
 *
 * ⚠️ A skipped neighbour contributes zero and an I_PCM one contributes
 * sixteen. Everything else contributes the count of coefficients it
 * actually had, which is why that count has to be kept per block and
 * survive across macroblocks. */
static int quanti_vicini(const h264d_mb_t *m, int blocco, int piano)
{
    if (!m) return -1;                          /* not available */
    if (m->type == H264D_MB_I_PCM) return 16;
    if (m->type == H264D_MB_P_SKIP || m->type == H264D_MB_B_SKIP) return 0;
    return m->nnz[piano][blocco];
}

static int nc_di(const h264_decoder_t *d, int blocco, int piano, bool croma)
{
    int pa, pb;
    const h264d_mb_t *a = croma ? sinistra_croma(d, blocco, &pa)
                                : sinistra4(d, blocco, &pa);
    const h264d_mb_t *b = croma ? sopra_croma(d, blocco, &pb)
                                : sopra4(d, blocco, &pb);
    const int na = quanti_vicini(a, pa, piano);
    const int nb = quanti_vicini(b, pb, piano);

    if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
    if (na >= 0) return na;
    if (nb >= 0) return nb;
    return 0;
}

/* ------------------------------------------------------ syntax elements */

/* te(v), clause 9.1.1: one inverted bit when the range is two, otherwise
 * an ordinary Exp-Golomb code. */
static int leggi_te(br_t *br, int cap)
{
    if (cap <= 1) return 0;
    if (cap == 2) return 1 - (int)br_read1(br);
    return (int)br_read_ue(br);
}

/* me(v), clause 9.1.2: an Exp-Golomb code mapped through Table 9-4. */
static int leggi_cbp(br_t *br, bool intra_nxn)
{
    const uint32_t k = br_read_ue(br);
    if (k > 47) return 0;
    return intra_nxn ? h264d_golomb_to_intra4x4_cbp[k]
                     : h264d_golomb_to_inter_cbp[k];
}

/* ------------------------------------------------------------- residual */

static void leggi_residuo_mb(h264_decoder_t *d, h264d_mb_t *m, bool i16)
{
    br_t *br = &d->br;

    if (i16) {
        /* The DC block borrows luma block 0's neighbourhood. */
        int16_t tmp[16];
        const int nz = h264d_cavlc_residuo(br, nc_di(d, 0, 0, false), 16, tmp);
        for (int k = 0; k < 16; k++)
            d->dc_luma[h264d_zigzag4[k]] = tmp[k];
        m->cbf_dc[0] = nz ? 1 : 0;
    }

    /* ⚠️ CAVLC never codes an 8x8 block. With the 8x8 transform its
     * coefficients are carried as four 4x4 blocks and interleaved, one
     * taking every fourth position of the 8x8 scan (7.3.5.3.1). */
    for (int i8 = 0; i8 < 4; i8++) {
        int16_t otto[64];
        if (m->transform8x8)
            memset(otto, 0, sizeof(otto));

        for (int i4 = 0; i4 < 4; i4++) {
            const int k = i8 * 4 + i4;
            const int b = zscan[k];
            int16_t tmp[16];

            if (!((m->cbp >> i8) & 1)) {
                memset(d->coeff[0][b], 0, sizeof(d->coeff[0][b]));
                m->nnz[0][b] = 0;
                continue;
            }

            const int n = i16 ? 15 : 16;
            const int nz = h264d_cavlc_residuo(br, nc_di(d, b, 0, false), n, tmp);
            m->nnz[0][b] = (uint8_t)nz;

            if (m->transform8x8) {
                for (int c = 0; c < 16; c++)
                    otto[4 * c + i4] = tmp[c];
            } else {
                memset(d->coeff[0][b], 0, sizeof(d->coeff[0][b]));
                if (i16)
                    for (int c = 0; c < 15; c++)
                        d->coeff[0][b][h264d_zigzag4[c + 1]] = tmp[c];
                else
                    for (int c = 0; c < 16; c++)
                        d->coeff[0][b][h264d_zigzag4[c]] = tmp[c];
            }
        }

        if (m->transform8x8) {
            memset(d->coeff8[i8], 0, sizeof(d->coeff8[i8]));
            if ((m->cbp >> i8) & 1)
                for (int c = 0; c < 64; c++)
                    d->coeff8[i8][h264d_zigzag8[c]] = otto[c];
        }
    }

    /* Chroma DC, then chroma AC. */
    const int cbp_c = m->cbp >> 4;
    for (int p = 0; p < 2; p++) {
        if (cbp_c) {
            /* nC is -1 for a 4:2:0 chroma DC block: it has a table of its
             * own and no neighbourhood. */
            const int nz = h264d_cavlc_residuo(br, -1, 4, d->dc_chroma[p]);
            m->cbf_dc[p + 1] = nz ? 1 : 0;
        } else {
            memset(d->dc_chroma[p], 0, sizeof(d->dc_chroma[p]));
            m->cbf_dc[p + 1] = 0;
        }
    }
    for (int p = 0; p < 2; p++) {
        for (int b = 0; b < 4; b++) {
            memset(d->coeff[p + 1][b], 0, sizeof(d->coeff[p + 1][b]));
            if (cbp_c != 2) {
                m->nnz[p + 1][b] = 0;
                continue;
            }
            int16_t tmp[16];
            const int nz = h264d_cavlc_residuo(br, nc_di(d, b, p + 1, true),
                                               15, tmp);
            m->nnz[p + 1][b] = (uint8_t)nz;
            for (int c = 0; c < 15; c++)
                d->coeff[p + 1][b][h264d_zigzag4[c + 1]] = tmp[c];
        }
    }
}

/* --------------------------------------------------------- the macroblock */

static void azzera_mb(h264d_mb_t *m)
{
    memset(m, 0, sizeof(*m));
    memset(m->ref, -1, sizeof(m->ref));
    memset(m->ref_idx, -1, sizeof(m->ref_idx));
    memset(m->sub_tipo, -1, sizeof(m->sub_tipo));
}

static void spacchetta_i16(int t, int *modo, int *cbp)
{
    const int k = t - 1;
    *modo = k & 3;
    *cbp = ((k / 4) % 3) << 4;
    if (k >= 12) *cbp |= 15;
}

/* The motion of one macroblock, clause 7.3.5.1 and 7.3.5.2. Same shape as
 * the CABAC path: reference indices per partition, differences per
 * sub-partition, four passes, and the derivation in partition order. */
static int leggi_movimento(h264_decoder_t *d, h264d_mb_t *m, bool bslice,
                           int t, bool ref0_forzato)
{
    br_t *br = &d->br;
    struct {
        uint8_t pred, n, blk[4], w4, h4;
        int8_t  ref[2];
        int16_t dx[2][4], dy[2][4];
    } parte[4];
    int np = 0;
    bool sotto_8x8 = false;

    if (m->type == H264D_MB_P_8x8 || m->type == H264D_MB_B_8x8) {
        h264d_sub_t sub[4];
        for (int i = 0; i < 4; i++) {
            const int s = (int)br_read_ue(br);
            m->sub_tipo[i] = (int8_t)s;
            sub[i] = bslice ? h264d_sub_b[s >= 0 && s < 13 ? s : 12]
                            : h264d_sub_p[s >= 0 && s < 4 ? s : 3];
            if (bslice && s == 0) {
                if (!d->pic.direct_8x8_inference) sotto_8x8 = true;
            } else if (sub[i].w4 < 2 || sub[i].h4 < 2) {
                sotto_8x8 = true;
            }
        }
        for (int i = 0; i < 4; i++) {
            parte[i].pred = sub[i].pred;
            parte[i].n = sub[i].n;
            parte[i].w4 = sub[i].w4;
            parte[i].h4 = sub[i].h4;
            for (int k = 0; k < sub[i].n; k++)
                parte[i].blk[k] = (uint8_t)(h264d_blk8[i]
                                            + h264d_sub_offset(&sub[i], k));
        }
        np = 4;
    } else if (m->type == H264D_MB_P_16x16 || m->type == H264D_MB_B_16x16) {
        parte[0].pred = (uint8_t)(bslice ? (t == 1 ? H264D_PRED_L0
                                          : t == 2 ? H264D_PRED_L1
                                                   : H264D_PRED_BI)
                                         : H264D_PRED_L0);
        parte[0].n = 1; parte[0].blk[0] = 0; parte[0].w4 = 4; parte[0].h4 = 4;
        np = 1;
    } else if (m->type == H264D_MB_B_DIRECT) {
        parte[0].pred = H264D_PRED_DIRECT;
        parte[0].n = 1; parte[0].blk[0] = 0; parte[0].w4 = 4; parte[0].h4 = 4;
        np = 1;
    } else {
        const bool verticale = (m->type == H264D_MB_P_8x16
                             || m->type == H264D_MB_B_8x16);
        const int w4 = verticale ? 2 : 4, h4 = verticale ? 4 : 2;
        const int secondo = verticale ? 2 : 8;
        uint8_t p0 = H264D_PRED_L0, p1 = H264D_PRED_L0;
        if (bslice) {
            int coppia = (t - 4) / 2;
            if (coppia < 0) coppia = 0;
            if (coppia > 8) coppia = 8;
            p0 = h264d_b_pair[coppia][0];
            p1 = h264d_b_pair[coppia][1];
        }
        parte[0].pred = p0; parte[0].n = 1; parte[0].blk[0] = 0;
        parte[0].w4 = (uint8_t)w4; parte[0].h4 = (uint8_t)h4;
        parte[1].pred = p1; parte[1].n = 1; parte[1].blk[0] = (uint8_t)secondo;
        parte[1].w4 = (uint8_t)w4; parte[1].h4 = (uint8_t)h4;
        np = 2;
    }

    m->sub_8x8 = sotto_8x8 ? 1 : 0;
    m->direct = 0;
    for (int i = 0; i < np; i++)
        if (parte[i].pred == H264D_PRED_DIRECT)
            m->direct |= (uint8_t)(np == 1 ? 0xf : (1 << i));

    for (int i = 0; i < np; i++) {
        parte[i].ref[0] = parte[i].ref[1] = -1;
        memset(parte[i].dx, 0, sizeof(parte[i].dx));
        memset(parte[i].dy, 0, sizeof(parte[i].dy));
    }

    for (int lista = 0; lista < (bslice ? 2 : 1); lista++) {
        const int cap = d->slice.num_ref_idx[lista];
        for (int i = 0; i < np; i++) {
            if (parte[i].pred == H264D_PRED_DIRECT
                || !((parte[i].pred == H264D_PRED_BI)
                     || parte[i].pred == (lista ? H264D_PRED_L1 : H264D_PRED_L0)))
                continue;
            /* P_8x8ref0 pins every partition to reference zero and signals
             * nothing. It exists only in CAVLC. */
            const int r = ref0_forzato ? 0 : leggi_te(br, cap);
            parte[i].ref[lista] = (int8_t)r;
            for (int k = 0; k < parte[i].n; k++) {
                const int b = parte[i].blk[k];
                const int x4 = b & 3, y4 = b >> 2;
                for (int yy = 0; yy < parte[i].h4; yy++)
                    for (int xx = 0; xx < parte[i].w4; xx++) {
                        const int bb = (y4 + yy) * 4 + x4 + xx;
                        m->ref[lista][h264d_part8(bb)] =
                            (int8_t)d->slice.ref_list[lista][r];
                        m->ref_idx[lista][h264d_part8(bb)] = (int8_t)r;
                    }
            }
        }
    }

    for (int lista = 0; lista < (bslice ? 2 : 1); lista++) {
        for (int i = 0; i < np; i++) {
            if (parte[i].pred == H264D_PRED_DIRECT
                || !((parte[i].pred == H264D_PRED_BI)
                     || parte[i].pred == (lista ? H264D_PRED_L1 : H264D_PRED_L0)))
                continue;
            for (int k = 0; k < parte[i].n; k++) {
                const int dx = br_read_se(br);
                const int dy = br_read_se(br);
                parte[i].dx[lista][k] = (int16_t)dx;
                parte[i].dy[lista][k] = (int16_t)dy;
                const int b = parte[i].blk[k];
                const int x4 = b & 3, y4 = b >> 2;
                for (int yy = 0; yy < parte[i].h4; yy++)
                    for (int xx = 0; xx < parte[i].w4; xx++) {
                        const int bb = (y4 + yy) * 4 + x4 + xx;
                        m->mvd[lista][bb][0] = (int16_t)dx;
                        m->mvd[lista][bb][1] = (int16_t)dy;
                    }
            }
        }
    }

    for (int i = 0; i < np; i++) {
        if (parte[i].pred == H264D_PRED_DIRECT) {
            const int maschera = (np == 1) ? 0xf : (1 << i);
            if (h264d_direct(d, m, maschera) != 0)
                return 1;
            continue;
        }
        for (int lista = 0; lista < (bslice ? 2 : 1); lista++) {
            if (!((parte[i].pred == H264D_PRED_BI)
                  || parte[i].pred == (lista ? H264D_PRED_L1 : H264D_PRED_L0)))
                continue;
            const int r = parte[i].ref[lista];
            for (int k = 0; k < parte[i].n; k++) {
                const int b = parte[i].blk[k];
                int16_t pmv[2];
                h264d_predict_mv(d, lista, b, parte[i].w4, parte[i].h4, r, pmv);
                const int x4 = b & 3, y4 = b >> 2;
                const int16_t mx = (int16_t)(pmv[0] + parte[i].dx[lista][k]);
                const int16_t my = (int16_t)(pmv[1] + parte[i].dy[lista][k]);
                for (int yy = 0; yy < parte[i].h4; yy++)
                    for (int xx = 0; xx < parte[i].w4; xx++) {
                        const int bb = (y4 + yy) * 4 + x4 + xx;
                        m->mv[lista][bb][0] = mx;
                        m->mv[lista][bb][1] = my;
                    }
            }
        }
    }
    return 0;
}

int h264d_decode_mb_cavlc(h264_decoder_t *d)
{
    br_t *br = &d->br;
    h264d_mb_t *m = &d->mbs[d->mb_idx];
    azzera_mb(m);

    const bool bslice = d->slice.type == 1;
    const bool islice = d->slice.type == 2;

    int t = (int)br_read_ue(br);

    int i16_modo = -1;
    bool ref0_forzato = false;

    if (islice || (!bslice && t >= 5) || (bslice && t >= 23)) {
        const int ti = islice ? t : (bslice ? t - 23 : t - 5);
        if (ti == 25) {
            m->type = H264D_MB_I_PCM;
            m->intra = 1;
            return 2;                   /* I_PCM: not supported yet */
        }
        if (ti == 0) {
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
        /* Table 7-13. mb_type 4 is P_8x8ref0, which has no CABAC
         * binarization and so appears only here: the same partitioning as
         * P_8x8 with every reference pinned to zero and nothing signalled. */
        static const uint8_t mappa_p[5] = {
            H264D_MB_P_16x16, H264D_MB_P_16x8, H264D_MB_P_8x16,
            H264D_MB_P_8x8, H264D_MB_P_8x8
        };
        m->type = mappa_p[t < 5 ? t : 3];
        ref0_forzato = (t == 4);
    }

    if (m->intra) {
        if (m->type == H264D_MB_I_NxN) {
            if (d->pic.transform_8x8_mode)
                m->transform8x8 = (uint8_t)br_read1(br);

            const int passo = m->transform8x8 ? 4 : 1;
            for (int k = 0; k < 16; k += passo) {
                const int b = zscan[k];
                int pa, pb;
                const h264d_mb_t *a = sinistra4(d, b, &pa);
                const h264d_mb_t *bm = sopra4(d, b, &pb);
                const int ma = (a && a->type == H264D_MB_I_NxN) ? a->ipred[pa]
                             : (a ? 2 : -1);
                const int mb2 = (bm && bm->type == H264D_MB_I_NxN) ? bm->ipred[pb]
                              : (bm ? 2 : -1);
                const int previsto = (ma < 0 || mb2 < 0) ? 2
                                   : (ma < mb2 ? ma : mb2);
                int modo;
                if (br_read1(br)) {
                    modo = previsto;
                } else {
                    const int r = (int)br_read(br, 3);
                    modo = r < previsto ? r : r + 1;
                }
                if (passo == 1) {
                    m->ipred[b] = (int8_t)modo;
                } else {
                    const int bx = b & 3, by = b >> 2;
                    for (int y = 0; y < 2; y++)
                        for (int x = 0; x < 2; x++)
                            m->ipred[(by + y) * 4 + bx + x] = (int8_t)modo;
                }
            }
        }
        m->chroma_pred_mode = (int8_t)br_read_ue(br);
    } else {
        if (leggi_movimento(d, m, bslice, t, ref0_forzato) != 0)
            return 1;
    }

    const bool i16 = (m->type == H264D_MB_I_16x16);
    if (!i16) {
        m->cbp = (uint8_t)leggi_cbp(br, m->type == H264D_MB_I_NxN);
        if ((m->cbp & 15) && d->pic.transform_8x8_mode
            && m->type != H264D_MB_I_NxN && !m->sub_8x8
            && (m->type != H264D_MB_B_DIRECT || d->pic.direct_8x8_inference))
            m->transform8x8 = (uint8_t)br_read1(br);
    }

    if (m->cbp || i16) {
        d->qpy += br_read_se(br);
        d->qpy = ((d->qpy + 52) % 52 + 52) % 52;
    }
    m->qpy = (int8_t)d->qpy;

    leggi_residuo_mb(d, m, i16);
    if (i16)
        m->ipred[0] = (int8_t)i16_modo;

    h264d_reconstruct_mb(d);
    return 0;
}

/* A run of skipped macroblocks, clause 7.3.4. CAVLC counts them instead of
 * flagging each one. */
int h264d_cavlc_skip(h264_decoder_t *d)
{
    h264d_mb_t *m = &d->mbs[d->mb_idx];
    azzera_mb(m);

    const bool bslice = d->slice.type == 1;
    m->type = (uint8_t)(bslice ? H264D_MB_B_SKIP : H264D_MB_P_SKIP);
    m->qpy = (int8_t)d->qpy;

    memset(d->dc_luma, 0, sizeof(d->dc_luma));
    memset(d->dc_chroma, 0, sizeof(d->dc_chroma));
    memset(d->coeff, 0, sizeof(d->coeff));
    memset(d->coeff8, 0, sizeof(d->coeff8));

    if (bslice) {
        m->direct = 0xf;
        if (h264d_direct(d, m, 0xf) != 0)
            return 1;
    } else {
        int16_t mv[2];
        h264d_skip_mv_p(d, mv);
        const int slot = d->slice.ref_list[0][0];
        for (int b = 0; b < 16; b++) {
            m->mv[0][b][0] = mv[0];
            m->mv[0][b][1] = mv[1];
        }
        for (int p = 0; p < 4; p++) {
            m->ref[0][p] = (int8_t)slot;
            m->ref_idx[0][p] = 0;
        }
    }

    h264d_reconstruct_mb(d);
    return 0;
}
