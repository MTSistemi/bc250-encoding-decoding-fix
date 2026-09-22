/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_parts.h - how a macroblock's mb_type and sub_mb_type break down into
 * motion partitions, Rec. ITU-T H.264 Tables 7-13, 7-14, 7-17 and 7-18.
 *
 * Shared by the CABAC and the CAVLC macroblock layers: the two read the
 * numbers differently, but what the numbers mean is the same.
 */
#ifndef BC250_H264_PARTS_H
#define BC250_H264_PARTS_H

#include <stdint.h>

/* Which reference lists a partition predicts from. */
enum {
    H264D_PRED_L0 = 0,
    H264D_PRED_L1,
    H264D_PRED_BI,
    H264D_PRED_DIRECT,
    H264D_PRED_NONE,
};

/* One partition: where it starts as a raster 4x4 index, its size in 4x4
 * units, and which lists it uses. */
typedef struct {
    uint8_t blk;
    uint8_t w4, h4;
    uint8_t pred;
} h264d_part_t;

/* The two partition prediction modes of a B macroblock, Table 7-14.
 * mb_type 4 upwards alternates 16x8 and 8x16 over the same nine pairs, so
 * the pair is (mb_type - 4) / 2. */
static const uint8_t h264d_b_pair[9][2] = {
    { H264D_PRED_L0, H264D_PRED_L0 },
    { H264D_PRED_L1, H264D_PRED_L1 },
    { H264D_PRED_L0, H264D_PRED_L1 },
    { H264D_PRED_L1, H264D_PRED_L0 },
    { H264D_PRED_L0, H264D_PRED_BI },
    { H264D_PRED_L1, H264D_PRED_BI },
    { H264D_PRED_BI, H264D_PRED_L0 },
    { H264D_PRED_BI, H264D_PRED_L1 },
    { H264D_PRED_BI, H264D_PRED_BI },
};

/* A B sub_mb_type, Table 7-18: how it predicts, how many partitions it has
 * and how big they are in 4x4 units. */
typedef struct {
    uint8_t pred;
    uint8_t n;
    uint8_t w4, h4;
} h264d_sub_t;

static const h264d_sub_t h264d_sub_b[13] = {
    { H264D_PRED_DIRECT, 4, 1, 1 },   /* B_Direct_8x8 */
    { H264D_PRED_L0, 1, 2, 2 },
    { H264D_PRED_L1, 1, 2, 2 },
    { H264D_PRED_BI, 1, 2, 2 },
    { H264D_PRED_L0, 2, 2, 1 },       /* 8x4 */
    { H264D_PRED_L0, 2, 1, 2 },       /* 4x8 */
    { H264D_PRED_L1, 2, 2, 1 },
    { H264D_PRED_L1, 2, 1, 2 },
    { H264D_PRED_BI, 2, 2, 1 },
    { H264D_PRED_BI, 2, 1, 2 },
    { H264D_PRED_L0, 4, 1, 1 },
    { H264D_PRED_L1, 4, 1, 1 },
    { H264D_PRED_BI, 4, 1, 1 },
};

/* Table 7-17, the P sub_mb_types. All of them predict from list 0. */
static const h264d_sub_t h264d_sub_p[4] = {
    { H264D_PRED_L0, 1, 2, 2 },
    { H264D_PRED_L0, 2, 2, 1 },
    { H264D_PRED_L0, 2, 1, 2 },
    { H264D_PRED_L0, 4, 1, 1 },
};

/* The 8x8 partition a raster 4x4 block belongs to: bit 1 of its row and bit
 * 1 of its column, which are bits 3 and 1 of the raster index - not bit 0,
 * which is the low bit of the column. */
static inline int h264d_part8(int b)
{
    return ((b >> 2) & 2) | ((b >> 1) & 1);
}

/* The raster 4x4 index of the top-left corner of each 8x8 partition. */
static const uint8_t h264d_blk8[4] = { 0, 2, 8, 10 };

/* Where sub-partition `i` of a sub-macroblock sits, relative to the
 * sub-macroblock's own corner, in raster 4x4 units. */
static inline int h264d_sub_offset(const h264d_sub_t *s, int i)
{
    if (s->n == 1) return 0;
    if (s->n == 2) return (s->w4 == 2) ? i * 4 : i;   /* 8x4 stacks, 4x8 sits side by side */
    return (i >> 1) * 4 + (i & 1);                    /* 4x4 */
}

#endif /* BC250_H264_PARTS_H */
