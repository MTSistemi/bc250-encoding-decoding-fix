/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_residual.c - residual_coding(), Rec. ITU-T H.265 clause 7.3.8.11.
 *
 * A transform block's coefficients, read backwards. Where the last one is
 * comes first; then the block is walked from there towards the DC in
 * groups of sixteen, and each group is read in passes - which coefficients
 * are there, which are bigger than one, which one is bigger than two, and
 * what is left over.
 *
 * ⚠️ Almost every context here depends on where in the block the
 * coefficient sits, and several on what the groups to the right and below
 * turned out to be. Nothing can be approximated: reading one bin against
 * the wrong context does not fail, it desynchronises the arithmetic
 * decoder several bins later, somewhere else entirely.
 */
#include "hevc_dec_internal.h"

#include <string.h>

/* 9.3.3.11: what is left of a level after the flags. A Golomb-Rice code
 * whose prefix escapes into exp-Golomb once it reaches three. */
static int leggi_resto(hevcd_cabac_t *c, int rice)
{
    int prefisso = 0;
    while (prefisso < 32 && hevcd_bypass(c))
        prefisso++;

    if (prefisso < 3)
        return (prefisso << rice) + (int)(rice ? hevcd_bypass_n(c, rice) : 0);

    const int quanti = prefisso - 3 + rice;
    if (quanti > 30) return 0;            /* a stream this broken is over */
    const uint32_t suffisso = quanti ? hevcd_bypass_n(c, quanti) : 0;
    return (((1 << (prefisso - 3)) + 2) << rice) + (int)suffisso;
}

