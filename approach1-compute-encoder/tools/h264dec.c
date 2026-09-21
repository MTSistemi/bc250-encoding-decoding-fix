/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264dec.c - a standalone driver for the decoder, for testing only.
 *
 *     h264dec <input.264> <output.yuv>
 *
 * Under VA-API the application parses the sequence and picture parameter
 * sets and every slice header, and hands the decoder the results. That is a
 * good deal less code for the driver, but it also means the decoder cannot
 * be run on a file without something doing that parsing - so this does, and
 * only well enough to drive the tests:
 *
 *   - progressive 4:2:0 8-bit, which is what the decoder supports anyway;
 *   - picture order count types 0 and 2;
 *   - the default reference list order, and the modification syntax;
 *   - one slice group.
 *
 * It writes decoded frames in DECODE order, not output order. For an I/P
 * stream the two are the same; when B pictures arrive, this has to grow a
 * reorder buffer, and until then it is the reference decoder's -fps_mode
 * passthrough output that has to match.
 *
 * This file is not part of the driver and is not built into it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decoder_h264.h"
#include "bitreader.h"

/* The harness runs the decoder with no GPU context, so end_picture keeps the
 * planes instead of uploading them. The upload is still referenced from the
 * object file, so it needs a body to link against; reaching it would mean the
 * null-context path had been lost, which is why it says so rather than
 * returning quietly. */
int gpu_compute_upload_nv12(gpu_context_t *ctx, gpu_image_t *image,
                            gpu_memory_t memory,
                            const uint8_t *y, int ys,
                            const uint8_t *uv, int uvs, int w, int h);
int gpu_compute_upload_nv12(gpu_context_t *ctx, gpu_image_t *image,
                            gpu_memory_t memory,
                            const uint8_t *y, int ys,
                            const uint8_t *uv, int uvs, int w, int h)
{
    (void)ctx; (void)image; (void)memory;
    (void)y; (void)ys; (void)uv; (void)uvs; (void)w; (void)h;
    fprintf(stderr, "h264dec: il decoder ha provato a caricare una superficie "
                    "senza contesto GPU\n");
    abort();
}

/* ------------------------------------------------------- parameter sets */

typedef struct {
    int valid;
    int profile_idc;
    int chroma_format_idc;
    int bit_depth_luma, bit_depth_chroma;
    int log2_max_frame_num;
    int pic_order_cnt_type;
    int log2_max_poc_lsb;
    int delta_pic_order_always_zero;
    int max_num_ref_frames;
    int mb_width, mb_height;
    int crop_left, crop_right, crop_top, crop_bottom;   /* in samples */
    int frame_mbs_only;
    int mb_adaptive;
    int direct_8x8_inference;
    uint8_t scaling4[6][16];
    uint8_t scaling8[6][64];
} sps_t;

typedef struct {
    int valid;
    int sps_id;
    int entropy_coding_mode;
    int pic_order_present;
    int num_ref_idx_default[2];
    int weighted_pred;
    int weighted_bipred_idc;
    int pic_init_qp;
    int chroma_qp_index_offset;
    int second_chroma_qp_index_offset;
    int deblocking_filter_control_present;
    int constrained_intra_pred;
    int redundant_pic_cnt_present;
    int transform_8x8_mode;
    uint8_t scaling4[6][16];
    uint8_t scaling8[6][64];
} pps_t;

static sps_t sps_store[32];
static pps_t pps_store[256];

static void piatte(uint8_t s4[6][16], uint8_t s8[6][64])
{
    for (int i = 0; i < 6; i++) {
        memset(s4[i], 16, 16);
        memset(s8[i], 16, 64);
    }
}

/* Clause 7.3.2.1.1.1. A list that is not sent is inherited, and the
 * "use default" case falls back to the flat matrix here rather than to the
 * standard's default lists: streams that ask for those are rare and the
 * harness refuses them loudly below. */
