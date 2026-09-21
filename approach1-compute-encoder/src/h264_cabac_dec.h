/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_cabac_dec.h - the CABAC arithmetic decoding engine, Rec. ITU-T H.264
 * clause 9.3.3.2.
 *
 * The encoder's engine lives in cabac.c and the two share the normative
 * tables (rangeTabLPS and the packed transition table) but nothing else: an
 * arithmetic coder and its decoder are not symmetric enough for shared code
 * to be worth the indirection in either hot loop.
 *
 * State packing follows the encoder's, which follows x264's: one byte per
 * context holding (pStateIdx << 1) | valMPS, where pStateIdx is the
 * standard's index counted backwards (x264 index = 63 - spec index), so that
 * the most skewed state sits at 0. cabac_context_init() in cabac.c produces
 * exactly this packing.
 *
 * ⚠️ Renormalisation here uses a leading-zero count, not the encoder's
 * cabac_renorm_shift[] lookup. That table is indexed by range>>3 and its
 * entry 0 covers ranges 2..7 with a single shift of 6, which is only correct
 * from 4 upwards - range 2 would renormalise to 128 and the next decision's
 * `(range >> 6) - 4` would index the LPS table at -2. It is dead code in
 * practice, because x264 state 0 (the only one with rangeLPS = 2) is
 * unreachable: the transition table's rows 0 and 1 are absorbing, no other
 * row points into them, and context initialisation produces pStateIdx in
 * 1..63. A decoder eats attacker-shaped bytes, though, so it does not get to
 * rely on that; clz is exact for every range and costs one instruction.
 */
#ifndef BC250_H264_CABAC_DEC_H
#define BC250_H264_CABAC_DEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cabac.h"            /* cabac_range_lps, cabac_transition */
#include "h264_dec_tables.h"  /* h264d_cabac_init_I, h264d_cabac_init_PB */

#define H264D_CABAC_CTX 1024

typedef struct {
    uint32_t range;          /* codIRange, invariant 256..510 */
    uint32_t low;            /* codIOffset, invariant below range */

    uint64_t cache;          /* bits read ahead, most significant first */
    int      cache_bits;     /* how many of them are still unused */
    const uint8_t *start;
    const uint8_t *ptr;      /* next byte to pull into the cache */
    const uint8_t *end;

    uint8_t  state[H264D_CABAC_CTX];
} h264d_cabac_t;

/* Past the end we feed zero bytes rather than stopping. A slice that reads
 * beyond its own data is corrupt and its macroblocks are discarded, but the
 * decode loop has to be able to keep running until the slice ends rather
 * than unwinding out of the middle of a macroblock. */
static inline void h264d_cabac_refill(h264d_cabac_t *c)
{
    while (c->cache_bits <= 56) {
        uint8_t b = (c->ptr < c->end) ? *c->ptr++ : 0;
        c->cache = (c->cache << 8) | (uint64_t)b;
        c->cache_bits += 8;
    }
}

/* Whether the slice has actually been decoded past its own end.
 *
 * ⚠️ Not the same question as "has a byte past the end been touched". The
 * engine keeps up to eight bytes in its cache, so it reads ahead as a matter
 * of course and reaches the last byte of a perfectly good slice long before
 * it has consumed it. What counts is how many bits have been handed out,
 * which is what this computes. Checking the pointer instead flags every
 * healthy slice.
 */
static inline bool h264d_cabac_overrun(const h264d_cabac_t *c)
{
    ptrdiff_t letti = (c->ptr - c->start) * 8 - c->cache_bits;
    return letti > (ptrdiff_t)(c->end - c->start) * 8;
}

static inline uint32_t h264d_cabac_bits(h264d_cabac_t *c, int n)
{
    if (n <= 0) return 0;
    if (c->cache_bits < n) h264d_cabac_refill(c);
    c->cache_bits -= n;
    return (uint32_t)(c->cache >> c->cache_bits) & ((1u << n) - 1u);
}

/* Clause 9.3.1.1, context variable initialisation.
 *
 * The encoder's cabac_context_init() does the same arithmetic but only over
 * the 276 contexts it can write and only with cabac_init_idc 0, because an
 * encoder picks its own idc. A decoder is told which one to use and has to
 * have all 1024 contexts of all four tables.
 */
