/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_pred.c - intra prediction, Rec. ITU-T H.264 clause 8.3.
 *
 * The directional modes are written against a single reference array rather
 * than against separate top and left arrays. The standard writes them as
 * three or four cases on the sign of a diagonal counter, each case reaching
 * into a different neighbour, and transcribed literally that becomes a nest
 * of conditionals where an off-by-one hides easily.
 *
 * Laid out end to end instead - left read upwards, then the corner, then top
 * read rightwards - the neighbours become one line of samples, p[-1,y] and
 * p[x,-1] become one index each, and most of the cases collapse. Diagonal
 * Down Right, for instance, is one expression for the whole block:
 *
 *      r[C-1 + x-y], r[C + x-y], r[C+1 + x-y]
 *
 * where C is where the corner sits. The standard's three cases (x>y, x<y,
 * x==y) are the same three samples read from either side of the corner.
 */
#include "h264_pred.h"

#include <string.h>

static inline uint8_t clip_uint8(int v)
{
    return (uint8_t)(v & ~255 ? (-v) >> 31 : v);
}

/* The two filters every directional mode is built from. f121 is symmetric in
 * its outer arguments, which is why a run read backwards gives the same
 * answer as the same run read forwards. */
static inline int f121(int a, int b, int c) { return (a + 2 * b + c + 2) >> 2; }
static inline int f11(int a, int b)         { return (a + b + 1) >> 1; }

/* ------------------------------------------------------------------ 4x4 */

/* r[0..3] = p[-1,3] p[-1,2] p[-1,1] p[-1,0], r[4] = p[-1,-1],
 * r[5..12] = p[0,-1] .. p[7,-1].  So p[-1,y] = r[3-y] and p[x,-1] = r[5+x],
 * and both still work at -1, landing on the corner. */
#define C4 4

void h264d_pred4x4(uint8_t *dst, int stride, int mode,
                   const uint8_t t[8], const uint8_t l[4], uint8_t c,
                   bool avail_top, bool avail_left)
{
    uint8_t r[13];
    r[0] = l[3]; r[1] = l[2]; r[2] = l[1]; r[3] = l[0];
    r[C4] = c;
    memcpy(r + 5, t, 8);

#define P(x, y) dst[(y) * stride + (x)]
    switch (mode) {
    case H264D_I4_VERT:
        for (int y = 0; y < 4; y++)
            memcpy(dst + y * stride, t, 4);
        break;

    case H264D_I4_HOR:
        for (int y = 0; y < 4; y++)
            memset(dst + y * stride, l[y], 4);
        break;

    case H264D_I4_DC: {
        int v;
        if (avail_top && avail_left)
            v = (t[0] + t[1] + t[2] + t[3] + l[0] + l[1] + l[2] + l[3] + 4) >> 3;
        else if (avail_left)
            v = (l[0] + l[1] + l[2] + l[3] + 2) >> 2;
        else if (avail_top)
            v = (t[0] + t[1] + t[2] + t[3] + 2) >> 2;
        else
            v = 128;
        for (int y = 0; y < 4; y++)
            memset(dst + y * stride, v, 4);
        break;
    }

    case H264D_I4_DIAG_DOWN_LEFT:
        /* The bottom-right sample repeats p[7,-1], which is the standard's
         * own special case and not an off-by-one. */
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int i = C4 + 1 + x + y;
                P(x, y) = (uint8_t)(x + y == 6 ? f121(r[11], r[12], r[12])
                                               : f121(r[i], r[i + 1], r[i + 2]));
            }
        break;

    case H264D_I4_DIAG_DOWN_RIGHT:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int i = C4 + x - y;
                P(x, y) = (uint8_t)f121(r[i - 1], r[i], r[i + 1]);
            }
        break;

    case H264D_I4_VERT_RIGHT:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int z = 2 * x - y;
                int i = C4 + 1 + x - (y >> 1);
                if (z >= 0 && !(z & 1))      P(x, y) = (uint8_t)f11(r[i - 1], r[i]);
                else if (z >= -1)            P(x, y) = (uint8_t)f121(r[i - 2], r[i - 1], r[i]);
                else                         P(x, y) = (uint8_t)f121(r[C4 + z], r[C4 + 1 + z],
                                                                     r[C4 + 2 + z]);
            }
        break;

    case H264D_I4_HOR_DOWN:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int z = 2 * y - x;
                int i = C4 - 1 - y + (x >> 1);
                if (z >= 0 && !(z & 1))      P(x, y) = (uint8_t)f11(r[i + 1], r[i]);
                else if (z >= -1)            P(x, y) = (uint8_t)f121(r[i + 2], r[i + 1], r[i]);
                else                         P(x, y) = (uint8_t)f121(r[C4 - z], r[C4 - 1 - z],
                                                                     r[C4 - 2 - z]);
            }
        break;

    case H264D_I4_VERT_LEFT:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int i = C4 + 1 + x + (y >> 1);
                P(x, y) = (uint8_t)((y & 1) ? f121(r[i], r[i + 1], r[i + 2])
                                            : f11(r[i], r[i + 1]));
            }
        break;

    case H264D_I4_HOR_UP:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int z = x + 2 * y;
                int i = C4 - 1 - y - (x >> 1);
                if (z < 5 && !(z & 1))       P(x, y) = (uint8_t)f11(r[i], r[i - 1]);
                else if (z < 5)              P(x, y) = (uint8_t)f121(r[i], r[i - 1], r[i - 2]);
                else if (z == 5)             P(x, y) = (uint8_t)f121(r[1], r[0], r[0]);
                else                         P(x, y) = r[0];
            }
        break;

    default:
        for (int y = 0; y < 4; y++)
            memset(dst + y * stride, 128, 4);
        break;
    }
