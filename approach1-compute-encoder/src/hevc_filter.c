/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_filter.c - the deblocking filter, Rec. ITU-T H.265 clause 8.7.2.
 *
 * Block transforms leave steps at the block edges, and the eye finds a
 * straight edge that is not in the picture far more readily than it finds
 * the error that produced it. So the standard smooths them - and does it
 * normatively, in the decoding loop, because the filtered picture is what
 * the next one predicts from. A decoder that skips it does not merely look
 * worse: it drifts.
 *
 * ⚠️ Every vertical edge in the picture is filtered before any horizontal
 * one, and the horizontal pass reads what the vertical pass wrote. Doing
 * it edge by edge, both directions at once, gives a different picture.
 * Two passes over the whole picture is the simplest way to be sure, and
 * the passes are independent inside themselves: the edges of one direction
 * are eight samples apart and reach four, so no two of them touch.
 */
#include "hevc_dec_internal.h"

#include <stdlib.h>
#include <string.h>

/* The edge offset at eight bits, sixteen samples at a time. SSSE3 for
 * _mm_shuffle_epi8, which looks the five cases up in one instruction; the
 * decoder asks for it at run time, as hevc_mc.c does, and BC250_HEVC_NOSIMD
 * turns it off to compare with the scalar loop beside it.
 *
 * ⚠️ The samples are compared as signed bytes after flipping the top bit,
 * which orders them the same way as unsigned ones: SSE has no unsigned
 * byte comparison. */
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

static int sao_vector(void)
{
    static int answer = -1;
    if (answer < 0)
        answer = !getenv("BC250_HEVC_NOSIMD")
                 && __builtin_cpu_supports("ssse3") ? 1 : 0;
    return answer;
}

__attribute__((target("ssse3")))
static void sao_edge_row_ssse3(uint8_t *out, const uint8_t *cur,
                               const uint8_t *a, const uint8_t *b, int n,
                               const int8_t table[16])
{
    const __m128i flip = _mm_set1_epi8((char)0x80);
    const __m128i two = _mm_set1_epi8(2);
    const __m128i zero = _mm_setzero_si128();
    const __m128i lut = _mm_loadu_si128((const __m128i *)table);
    int x = 0;
    for (; x + 16 <= n; x += 16) {
        const __m128i v = _mm_loadu_si128((const __m128i *)(cur + x));
        const __m128i vs = _mm_xor_si128(v, flip);
        const __m128i as = _mm_xor_si128(
            _mm_loadu_si128((const __m128i *)(a + x)), flip);
        const __m128i bs = _mm_xor_si128(
            _mm_loadu_si128((const __m128i *)(b + x)), flip);
        /* sign(v - a): the two comparisons are all ones where true. */
        const __m128i sa = _mm_sub_epi8(_mm_cmpgt_epi8(as, vs),
                                        _mm_cmpgt_epi8(vs, as));
        const __m128i sb = _mm_sub_epi8(_mm_cmpgt_epi8(bs, vs),
                                        _mm_cmpgt_epi8(vs, bs));
        const __m128i idx = _mm_add_epi8(_mm_add_epi8(sa, sb), two);
        const __m128i off = _mm_shuffle_epi8(lut, idx);
        const __m128i neg = _mm_cmpgt_epi8(zero, off);
        const __m128i lo = _mm_add_epi16(_mm_unpacklo_epi8(v, zero),
                                         _mm_unpacklo_epi8(off, neg));
        const __m128i hi = _mm_add_epi16(_mm_unpackhi_epi8(v, zero),
                                         _mm_unpackhi_epi8(off, neg));
        _mm_storeu_si128((__m128i *)(out + x), _mm_packus_epi16(lo, hi));
    }
    for (; x < n; x++) {
        const int v = cur[x];
        const int idx = 2 + (v > a[x]) - (v < a[x]) + (v > b[x]) - (v < b[x]);
        const int r = v + table[idx];
        out[x] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
    }
}
#endif

#define BIT_DEPTH 8
#include "hevc_pixel.h"
#include "hevc_filter_template.c"
#undef BIT_DEPTH

#define BIT_DEPTH 10
#include "hevc_pixel.h"
#include "hevc_filter_template.c"
#undef BIT_DEPTH

void hevcd_deblock(hevcd_t *d)
{
    if (d->sps->bit_depth_luma > 8) deblock_10(d);
    else                            deblock_8(d);
}

void hevcd_sao(hevcd_t *d)
{
    if (d->sps->bit_depth_luma > 8) sao_10(d);
    else                            sao_8(d);
}

void hevcd_free_filters(hevcd_t *d)
{
    free(d->sao);
    d->sao = NULL;
    d->n_sao = 0;
    for (int c = 0; c < 3; c++) { free(d->copy_of[c]); d->copy_of[c] = NULL; }
    d->n_copy = 0;
}
