/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_mc.c - fetching the samples a motion vector points at,
 * Rec. ITU-T H.265 clause 8.5.3.3.
 *
 * A motion vector points a quarter of a sample at a time for luma and an
 * eighth for chroma, so most of the time it points between samples and the
 * ones in between have to be made up. H.265 makes them with an eight-tap
 * filter where H.264 used six, which is most of the reason its motion
 * compensation is sharper and most of the reason it is slower.
 *
 * ⚠️ Everything in here works at fourteen bits and clips to eight only at
 * the very end. Two predictions averaged after each has been rounded to
 * eight bits are wrong by half a level per sample, everywhere, for ever.
 */
#include "hevc_dec_internal.h"

#include <string.h>

#define LATO_MAX 64

static inline uint8_t ritaglia8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* ⚠️ A motion vector may point off the edge of the reference picture, and
 * legitimately: an object entering the frame was not there before. The
 * edge sample is repeated outwards rather than the fetch being refused. */
static inline int campione(const uint8_t *p, int passo, int w, int h,
                           int x, int y)
{
    x = x < 0 ? 0 : (x >= w ? w - 1 : x);
    y = y < 0 ? 0 : (y >= h ? h - 1 : y);
    return p[(size_t)y * passo + x];
}

/* ------------------------------------------------- the vector paths */

/* SSE2 is part of the x86-64 ABI, so the two stages that take fourteen
 * bits back down to eight need no runtime check. The filter itself wants
 * _mm_maddubs_epi16, which is SSSE3, and asks first.
 *
 * ⚠️ Nothing here is allowed to disagree with the scalar twin beside it by
 * so much as a level. Both are exercised by the same suites, which compare
 * whole sequences with ffmpeg byte for byte, so a vector path that gets an
 * order or a shift wrong fails loudly instead of quietly softening the
 * picture. That is what makes these safe to write. */
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

static int ha_ssse3(void)
{
    static int risposta = -1;
    if (risposta < 0) risposta = __builtin_cpu_supports("ssse3") ? 1 : 0;
    return risposta;
}

/* Two consecutive taps, broadcast: the shape _mm_maddubs_epi16 wants,
 * which multiplies unsigned samples by signed taps and adds each adjacent
 * pair into sixteen bits. */
__attribute__((target("ssse3")))
static inline __m128i coppia(const int8_t *f, int k)
{
    const uint16_t due_byte = (uint16_t)((uint8_t)f[k])
                            | (uint16_t)((uint8_t)f[k + 1] << 8);
    return _mm_set1_epi16((int16_t)due_byte);
}

/* The same pair as two sixteen-bit lanes, for _mm_madd_epi16 when the
 * samples coming in are already fourteen-bit. */
__attribute__((target("ssse3")))
static inline __m128i coppia32(const int8_t *f, int k)
{
    const uint32_t due_corti = (uint32_t)(uint16_t)(int16_t)f[k]
                             | ((uint32_t)(uint16_t)(int16_t)f[k + 1] << 16);
    return _mm_set1_epi32((int32_t)due_corti);
}

/* ⚠️ Eight outputs of an eight tap filter need fifteen bytes and the load
 * takes sixteen. At the end of a row that sixteenth byte can be one past
 * the end of the reference picture, and a picture whose last row ends on a
 * page boundary would fault on a read the filter never uses. */
__attribute__((target("ssse3")))
static inline __m128i carica(const uint8_t *p, int disponibili)
{
    if (disponibili >= 16) return _mm_loadu_si128((const __m128i *)p);
    uint8_t t[16];
    memset(t, 0, sizeof t);
    memcpy(t, p, (size_t)disponibili);
    return _mm_loadu_si128((const __m128i *)t);
}

/* Along a row, eight or four taps, eight outputs at a time.
 *
 * The taps of one output overlap the taps of the next, so one load covers
 * all eight: _mm_shuffle_epi8 lays out the pair each output needs for tap
 * k and k+1, and four (or two) maddubs and three (or one) adds finish it.
 *
 * ⚠️ maddubs saturates. It cannot bite here - the largest H.265 luma
 * filter sums to 112, so a pair reaches at most 75 * 255 and the whole
 * sum 112 * 255, both inside sixteen bits - and that is why the adds may
 * be plain wrapping adds that match the scalar truncation exactly. */
