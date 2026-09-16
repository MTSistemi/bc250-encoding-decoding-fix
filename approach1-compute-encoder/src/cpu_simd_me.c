/* bc250-encoding-decoding-fix v0.4.0 - https://github.com/simpmix/bc250-encoding-decoding-fix */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * cpu_simd_me.c - Multi-threaded SSE2/AVX2 SIMD Motion Estimation for BC-250 Zen 2 CPU
 */

#include "cpu_simd_me.h"
#include <stdlib.h>
#include <string.h>

#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

#define STATIC_MB_THRESHOLD 512
#define EARLY_TERMINATION_COST 768

uint32_t cpu_simd_sad_16x16(const uint8_t *src, int src_stride,
                            const uint8_t *ref, int ref_stride)
{
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
    __m128i acc = _mm_setzero_si128();
    for (int r = 0; r < 16; r++) {
        __m128i s_row = _mm_loadu_si128((const __m128i *)(src + r * src_stride));
        __m128i r_row = _mm_loadu_si128((const __m128i *)(ref + r * ref_stride));
        acc = _mm_add_epi32(acc, _mm_sad_epu8(s_row, r_row));
    }
    uint32_t lo = (uint32_t)_mm_cvtsi128_si32(acc);
    uint32_t hi = (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(acc, 8));
    return lo + hi;
#else
    uint32_t sad = 0;
    for (int r = 0; r < 16; r++) {
        const uint8_t *s = src + r * src_stride;
        const uint8_t *rf = ref + r * ref_stride;
        for (int c = 0; c < 16; c++) {
            int d = (int)s[c] - (int)rf[c];
            sad += (d < 0) ? -d : d;
        }
    }
    return sad;
#endif
}

void cpu_simd_me_config_init(cpu_simd_me_config_t *cfg, uint32_t width, uint32_t height)
{
    if (!cfg) return;
    cfg->width = width;
    cfg->height = height;
    cfg->width_in_mbs = (width + 15) / 16;
    cfg->height_in_mbs = (height + 15) / 16;
    cfg->search_radius = 8;
    cfg->num_threads = 2; /* Strictly limit to 2 worker threads on Zen 2 */
}

typedef struct {
    int x;
    int y;
} ivec2_t;

int cpu_simd_me_search_frame(const uint8_t *src_y, int src_pitch,
                             const uint8_t *ref_y, int ref_pitch,
                             uint32_t width, uint32_t height,
                             gpu_mv_t *out_mvs,
                             const cpu_simd_me_config_t *cfg)
{
    if (!src_y || !ref_y || !out_mvs) return -1;

    uint32_t width_mbs = (width + 15) / 16;
    uint32_t height_mbs = (height + 15) / 16;
    uint32_t max_rad = cfg ? cfg->search_radius : 8;
    if (max_rad < 2) max_rad = 2;
    if (max_rad > 16) max_rad = 16;

    int threads = (cfg && cfg->num_threads > 0) ? cfg->num_threads : 2;
    if (threads > 2) threads = 2; /* Cap at 2 threads to guarantee game CPU headroom */

    const ivec2_t search_pattern[8] = {
        { 0,  1}, { 0, -1}, { 1,  0}, {-1,  0},
        { 1,  1}, {-1, -1}, { 1, -1}, {-1,  1}
    };
    const uint32_t lambda_motion = 5;

#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
    for (int mby = 0; mby < (int)height_mbs; mby++) {
        for (int mbx = 0; mbx < (int)width_mbs; mbx++) {
            uint32_t mb_idx = (uint32_t)mby * width_mbs + (uint32_t)mbx;
            int px = mbx * 16;
            int py = mby * 16;

            const uint8_t *curr_mb = src_y + py * src_pitch + px;
            const uint8_t *ref_mb_zero = ref_y + py * ref_pitch + px;

            /* 1. Fast Zero-Motion Check (SAD at (0, 0)) */
            uint32_t zero_sad = cpu_simd_sad_16x16(curr_mb, src_pitch, ref_mb_zero, ref_pitch);

            /* Early exit if block is static (saves execution time on video/game content) */
            if (zero_sad <= STATIC_MB_THRESHOLD) {
                out_mvs[mb_idx].mvx = 0;
                out_mvs[mb_idx].mvy = 0;
                out_mvs[mb_idx].sad = zero_sad;
                out_mvs[mb_idx]._pad = 0;
                continue;
            }

            /* 2. Hierarchical Adaptive Diamond/Square Search */
            ivec2_t best_mv = {0, 0};
            uint32_t best_cost = zero_sad;

            for (int step = (int)(max_rad / 2); step >= 1; step /= 2) {
                ivec2_t center = best_mv;
                for (int c = 0; c < 8; c++) {
                    int cand_x = center.x + search_pattern[c].x * step;
                    int cand_y = center.y + search_pattern[c].y * step;

                    if (abs(cand_x) > (int)max_rad || abs(cand_y) > (int)max_rad) continue;

                    int test_ref_x = px + cand_x;
                    int test_ref_y = py + cand_y;

                    /* Bounds check against frame edges */
                    if (test_ref_x < 0 || test_ref_x + 16 > (int)width ||
                        test_ref_y < 0 || test_ref_y + 16 > (int)height) {
                        continue;
                    }

                    const uint8_t *cand_ref = ref_y + test_ref_y * ref_pitch + test_ref_x;
                    uint32_t cand_sad = cpu_simd_sad_16x16(curr_mb, src_pitch, cand_ref, ref_pitch);
                    uint32_t cost = cand_sad + lambda_motion * (uint32_t)(abs(cand_x) + abs(cand_y));

                    if (cost < best_cost) {
                        best_cost = cost;
                        best_mv.x = cand_x;
                        best_mv.y = cand_y;
                    }
                }

                if (best_cost < EARLY_TERMINATION_COST) break;
            }

            /* Convert integer motion vector to quarter-pel units (multiply by 4) */
            out_mvs[mb_idx].mvx = best_mv.x * 4;
            out_mvs[mb_idx].mvy = best_mv.y * 4;
            out_mvs[mb_idx].sad = best_cost;
            out_mvs[mb_idx]._pad = 0;
        }
    }

    return 0;
}
