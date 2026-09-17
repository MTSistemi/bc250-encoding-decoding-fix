/* bc250-encoding-decoding-fix v0.4.2 - https://github.com/simpmix/bc250-encoding-decoding-fix */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_cpu_simd_me.c - Unit test for CPU SIMD Motion Estimation Engine
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "cpu_simd_me.h"

static void test_sad_16x16(void)
{
    printf("[TEST] Testing cpu_simd_sad_16x16...\n");

    uint8_t blk_a[16 * 16];
    uint8_t blk_b[16 * 16];

    /* 1. Exact match */
    memset(blk_a, 128, sizeof(blk_a));
    memset(blk_b, 128, sizeof(blk_b));
    uint32_t sad_zero = cpu_simd_sad_16x16(blk_a, 16, blk_b, 16);
    assert(sad_zero == 0);

    /* 2. Uniform difference of 10 per pixel */
    memset(blk_b, 138, sizeof(blk_b));
    uint32_t sad_diff = cpu_simd_sad_16x16(blk_a, 16, blk_b, 16);
    assert(sad_diff == 16 * 16 * 10);

    /* 3. Stride handling */
    uint8_t canvas_a[32 * 32];
    uint8_t canvas_b[32 * 32];
    memset(canvas_a, 50, sizeof(canvas_a));
    memset(canvas_b, 60, sizeof(canvas_b));
    uint32_t sad_stride = cpu_simd_sad_16x16(canvas_a, 32, canvas_b, 32);
    assert(sad_stride == 16 * 16 * 10);

    printf("  âœ“ cpu_simd_sad_16x16 verified!\n");
}

static void test_motion_search(void)
{
    printf("[TEST] Testing cpu_simd_me_search_frame...\n");

    const int width = 64;
    const int height = 64;
    const int pitch = 64;

    uint8_t *ref = calloc(1, width * height);
    uint8_t *cur = calloc(1, width * height);
    assert(ref && cur);

    /* Background flat gray */
    memset(ref, 100, width * height);
    memset(cur, 100, width * height);

    /* Target macroblock at (16, 16) - i.e. mbx=1, mby=1 in cur frame.
     * Place a unique distinctive non-repeating pattern in cur at (16..31, 16..31) */
    int shift_x = 2;
    int shift_y = 2;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            cur[(16 + y) * pitch + (16 + x)] = (uint8_t)((x * 13 + y * 7 + 40) % 256);
        }
    }

    /* In ref, place the exact same pattern shifted by (shift_x, shift_y) to (18..33, 18..33) */
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            ref[(16 + shift_y + y) * pitch + (16 + shift_x + x)] = cur[(16 + y) * pitch + (16 + x)];
        }
    }

    uint32_t width_mbs = width / 16;
    uint32_t height_mbs = height / 16;
    gpu_mv_t *mvs = calloc(width_mbs * height_mbs, sizeof(gpu_mv_t));
    assert(mvs);

    cpu_simd_me_config_t cfg;
    cpu_simd_me_config_init(&cfg, width, height);
    cfg.search_radius = 8;
    cfg.num_threads = 2;

    int rc = cpu_simd_me_search_frame(cur, pitch, ref, pitch, width, height, mvs, &cfg);
    assert(rc == 0);

    /* Check static blocks: mb (0, 0) should be zero MV */
    assert(mvs[0].mvx == 0);
    assert(mvs[0].mvy == 0);

    /* Check moving block: mb (1, 1) index is 1 * 4 + 1 = 5 */
    uint32_t moving_mb_idx = 1 * width_mbs + 1;
    printf("  Found MV for mb(1,1): mvx=%d, mvy=%d, sad=%u (expected shift dx=%d, dy=%d => mvx=%d, mvy=%d in qpel)\n",
           mvs[moving_mb_idx].mvx, mvs[moving_mb_idx].mvy, mvs[moving_mb_idx].sad,
           shift_x, shift_y, shift_x * 4, shift_y * 4);

    /* The displacement found in reference frame is (shift_x, shift_y) in integer pixels,
     * which in quarter-pel units is shift * 4. */
    assert(mvs[moving_mb_idx].mvx == shift_x * 4);
    assert(mvs[moving_mb_idx].mvy == shift_y * 4);

    free(ref);
    free(cur);
    free(mvs);
    printf("  âœ“ cpu_simd_me_search_frame verified!\n");
}