#define ORIZZONTALE_V(nome, N, SCALARE)                                       \
__attribute__((target("ssse3")))                                              \
static void nome(const uint8_t *src, int sp, int w, int h,                    \
                 const int8_t *f, int16_t *fuori, int pf)                     \
{                                                                             \
    const __m128i c0 = coppia(f, 0), c2 = coppia(f, 2);                       \
    const __m128i c4 = (N) == 8 ? coppia(f, 4) : _mm_setzero_si128();         \
    const __m128i c6 = (N) == 8 ? coppia(f, 6) : _mm_setzero_si128();         \
    const __m128i m0 = _mm_setr_epi8(0, 1, 1, 2, 2, 3, 3, 4,                  \
                                     4, 5, 5, 6, 6, 7, 7, 8);                 \
    const __m128i m2 = _mm_setr_epi8(2, 3, 3, 4, 4, 5, 5, 6,                  \
                                     6, 7, 7, 8, 8, 9, 9, 10);                \
    const __m128i m4 = _mm_setr_epi8(4, 5, 5, 6, 6, 7, 7, 8,                  \
                                     8, 9, 9, 10, 10, 11, 11, 12);            \
    const __m128i m6 = _mm_setr_epi8(6, 7, 7, 8, 8, 9, 9, 10,                 \
                                     10, 11, 11, 12, 12, 13, 13, 14);         \
    for (int r = 0; r < h; r++) {                                             \
        const uint8_t *s = src + (size_t)r * sp;                              \
        int16_t *o = fuori + (size_t)r * pf;                                  \
        int c = 0;                                                            \
        for (; c + 8 <= w; c += 8) {                                          \
            const __m128i v = carica(s + c, w + (N) - 1 - c);                 \
            __m128i a = _mm_maddubs_epi16(_mm_shuffle_epi8(v, m0), c0);       \
            a = _mm_add_epi16(a, _mm_maddubs_epi16(                           \
                    _mm_shuffle_epi8(v, m2), c2));                            \
            if ((N) == 8) {                                                   \
                a = _mm_add_epi16(a, _mm_maddubs_epi16(                       \
                        _mm_shuffle_epi8(v, m4), c4));                        \
                a = _mm_add_epi16(a, _mm_maddubs_epi16(                       \
                        _mm_shuffle_epi8(v, m6), c6));                        \
            }                                                                 \
            _mm_storeu_si128((__m128i *)(o + c), a);                          \
        }                                                                     \
        if (c < w) SCALARE(s, o, c, w, f);                                    \
    }                                                                         \
}

/* Down a column, from whole samples. One load per tap row, and
 * _mm_unpacklo_epi8 puts tap k and tap k+1 of the same column side by side
 * where maddubs expects them. */
#define VERTICALE_V(nome, N, SCALARE)                                         \
__attribute__((target("ssse3")))                                              \
static void nome(const uint8_t *src, int sp, int w, int h,                    \
                 const int8_t *f, int16_t *fuori, int pf)                     \
{                                                                             \
    const __m128i c0 = coppia(f, 0), c2 = coppia(f, 2);                       \
    const __m128i c4 = (N) == 8 ? coppia(f, 4) : _mm_setzero_si128();         \
    const __m128i c6 = (N) == 8 ? coppia(f, 6) : _mm_setzero_si128();         \
    for (int r = 0; r < h; r++) {                                             \
        const uint8_t *s = src + (size_t)r * sp;                              \
        int16_t *o = fuori + (size_t)r * pf;                                  \
        int c = 0;                                                            \
        for (; c + 8 <= w; c += 8) {                                          \
            __m128i l[8];                                                     \
            for (int k = 0; k < (N); k++)                                     \
                l[k] = _mm_loadl_epi64(                                       \
                    (const __m128i *)(s + (size_t)k * sp + c));               \
            __m128i a = _mm_maddubs_epi16(_mm_unpacklo_epi8(l[0], l[1]), c0); \
            a = _mm_add_epi16(a, _mm_maddubs_epi16(                           \
                    _mm_unpacklo_epi8(l[2], l[3]), c2));                      \
            if ((N) == 8) {                                                   \
                a = _mm_add_epi16(a, _mm_maddubs_epi16(                       \
                        _mm_unpacklo_epi8(l[4], l[5]), c4));                  \
                a = _mm_add_epi16(a, _mm_maddubs_epi16(                       \
                        _mm_unpacklo_epi8(l[6], l[7]), c6));                  \
            }                                                                 \
            _mm_storeu_si128((__m128i *)(o + c), a);                          \
        }                                                                     \
        if (c < w) SCALARE(s, sp, o, c, w, f);                                \
    }                                                                         \
}

