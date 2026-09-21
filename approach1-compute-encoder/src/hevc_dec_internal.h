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
/* One coding tree block's sample adaptive offset, 7.3.8.3.
 *
 * The filter is a lookup table with four entries that the encoder chose
 * and sent: it corrects whatever the rest of the loop got wrong, in
 * whichever direction, which is why it is the last thing to run and why
 * nothing predicts from anything but its output. */
typedef struct {
    uint8_t tipo[3];        /* 0 nothing, 1 by band, 2 by edge */
    int8_t off[3][4];       /* signed already: edge offsets have fixed signs */
    uint8_t posizione[3];   /* which four bands, for the band type */
    uint8_t classe[3];      /* which way the edge runs, for the edge type */
} hevcd_sao_t;

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
    /* ⚠️ Where each smallest transform block sits in the z-scan order of
     * the whole picture. Whether a neighbour has been decoded yet is a
     * question about that order and not about coordinates: a coding tree
     * unit is a quadtree, so the block above right of a transform block
     * may or may not have come first. */
    int32_t *min_tb_addr_zs;
    int8_t *qp_y_map;               /* per min coding block */
    /* Which 8x8 cells of the picture have a block boundary on their left
     * edge (bit 0) and on their top edge (bit 1). The deblocking filter
     * only ever looks at that grid, so a transform block boundary at four
     * samples is not one of these: it is a boundary the filter is not
     * allowed to cross. */
    uint8_t *bordi;
    int bordi_passo;
    size_t n_bordi;
    /* Per min coding block: a unit whose samples the loop filters must
     * leave exactly as they are. Lossless coding, today. */
    uint8_t *no_filtro;
    /* Per coding tree block, and the picture as the deblocking filter left
     * it: the offset by an edge asks what the neighbours were before this
     * filter touched them, so it cannot read the plane it is writing. */
    hevcd_sao_t *sao;
    size_t n_sao;
    uint8_t *copia[3];
    size_t n_copia;
    size_t n_ct_depth, n_intra_mode, n_zs, n_qp, n_no_filtro;
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
    /* The last coding unit's QpY, which is qPY_PREV for the next
     * quantisation group, and where that group starts. */
    int qp_y_prev;
    int qg_x, qg_y;
    /* The group takes the slice's parameter rather than the previous
     * group's: first of a slice, of a tile, or of a row under WPP. */
    bool qg_riparte;
    bool cu_qp_delta_coded;
    int cu_qp_delta;

    /* One transform block's coefficients, in raster order inside it. */
    int16_t coeff[32 * 32];
    bool transform_skip;            /* of the block just read */

    /* Where the picture is written. The decoder owns these; the harness
     * reads them back. */
    uint8_t *piano[3];
    int passo[3];
    size_t n_piano;

    int ctb_addr;                   /* in the picture's raster order */
    bool fine_slice;
} hevcd_t;

/* Reading one coding tree unit and everything inside it. Returns 0, or
 * non-zero when the slice cannot go on. */
int hevcd_leggi_ctu(hevcd_t *d, int x0, int y0);

/* 8.7.2 and 8.7.3, over the whole finished picture, in that order. */
void hevcd_deblocca(hevcd_t *d);
void hevcd_sao(hevcd_t *d);
void hevcd_libera_filtri(hevcd_t *d);

/* residual_coding(), clause 7.3.8.11. The coefficients land in d->coeff,
 * in raster order inside the transform block. */
void hevcd_leggi_residuo(hevcd_t *d, int x0, int y0, int log2_size, int c_idx);

/* Intra prediction, 8.4.4.2: writes the prediction straight into the
 * picture, where the residual is then added to it. */
void hevcd_predici_intra(hevcd_t *d, int c_idx, int x0, int y0, int log2_size,
                         int modo);

/* 8.6.2 to 8.6.4: the coefficients into a residual, and onto the picture. */
void hevcd_dequantizza(int16_t *coeff, int log2_size, int qp);
void hevcd_trasforma(int16_t *coeff, int log2_size, bool dst);
void hevcd_salta_trasformata(int16_t *coeff, int log2_size);
void hevcd_aggiungi(uint8_t *dst, int passo, const int16_t *res, int log2_size);

/* 6.5.2: the z-scan address of every smallest transform block. Built once
 * per sequence parameter set. */
int hevcd_prepara_zscan(hevcd_t *d);

#endif /* BC250_HEVC_DEC_INTERNAL_H */