void hevcd_leggi_residuo(hevcd_t *d, int x0, int y0, int log2_size, int c_idx)
{
    const hevc_pps_t *pps = d->pps;
    hevcd_cabac_t *c = &d->cabac;

    const int lato = 1 << log2_size;
    memset(d->coeff, 0, (size_t)lato * lato * sizeof(int16_t));

    if (pps->transform_skip_enabled && !d->cu.transquant_bypass
        && log2_size == 2)
        hevcd_bin(c, HEVCD_CTX_TRANSFORM_SKIP_FLAG + (c_idx ? 1 : 0));

    /* 8.4.4.2. An intra block small enough has its scan chosen by the
     * prediction mode: a near-horizontal prediction leaves a residual with
     * vertical structure, and the other way round. Anything larger, and
     * anything inter, is scanned diagonally. */
    int scan_idx = HEVCD_SCAN_DIAG;
    if (d->cu.pred_mode == HEVCD_MODE_INTRA
        && (log2_size == 2 || (log2_size == 3 && c_idx == 0))) {
        const int modo = c_idx
            ? d->cu.intra_mode_c
            : d->intra_mode[(y0 >> 2) * d->min_pu_width + (x0 >> 2)];
        if (modo >= 6 && modo <= 14) scan_idx = HEVCD_SCAN_VERT;
        else if (modo >= 22 && modo <= 30) scan_idx = HEVCD_SCAN_HORIZ;
    }

    /* --- where the last coefficient is, 9.3.3.10 -------------------- */
    const int max = (log2_size << 1) - 1;
    int off, shift;
    if (c_idx == 0) {
        off = 3 * (log2_size - 2) + ((log2_size - 1) >> 2);
        shift = (log2_size + 1) >> 2;
    } else {
        off = 15;
        shift = log2_size - 2;
    }

    int ux = 0, uy = 0;
    while (ux < max && hevcd_bin(c, HEVCD_CTX_LAST_SIGNIFICANT_COEFF_X_PREFIX
                                    + (ux >> shift) + off))
        ux++;
    while (uy < max && hevcd_bin(c, HEVCD_CTX_LAST_SIGNIFICANT_COEFF_Y_PREFIX
                                    + (uy >> shift) + off))
        uy++;
    if (ux > 3) {
        const int q = (ux >> 1) - 1;
        ux = (1 << q) * (2 + (ux & 1)) + (int)hevcd_bypass_n(c, q);
    }
    if (uy > 3) {
        const int q = (uy >> 1) - 1;
        uy = (1 << q) * (2 + (uy & 1)) + (int)hevcd_bypass_n(c, q);
    }
    if (scan_idx == HEVCD_SCAN_VERT) { const int t = ux; ux = uy; uy = t; }
    if (ux >= lato || uy >= lato) return;        /* malformed */

    /* --- the scans, and how far into them the last coefficient is --- */
    const uint8_t *sx, *sy, *gx, *gy;
    int quanti;
    if (scan_idx == HEVCD_SCAN_DIAG) {
        sx = hevcd_diag4_x; sy = hevcd_diag4_y;
        quanti = hevcd_diag4_inv[uy & 3][ux & 3];
        switch (lato) {
        case 4: {
            static const uint8_t solo[1] = { 0 };
            gx = gy = solo;                 /* one group, at the corner */
            break;
        }
        case 8:
            quanti += hevcd_diag2_inv[uy >> 2][ux >> 2] << 4;
            gx = hevcd_diag2_x; gy = hevcd_diag2_y;
            break;
        case 16:
            quanti += hevcd_diag4_inv[uy >> 2][ux >> 2] << 4;
            gx = hevcd_diag4_x; gy = hevcd_diag4_y;
            break;
        default:
            quanti += hevcd_diag8_inv[uy >> 2][ux >> 2] << 4;
            gx = hevcd_diag8_x; gy = hevcd_diag8_y;
            break;
        }
    } else if (scan_idx == HEVCD_SCAN_HORIZ) {
        /* ⚠️ Only 4x4 and 8x8 blocks are ever scanned this way, so the
         * groups are always 2x2 and the inverse scan is always the 8x8
         * one. There is no sixteen-wide horizontal scan to look for. */
        gx = hevcd_horiz2_x; gy = hevcd_horiz2_y;
        sx = hevcd_horiz4_x; sy = hevcd_horiz4_y;
        quanti = hevcd_horiz8_inv[uy][ux];
    } else {
        gx = hevcd_horiz2_y; gy = hevcd_horiz2_x;
        sx = hevcd_horiz4_y; sy = hevcd_horiz4_x;
        quanti = hevcd_horiz8_inv[ux][uy];
    }
    quanti++;
    const int ultimo_gruppo = (quanti - 1) >> 4;
    const int lato_g = lato >> 2;

    uint8_t gruppo[8][8];
    memset(gruppo, 0, sizeof(gruppo));

    /* ⚠️ Carried from one group to the next, not reset with them: the set
     * of contexts the next group starts from depends on how the previous
     * one ended. */
    int greater1_ctx = 1;

    for (int i = ultimo_gruppo; i >= 0; i--) {
        const int x_cg = gx[i], y_cg = gy[i];
        bool dc_implicito = false;

        if (i < ultimo_gruppo && i > 0) {
            int ctx = 0;
            if (x_cg < lato_g - 1) ctx += gruppo[y_cg][x_cg + 1];
            if (y_cg < lato_g - 1) ctx += gruppo[y_cg + 1][x_cg];
            const int inc = (ctx > 1 ? 1 : ctx) + (c_idx ? 2 : 0);
            gruppo[y_cg][x_cg] = (uint8_t)
                hevcd_bin(c, HEVCD_CTX_SIGNIFICANT_COEFF_GROUP_FLAG + inc);
            dc_implicito = true;
        } else {
            /* The group holding the last coefficient, and the one holding
             * the DC, are coded by definition. */
            gruppo[y_cg][x_cg] = 1;
        }
        if (!gruppo[y_cg][x_cg]) continue;

        int prev = 0;
        if (x_cg < lato_g - 1) prev = gruppo[y_cg][x_cg + 1] ? 1 : 0;
        if (y_cg < lato_g - 1) prev += gruppo[y_cg + 1][x_cg] ? 2 : 0;

        /* 9.3.4.2.5 */
        int scf_off = c_idx ? 27 : 0;
        const uint8_t *mappa;
        if (log2_size == 2) {
            mappa = &hevcd_ctx_idx_map[0];
        } else {
            mappa = &hevcd_ctx_idx_map[(prev + 1) << 4];
            if (c_idx == 0) {
                if (x_cg > 0 || y_cg > 0) scf_off += 3;
                scf_off += (log2_size == 3)
                         ? ((scan_idx == HEVCD_SCAN_DIAG) ? 9 : 15) : 21;
            } else {
                scf_off += (log2_size == 3) ? 9 : 12;
            }
        }

        uint8_t sig[16];
        int n_sig = 0;
        int n_alto;
        if (i == ultimo_gruppo) {
            const int ultima_pos = (quanti - 1) & 15;
            sig[n_sig++] = (uint8_t)ultima_pos;
            n_alto = ultima_pos - 1;
        } else {
            n_alto = 15;
        }

        for (int n = n_alto; n > 0; n--) {
            const int inc = mappa[(sy[n] << 2) + sx[n]] + scf_off;
            if (hevcd_bin(c, HEVCD_CTX_SIGNIFICANT_COEFF_FLAG + inc)) {
                sig[n_sig++] = (uint8_t)n;
                dc_implicito = false;
            }
        }
        if (n_alto >= 0) {
            if (dc_implicito) {
                sig[n_sig++] = 0;
            } else {
                const int inc = (i == 0) ? (c_idx ? 27 : 0) : (2 + scf_off);
                if (hevcd_bin(c, HEVCD_CTX_SIGNIFICANT_COEFF_FLAG + inc))
                    sig[n_sig++] = 0;
            }
        }
        if (n_sig == 0) continue;

        /* --- the levels, 9.3.4.2.6 and 9.3.4.2.7 ------------------- */
        int ctx_set = (i > 0 && c_idx == 0) ? 2 : 0;
        if (i != ultimo_gruppo && greater1_ctx == 0)
            ctx_set++;
        greater1_ctx = 1;

        uint8_t g1[8];
        memset(g1, 0, sizeof(g1));
        int primo_g1 = -1;
        const int quanti_g1 = n_sig < 8 ? n_sig : 8;
        for (int k = 0; k < quanti_g1; k++) {
            const int inc = (ctx_set << 2) + greater1_ctx + (c_idx ? 16 : 0);
            g1[k] = (uint8_t)
                hevcd_bin(c, HEVCD_CTX_COEFF_ABS_LEVEL_GREATER1_FLAG + inc);
            if (g1[k]) {
                greater1_ctx = 0;
                if (primo_g1 < 0) primo_g1 = k;
            } else if (greater1_ctx > 0 && greater1_ctx < 3) {
                greater1_ctx++;
            }
        }
        if (primo_g1 >= 0)
            g1[primo_g1] += (uint8_t)
                hevcd_bin(c, HEVCD_CTX_COEFF_ABS_LEVEL_GREATER2_FLAG
                             + ctx_set + (c_idx ? 4 : 0));

        /* 7.4.9.11: when the run of coefficients is long enough, the sign
         * of the lowest one is not sent - it is carried by the parity of
         * the sum, which costs nothing because the sum has to be right
         * anyway. */
        const int alto = sig[0], basso = sig[n_sig - 1];
        const bool segno_nascosto = pps->sign_data_hiding
                                 && !d->cu.transquant_bypass
                                 && (alto - basso) >= 4;

        const int quanti_segni = segno_nascosto ? n_sig - 1 : n_sig;
        uint32_t segni = quanti_segni ? hevcd_bypass_n(c, quanti_segni) : 0;

        int rice = 0, somma = 0;
        int livelli[16];
        for (int k = 0; k < n_sig; k++) {
            int base = (k < 8) ? 1 + g1[k] : 1;
            const int soglia = (k < 8) ? ((k == primo_g1) ? 3 : 2) : 1;
            if (base == soglia) {
                base += leggi_resto(c, rice);
                if (base > (3 << rice) && rice < 4) rice++;
            }
            livelli[k] = base;
            somma += base;
        }

        for (int k = 0; k < n_sig; k++) {
            bool negativo;
            if (segno_nascosto && k == n_sig - 1)
                negativo = (somma & 1) != 0;
            else
                negativo = ((segni >> (quanti_segni - 1 - k)) & 1) != 0;
            const int n = sig[k];
            const int xc = (x_cg << 2) + sx[n], yc = (y_cg << 2) + sy[n];
            d->coeff[yc * lato + xc] =
                (int16_t)(negativo ? -livelli[k] : livelli[k]);
        }
    }
}