/* Down a column, from the fourteen-bit output of a horizontal pass. These
 * no longer fit in sixteen bits once multiplied, so _mm_madd_epi16 carries
 * them in thirty-two.
 *
 * ⚠️ And the way back down is a shuffle, not a pack. _mm_packs_epi32
 * saturates; the scalar path truncates. They agree on every stream that
 * conforms and part company on one that does not, which is the kind of
 * difference that surfaces years later in a crash report. */
#define VERTICALE16_V(nome, N, SCALARE)                                       \
__attribute__((target("ssse3")))                                              \
static void nome(const int16_t *src, int sp, int w, int h,                    \
                 const int8_t *f, int16_t *fuori, int pf)                     \
{                                                                             \
    const __m128i c0 = coppia32(f, 0), c2 = coppia32(f, 2);                   \
    const __m128i c4 = (N) == 8 ? coppia32(f, 4) : _mm_setzero_si128();       \
    const __m128i c6 = (N) == 8 ? coppia32(f, 6) : _mm_setzero_si128();       \
    const __m128i giu = _mm_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13,               \
                                      -1, -1, -1, -1, -1, -1, -1, -1);        \
    for (int r = 0; r < h; r++) {                                             \
        const int16_t *s = src + (size_t)r * sp;                              \
        int16_t *o = fuori + (size_t)r * pf;                                  \
        int c = 0;                                                            \
        for (; c + 8 <= w; c += 8) {                                          \
            __m128i v[8];                                                     \
            for (int k = 0; k < (N); k++)                                     \
                v[k] = _mm_loadu_si128(                                       \
                    (const __m128i *)(s + (size_t)k * sp + c));               \
            __m128i lo = _mm_madd_epi16(_mm_unpacklo_epi16(v[0], v[1]), c0);  \
            __m128i hi = _mm_madd_epi16(_mm_unpackhi_epi16(v[0], v[1]), c0);  \
            lo = _mm_add_epi32(lo, _mm_madd_epi16(                            \
                    _mm_unpacklo_epi16(v[2], v[3]), c2));                     \
            hi = _mm_add_epi32(hi, _mm_madd_epi16(                            \
                    _mm_unpackhi_epi16(v[2], v[3]), c2));                     \
            if ((N) == 8) {                                                   \
                lo = _mm_add_epi32(lo, _mm_madd_epi16(                        \
                        _mm_unpacklo_epi16(v[4], v[5]), c4));                 \
                hi = _mm_add_epi32(hi, _mm_madd_epi16(                        \
                        _mm_unpackhi_epi16(v[4], v[5]), c4));                 \
                lo = _mm_add_epi32(lo, _mm_madd_epi16(                        \
                        _mm_unpacklo_epi16(v[6], v[7]), c6));                 \
                hi = _mm_add_epi32(hi, _mm_madd_epi16(                        \
                        _mm_unpackhi_epi16(v[6], v[7]), c6));                 \
            }                                                                 \
            lo = _mm_srai_epi32(lo, 6);                                       \
            hi = _mm_srai_epi32(hi, 6);                                       \
            _mm_storeu_si128((__m128i *)(o + c),                              \
                _mm_unpacklo_epi64(_mm_shuffle_epi8(lo, giu),                 \
                                   _mm_shuffle_epi8(hi, giu)));               \
        }                                                                     \
        if (c < w) SCALARE(s, sp, o, c, w, f);                                \
    }                                                                         \
}

/* The tails, for the columns at the right edge that do not fill a
 * register. The same arithmetic, one sample at a time. */
#define CODA_ORIZ(N)                                                          \
static inline void coda_oriz##N(const uint8_t *s, int16_t *o, int c, int w,   \
                                const int8_t *f)                              \
{                                                                             \
    for (; c < w; c++) {                                                      \
        int v = 0;                                                            \
        for (int k = 0; k < (N); k++) v += f[k] * s[c + k];                   \
        o[c] = (int16_t)v;                                                    \
    }                                                                         \
}

#define CODA_VERT(N, TIPO, GIU, suffisso)                                     \
static inline void coda_vert##suffisso(const TIPO *s, int sp, int16_t *o,     \
                                       int c, int w, const int8_t *f)         \
{                                                                             \
    for (; c < w; c++) {                                                      \
        int v = 0;                                                            \
        for (int k = 0; k < (N); k++) v += f[k] * s[(size_t)k * sp + c];      \
        o[c] = (int16_t)(v >> (GIU));                                         \
    }                                                                         \
}

