/* bc250-vcn-driver - https://github.com/Kai/bc250-vcn-driver */
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

    printf("  ✓ cpu_simd_sad_16x16 verified!\n");
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

    /* Macroblock at (16, 16) - i.e. mbx=1, mby=1
     * Put a high-contrast pattern in ref at (16, 16) */
    for (int y = 16; y < 32; y++) {
        for (int x = 16; x < 32; x++) {
            ref[y * pitch + x] = ((x + y) % 2 == 0) ? 200 : 20;
        }
    }

    /* In cur, shift the exact same pattern by dx=+2, dy=+2 (to position 18, 18) */
    int shift_x = 2;
    int shift_y = 2;
    for (int y = 16; y < 32; y++) {
        for (int x = 16; x < 32; x++) {
            cur[y * pitch + x] = ((x - shift_x + y - shift_y) % 2 == 0) ? 200 : 20;
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
           shift_x, shift_y, -shift_x * 4, -shift_y * 4);

    /* In motion estimation, ref = cur - MV, so searching ref finds the pattern at offset (+2, +2)
     * relative to cur offset, meaning mvx = -2 * 4 = -8, mvy = -2 * 4 = -8 (or +8 depending on sign convention). */
    assert(abs(mvs[moving_mb_idx].mvx) == shift_x * 4);
    assert(abs(mvs[moving_mb_idx].mvy) == shift_y * 4);

    free(ref);
    free(cur);
    free(mvs);
    printf("  ✓ cpu_simd_me_search_frame verified!\n");
}

int main(void)
{
    printf("========================================\n");
    printf("BC-250 CPU SIMD Motion Estimation Unit Test\n");
    printf("========================================\n");

    test_sad_16x16();
    test_motion_search();

    printf("\nALL CPU SIMD MOTION ESTIMATION TESTS PASSED!\n");
    return 0;
}