static inline void h264d_cabac_ctx_init(uint8_t *state, bool intra_slice,
                                        int cabac_init_idc, int qp)
{
    if (qp < 0) qp = 0;
    if (qp > 51) qp = 51;
    if (cabac_init_idc < 0) cabac_init_idc = 0;
    if (cabac_init_idc > 2) cabac_init_idc = 2;

    const int8_t (*t)[2] = intra_slice ? h264d_cabac_init_I
                                       : h264d_cabac_init_PB[cabac_init_idc];
    for (int j = 0; j < H264D_CABAC_CTX; j++) {
        int s = ((t[j][0] * qp) >> 4) + t[j][1];
        s = s < 1 ? 1 : (s > 126 ? 126 : s);
        int p = s < 127 - s ? s : 127 - s;   /* x264's index: 63 - spec's */
        state[j] = (uint8_t)((p << 1) | (s >= 64 ? 1 : 0));
    }
}

/* Clause 9.3.1.2. The caller has already skipped the alignment bits, so
 * `data` starts on a byte boundary. */
static inline void h264d_cabac_init(h264d_cabac_t *c,
                                    const uint8_t *data, size_t size,
                                    bool intra_slice, int cabac_init_idc, int qp)
{
    c->start = data;
    c->ptr = data;
    c->end = data + size;
    c->cache = 0;
    c->cache_bits = 0;
    c->range = 510;
    c->low = h264d_cabac_bits(c, 9);
    h264d_cabac_ctx_init(c->state, intra_slice, cabac_init_idc, qp);
}

static inline void h264d_cabac_renorm(h264d_cabac_t *c)
{
    /* range is at least 2 here, so clz is defined. 31 - clz(range) is the
     * index of its top bit; the shift brings that bit to position 8. */
    int sh = (int)__builtin_clz(c->range) - 23;
    if (sh > 0) {
        c->range <<= sh;
        c->low = (c->low << sh) | h264d_cabac_bits(c, sh);
    }
}

/* Clause 9.3.3.2.1, DecodeDecision. */
static inline int h264d_cabac_decision(h264d_cabac_t *c, int ctx)
{
    unsigned s = c->state[ctx];
    uint32_t rlps = cabac_range_lps[s >> 1][(c->range >> 6) - 4];
    c->range -= rlps;
    int bin;
    if (c->low >= c->range) {
        c->low -= c->range;
        c->range = rlps;
        bin = (int)(~s & 1u);
    } else {
        bin = (int)(s & 1u);
    }
    c->state[ctx] = cabac_transition[s][bin];
    h264d_cabac_renorm(c);
    return bin;
}

/* Clause 9.3.3.2.3, DecodeBypass. No context, no renormalisation of range. */
static inline int h264d_cabac_bypass(h264d_cabac_t *c)
{
    c->low = (c->low << 1) | h264d_cabac_bits(c, 1);
    if (c->low >= c->range) {
        c->low -= c->range;
        return 1;
    }
    return 0;
}

/* Several bypass bins at once, which is what the sign and suffix bits of a
 * coefficient level need. Equivalent to calling the single-bin version n
 * times, but it touches `low` once per bin without the branch being
 * mispredicted on data that is by construction incompressible. */
static inline uint32_t h264d_cabac_bypass_n(h264d_cabac_t *c, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (uint32_t)h264d_cabac_bypass(c);
    return v;
}

/* Clause 9.3.3.2.4, DecodeTerminate. Returns 1 when the slice ends here, in
 * which case the engine must not be used again. */
static inline int h264d_cabac_terminate(h264d_cabac_t *c)
{
    c->range -= 2;
    if (c->low >= c->range)
        return 1;
    h264d_cabac_renorm(c);
    return 0;
}

/* Unary with a cap, the shape most of the mb-layer syntax elements take. */
static inline int h264d_cabac_unary(h264d_cabac_t *c, const int *ctx, int n_ctx, int cap)
{
    int v = 0;
    while (v < cap && h264d_cabac_decision(c, ctx[v < n_ctx ? v : n_ctx - 1]))
        v++;
    return v;
}

/* Exp-Golomb suffix in bypass mode, clause 9.3.2.3. Used by the coefficient
 * levels and the motion vector differences once their unary prefix has run
 * out. */
static inline uint32_t h264d_cabac_eg_bypass(h264d_cabac_t *c, int k)
{
    uint32_t v = 0;
    while (h264d_cabac_bypass(c)) {
        v += 1u << k;
        k++;
        if (k > 30) break;          /* a corrupt stream must not spin here */
    }
    return v + h264d_cabac_bypass_n(c, k);
}

#endif /* BC250_H264_CABAC_DEC_H */