#undef P
}

/* ----------------------------------------------------------------- 16x16 */

void h264d_pred16x16(uint8_t *dst, int stride, int mode,
                     const uint8_t t[16], const uint8_t l[16], uint8_t c,
                     bool avail_top, bool avail_left)
{
    switch (mode) {
    case H264D_I16_VERT:
        for (int y = 0; y < 16; y++)
            memcpy(dst + y * stride, t, 16);
        break;

    case H264D_I16_HOR:
        for (int y = 0; y < 16; y++)
            memset(dst + y * stride, l[y], 16);
        break;

    case H264D_I16_DC: {
        int v = 0;
        if (avail_top && avail_left) {
            for (int i = 0; i < 16; i++) v += t[i] + l[i];
            v = (v + 16) >> 5;
        } else if (avail_top) {
            for (int i = 0; i < 16; i++) v += t[i];
            v = (v + 8) >> 4;
        } else if (avail_left) {
            for (int i = 0; i < 16; i++) v += l[i];
            v = (v + 8) >> 4;
        } else {
            v = 128;
        }
        for (int y = 0; y < 16; y++)
            memset(dst + y * stride, v, 16);
        break;
    }

    case H264D_I16_PLANE: {
        /* Clause 8.3.3.4. The i = 7 term of each sum reaches the corner,
         * which is why it is singled out. */
        int h = 0, v = 0;
        for (int i = 0; i < 8; i++) {
            h += (i + 1) * (t[8 + i] - (i == 7 ? c : t[6 - i]));
            v += (i + 1) * (l[8 + i] - (i == 7 ? c : l[6 - i]));
        }
        int a = 16 * (l[15] + t[15]);
        int b = (5 * h + 32) >> 6;
        int d = (5 * v + 32) >> 6;
        for (int y = 0; y < 16; y++) {
            uint8_t *row = dst + y * stride;
            for (int x = 0; x < 16; x++)
                row[x] = clip_uint8((a + b * (x - 7) + d * (y - 7) + 16) >> 5);
        }
        break;
    }

    default:
        for (int y = 0; y < 16; y++)
            memset(dst + y * stride, 128, 16);
        break;
    }
}

/* ---------------------------------------------------------------- chroma */