CODA_ORIZ(8)
CODA_ORIZ(4)
CODA_VERT(8, uint8_t, 0, 8)
CODA_VERT(4, uint8_t, 0, 4)
CODA_VERT(8, int16_t, 6, 8_16)
CODA_VERT(4, int16_t, 6, 4_16)

ORIZZONTALE_V(oriz8_v, 8, coda_oriz8)
ORIZZONTALE_V(oriz4_v, 4, coda_oriz4)
VERTICALE_V(vert8_v, 8, coda_vert8)
VERTICALE_V(vert4_v, 4, coda_vert4)
VERTICALE16_V(vert8_16_v, 8, coda_vert8_16)
VERTICALE16_V(vert4_16_v, 4, coda_vert4_16)

/* A motion vector that lands on a whole sample: nothing to filter, just
 * the samples moved up into fourteen bits. */
static void copia14_v(const uint8_t *src, int sp, int w, int h,
                      int16_t *fuori, int pf)
{
    const __m128i zero = _mm_setzero_si128();
    for (int r = 0; r < h; r++) {
        const uint8_t *s = src + (size_t)r * sp;
        int16_t *o = fuori + (size_t)r * pf;
        int c = 0;
        for (; c + 8 <= w; c += 8) {
            const __m128i v = _mm_loadl_epi64((const __m128i *)(s + c));
            _mm_storeu_si128((__m128i *)(o + c),
                _mm_slli_epi16(_mm_unpacklo_epi8(v, zero), 6));
        }
        for (; c < w; c++) o[c] = (int16_t)(s[c] << 6);
    }
}

/* One prediction down to eight bits. ⚠️ Adding 32 in sixteen bits is safe
 * only because the largest fourteen-bit intermediate is 112 * 255. */
static void uno_v(uint8_t *dst, int passo, int w, int h,
                  const int16_t *a, int passo_a)
{
    const __m128i trentadue = _mm_set1_epi16(32);
    for (int r = 0; r < h; r++) {
        const int16_t *s = a + (size_t)r * passo_a;
        uint8_t *o = dst + (size_t)r * passo;
        int c = 0;
        for (; c + 8 <= w; c += 8) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + c));
            v = _mm_srai_epi16(_mm_add_epi16(v, trentadue), 6);
            _mm_storel_epi64((__m128i *)(o + c), _mm_packus_epi16(v, v));
        }
        for (; c < w; c++) o[c] = ritaglia8((s[c] + 32) >> 6);
    }
}

/* Two averaged. ⚠️ Two fourteen-bit values added do not fit in sixteen,
 * so this one widens first - which is also why it is the slower of the
 * two and worth having in vectors at all. */
static void due_v(uint8_t *dst, int passo, int w, int h,
                  const int16_t *a, const int16_t *b, int passo_p)
{
    const __m128i sessantaquattro = _mm_set1_epi32(64);
    for (int r = 0; r < h; r++) {
        const int16_t *sa = a + (size_t)r * passo_p;
        const int16_t *sb = b + (size_t)r * passo_p;
        uint8_t *o = dst + (size_t)r * passo;
        int c = 0;
        for (; c + 8 <= w; c += 8) {
            const __m128i va = _mm_loadu_si128((const __m128i *)(sa + c));
            const __m128i vb = _mm_loadu_si128((const __m128i *)(sb + c));
            __m128i lo = _mm_add_epi32(
                _mm_srai_epi32(_mm_unpacklo_epi16(va, va), 16),
                _mm_srai_epi32(_mm_unpacklo_epi16(vb, vb), 16));
            __m128i hi = _mm_add_epi32(
                _mm_srai_epi32(_mm_unpackhi_epi16(va, va), 16),
                _mm_srai_epi32(_mm_unpackhi_epi16(vb, vb), 16));
            lo = _mm_srai_epi32(_mm_add_epi32(lo, sessantaquattro), 7);
            hi = _mm_srai_epi32(_mm_add_epi32(hi, sessantaquattro), 7);
            const __m128i sedici = _mm_packs_epi32(lo, hi);
            _mm_storel_epi64((__m128i *)(o + c),
                             _mm_packus_epi16(sedici, sedici));
        }
        for (; c < w; c++) o[c] = ritaglia8((sa[c] + sb[c] + 64) >> 7);
    }
}

