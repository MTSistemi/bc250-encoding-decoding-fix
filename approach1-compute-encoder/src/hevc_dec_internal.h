/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_dec_internal.h - the H.265 decoder's own state.
 *
 * H.265 replaces the macroblock with a coding tree unit that splits down a
 * quadtree, so almost nothing here is indexed per macroblock. What the
 * syntax needs from its neighbours - the depth a coding unit was split to,
 * the intra prediction mode a prediction unit chose - is kept on a grid of
 * the smallest block each of those can be, and read at whatever size the
 * question is asked.
 */
#ifndef BC250_HEVC_DEC_INTERNAL_H
#define BC250_HEVC_DEC_INTERNAL_H

#include "hevc_ps.h"
#include "hevc_cabac_dec.h"

/* Prediction modes, 7.4.9.5. */
enum { HEVCD_MODE_INTER = 0, HEVCD_MODE_INTRA = 1 };

/* Partition modes, Table 7-10. Only the two an intra coding unit can use
 * are named; the inter ones come later. */
enum {
    HEVCD_PART_2Nx2N = 0, HEVCD_PART_2NxN = 1, HEVCD_PART_Nx2N = 2,
    HEVCD_PART_NxN = 3,
    HEVCD_PART_2NxnU = 4, HEVCD_PART_2NxnD = 5,
    HEVCD_PART_nLx2N = 6, HEVCD_PART_nRx2N = 7,
};

/* The three intra prediction modes with names; the rest are angles. */
enum { HEVCD_INTRA_PLANAR = 0, HEVCD_INTRA_DC = 1,
       HEVCD_INTRA_ANGULAR_26 = 26, HEVCD_INTRA_ANGULAR_10 = 10 };

/* Coefficient scan orders, 6.5.3. */
enum { HEVCD_SCAN_DIAG = 0, HEVCD_SCAN_HORIZ = 1, HEVCD_SCAN_VERT = 2 };

/* One picture's worth of decoding state. */
typedef struct {
    const hevc_sps_t *sps;
    const hevc_pps_t *pps;
    const hevc_slice_t *slice;

    hevcd_cabac_t cabac;

    /* ⚠️ Per smallest coding block, not per coding unit: split_cu_flag's
     * context asks how deep the neighbour was split, and the neighbour may
     * be any size. */
    uint8_t *ct_depth;              /* [min_cb_height][min_cb_width] */
    /* Per smallest prediction block, which at 4x4 is also the smallest
     * transform block: the intra mode, for the most-probable-mode
     * derivation and for which way the coefficients are scanned. */
    uint8_t *intra_mode;            /* [h >> 2][w >> 2] */
    uint8_t *skip;                  /* per min coding block */
    size_t n_ct_depth, n_intra_mode;
    int min_pu_width, min_pu_height;

    /* The coding unit being read. */
    struct {
        int x, y, log2_size;
        int pred_mode;
        int part_mode;
        bool transquant_bypass;
        bool intra_split;
        int intra_mode_c;           /* the chroma mode, IntraPredModeC */
    } cu;

    /* Quantisation, 8.6.1. QpY carries across coding units. */
    int qp_y, qp_y_pred;
    bool cu_qp_delta_coded;
    int cu_qp_delta;

    /* One transform block's coefficients, in raster order inside it. */
    int16_t coeff[32 * 32];

    /* Where the picture is written. The decoder owns these; the harness
     * reads them back. */
    uint8_t *piano[3];
    int passo[3];

    int ctb_addr;                   /* in the picture's raster order */
    bool fine_slice;
} hevcd_t;

/* Reading one coding tree unit and everything inside it. Returns 0, or
 * non-zero when the slice cannot go on. */
int hevcd_leggi_ctu(hevcd_t *d, int x0, int y0);

/* residual_coding(), clause 7.3.8.11. The coefficients land in d->coeff,
 * in raster order inside the transform block. */
void hevcd_leggi_residuo(hevcd_t *d, int x0, int y0, int log2_size, int c_idx);

#endif /* BC250_HEVC_DEC_INTERNAL_H */