void h264d_pred_chroma(uint8_t *dst, int stride, int mode,
                       const uint8_t t[8], const uint8_t l[8], uint8_t c,
                       bool avail_top, bool avail_left)
{
    switch (mode) {
    case H264D_C8_DC:
        /* Clause 8.3.4.1: each 4x4 quadrant gets its own DC, and the two
         * off-diagonal quadrants prefer the edge that runs along them rather
         * than averaging both. */
        for (int qy = 0; qy < 2; qy++) {
            for (int qx = 0; qx < 2; qx++) {
                int above = 0, left = 0;
                for (int i = 0; i < 4; i++) {
                    above += t[qx * 4 + i];
                    left += l[qy * 4 + i];
                }
                bool above_only = (qx == 1 && qy == 0);
                bool left_only = (qx == 0 && qy == 1);
                int v;
                if (avail_top && avail_left) {
                    if (above_only)         v = (above + 2) >> 2;
                    else if (left_only) v = (left + 2) >> 2;
                    else                    v = (above + left + 4) >> 3;
                } else if (avail_top) {
                    v = (above + 2) >> 2;
                } else if (avail_left) {
                    v = (left + 2) >> 2;
                } else {
                    v = 128;
                }
                for (int y = 0; y < 4; y++)
                    memset(dst + (qy * 4 + y) * stride + qx * 4, v, 4);
            }
        }
        break;

    case H264D_C8_HOR:
        for (int y = 0; y < 8; y++)
            memset(dst + y * stride, l[y], 8);
        break;

    case H264D_C8_VERT:
        for (int y = 0; y < 8; y++)
            memcpy(dst + y * stride, t, 8);
        break;

    case H264D_C8_PLANE: {
        int h = 0, v = 0;
        for (int i = 0; i < 4; i++) {
            h += (i + 1) * (t[4 + i] - (i == 3 ? c : t[2 - i]));
            v += (i + 1) * (l[4 + i] - (i == 3 ? c : l[2 - i]));
        }
        int a = 16 * (l[7] + t[7]);
        int b = (34 * h + 32) >> 6;
        int d = (34 * v + 32) >> 6;
        for (int y = 0; y < 8; y++) {
            uint8_t *row = dst + y * stride;
            for (int x = 0; x < 8; x++)
                row[x] = clip_uint8((a + b * (x - 3) + d * (y - 3) + 16) >> 5);
        }
        break;
    }

    default:
        for (int y = 0; y < 8; y++)
            memset(dst + y * stride, 128, 8);
        break;
    }
}

/* ------------------------------------------------------------- 8x8 luma */

/* Clause 8.3.2.2.1. The 8x8 modes predict from low-pass filtered references,
 * and the filtering itself depends on which neighbours exist: the two ends of
 * each run are handled differently from the middle, and the corner is only
 * filtered across both runs when both of them are there. */
static void filter_edge(const uint8_t t[16], const uint8_t l[8], uint8_t c,
                   bool avail_top, bool avail_left, bool avail_corner,
                   uint8_t ft[16], uint8_t fl[8], uint8_t *fc)
{
    if (avail_top) {
        ft[0] = (uint8_t)(avail_corner ? f121(c, t[0], t[1])
                                       : f121(t[0], t[0], t[1]));
        for (int i = 1; i < 15; i++)
            ft[i] = (uint8_t)f121(t[i - 1], t[i], t[i + 1]);
        ft[15] = (uint8_t)f121(t[14], t[15], t[15]);
    }
    if (avail_left) {
        fl[0] = (uint8_t)(avail_corner ? f121(c, l[0], l[1])
                                       : f121(l[0], l[0], l[1]));
        for (int i = 1; i < 7; i++)
            fl[i] = (uint8_t)f121(l[i - 1], l[i], l[i + 1]);
        fl[7] = (uint8_t)f121(l[6], l[7], l[7]);
    }
    if (avail_corner) {
        if (avail_top && avail_left) *fc = (uint8_t)f121(t[0], c, l[0]);
        else if (avail_top)          *fc = (uint8_t)f121(t[0], c, t[0]);
        else if (avail_left)         *fc = (uint8_t)f121(l[0], c, l[0]);
    }
}

/* r[0..7] = p[-1,7] .. p[-1,0], r[8] = p[-1,-1], r[9..24] = p[0,-1]..p[15,-1] */
#define C8 8