/* 8.5.3.3.4.3 in vectors. The product of a fourteen-bit sample and a
 * weight does not fit in sixteen bits, so _mm_madd_epi16 carries it in
 * thirty-two: pairing each sample with a zero and each weight with a zero
 * turns one multiply-add into exactly the multiply we want, sign and all,
 * with no widening step of its own. */
static void uno_pesato_v(uint8_t *dst, int passo, int w, int h,
                         const int16_t *a, int passo_a,
                         int peso, int off, int den)
{
    const int log2wd = den + 6;
    const __m128i zero = _mm_setzero_si128();
    const __m128i pv = _mm_set1_epi32((int32_t)(uint32_t)(uint16_t)peso);
    const __m128i tondo = _mm_set1_epi32(1 << (log2wd - 1));
    const __m128i ov = _mm_set1_epi32(off);
    const __m128i giu = _mm_cvtsi32_si128(log2wd);

    for (int r = 0; r < h; r++) {
        const int16_t *s = a + (size_t)r * passo_a;
        uint8_t *o = dst + (size_t)r * passo;
        int c = 0;
        for (; c + 8 <= w; c += 8) {
            const __m128i v = _mm_loadu_si128((const __m128i *)(s + c));
            __m128i lo = _mm_madd_epi16(_mm_unpacklo_epi16(v, zero), pv);
            __m128i hi = _mm_madd_epi16(_mm_unpackhi_epi16(v, zero), pv);
            lo = _mm_add_epi32(_mm_sra_epi32(_mm_add_epi32(lo, tondo), giu), ov);
            hi = _mm_add_epi32(_mm_sra_epi32(_mm_add_epi32(hi, tondo), giu), ov);
            const __m128i sedici = _mm_packs_epi32(lo, hi);
            _mm_storel_epi64((__m128i *)(o + c),
                             _mm_packus_epi16(sedici, sedici));
        }
        for (; c < w; c++) {
            const int v = s[c];
            o[c] = ritaglia8(((v * peso + (1 << (log2wd - 1))) >> log2wd) + off);
        }
    }
}

static void due_pesate_v(uint8_t *dst, int passo, int w, int h,
                         const int16_t *a, const int16_t *b, int passo_p,
                         int pa, int pb, int oa, int ob, int den)
{
    const int log2wd = den + 6;
    const __m128i zero = _mm_setzero_si128();
    const __m128i pav = _mm_set1_epi32((int32_t)(uint32_t)(uint16_t)pa);
    const __m128i pbv = _mm_set1_epi32((int32_t)(uint32_t)(uint16_t)pb);
    const __m128i tondo = _mm_set1_epi32((oa + ob + 1) << log2wd);
    const __m128i giu = _mm_cvtsi32_si128(log2wd + 1);

    for (int r = 0; r < h; r++) {
        const int16_t *sa = a + (size_t)r * passo_p;
        const int16_t *sb = b + (size_t)r * passo_p;
        uint8_t *o = dst + (size_t)r * passo;
        int c = 0;
        for (; c + 8 <= w; c += 8) {
            const __m128i va = _mm_loadu_si128((const __m128i *)(sa + c));
            const __m128i vb = _mm_loadu_si128((const __m128i *)(sb + c));
            __m128i lo = _mm_add_epi32(
                _mm_madd_epi16(_mm_unpacklo_epi16(va, zero), pav),
                _mm_madd_epi16(_mm_unpacklo_epi16(vb, zero), pbv));
            __m128i hi = _mm_add_epi32(
                _mm_madd_epi16(_mm_unpackhi_epi16(va, zero), pav),
                _mm_madd_epi16(_mm_unpackhi_epi16(vb, zero), pbv));
            lo = _mm_sra_epi32(_mm_add_epi32(lo, tondo), giu);
            hi = _mm_sra_epi32(_mm_add_epi32(hi, tondo), giu);
            const __m128i sedici = _mm_packs_epi32(lo, hi);
            _mm_storel_epi64((__m128i *)(o + c),
                             _mm_packus_epi16(sedici, sedici));
        }
        for (; c < w; c++)
            o[c] = ritaglia8((sa[c] * pa + sb[c] * pb
                              + ((oa + ob + 1) << log2wd)) >> (log2wd + 1));
    }
}

#endif /* x86-64 */

/* ----------------------------------------------- and the scalar twins */