static int lista_di_scala(br_t *br, uint8_t *lista, int n, int *usa_default)
{
    int last = 8, next = 8;
    *usa_default = 0;
    for (int j = 0; j < n; j++) {
        if (next) {
            const int delta = br_read_se(br);
            next = (last + delta + 256) % 256;
            if (j == 0 && next == 0) { *usa_default = 1; return 0; }
        }
        lista[j] = (uint8_t)(next ? next : last);
        last = lista[j];
    }
    return 0;
}

static int leggi_sps(const uint8_t *rbsp, size_t n)
{
    br_t br;
    br_init(&br, rbsp, n);
    br_read(&br, 8);                        /* nal header already stripped? no */

    sps_t s;
    memset(&s, 0, sizeof(s));
    piatte(s.scaling4, s.scaling8);

    s.profile_idc = (int)br_read(&br, 8);
    br_read(&br, 8);                        /* constraint flags + reserved */
    br_read(&br, 8);                        /* level_idc */
    const int id = (int)br_read_ue(&br);
    if (id < 0 || id >= 32) return -1;

    s.chroma_format_idc = 1;
    s.bit_depth_luma = s.bit_depth_chroma = 8;
    if (s.profile_idc == 100 || s.profile_idc == 110 || s.profile_idc == 122
        || s.profile_idc == 244 || s.profile_idc == 44 || s.profile_idc == 83
        || s.profile_idc == 86 || s.profile_idc == 118 || s.profile_idc == 128) {
        s.chroma_format_idc = (int)br_read_ue(&br);
        if (s.chroma_format_idc == 3) br_read1(&br);
        s.bit_depth_luma = 8 + (int)br_read_ue(&br);
        s.bit_depth_chroma = 8 + (int)br_read_ue(&br);
        br_read1(&br);                      /* qpprime_y_zero_transform_bypass */
        if (br_read1(&br)) {                /* seq_scaling_matrix_present */
            for (int i = 0; i < 8; i++) {
                if (!br_read1(&br)) continue;
                int def = 0;
                if (i < 6) lista_di_scala(&br, s.scaling4[i], 16, &def);
                else       lista_di_scala(&br, s.scaling8[i - 6], 64, &def);
                if (def) {
                    fprintf(stderr, "questo flusso chiede le liste di scala "
                                    "di serie, non implementate\n");
                    return -1;
                }
            }
        }
    }

    s.log2_max_frame_num = 4 + (int)br_read_ue(&br);
    s.pic_order_cnt_type = (int)br_read_ue(&br);
    if (s.pic_order_cnt_type == 0) {
        s.log2_max_poc_lsb = 4 + (int)br_read_ue(&br);
    } else if (s.pic_order_cnt_type == 1) {
        s.delta_pic_order_always_zero = (int)br_read1(&br);
        br_read_se(&br);
        br_read_se(&br);
        const int c = (int)br_read_ue(&br);
        for (int i = 0; i < c; i++) br_read_se(&br);
    }
    s.max_num_ref_frames = (int)br_read_ue(&br);
    br_read1(&br);                          /* gaps_in_frame_num_allowed */
    s.mb_width = 1 + (int)br_read_ue(&br);
    const int alt = 1 + (int)br_read_ue(&br);
    s.frame_mbs_only = (int)br_read1(&br);
    if (!s.frame_mbs_only) s.mb_adaptive = (int)br_read1(&br);
    s.mb_height = alt * (2 - s.frame_mbs_only);
    s.direct_8x8_inference = (int)br_read1(&br);

    /* frame_cropping. The decoder works in whole macroblocks; what the
     * picture is actually meant to be is this much smaller, and a reference
     * decoder writes out the cropped picture. Comparing the uncropped one
     * would fail on every size that is not a multiple of sixteen. */
    if (br_read1(&br)) {
        const int sx = 2;                       /* 4:2:0 horizontal units */
        const int sy = 2 * (2 - s.frame_mbs_only);
        s.crop_left = sx * (int)br_read_ue(&br);
        s.crop_right = sx * (int)br_read_ue(&br);
        s.crop_top = sy * (int)br_read_ue(&br);
        s.crop_bottom = sy * (int)br_read_ue(&br);
    }

    s.valid = 1;
    sps_store[id] = s;
    return id;
}