static void test_spatial_predictor_and_boundaries(void)
{
    printf("[TEST] Testing spatial predictor & macroblock boundary handling...\n");

    /* Test 64x64 frame with adjacent moving blocks to test predictor */
    const int width = 64;
    const int height = 64;
    const int pitch = 64;

    uint8_t *ref = calloc(1, width * height);
    uint8_t *cur = calloc(1, width * height);
    assert(ref && cur);

    memset(ref, 120, width * height);
    memset(cur, 120, width * height);

    /* Move both mb(1, 1) and mb(2, 1) by (+2, +2) */
    int shift_x = 2;
    int shift_y = 2;
    for (int mbx = 1; mbx <= 2; mbx++) {
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                cur[(16 + y) * pitch + (mbx * 16 + x)] = (uint8_t)((x * 13 + y * 7 + mbx * 30) % 256);
                ref[(16 + shift_y + y) * pitch + (mbx * 16 + shift_x + x)] = cur[(16 + y) * pitch + (mbx * 16 + x)];
            }
        }
    }

    uint32_t width_mbs = width / 16;
    uint32_t height_mbs = height / 16;
    gpu_mv_t *mvs = calloc(width_mbs * height_mbs, sizeof(gpu_mv_t));
    assert(mvs);

    cpu_simd_me_config_t cfg;
    cpu_simd_me_config_init(&cfg, width, height);
    cfg.search_radius = 8;
    cfg.num_threads = 1;

    int rc = cpu_simd_me_search_frame(cur, pitch, ref, pitch, width, height, mvs, &cfg);
    assert(rc == 0);

    printf("  Predictor test: mvs[5]=(%d,%d,sad=%u), mvs[6]=(%d,%d,sad=%u)\n",
           mvs[5].mvx, mvs[5].mvy, mvs[5].sad, mvs[6].mvx, mvs[6].mvy, mvs[6].sad);

    /* mb(1, 1) index: 1 * 4 + 1 = 5 */
    /* mb(2, 1) index: 1 * 4 + 2 = 6 */
    assert(mvs[5].mvx == shift_x * 4);
    assert(mvs[5].mvy == shift_y * 4);
    assert(mvs[6].mvx == shift_x * 4);
    assert(mvs[6].mvy == shift_y * 4);
    assert(mvs[6].sad < 100);

    free(ref);
    free(cur);
    free(mvs);

    /* Test non-multiple height (e.g. 50 lines, 4 MB rows = 64 lines padded) */
    const int w2 = 32;
    const int h2 = 50;
    const int p2 = 32;
    const int w2_mbs = (w2 + 15) / 16; /* 2 */
    const int h2_mbs = (h2 + 15) / 16; /* 4 */
    const int max_y = h2_mbs * 16;      /* 64 */

    uint8_t *ref2 = calloc(1, p2 * max_y);
    uint8_t *cur2 = calloc(1, p2 * max_y);
    gpu_mv_t *mvs2 = calloc(w2_mbs * h2_mbs, sizeof(gpu_mv_t));
    assert(ref2 && cur2 && mvs2);

    cpu_simd_me_config_init(&cfg, w2, h2);
    rc = cpu_simd_me_search_frame(cur2, p2, ref2, p2, w2, h2, mvs2, &cfg);
    assert(rc == 0);

    free(ref2);
    free(cur2);
    free(mvs2);

    printf("  âœ“ Spatial predictor & boundary handling verified!\n");
}

static void test_sad_equivalence_with_scalar(void)
{
    printf("[TEST] Testing SIMD SAD equivalence against scalar oracle across patterns...\n");

    uint8_t a[64 * 64];
    uint8_t b[64 * 64];

    for (int i = 0; i < 64 * 64; i++) {
        a[i] = (uint8_t)((i * 37 + 13) & 0xFF);
        b[i] = (uint8_t)((i * 19 + 71) & 0xFF);
    }

    for (int y = 0; y < 40; y += 4) {
        for (int x = 0; x < 40; x += 4) {
            const uint8_t *p_a = a + y * 64 + x;
            const uint8_t *p_b = b + y * 64 + x;

            /* Compute reference scalar SAD */
            uint32_t ref_sad = 0;
            for (int r = 0; r < 16; r++) {
                for (int c = 0; c < 16; c++) {
                    int d = (int)p_a[r * 64 + c] - (int)p_b[r * 64 + c];
                    ref_sad += (d < 0) ? -d : d;
                }
            }

            uint32_t simd_sad = cpu_simd_sad_16x16(p_a, 64, p_b, 64);
            assert(simd_sad == ref_sad);
        }
    }

    printf("  âœ“ SIMD (AVX2/SSE2) vs scalar equivalence verified!\n");
}

static void test_cpu_affinity_config(void)
{
    printf("[TEST] Testing CPU core pinning affinity configuration...\n");

    cpu_simd_me_config_t cfg;
    cpu_simd_me_config_init(&cfg, 1920, 1080);
    /* Default: unpinned */
    assert(cfg.core_ids[0] == -1);
    assert(cfg.core_ids[1] == -1);

    /* Test programmatic pinning */
    cfg.core_ids[0] = 6;
    cfg.core_ids[1] = 7;

    uint8_t cur[16 * 16] = {0};
    uint8_t ref[16 * 16] = {0};
    gpu_mv_t mv = {0};

    int rc = cpu_simd_me_search_frame(cur, 16, ref, 16, 16, 16, &mv, &cfg);
    assert(rc == 0);

    printf("  ✓ CPU core pinning affinity configuration verified!\n");
}

int main(void)
{
    printf("========================================\n");
    printf("BC-250 CPU SIMD Motion Estimation Unit Test\n");
    printf("========================================\n");

    test_sad_16x16();
    test_sad_equivalence_with_scalar();
    test_motion_search();
    test_spatial_predictor_and_boundaries();
    test_cpu_affinity_config();

    printf("\nALL CPU SIMD MOTION ESTIMATION TESTS PASSED!\n");
    return 0;
}