/* One pass of the filter across a rectangle, reading whole samples along
 * a row.
 *
 * ⚠️ The tap count is a compile time constant in each instance. With it
 * in a variable the compiler keeps neither the eight coefficients in
 * registers nor any chance of doing several samples at once, and this is
 * three quarters of the decoder's time. */
#define ORIZZONTALE(nome, N)                                               \
static void nome(const uint8_t *src, int sp, int w, int h,                 \
                 const int8_t *f, int16_t *fuori, int pf)                  \
{                                                                          \
    for (int r = 0; r < h; r++) {                                          \
        const uint8_t *s = src + (size_t)r * sp;                           \
        int16_t *o = fuori + (size_t)r * pf;                               \
        for (int c = 0; c < w; c++) {                                      \
            int v = 0;                                                     \
            for (int k = 0; k < N; k++) v += f[k] * s[c + k];              \
            o[c] = (int16_t)v;                                             \
        }                                                                  \
    }                                                                      \
}

/* The same down a column. `TIPO` is whole samples on the way in and the
 * fourteen-bit output of a horizontal pass on the way back, `GIU` the
 * shift that takes the second pass back to fourteen bits. */
#define VERTICALE(nome, N, TIPO, GIU)                                      \
static void nome(const TIPO *src, int sp, int w, int h,                    \
                 const int8_t *f, int16_t *fuori, int pf)                  \
{                                                                          \
    for (int r = 0; r < h; r++) {                                          \
        const TIPO *s = src + (size_t)r * sp;                              \
        int16_t *o = fuori + (size_t)r * pf;                               \
        for (int c = 0; c < w; c++) {                                      \
            int v = 0;                                                     \
            for (int k = 0; k < N; k++) v += f[k] * s[(size_t)k * sp + c]; \
            o[c] = (int16_t)(v >> (GIU));                                  \
        }                                                                  \
    }                                                                      \
}

ORIZZONTALE(oriz8, 8)
ORIZZONTALE(oriz4, 4)
VERTICALE(vert8, 8, uint8_t, 0)
VERTICALE(vert4, 4, uint8_t, 0)
VERTICALE(vert8_16, 8, int16_t, 6)
VERTICALE(vert4_16, 4, int16_t, 6)

/* ------------------------------------------------ and which one to use */

#if defined(__x86_64__) || defined(_M_X64)
#define ORIZ(n) (ha_ssse3() ? oriz##n##_v : oriz##n)
#define VERT(n) (ha_ssse3() ? vert##n##_v : vert##n)
#define VERT16(n) (ha_ssse3() ? vert##n##_16_v : vert##n##_16)
#else
#define ORIZ(n) oriz##n
#define VERT(n) vert##n
#define VERT16(n) vert##n##_16
#endif

/* One rectangle of one plane, at a fractional position, into fourteen-bit
 * intermediate values.
 *
 * `prima` is how far back the filter reaches: three samples for the eight
 * taps of luma, one for the four of chroma. */
