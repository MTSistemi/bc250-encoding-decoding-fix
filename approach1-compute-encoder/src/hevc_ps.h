/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_ps.h - the parameter sets and the slice segment header, Rec. ITU-T
 * H.265 clause 7.3.2 and 7.3.6.
 *
 * ⚠️ None of this is the decoder's. Under VA-API the application has
 * already parsed all of it and hands the driver the values; this exists so
 * the standalone harness can drive the decoder from a file, and so the two
 * can be compared. Everything here is therefore free to refuse whatever it
 * does not need, loudly.
 */
#ifndef BC250_HEVC_PS_H
#define BC250_HEVC_PS_H

#include <stdbool.h>
#include <stdint.h>

#include "bitreader.h"

/* NAL unit types, Table 7-1. Only the ones that change what happens. */
enum {
    HEVC_NAL_TRAIL_N = 0, HEVC_NAL_TRAIL_R = 1,
    HEVC_NAL_TSA_N = 2,   HEVC_NAL_TSA_R = 3,
    HEVC_NAL_STSA_N = 4,  HEVC_NAL_STSA_R = 5,
    HEVC_NAL_RADL_N = 6,  HEVC_NAL_RADL_R = 7,
    HEVC_NAL_RASL_N = 8,  HEVC_NAL_RASL_R = 9,
    HEVC_NAL_BLA_W_LP = 16, HEVC_NAL_BLA_W_RADL = 17, HEVC_NAL_BLA_N_LP = 18,
    HEVC_NAL_IDR_W_RADL = 19, HEVC_NAL_IDR_N_LP = 20, HEVC_NAL_CRA = 21,
    HEVC_NAL_VPS = 32, HEVC_NAL_SPS = 33, HEVC_NAL_PPS = 34,
    HEVC_NAL_AUD = 35, HEVC_NAL_EOS = 36, HEVC_NAL_EOB = 37,
    HEVC_NAL_FD = 38, HEVC_NAL_SEI_PREFIX = 39, HEVC_NAL_SEI_SUFFIX = 40,
};

static inline bool hevc_nal_e_slice(int t)
{
    return t <= HEVC_NAL_RASL_R || (t >= HEVC_NAL_BLA_W_LP && t <= HEVC_NAL_CRA);
}

static inline bool hevc_nal_e_irap(int t)
{
    return t >= HEVC_NAL_BLA_W_LP && t <= HEVC_NAL_CRA;
}

static inline bool hevc_nal_e_idr(int t)
{
    return t == HEVC_NAL_IDR_W_RADL || t == HEVC_NAL_IDR_N_LP;
}

/* A short-term reference picture set, clause 7.4.8. The deltas are already
 * resolved: what comes out is how far back or forward each picture is and
 * whether it is used by the current one. */
#define HEVC_MAX_RPS 16

typedef struct {
    int num_negative, num_positive;
    int delta_poc[HEVC_MAX_RPS * 2];   /* negatives first, then positives */
    bool used[HEVC_MAX_RPS * 2];
} hevc_st_rps_t;

/* Only what the decoder or the harness actually reads. profile_tier_level
 * is parsed to get past it, not to act on it. */
typedef struct {
    bool valid;
    int sps_id, vps_id;
    int chroma_format_idc;
    bool separate_colour_plane;
    int width, height;                  /* pic_width/height_in_luma_samples */
    int crop_left, crop_right, crop_top, crop_bottom;   /* in luma samples */
    int bit_depth_luma, bit_depth_chroma;
    int log2_max_poc_lsb;
    int max_dec_pic_buffering, num_reorder_pics;

    int log2_min_cb, log2_ctb;          /* coding block, smallest and largest */
    int log2_min_tb, log2_max_tb;
    int max_transform_hierarchy_depth_inter, max_transform_hierarchy_depth_intra;

    bool scaling_list_enabled, sps_scaling_list_present;
    bool amp_enabled, sao_enabled;
    bool pcm_enabled;
    int pcm_bit_depth_luma, pcm_bit_depth_chroma;
    int log2_min_pcm_cb, log2_max_pcm_cb;
    bool pcm_loop_filter_disabled;

    int num_st_rps;
    hevc_st_rps_t st_rps[65];
    bool long_term_ref_pics_present;
    int num_long_term_sps;

    bool temporal_mvp_enabled, strong_intra_smoothing;

    /* Derived, because everything downstream wants them. */
    int ctb_size, ctb_width, ctb_height, ctb_count;
    int min_cb_width, min_cb_height;
} hevc_sps_t;