static int leggi_pps(const uint8_t *rbsp, size_t n)
{
    br_t br;
    br_init(&br, rbsp, n);
    br_read(&br, 8);

    pps_t p;
    memset(&p, 0, sizeof(p));

    const int id = (int)br_read_ue(&br);
    if (id < 0 || id >= 256) return -1;
    p.sps_id = (int)br_read_ue(&br);
    if (p.sps_id < 0 || p.sps_id >= 32 || !sps_store[p.sps_id].valid) return -1;

    memcpy(p.scaling4, sps_store[p.sps_id].scaling4, sizeof(p.scaling4));
    memcpy(p.scaling8, sps_store[p.sps_id].scaling8, sizeof(p.scaling8));

    p.entropy_coding_mode = (int)br_read1(&br);
    p.pic_order_present = (int)br_read1(&br);
    const int gruppi = 1 + (int)br_read_ue(&br);
    if (gruppi != 1) { fprintf(stderr, "piu' di un gruppo di slice\n"); return -1; }
    p.num_ref_idx_default[0] = 1 + (int)br_read_ue(&br);
    p.num_ref_idx_default[1] = 1 + (int)br_read_ue(&br);
    p.weighted_pred = (int)br_read1(&br);
    p.weighted_bipred_idc = (int)br_read(&br, 2);
    p.pic_init_qp = 26 + br_read_se(&br);
    br_read_se(&br);                        /* pic_init_qs */
    p.chroma_qp_index_offset = br_read_se(&br);
    p.second_chroma_qp_index_offset = p.chroma_qp_index_offset;
    p.deblocking_filter_control_present = (int)br_read1(&br);
    p.constrained_intra_pred = (int)br_read1(&br);
    p.redundant_pic_cnt_present = (int)br_read1(&br);

    if (br_more_rbsp_data(&br)) {
        p.transform_8x8_mode = (int)br_read1(&br);
        if (br_read1(&br)) {                /* pic_scaling_matrix_present */
            const int quante = 6 + (p.transform_8x8_mode ? 2 : 0);
            for (int i = 0; i < quante; i++) {
                if (!br_read1(&br)) continue;
                int def = 0;
                if (i < 6) lista_di_scala(&br, p.scaling4[i], 16, &def);
                else       lista_di_scala(&br, p.scaling8[i - 6], 64, &def);
                if (def) {
                    fprintf(stderr, "liste di scala di serie, non implementate\n");
                    return -1;
                }
            }
        }
        p.second_chroma_qp_index_offset = br_read_se(&br);
    }

    p.valid = 1;
    pps_store[id] = p;
    return id;
}

/* Write one frame, cropped the way the stream asks for. */
static void scrivi(FILE *fo, const h264d_frame_t *f, const sps_t *sp)
{
    const int w = sp->mb_width * 16 - sp->crop_left - sp->crop_right;
    const int h = sp->mb_height * 16 - sp->crop_top - sp->crop_bottom;
    for (int y = 0; y < h; y++)
        fwrite(f->y + (size_t)(y + sp->crop_top) * f->stride_y + sp->crop_left,
               1, (size_t)w, fo);
    for (int p = 0; p < 2; p++) {
        const uint8_t *pl = p ? f->cr : f->cb;
        for (int y = 0; y < h / 2; y++)
            fwrite(pl + (size_t)(y + sp->crop_top / 2) * f->stride_c
                      + sp->crop_left / 2, 1, (size_t)(w / 2), fo);
    }
}

/* ---------------------------------------------------------- the driver */