static void interpola(const uint8_t *rif, int passo, int w_pic, int h_pic,
                      int x, int y, int w, int h, int fx, int fy,
                      const int8_t *filtro, int quanti, int prima,
                      int16_t *fuori, int passo_fuori)
{
    const int8_t *fh = filtro + (size_t)fx * quanti;
    const int8_t *fv = filtro + (size_t)fy * quanti;

    /* The rectangle of whole samples the passes will read: as far back as
     * the filter reaches, and only in the directions it actually filters. */
    const int px = fx ? prima : 0, tx = fx ? quanti : 1;
    const int py = fy ? prima : 0, ty = fy ? quanti : 1;
    const int bx = x - px, by = y - py;
    const int bw = w + tx - 1, bh = h + ty - 1;

    /* ⚠️ A motion vector may point off the edge of the reference picture,
     * and legitimately: an object entering the frame was not there before.
     * The edge sample is repeated outwards rather than the fetch being
     * refused - but deciding that once per tap, eight times per sample,
     * is what made this the slowest thing in the decoder. Decide it once
     * per block instead: either the whole window is inside the picture and
     * the filter reads it where it lies, or the window is copied out once
     * with its edges repeated and the filter reads the copy. */
    const uint8_t *src;
    int sp;
    uint8_t orlo[(LATO_MAX + 7) * (LATO_MAX + 7)];
    if (bx >= 0 && by >= 0 && bx + bw <= w_pic && by + bh <= h_pic) {
        src = rif + (size_t)by * passo + bx;
        sp = passo;
    } else {
        /* The window hangs over an edge. Clamping every sample by itself
         * is how it was written first and it costs more than the filter
         * that reads the result: the row is the same for a whole span of
         * columns, so each output row is a repeat of one sample, a copy
         * of the middle, and a repeat of the last. */
        sp = LATO_MAX + 7;
        const int sinistra = bx < 0 ? (-bx > bw ? bw : -bx) : 0;
        const int dentro_fine = bx + bw > w_pic ? w_pic - bx : bw;
        const int destra = dentro_fine < sinistra ? sinistra : dentro_fine;
        for (int r = 0; r < bh; r++) {
            int sy = by + r;
            sy = sy < 0 ? 0 : (sy >= h_pic ? h_pic - 1 : sy);
            const uint8_t *riga_rif = rif + (size_t)sy * passo;
            uint8_t *o = orlo + (size_t)r * sp;
            if (sinistra > 0) memset(o, riga_rif[0], (size_t)sinistra);
            if (destra > sinistra)
                memcpy(o + sinistra, riga_rif + bx + sinistra,
                       (size_t)(destra - sinistra));
            if (bw > destra)
                memset(o + destra, riga_rif[w_pic - 1], (size_t)(bw - destra));
        }
        src = orlo;
    }

    if (!fx && !fy) {
#if defined(__x86_64__) || defined(_M_X64)
        copia14_v(src, sp, w, h, fuori, passo_fuori);
#else
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++)
                fuori[r * passo_fuori + c] =
                    (int16_t)(src[(size_t)r * sp + c] << 6);
#endif
        return;
    }

    if (!fy) {
        if (quanti == 8) ORIZ(8)(src, sp, w, h, fh, fuori, passo_fuori);
        else             ORIZ(4)(src, sp, w, h, fh, fuori, passo_fuori);
        return;
    }

    if (!fx) {
        if (quanti == 8) VERT(8)(src, sp, w, h, fv, fuori, passo_fuori);
        else             VERT(4)(src, sp, w, h, fv, fuori, passo_fuori);
        return;
    }

    /* Both: horizontally first, over enough extra rows above and below for
     * the vertical pass to have something to stand on. */
    int16_t mezzo[(LATO_MAX + 7) * LATO_MAX];
    const int alte = h + quanti - 1;
    if (quanti == 8) {
        ORIZ(8)(src, sp, w, alte, fh, mezzo, LATO_MAX);
        VERT16(8)(mezzo, LATO_MAX, w, h, fv, fuori, passo_fuori);
    } else {
        ORIZ(4)(src, sp, w, alte, fh, mezzo, LATO_MAX);
        VERT16(4)(mezzo, LATO_MAX, w, h, fv, fuori, passo_fuori);
    }
}

/* ------------------------------------- fourteen bits back down to eight */

static void uno(uint8_t *dst, int passo, int w, int h,
                const int16_t *a, int passo_a)
{
#if defined(__x86_64__) || defined(_M_X64)
    uno_v(dst, passo, w, h, a, passo_a);
#else
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            dst[r * passo + c] = ritaglia8((a[r * passo_a + c] + 32) >> 6);
#endif
}

static void due(uint8_t *dst, int passo, int w, int h,
                const int16_t *a, const int16_t *b, int passo_p)
{
#if defined(__x86_64__) || defined(_M_X64)
    due_v(dst, passo, w, h, a, b, passo_p);
#else
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            dst[r * passo + c] =
                ritaglia8((a[r * passo_p + c] + b[r * passo_p + c] + 64) >> 7);
#endif
}

/* 8.5.3.3.4.3. ⚠️ Used whenever the slice carries a weight table, even
 * where the weight happens to be neutral: for a neutral weight this is
 * the same arithmetic as the plain path, so there is nothing to gain by
 * deciding per block and something to lose by getting the decision
 * wrong. */
static void uno_pesato(uint8_t *dst, int passo, int w, int h,
                       const int16_t *a, int passo_a,
                       int peso, int off, int den)
{
    const int log2wd = den + 6;
#if defined(__x86_64__) || defined(_M_X64)
    /* log2wd is den + 6 and den is never negative, so the other branch is
     * unreachable on any stream the parser accepts. It stays anyway. */
    if (log2wd >= 1) {
        uno_pesato_v(dst, passo, w, h, a, passo_a, peso, off, den);
        return;
    }
#endif
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            const int v = a[r * passo_a + c];
            dst[r * passo + c] = ritaglia8(
                log2wd >= 1 ? (((v * peso + (1 << (log2wd - 1))) >> log2wd) + off)
                            : (v * peso + off));
        }
}

