/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_cavlc_dec.c - the residual blocks read with CAVLC, Rec. ITU-T H.264
 * clause 9.2.
 *
 * CABAC and CAVLC read the same macroblocks and disagree about almost
 * everything else. CAVLC has no arithmetic coder and no contexts: it is
 * variable-length codes out of fixed tables, and what it adapts instead is
 * WHICH table. The number of coefficients in the blocks above and to the
 * left picks the coeff_token table, and the size of the levels already
 * decoded picks how many suffix bits the next one has.
 *
 * ⚠️ That adaptation is why the count of non-zero coefficients has to be
 * kept per block and not just per macroblock: it is the context of the next
 * block's first symbol, and it crosses macroblock boundaries.
 *
 * Coefficients come out in reverse scan order - the highest frequency
 * first - which is the opposite of how they are stored, and the runs of
 * zeros between them are read afterwards, separately.
 */
#include "h264_dec_internal.h"

#include <string.h>

#include "h264_dec_tables.h"

/* ------------------------------------------------------- the code tables */

/* Find a code in a (length, bits) table. Linear, which is fine for
 * correctness and is the first thing to replace when this gets optimised:
 * a table indexed by the next sixteen bits would answer in one read. */
static int codice(br_t *br, const uint8_t *len, const uint8_t *bits, int n)
{
    for (int l = 1; l <= 16; l++) {
        const uint32_t v = br_peek(br, l);
        for (int i = 0; i < n; i++)
            if (len[i] == l && bits[i] == v) {
                br_skip(br, l);
                return i;
            }
    }
    br_skip(br, 16);
    return -1;
}

/* Clause 9.2.1. `nc` chooses the table: the number of coefficients in the
 * neighbouring blocks, or -1 for a chroma DC block, which has one of its
 * own. Returns TotalCoeff in *totale and TrailingOnes in *uni. */
static int coeff_token(br_t *br, int nc, int *totale, int *uni)
{
    int idx;
    if (nc == -1) {
        idx = codice(br, h264d_chroma_dc_coeff_token_len,
                     h264d_chroma_dc_coeff_token_bits, 20);
        if (idx < 0) return -1;
        *totale = idx >> 2;
        *uni = idx & 3;
        return 0;
    }

    const int t = (nc < 2) ? 0 : (nc < 4) ? 1 : (nc < 8) ? 2 : 3;
    idx = codice(br, h264d_coeff_token_len[t], h264d_coeff_token_bits[t], 68);
    if (idx < 0) return -1;
    *totale = idx >> 2;
    *uni = idx & 3;
    return 0;
}

/* Clause 9.2.2: the levels, read from the highest frequency downwards.
 *
 * ⚠️ suffixLength is state that carries from one level to the next inside
 * the block: it starts at 1 rather than 0 for a block with many
 * coefficients and few trailing ones, and it grows as the levels get
 * bigger. Resetting it per level turns large coefficients into nonsense
 * without ever failing loudly.
 */
static void livelli(br_t *br, int totale, int uni, int16_t *val)
{
    int suffisso = (totale > 10 && uni < 3) ? 1 : 0;

    for (int i = 0; i < uni; i++)
        val[i] = br_read1(br) ? -1 : 1;

    for (int i = uni; i < totale; i++) {
        int prefisso = 0;
        while (prefisso < 32 && !br_read1(br))
            prefisso++;

        int quanti;
        if (prefisso == 14 && suffisso == 0) quanti = 4;
        else if (prefisso >= 15)             quanti = prefisso - 3;
        else                                 quanti = suffisso;

        int codice_livello = (prefisso < 15 ? prefisso : 15) << suffisso;
        if (quanti > 0)
            codice_livello += (int)br_read(br, quanti);
        if (prefisso >= 15 && suffisso == 0)
            codice_livello += 15;
        if (prefisso >= 16)
            codice_livello += (1 << (prefisso - 3)) - 4096;
        /* The first level after the trailing ones cannot be +-1, because a
         * +-1 there would have been coded as a trailing one. The standard
         * reclaims the two codes it would have used. */
        if (i == uni && uni < 3)
            codice_livello += 2;

        val[i] = (int16_t)((codice_livello & 1) ? ((-codice_livello - 1) >> 1)
                                                : ((codice_livello + 2) >> 1));

        if (suffisso == 0)
            suffisso = 1;
        const int a = val[i] < 0 ? -val[i] : val[i];
        if (suffisso < 6 && a > (3 << (suffisso - 1)))
            suffisso++;
    }
}

/* Clause 9.2.3 and 9.2.4: how many zeros there are before the coefficients,
 * and how they are spread between them. */
static void corse(br_t *br, int totale, int n_max, int croma_dc, int *run)
{
    int zeri = 0;
    if (totale < n_max) {
        if (croma_dc)
            zeri = codice(br, h264d_chroma_dc_total_zeros_len[totale - 1],
                          h264d_chroma_dc_total_zeros_bits[totale - 1], 4);
        else
            zeri = codice(br, h264d_total_zeros_len[totale - 1],
                          h264d_total_zeros_bits[totale - 1], 16);
        if (zeri < 0) zeri = 0;
    }

    for (int i = 0; i < totale - 1; i++) {
        int r = 0;
        if (zeri > 0) {
            const int t = zeri < 7 ? zeri - 1 : 6;
            r = codice(br, h264d_run_len[t], h264d_run_bits[t], 16);
            if (r < 0) r = 0;
            if (r > zeri) r = zeri;
        }
        run[i] = r;
        zeri -= r;
    }
    run[totale - 1] = zeri;
}

/* One residual block. `out` receives the coefficients in SCAN order, which
 * is what the caller un-zigzags, exactly as the CABAC path does. Returns
 * TotalCoeff, which the next block's coeff_token table depends on. */
int h264d_cavlc_residuo(br_t *br, int nc, int n_max, int16_t *out)
{
    memset(out, 0, (size_t)n_max * sizeof(int16_t));

    int totale = 0, uni = 0;
    if (coeff_token(br, nc, &totale, &uni) != 0)
        return 0;
    if (totale <= 0)
        return 0;
    if (totale > n_max)
        totale = n_max;
    if (uni > totale)
        uni = totale;

    int16_t val[64];
    int run[64];
    livelli(br, totale, uni, val);
    corse(br, totale, n_max, nc == -1, run);

    /* The levels came out highest frequency first; the runs put them back.
     * coeffNum walks up from the DC end, so the last level decoded lands
     * lowest. */
    int pos = -1;
    for (int i = totale - 1; i >= 0; i--) {
        pos += run[i] + 1;
        if (pos >= 0 && pos < n_max)
            out[pos] = val[i];
    }
    return totale;
}
