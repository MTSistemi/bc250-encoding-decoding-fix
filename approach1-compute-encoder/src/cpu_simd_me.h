/* bc250-encoding-decoding-fix v0.4.1 - https://github.com/simpmix/bc250-encoding-decoding-fix */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * cpu_simd_me.h - Multi-threaded SSE2/AVX2 SIMD Motion Estimation for BC-250 Zen 2 CPU
 */

#ifndef BC250_CPU_SIMD_ME_H
#define BC250_CPU_SIMD_ME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "gpu_compute.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t width_in_mbs;
    uint32_t height_in_mbs;
    uint32_t search_radius;  /* Default 4 or 8 pixels */
    int num_threads;         /* Capped at 2 worker threads to preserve CPU headroom */
} cpu_simd_me_config_t;

/**
 * Computes 16x16 macroblock Sum of Absolute Differences (SAD) using SSE2 SIMD
 * with scalar fallback.
 */
uint32_t cpu_simd_sad_16x16(const uint8_t *src, int src_stride,
                            const uint8_t *ref, int ref_stride);

/**
 * Initializes default CPU SIMD ME configuration for the given frame dimensions.
 */
void cpu_simd_me_config_init(cpu_simd_me_config_t *cfg, uint32_t width, uint32_t height);

/**
 * Performs fast multi-threaded motion estimation for a P-frame on the CPU.
 *
 * @src_y: Pointer to current frame luma plane
 * @src_pitch: Row pitch of current frame
 * @ref_y: Pointer to reference frame reconstructed luma plane
 * @ref_pitch: Row pitch of reference frame
 * @width: Frame width in pixels
 * @height: Frame height in pixels
 * @out_mvs: Output array of gpu_mv_t (size = width_in_mbs * height_in_mbs)
 * @cfg: Configuration (search radius, thread count)
 *
 * Output MVs are scaled to quarter-pel units (matching gpu_mv_t expected by residual_predict.comp).
 * Returns 0 on success, -1 on error.
 */
int cpu_simd_me_search_frame(const uint8_t *src_y, int src_pitch,
                             const uint8_t *ref_y, int ref_pitch,
                             uint32_t width, uint32_t height,
                             gpu_mv_t *out_mvs,
                             const cpu_simd_me_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* BC250_CPU_SIMD_ME_H */