typedef struct {
    bool valid;
    int pps_id, sps_id;
    bool dependent_slice_segments_enabled;
    bool output_flag_present;
    int num_extra_slice_header_bits;
    bool sign_data_hiding, cabac_init_present;
    int num_ref_idx_default[2];
    int init_qp;
    bool constrained_intra_pred, transform_skip_enabled;
    bool cu_qp_delta_enabled;
    int diff_cu_qp_delta_depth;
    int cb_qp_offset, cr_qp_offset;
    bool slice_chroma_qp_offsets_present;
    bool weighted_pred, weighted_bipred;
    bool transquant_bypass_enabled;
    bool tiles_enabled, entropy_coding_sync_enabled;
    int num_tile_columns, num_tile_rows;
    bool uniform_spacing;
    int column_width[32], row_height[32];
    bool loop_filter_across_tiles, loop_filter_across_slices;
    bool deblocking_filter_control_present, deblocking_filter_override_enabled;
    bool deblocking_filter_disabled;
    int beta_offset, tc_offset;          /* already doubled */
    bool pps_scaling_list_present;
    bool lists_modification_present;
    int log2_parallel_merge_level;
    bool slice_segment_header_extension_present;
} hevc_pps_t;

/* What one slice segment header says. */
typedef struct {
    int nal_type;
    bool first_slice_in_pic;
    bool no_output_of_prior_pics;
    int pps_id;
    bool dependent_slice_segment;
    int segment_address;                 /* in coding tree blocks */

    int type;                            /* 0 B, 1 P, 2 I */
    bool pic_output_flag;
    int poc_lsb;
    int poc;                             /* derived by the caller */

    bool short_term_ref_pic_set_sps_flag;
    int short_term_ref_pic_set_idx;
    hevc_st_rps_t st_rps;                /* the one this slice uses */

    bool temporal_mvp_enabled;
    bool sao_luma, sao_chroma;

    int num_ref_idx[2];
    bool mvd_l1_zero, cabac_init_flag;
    bool collocated_from_l0;
    int collocated_ref_idx;
    int five_minus_max_num_merge_cand;

    int qp;                              /* SliceQpY */
    int cb_qp_offset, cr_qp_offset;
    bool deblocking_filter_disabled;
    int beta_offset, tc_offset;          /* already doubled */
    bool loop_filter_across_slices;

    /* Explicit weighted prediction, clause 7.3.6.3. */
    int luma_log2_weight_denom, chroma_log2_weight_denom;
    int16_t luma_weight[2][16], luma_offset[2][16];
    int16_t chroma_weight[2][16][2], chroma_offset[2][16][2];

    int num_entry_point_offsets;
    /* ⚠️ In bytes of the NAL unit, emulation prevention bytes included,
     * counted from the first byte of the slice segment data. A decoder
     * that works on the un-escaped payload has to take them out again. */
    uint32_t entry_point[600];
    size_t data_bit_offset;              /* where slice_segment_data starts */
} hevc_slice_t;

/* Returns 0, or a negative code naming what was refused. */
int hevc_ps_read_sps(hevc_sps_t *out, const uint8_t *rbsp, size_t n);
int hevc_ps_read_pps(hevc_pps_t *out, const uint8_t *rbsp, size_t n);
int hevc_ps_read_slice(hevc_slice_t *out, const uint8_t *rbsp, size_t n,
                        int nal_type, const hevc_sps_t *sps_store,
                        const hevc_pps_t *pps_store);

#endif /* BC250_HEVC_PS_H */