static void due_pesate(uint8_t *dst, int passo, int w, int h,
                       const int16_t *a, const int16_t *b, int passo_p,
                       int pa, int pb, int oa, int ob, int den)
{
    const int log2wd = den + 6;
#if defined(__x86_64__) || defined(_M_X64)
    due_pesate_v(dst, passo, w, h, a, b, passo_p, pa, pb, oa, ob, den);
    (void)log2wd;
    return;
#else
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            dst[r * passo + c] = ritaglia8(
                (a[r * passo_p + c] * pa + b[r * passo_p + c] * pb
                 + ((oa + ob + 1) << log2wd)) >> (log2wd + 1));
#endif
}

/* Does this slice carry a weight table at all. */
static bool pesato(const hevcd_t *d)
{
    return (d->pps->weighted_pred && d->slice->type == 1)
        || (d->pps->weighted_bipred && d->slice->type == 0);
}

void hevcd_predici_inter(hevcd_t *d, int x0, int y0, int w, int h,
                         const hevcd_mvf_t *m)
{
    const hevc_sps_t *sps = d->sps;
    /* ⚠️ On the stack, not static. Sixteen kilobytes is nothing and one
     * shared buffer would be one prediction handed to whichever row asked
     * last. */
    int16_t p[2][LATO_MAX * LATO_MAX];
    const bool usa[2] = { (m->pred_flag & HEVCD_PF_L0) != 0,
                          (m->pred_flag & HEVCD_PF_L1) != 0 };
    const bool pesi = pesato(d);

    for (int piano = 0; piano < 3; piano++) {
        const int giu = piano ? 1 : 0;
        const int pw = w >> giu, ph = h >> giu;
        const int px = x0 >> giu, py = y0 >> giu;
        const int w_pic = sps->width >> giu, h_pic = sps->height >> giu;

        for (int l = 0; l < 2; l++) {
            if (!usa[l]) continue;
            const int i = m->ref_idx[l];
            if (i < 0 || i >= d->n_rif[l] || !d->rif[l][i]) return;
            const hevcd_img_t *r = d->rif[l][i];
            const int mvx = m->mv[l][0], mvy = m->mv[l][1];
            /* ⚠️ Luma counts quarters and chroma eighths. At 4:2:0 the
             * chroma plane is half the size, so the same vector lands on a
             * finer grid there, not a coarser one. */
            const int passi = piano ? 3 : 2;
            interpola(r->piano[piano], r->passo[piano], w_pic, h_pic,
                      px + (mvx >> passi), py + (mvy >> passi), pw, ph,
                      mvx & ((1 << passi) - 1), mvy & ((1 << passi) - 1),
                      piano ? &hevcd_epel[0][0] : &hevcd_qpel[0][0],
                      piano ? 4 : 8, piano ? 1 : 3,
                      p[l], LATO_MAX);
        }

        uint8_t *dst = d->piano[piano] + (size_t)py * d->passo[piano] + px;

        if (!pesi) {
            if (usa[0] && usa[1])
                due(dst, d->passo[piano], pw, ph, p[0], p[1], LATO_MAX);
            else
                uno(dst, d->passo[piano], pw, ph, p[usa[0] ? 0 : 1], LATO_MAX);
            continue;
        }

        const int den = piano ? d->slice->chroma_log2_weight_denom
                              : d->slice->luma_log2_weight_denom;
        int peso[2] = { 1 << den, 1 << den }, off[2] = { 0, 0 };
        for (int l = 0; l < 2; l++) {
            if (!usa[l]) continue;
            const int i = m->ref_idx[l];
            peso[l] = piano ? d->slice->chroma_weight[l][i][piano - 1]
                            : d->slice->luma_weight[l][i];
            off[l] = piano ? d->slice->chroma_offset[l][i][piano - 1]
                           : d->slice->luma_offset[l][i];
        }

        if (usa[0] && usa[1])
            due_pesate(dst, d->passo[piano], pw, ph, p[0], p[1], LATO_MAX,
                       peso[0], peso[1], off[0], off[1], den);
        else {
            const int l = usa[0] ? 0 : 1;
            uno_pesato(dst, d->passo[piano], pw, ph, p[l], LATO_MAX,
                       peso[l], off[l], den);
        }
    }
}
