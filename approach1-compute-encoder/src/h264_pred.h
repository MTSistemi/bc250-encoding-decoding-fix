/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_pred.h - intra prediction, Rec. ITU-T H.264 clause 8.3.
 *
 * The prediction functions are pure: they take the reference samples as
 * arrays and write the predicted block, and know nothing about macroblocks,
 * availability or constrained intra prediction. Picking which samples are
 * available and substituting for the ones that are not is the caller's job,
 * in h264_mb.c, because that is where the neighbour bookkeeping lives.
 *
 * Reference samples follow the standard's naming: `top` is p[x, -1] for
 * x = 0.., `left` is p[-1, y] for y = 0.., and `corner` is p[-1, -1].
 */
#ifndef BC250_H264_PRED_H
#define BC250_H264_PRED_H

#include <stdbool.h>
#include <stdint.h>

/* Intra_4x4 prediction modes, clause 8.3.1.2. */
enum {
    H264D_I4_VERT = 0,
    H264D_I4_HOR,
    H264D_I4_DC,
    H264D_I4_DIAG_DOWN_LEFT,
    H264D_I4_DIAG_DOWN_RIGHT,
    H264D_I4_VERT_RIGHT,
    H264D_I4_HOR_DOWN,
    H264D_I4_VERT_LEFT,
    H264D_I4_HOR_UP,
};

/* Intra_16x16 prediction modes, clause 8.3.3. */
enum {
    H264D_I16_VERT = 0,
    H264D_I16_HOR,
    H264D_I16_DC,
    H264D_I16_PLANE,
};

/* Chroma prediction modes, clause 8.3.4.
 * ⚠️ Not the same numbering as luma 16x16: DC comes first here, and vertical
 * and horizontal are the other way round. Reusing the luma enum for chroma
 * swaps flat grey with a vertical smear, which is subtle enough on real
 * video to survive a casual look. */
enum {
    H264D_C8_DC = 0,
    H264D_C8_HOR,
    H264D_C8_VERT,
    H264D_C8_PLANE,
};

/* `avail_*` say whether those reference samples exist. The DC modes need to
 * know, because their fallback depends on which side is missing; the
 * directional modes are only ever chosen when their references exist. */
void h264d_pred4x4(uint8_t *dst, int stride, int mode,
                   const uint8_t top[8], const uint8_t left[4], uint8_t corner,
                   bool avail_top, bool avail_left);

void h264d_pred8x8_luma(uint8_t *dst, int stride, int mode,
                        const uint8_t top[16], const uint8_t left[8], uint8_t corner,
                        bool avail_top, bool avail_left, bool avail_corner,
                        bool avail_top_right);

void h264d_pred16x16(uint8_t *dst, int stride, int mode,
                     const uint8_t top[16], const uint8_t left[16], uint8_t corner,
                     bool avail_top, bool avail_left);

void h264d_pred_chroma(uint8_t *dst, int stride, int mode,
                       const uint8_t top[8], const uint8_t left[8], uint8_t corner,
                       bool avail_top, bool avail_left);

#endif /* BC250_H264_PRED_H */