void h264d_pred8x8_luma(uint8_t *dst, int stride, int mode,
                        const uint8_t t_in[16], const uint8_t l_in[8], uint8_t c_in,
                        bool avail_top, bool avail_left, bool avail_corner,
                        bool avail_top_right)
{
    uint8_t t[16], l[8];
    memcpy(t, t_in, 16);
    memcpy(l, l_in, 8);

    /* A missing top-right block is the last real sample repeated, and that
     * substitution happens before the filter runs, so the filter sees the
     * padded run the way the standard says it should. */
    if (avail_top && !avail_top_right)
        memset(t + 8, t[7], 8);

    uint8_t ft[16], fl[8], fc = c_in;
    memcpy(ft, t, 16);
    memcpy(fl, l, 8);
    filter_edge(t, l, c_in, avail_top, avail_left, avail_corner, ft, fl, &fc);

    uint8_t r[25];
    for (int i = 0; i < 8; i++)
        r[i] = fl[7 - i];
    r[C8] = fc;
    memcpy(r + 9, ft, 16);

#define P(x, y) dst[(y) * stride + (x)]
    switch (mode) {
    case H264D_I4_VERT:
        for (int y = 0; y < 8; y++)
            memcpy(dst + y * stride, ft, 8);
        break;

    case H264D_I4_HOR:
        for (int y = 0; y < 8; y++)
            memset(dst + y * stride, fl[y], 8);
        break;

    case H264D_I4_DC: {
        int v = 0;
        if (avail_top && avail_left) {
            for (int i = 0; i < 8; i++) v += ft[i] + fl[i];
            v = (v + 8) >> 4;
        } else if (avail_top) {
            for (int i = 0; i < 8; i++) v += ft[i];
            v = (v + 4) >> 3;
        } else if (avail_left) {
            for (int i = 0; i < 8; i++) v += fl[i];
            v = (v + 4) >> 3;
        } else {
            v = 128;
        }
        for (int y = 0; y < 8; y++)
            memset(dst + y * stride, v, 8);
        break;
    }

    case H264D_I4_DIAG_DOWN_LEFT:
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int i = C8 + 1 + x + y;
                P(x, y) = (uint8_t)(x + y == 14 ? f121(r[23], r[24], r[24])
                                                : f121(r[i], r[i + 1], r[i + 2]));
            }
        break;

    case H264D_I4_DIAG_DOWN_RIGHT:
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int i = C8 + x - y;
                P(x, y) = (uint8_t)f121(r[i - 1], r[i], r[i + 1]);
            }
        break;

    case H264D_I4_VERT_RIGHT:
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int z = 2 * x - y;
                int i = C8 + 1 + x - (y >> 1);
                /* ⚠️ The last branch indexes by z, not by y. The standard
                 * writes the 4x4 case as p[-1,y-1..y-3] because there x is
                 * always 0 when it is taken; at 8x8 it is taken with x up to
                 * 2 and the 2x term is real. One general form for both. */
                if (z >= 0 && !(z & 1))  P(x, y) = (uint8_t)f11(r[i - 1], r[i]);
                else if (z >= -1)        P(x, y) = (uint8_t)f121(r[i - 2], r[i - 1], r[i]);
                else                     P(x, y) = (uint8_t)f121(r[C8 + z], r[C8 + 1 + z],
                                                                 r[C8 + 2 + z]);
            }
        break;

    case H264D_I4_HOR_DOWN:
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int z = 2 * y - x;
                int i = C8 - 1 - y + (x >> 1);
                if (z >= 0 && !(z & 1))  P(x, y) = (uint8_t)f11(r[i + 1], r[i]);
                else if (z >= -1)        P(x, y) = (uint8_t)f121(r[i + 2], r[i + 1], r[i]);
                else                     P(x, y) = (uint8_t)f121(r[C8 - z], r[C8 - 1 - z],
                                                                 r[C8 - 2 - z]);
            }
        break;

    case H264D_I4_VERT_LEFT:
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int i = C8 + 1 + x + (y >> 1);
                P(x, y) = (uint8_t)((y & 1) ? f121(r[i], r[i + 1], r[i + 2])
                                            : f11(r[i], r[i + 1]));
            }
        break;

    case H264D_I4_HOR_UP:
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int z = x + 2 * y;
                int i = C8 - 1 - y - (x >> 1);
                if (z < 13 && !(z & 1))  P(x, y) = (uint8_t)f11(r[i], r[i - 1]);
                else if (z < 13)         P(x, y) = (uint8_t)f121(r[i], r[i - 1], r[i - 2]);
                else if (z == 13)        P(x, y) = (uint8_t)f121(r[1], r[0], r[0]);
                else                     P(x, y) = r[0];
            }
        break;

    default:
        for (int y = 0; y < 8; y++)
            memset(dst + y * stride, 128, 8);
        break;
    }
#undef P
}