typedef struct {
    uint32_t surface;
    int poc;
    int frame_num;
} rif_t;

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "uso: %s <ingresso.264> <uscita.yuv>\n", argv[0]);
        return 2;
    }

    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror(argv[1]); return 1; }
    fseek(fi, 0, SEEK_END);
    long len = ftell(fi);
    fseek(fi, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, fi) != (size_t)len) return 1;
    fclose(fi);

    FILE *fo = fopen(argv[2], "wb");
    if (!fo) { perror(argv[2]); return 1; }

    h264_decoder_t *dec = NULL;
    uint8_t *rbsp = malloc((size_t)len);
    int fotogrammi = 0, saltati = 0;

    /* Reference pictures, newest first, as the sliding window keeps them. */
    rif_t rifs[16];
    int n_rif = 0;
    uint32_t prossima_superficie = 1;
    int prev_poc_lsb = 0, prev_poc_msb = 0;
    int in_corso = 0;
    uint32_t superficie_corrente = 0;

    long i = 0;
    while (i + 3 < len) {
        /* Annex-B start code */
        if (!(buf[i] == 0 && buf[i + 1] == 0
              && (buf[i + 2] == 1 || (buf[i + 2] == 0 && buf[i + 3] == 1)))) {
            i++;
            continue;
        }
        const int sc = (buf[i + 2] == 1) ? 3 : 4;
        long inizio = i + sc;
        long fine = inizio;
        while (fine + 3 < len
               && !(buf[fine] == 0 && buf[fine + 1] == 0
                    && (buf[fine + 2] == 1
                        || (buf[fine + 2] == 0 && buf[fine + 3] == 1))))
            fine++;
        if (fine + 3 >= len) fine = len;
        i = fine;

        if (inizio >= fine) continue;
        const int tipo = buf[inizio] & 0x1f;
        const int nal_ref_idc = (buf[inizio] >> 5) & 3;

        if (tipo == 7) {
            size_t n = br_extract_rbsp(rbsp, (size_t)len, buf + inizio,
                                       (size_t)(fine - inizio));
            if (leggi_sps(rbsp, n) < 0) return 1;
            continue;
        }
        if (tipo == 8) {
            size_t n = br_extract_rbsp(rbsp, (size_t)len, buf + inizio,
                                       (size_t)(fine - inizio));
            if (leggi_pps(rbsp, n) < 0) return 1;
            continue;
        }
        if (tipo != 1 && tipo != 5) continue;

        /* ---- slice header ---- */
        size_t n = br_extract_rbsp(rbsp, (size_t)len, buf + inizio,
                                   (size_t)(fine - inizio));
        br_t br;
        br_init(&br, rbsp, n);
        br_read(&br, 8);

        const int first_mb = (int)br_read_ue(&br);
        int st = (int)br_read_ue(&br);
        const int slice_type = st % 5;      /* 0 P, 1 B, 2 I */
        const int pps_id = (int)br_read_ue(&br);
        if (pps_id < 0 || pps_id >= 256 || !pps_store[pps_id].valid) return 1;
        const pps_t *pp = &pps_store[pps_id];
        const sps_t *sp = &sps_store[pp->sps_id];

        if (!h264_decoder_supports(sp->profile_idc, sp->chroma_format_idc,
                                   sp->bit_depth_luma, sp->bit_depth_chroma,
                                   sp->frame_mbs_only, sp->mb_adaptive)) {
            fprintf(stderr, "flusso fuori da quello che il decoder copre\n");
            return 3;
        }

        const int frame_num = (int)br_read(&br, sp->log2_max_frame_num);
        const int idr = (tipo == 5);
        if (idr) br_read_ue(&br);           /* idr_pic_id */

        int poc = 0;
        if (sp->pic_order_cnt_type == 0) {
            const int lsb = (int)br_read(&br, sp->log2_max_poc_lsb);
            const int max = 1 << sp->log2_max_poc_lsb;
            int msb;
            if (idr) { prev_poc_lsb = 0; prev_poc_msb = 0; }
            if (lsb < prev_poc_lsb && prev_poc_lsb - lsb >= max / 2)
                msb = prev_poc_msb + max;
            else if (lsb > prev_poc_lsb && lsb - prev_poc_lsb > max / 2)
                msb = prev_poc_msb - max;
            else
                msb = prev_poc_msb;
            poc = msb + lsb;
            if (nal_ref_idc) { prev_poc_lsb = lsb; prev_poc_msb = msb; }
            if (pp->pic_order_present) br_read_se(&br);
        } else if (sp->pic_order_cnt_type == 2) {
            poc = 2 * frame_num;
        } else {
            fprintf(stderr, "pic_order_cnt_type 1 non implementato\n");
            return 3;
        }

        if (pp->redundant_pic_cnt_present && br_read_ue(&br) != 0) continue;

        h264d_slice_t sl;
        memset(&sl, 0, sizeof(sl));
        sl.type = slice_type;
        sl.first_mb = first_mb;
        sl.num_ref_idx[0] = pp->num_ref_idx_default[0];
        sl.num_ref_idx[1] = pp->num_ref_idx_default[1];

        if (slice_type == 1) sl.direct_spatial_mv_pred = br_read1(&br);
        if (slice_type != 2) {
            if (br_read1(&br)) {            /* num_ref_idx_active_override */
                sl.num_ref_idx[0] = 1 + (int)br_read_ue(&br);
                if (slice_type == 1) sl.num_ref_idx[1] = 1 + (int)br_read_ue(&br);
            }
        }

        /* ---- reference list, newest short-term first (8.2.4.2.1) ---- */
        /* Filled in after begin_picture, which is when the DPB slots the
         * list has to name are settled. */
        for (int k = 0; k < 32; k++) sl.ref_list[0][k] = sl.ref_list[1][k] = 0;

        /* reference list modification: refused rather than ignored */
        if (slice_type != 2) {
            if (br_read1(&br)) {
                fprintf(stderr, "ref_pic_list_modification presente, "
                                "non implementata\n");
                return 3;
            }
        }
        if ((pp->weighted_pred && slice_type == 0)
            || (pp->weighted_bipred_idc == 1 && slice_type == 1)) {
            fprintf(stderr, "pesi espliciti nell'header, non implementati\n");
            return 3;
        }
        if (nal_ref_idc) {
            if (idr) { br_read1(&br); br_read1(&br); }
            else if (br_read1(&br)) {       /* adaptive_ref_pic_marking */
                fprintf(stderr, "marcatura adattiva dei riferimenti, "
                                "non implementata\n");
                return 3;
            }
        }
        if (pp->entropy_coding_mode && slice_type != 2)
            sl.cabac_init_idc = (int)br_read_ue(&br);
        sl.qpy = pp->pic_init_qp + br_read_se(&br);

        if (pp->deblocking_filter_control_present) {
            sl.disable_deblocking_filter_idc = (int)br_read_ue(&br);
            if (sl.disable_deblocking_filter_idc != 1) {
                sl.alpha_c0_offset = 2 * br_read_se(&br);
                sl.beta_offset = 2 * br_read_se(&br);
            }
        }

        const int bit_offset = (int)br.bitpos;

        /* ---- the picture ---- */
        if (!dec) {
            dec = h264_decoder_create(NULL, (uint32_t)(sp->mb_width * 16),
                                      (uint32_t)(sp->mb_height * 16));
            if (!dec) return 1;
        }

        if (first_mb == 0) {
            if (in_corso) {
                h264_decoder_end_picture(dec, (gpu_image_t){0}, (gpu_memory_t){0});
                h264d_frame_t *f = h264_decoder_frame_for(dec, superficie_corrente);
                if (f) {
                    scrivi(fo, f, sp);
                    fotogrammi++;
                }
            }
            if (idr) n_rif = 0;
            superficie_corrente = prossima_superficie++;

            h264d_pic_t pic;
            memset(&pic, 0, sizeof(pic));
            pic.width = sp->mb_width * 16;
            pic.height = sp->mb_height * 16;
            pic.mb_width = sp->mb_width;
            pic.mb_height = sp->mb_height;
            pic.chroma_qp_index_offset = pp->chroma_qp_index_offset;
            pic.second_chroma_qp_index_offset = pp->second_chroma_qp_index_offset;
            pic.pic_init_qp = pp->pic_init_qp;
            pic.num_ref_idx_l0 = pp->num_ref_idx_default[0];
            pic.num_ref_idx_l1 = pp->num_ref_idx_default[1];
            pic.entropy_coding_mode = pp->entropy_coding_mode;
            pic.transform_8x8_mode = pp->transform_8x8_mode;
            pic.constrained_intra_pred = pp->constrained_intra_pred;
            pic.direct_8x8_inference = sp->direct_8x8_inference;
            pic.deblocking_filter_control_present = pp->deblocking_filter_control_present;
            pic.weighted_pred = pp->weighted_pred;
            pic.weighted_bipred_idc = pp->weighted_bipred_idc;
            pic.pic_order_present = pp->pic_order_present;
            memcpy(pic.scaling4, pp->scaling4, sizeof(pic.scaling4));
            memcpy(pic.scaling8, pp->scaling8, sizeof(pic.scaling8));

            h264_decoder_begin_picture(dec, &pic, superficie_corrente, poc, frame_num);

            uint32_t sup[16];
            int pocs[16];
            bool lt[16];
            for (int k = 0; k < n_rif; k++) {
                sup[k] = rifs[k].surface;
                pocs[k] = rifs[k].poc;
                lt[k] = false;
            }
            h264_decoder_set_references(dec, sup, pocs, lt, n_rif);
            in_corso = 1;
        }

        /* The reference list has to name DPB slots, which only exist once
         * begin_picture has run. */
        for (int k = 0; k < n_rif && k < 32; k++) {
            h264d_frame_t *f = h264_decoder_frame_for(dec, rifs[k].surface);
            sl.ref_list[0][k] = f ? (int8_t)h264_decoder_slot_of(dec, f) : 0;
        }

        if (getenv("BC250_H264_REFS"))
            fprintf(stderr, "fotogramma %d: tipo %d frame_num %d poc %d "
                            "superficie %u slot %d | num_ref %d | lista:%s",
                    fotogrammi, slice_type, frame_num, poc, superficie_corrente,
                    h264_decoder_slot_of(dec,
                        h264_decoder_frame_for(dec, superficie_corrente)),
                    sl.num_ref_idx[0], "");
        if (getenv("BC250_H264_REFS")) {
            for (int k = 0; k < n_rif; k++)
                fprintf(stderr, " sup%u=slot%d(fn%d)", rifs[k].surface,
                        sl.ref_list[0][k], rifs[k].frame_num);
            fprintf(stderr, "\n");
        }

        const int r = h264_decoder_slice(dec, &sl, rbsp, n, bit_offset);
        if (r) {
            fprintf(stderr, "slice rifiutata (%d) al macroblocco %d del "
                            "fotogramma %d\n", r, first_mb, fotogrammi);
            saltati++;
            if (r == -2 || r == -3) return 3;
            return 4;
        }

        if (nal_ref_idc) {
            for (int k = 15; k > 0; k--) rifs[k] = rifs[k - 1];
            rifs[0].surface = superficie_corrente;
            rifs[0].poc = poc;
            rifs[0].frame_num = frame_num;
            if (n_rif < sp->max_num_ref_frames && n_rif < 16) n_rif++;
        }
    }

    if (in_corso && dec) {
        h264_decoder_end_picture(dec, (gpu_image_t){0}, (gpu_memory_t){0});
        h264d_frame_t *f = h264_decoder_frame_for(dec, superficie_corrente);
        if (f) {
            const sps_t *sp = NULL;
            for (int k = 0; k < 32; k++) if (sps_store[k].valid) { sp = &sps_store[k]; break; }
            if (sp) {
                scrivi(fo, f, sp);
                fotogrammi++;
            }
        }
    }

    fclose(fo);
    h264_decoder_destroy(dec);
    free(buf);
    free(rbsp);
    printf("%d fotogrammi decodificati, %d slice rifiutate\n", fotogrammi, saltati);
    return saltati ? 5 : 0;
}
