/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_deblock.h - the deblocking filter, Rec. ITU-T H.264 clause 8.7.
 *
 * The filter runs over the whole picture once decoding has finished, not
 * macroblock by macroblock as they are decoded. Both orders give the same
 * answer - the standard defines the filter to read only samples that are
 * already final - but doing it in one pass at the end keeps the macroblock
 * loop free of it, and means the pass can be handed to several threads by
 * rows later on.
 *
 * ⚠️ Deblocking is not cosmetic and is not optional. Its output is what goes
 * into the reference picture buffer, so a decoder that skips it or gets it
 * wrong drifts away from the encoder's reconstruction over the whole group
 * of pictures rather than producing one slightly blocky frame.
 */
#ifndef BC250_H264_DEBLOCK_H
#define BC250_H264_DEBLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "decoder_h264.h"

/* Per-slice filter settings, kept per macroblock because a picture can hold
 * slices that disagree about them. */
typedef struct {
    int8_t disable_idc;      /* 0 filter, 1 off, 2 off across slice edges */
    int8_t alpha_offset;     /* already doubled, i.e. the standard's FilterOffsetA */
    int8_t beta_offset;
} h264d_deblock_params_t;

void h264d_deblock_picture(uint8_t *y, int stride_y,
                           uint8_t *cb, uint8_t *cr, int stride_c,
                           int mb_width, int mb_height,
                           const h264d_mb_t *mbs,
                           const uint8_t *slice_of_mb,
                           const h264d_deblock_params_t *params,
                           int chroma_qp_offset, int second_chroma_qp_offset);

#endif /* BC250_H264_DEBLOCK_H */
