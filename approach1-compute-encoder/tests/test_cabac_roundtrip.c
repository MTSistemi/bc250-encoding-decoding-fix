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
static uint64_t seed = 0x9E3779B97F4A7C15ull;
static uint32_t random_u32(void)
{
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return (uint32_t)((seed * 0x2545F4914F6CDD1Dull) >> 32);
}

#define MAX_BIN 200000

static int test_init(void)
{
    int faults = 0;
    for (int intra = 0; intra <= 1; intra++) {
        for (int qp = 0; qp <= 51; qp++) {
            cabac_engine_t enc;
            uint8_t ours[H264D_CABAC_CTX];
            memset(&enc, 0, sizeof(enc));
            cabac_context_init(&enc, intra != 0, 0, qp);
            h264d_cabac_ctx_init(ours, intra != 0, 0, qp);
            /* Every one of the encoder's 276 contexts has to come out the
             * same, the ones it zeroes included: the encoder writes (0,0)
             * for entries it never uses, and (0,0) is what the standard's
             * table holds there too, so the two agree on those as well. */
            for (int i = 0; i < CABAC_NUM_CTX; i++) {
                if (ours[i] != enc.state[i]) {
                    if (faults < 10)
                        printf("  init intra=%d qp=%d ctx=%d: encoder %u, noi %u\n",
                               intra, qp, i, enc.state[i], ours[i]);
                    faults++;
                }
            }
        }
    }
    return faults;
}

static int test_engine(int pass_index)
{
    static uint8_t bin[MAX_BIN];
    static uint16_t ctx[MAX_BIN];
    static uint8_t kind[MAX_BIN];     /* 0 decision, 1 bypass, 2 terminate-0 */
    static uint8_t buf[4 * MAX_BIN + 64];

    int n = 1000 + (int)(random_u32() % (MAX_BIN - 1001));
    int qp = (int)(random_u32() % 52);
    int intra = (int)(random_u32() & 1);

    cabac_engine_t enc;
    memset(&enc, 0, sizeof(enc));
    cabac_context_init(&enc, intra != 0, 0, qp);
    uint8_t initial_state[H264D_CABAC_CTX];
    memset(initial_state, 0, sizeof(initial_state));
    memcpy(initial_state, enc.state, CABAC_NUM_CTX);

    cabac_engine_init(&enc, buf, buf + sizeof(buf));

    for (int i = 0; i < n; i++) {
        uint32_t r = random_u32();
        /* One bin in eight bypasses, one in sixty-four is an end_of_slice
         * flag saying "carry on" - the same mix a real slice has, where a
         * terminate-0 follows every macroblock. */
        kind[i] = ((r & 63) == 0) ? 2 : (((r & 7) == 0) ? 1 : 0);
        ctx[i] = (uint16_t)((r >> 3) % CABAC_NUM_CTX);
        /* Bins that are not uniformly random, so the contexts actually skew
         * and the state machine gets exercised rather than sitting still. */
        bin[i] = (uint8_t)(((r >> 16) % 100) < 30 ? 1 : 0);
        if (kind[i] == 2)
            cabac_encode_terminal(&enc);
        else if (kind[i] == 1)
            cabac_encode_bypass(&enc, bin[i]);
        else
            cabac_encode_decision(&enc, ctx[i], bin[i]);
    }
    /* The final end_of_slice_flag is the value-1 termination, and the
     * encoder folds it into the flush rather than coding it separately. */
    size_t len = cabac_encode_flush(&enc, 0);
    if (enc.overflow) {
        printf("  round %d: the encoder overflowed\n", pass_index);
        return 1;
    }

    h264d_cabac_t dec;
    h264d_cabac_init(&dec, buf, len, intra != 0, 0, qp);
    memcpy(dec.state, initial_state, H264D_CABAC_CTX);

    static const char *type_name[3] = { "decision", "bypass", "terminate" };
    for (int i = 0; i < n; i++) {
        int expected = (kind[i] == 2) ? 0 : bin[i];
        int b = (kind[i] == 2) ? h264d_cabac_terminate(&dec)
              : (kind[i] == 1) ? h264d_cabac_bypass(&dec)
                               : h264d_cabac_decision(&dec, ctx[i]);
        if (b != expected) {
            printf("  round %d: bin %d of %d differs (expected %d, read %d), "
                   "type %s, ctx %d, qp %d, intra %d\n",
                   pass_index, i, n, expected, b, type_name[kind[i]],
                   ctx[i], qp, intra);
            return 1;
        }
    }
    if (!h264d_cabac_terminate(&dec)) {
        printf("  round %d: terminate did not close the slice\n", pass_index);
        return 1;
    }
    if (h264d_cabac_overrun(&dec)) {
        printf("  round %d: the decoder read past the end\n", pass_index);
        return 1;
    }
    return 0;
}

int main(void)
{
    printf("1. context initialisation\n");
    int g = test_init();
    if (g) {
        printf("   %d differences\n", g);
        return 1;
    }
    printf("   276 contexts x 52 QP x intra/inter: all equal to the encoder\n");

    printf("2. the arithmetic engine\n");
    long total_bins = 0;
    for (int pass_index = 0; pass_index < 200; pass_index++) {
        if (test_engine(pass_index))
            return 1;
        total_bins += 1;
    }
    printf("   200 rounds, out and back identical\n");
    printf("\nOK\n");
    (void)total_bins;
    return 0;
}
