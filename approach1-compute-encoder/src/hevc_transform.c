/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_transform.c - dequantisation and the inverse transforms,
 * Rec. ITU-T H.265 clauses 8.6.2 to 8.6.4.
 *
 * H.265 has one transform matrix, not four. The 32-point DCT is written
 * out in the standard and the 16-point one is its even rows, the 8-point
 * the even rows of those, and the 4-point the even rows of those again -
 * which is why every size can be read out of the same table with a stride.
 *
 * The one exception is a 4x4 intra luma block, which uses a DST instead.
 * Intra residuals grow away from the reference samples rather than being
 * flat, and the DST's first basis function does the same; at four samples
 * that is worth its own transform, and above four it stops being.
 */
#include "hevc_dec_internal.h"

#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

static inline int ritaglia16(int v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

static inline uint8_t ritaglia8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* 8.6.3. m[x][y] is 16 throughout: a stream with its own quantisation
 * matrices is refused at the parameter set. */
void hevcd_dequantizza(int16_t *coeff, int log2_size, int qp)
{
    const int shift = 8 + log2_size - 5;       /* BitDepth + Log2(nTbS) - 5 */
    const int add = 1 << (shift - 1);
    const int64_t scala = (int64_t)hevcd_level_scale[qp % 6] << (qp / 6);
    const int quanti = 1 << (2 * log2_size);

    for (int i = 0; i < quanti; i++) {
        if (!coeff[i]) continue;
        const int64_t v = ((int64_t)coeff[i] * scala * 16 + add) >> shift;
        coeff[i] = (int16_t)ritaglia16((int)(v < -32768 ? -32768
                                             : (v > 32767 ? 32767 : v)));
    }
}

/* 8.6.4.3: one line of the transform, over `n` points.
 *
 * ⚠️ The matrix row used is k * (32 / n), which is what "the even rows of
 * the even rows" means in practice. Reading hevcd_dct[k] instead would be
 * a 32-point transform truncated, which is a different function. */
static void linea(const int16_t *src, int passo, int32_t *fuori, int n)
{
    const int salto = 32 / n;

    /* Which inputs are not zero, and which matrix row each one reaches
     * for. Everything else contributes nothing to any output. */
    const int8_t *riga_m[32];
    int val[32];
    int quanti = 0;
    for (int k = 0; k < n; k++) {
        const int c = src[k * passo];
        if (!c) continue;
        val[quanti] = c;
        riga_m[quanti] = hevcd_dct[k * salto];
        quanti++;
    }
    if (!quanti) {
        memset(fuori, 0, (size_t)n * sizeof *fuori);
        return;
    }

#if defined(__x86_64__) || defined(_M_X64)
    /* Eight outputs at a time. The matrix is eight-bit and the input
     * sixteen, so each entry widens to sixteen and _mm_madd_epi16 carries
     * the product in thirty-two - the same pairing with a zero that the
     * weighted prediction uses, and for the same reason. */
    if (n >= 8) {
        const __m128i zero = _mm_setzero_si128();
        for (int i = 0; i < n; i += 8) {
            __m128i lo = zero, alto = zero;
            for (int j = 0; j < quanti; j++) {
                const __m128i cv =
                    _mm_set1_epi32((int32_t)(uint32_t)(uint16_t)val[j]);
                const __m128i m8 =
                    _mm_loadl_epi64((const __m128i *)(riga_m[j] + i));
                const __m128i m16 =
                    _mm_srai_epi16(_mm_unpacklo_epi8(m8, m8), 8);
                lo = _mm_add_epi32(lo, _mm_madd_epi16(
                        _mm_unpacklo_epi16(m16, zero), cv));
                alto = _mm_add_epi32(alto, _mm_madd_epi16(
                        _mm_unpackhi_epi16(m16, zero), cv));
            }
            _mm_storeu_si128((__m128i *)(fuori + i), lo);
            _mm_storeu_si128((__m128i *)(fuori + i + 4), alto);
        }
        return;
    }
#endif

    for (int i = 0; i < n; i++) {
        int32_t s = 0;
        for (int j = 0; j < quanti; j++)
            s += (int32_t)riga_m[j][i] * val[j];
        fuori[i] = s;
    }
}

/* The DST of 8.6.4.2, for a 4x4 intra luma block. Written as the standard
 * factors it rather than as a matrix product: four multiplications instead
 * of sixteen, and the same numbers. */
static void dst4(const int16_t *src, int passo, int32_t *fuori)
{
    const int c0 = src[0] + src[2 * passo];
    const int c1 = src[2 * passo] + src[3 * passo];
    const int c2 = src[0] - src[3 * passo];
    const int c3 = 74 * src[passo];

    fuori[0] = 29 * c0 + 55 * c1 + c3;
    fuori[1] = 55 * c2 - 29 * c1 + c3;
    /* ⚠️ x0 - x2 + x3, and none of x1. The factored form makes it easy to
     * write x1 in here by mistake, and the result is right in three of the
     * four outputs - which is exactly wrong enough to look like something
     * else's fault. */
    fuori[2] = 74 * (src[0] - src[2 * passo] + src[3 * passo]);
    fuori[3] = 55 * c0 + 29 * c2 - c3;
}

/* 8.6.4.2: columns first with a shift of seven, then rows with what is
 * left. Both stages clip to sixteen bits, which the standard says and
 * which matters: the intermediate really can leave the range. */
void hevcd_trasforma(int16_t *coeff, int log2_size, bool dst)
{
    const int n = 1 << log2_size;
    int16_t tmp[32 * 32];
    int32_t riga[32];

    for (int x = 0; x < n; x++) {
        if (dst) dst4(coeff + x, n, riga);
        else     linea(coeff + x, n, riga, n);
        for (int y = 0; y < n; y++)
            tmp[y * n + x] = (int16_t)ritaglia16((riga[y] + 64) >> 7);
    }

    const int shift = 20 - 8;
    const int add = 1 << (shift - 1);
    for (int y = 0; y < n; y++) {
        if (dst) dst4(tmp + y * n, 1, riga);
        else     linea(tmp + y * n, 1, riga, n);
        for (int x = 0; x < n; x++)
            coeff[y * n + x] = (int16_t)ritaglia16((riga[x] + add) >> shift);
    }
}

/* 8.6.2: a block whose transform was skipped is scaled and nothing else.
 * The seven and the final shift are the two stages the transform would
 * have done, with the transform taken out from between them. */
void hevcd_salta_trasformata(int16_t *coeff, int log2_size)
{
    const int quanti = 1 << (2 * log2_size);
    const int shift = 20 - 8;
    const int add = 1 << (shift - 1);
    for (int i = 0; i < quanti; i++)
        coeff[i] = (int16_t)ritaglia16((((int)coeff[i] << 7) + add) >> shift);
}

/* The residual onto the prediction, clipped back into eight bits. */
void hevcd_aggiungi(uint8_t *dst, int passo, const int16_t *res, int log2_size)
{
    const int n = 1 << log2_size;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            dst[y * passo + x] = ritaglia8(dst[y * passo + x] + res[y * n + x]);
}
