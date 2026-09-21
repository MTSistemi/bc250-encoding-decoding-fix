/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_cabac_roundtrip.c - the decoding engine against the encoding engine.
 *
 * The encoder in cabac.c is known good: the bitstreams it writes decode
 * bit-exactly in FFmpeg. So anything it encodes, the decoder in
 * h264_cabac_dec.h has to give back, bin for bin, and the first bin that
 * differs is the one to look at.
 *
 * Two things are checked, and they are separate on purpose:
 *
 *   1. context initialisation - h264d_cabac_ctx_init() against the encoder's
 *      cabac_context_init() over all 276 contexts the encoder knows and all
 *      52 QPs. This compares the generated 1024-entry tables against the
 *      hand-checked 276-entry ones, so a bad extraction shows up here rather
 *      than as a drifting picture much later;
 *
 *   2. the arithmetic itself - pseudo-random bins through decisions, bypass
 *      and terminate, with the decoder's contexts seeded from the encoder's
 *      so that only the engine is under test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cabac.h"
#include "h264_cabac_dec.h"

/* xorshift64*, so the sequence is the same run to run and on any machine */
static uint64_t seme = 0x9E3779B97F4A7C15ull;
static uint32_t casuale(void)
{
    seme ^= seme >> 12;
    seme ^= seme << 25;
    seme ^= seme >> 27;
    return (uint32_t)((seme * 0x2545F4914F6CDD1Dull) >> 32);
}

#define MAX_BIN 200000

static int prova_init(void)
{
    int guasti = 0;
    for (int intra = 0; intra <= 1; intra++) {
        for (int qp = 0; qp <= 51; qp++) {
            cabac_engine_t enc;
            uint8_t nostro[H264D_CABAC_CTX];
            memset(&enc, 0, sizeof(enc));
            cabac_context_init(&enc, intra != 0, 0, qp);
            h264d_cabac_ctx_init(nostro, intra != 0, 0, qp);
            /* Every one of the encoder's 276 contexts has to come out the
             * same, the ones it zeroes included: the encoder writes (0,0)
             * for entries it never uses, and (0,0) is what the standard's
             * table holds there too, so the two agree on those as well. */
            for (int i = 0; i < CABAC_NUM_CTX; i++) {
                if (nostro[i] != enc.state[i]) {
                    if (guasti < 10)
                        printf("  init intra=%d qp=%d ctx=%d: encoder %u, noi %u\n",
                               intra, qp, i, enc.state[i], nostro[i]);
                    guasti++;
                }
            }
        }
    }
    return guasti;
}

static int prova_motore(int giro)
{
    static uint8_t bin[MAX_BIN];
    static uint16_t ctx[MAX_BIN];
    static uint8_t tipo[MAX_BIN];     /* 0 decision, 1 bypass, 2 terminate-0 */
    static uint8_t buf[4 * MAX_BIN + 64];

    int n = 1000 + (int)(casuale() % (MAX_BIN - 1001));
    int qp = (int)(casuale() % 52);
    int intra = (int)(casuale() & 1);

    cabac_engine_t enc;
    memset(&enc, 0, sizeof(enc));
    cabac_context_init(&enc, intra != 0, 0, qp);
    uint8_t stato_iniziale[H264D_CABAC_CTX];
    memset(stato_iniziale, 0, sizeof(stato_iniziale));
    memcpy(stato_iniziale, enc.state, CABAC_NUM_CTX);

    cabac_engine_init(&enc, buf, buf + sizeof(buf));

    for (int i = 0; i < n; i++) {
        uint32_t r = casuale();
        /* One bin in eight bypasses, one in sixty-four is an end_of_slice
         * flag saying "carry on" - the same mix a real slice has, where a
         * terminate-0 follows every macroblock. */
        tipo[i] = ((r & 63) == 0) ? 2 : (((r & 7) == 0) ? 1 : 0);
        ctx[i] = (uint16_t)((r >> 3) % CABAC_NUM_CTX);
        /* Bins that are not uniformly random, so the contexts actually skew
         * and the state machine gets exercised rather than sitting still. */
        bin[i] = (uint8_t)(((r >> 16) % 100) < 30 ? 1 : 0);
        if (tipo[i] == 2)
            cabac_encode_terminal(&enc);
        else if (tipo[i] == 1)
            cabac_encode_bypass(&enc, bin[i]);
        else
            cabac_encode_decision(&enc, ctx[i], bin[i]);
    }
    /* The final end_of_slice_flag is the value-1 termination, and the
     * encoder folds it into the flush rather than coding it separately. */
    size_t len = cabac_encode_flush(&enc, 0);
    if (enc.overflow) {
        printf("  giro %d: l'encoder e' andato in overflow\n", giro);
        return 1;
    }

    h264d_cabac_t dec;
    h264d_cabac_init(&dec, buf, len, intra != 0, 0, qp);
    memcpy(dec.state, stato_iniziale, H264D_CABAC_CTX);

    static const char *nome_tipo[3] = { "decision", "bypass", "terminate" };
    for (int i = 0; i < n; i++) {
        int atteso = (tipo[i] == 2) ? 0 : bin[i];
        int b = (tipo[i] == 2) ? h264d_cabac_terminate(&dec)
              : (tipo[i] == 1) ? h264d_cabac_bypass(&dec)
                               : h264d_cabac_decision(&dec, ctx[i]);
        if (b != atteso) {
            printf("  giro %d: bin %d di %d diverso (atteso %d, letto %d), "
                   "tipo %s, ctx %d, qp %d, intra %d\n",
                   giro, i, n, atteso, b, nome_tipo[tipo[i]],
                   ctx[i], qp, intra);
            return 1;
        }
    }
    if (!h264d_cabac_terminate(&dec)) {
        printf("  giro %d: il terminate non ha chiuso la slice\n", giro);
        return 1;
    }
    if (h264d_cabac_overrun(&dec)) {
        printf("  giro %d: il decoder ha letto oltre la fine\n", giro);
        return 1;
    }
    return 0;
}

int main(void)
{
    printf("1. inizializzazione dei contesti\n");
    int g = prova_init();
    if (g) {
        printf("   %d differenze\n", g);
        return 1;
    }
    printf("   276 contesti x 52 QP x intra/inter: tutti uguali all'encoder\n");

    printf("2. motore aritmetico\n");
    long bin_totali = 0;
    for (int giro = 0; giro < 200; giro++) {
        if (prova_motore(giro))
            return 1;
        bin_totali += 1;
    }
    printf("   200 giri, andata e ritorno identici\n");
    printf("\nOK\n");
    (void)bin_totali;
    return 0;
}
