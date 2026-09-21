/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * decoder_h264.c - the picture level: the frame store, the slice loop and
 * the hand-over of the finished picture.
 */
#include "h264_dec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ lifetime */

bool h264_decoder_supports(int profile_idc, int chroma_format_idc,
                           int bit_depth_luma, int bit_depth_chroma,
                           bool frame_mbs_only, bool mb_adaptive)
{
    (void)profile_idc;          /* High, Main and Baseline are all fine */
    if (chroma_format_idc != 1) return false;      /* 4:2:0 only */
    if (bit_depth_luma != 8 || bit_depth_chroma != 8) return false;
    if (!frame_mbs_only || mb_adaptive) return false;   /* progressive only */
    return true;
}

static void libera_frame(h264d_frame_t *f)
{
    free(f->y);
    f->y = f->cb = f->cr = NULL;
    f->used = false;
    f->surface = ~0u;
}

static int alloca_frame(h264d_frame_t *f, int w, int h)
{
    const int sy = (w + 31) & ~31;
    const int sc = (w / 2 + 31) & ~31;
    const size_t n = (size_t)sy * h + 2 * (size_t)sc * (h / 2);
    f->y = calloc(1, n);
    if (!f->y) return -1;
    f->cb = f->y + (size_t)sy * h;
    f->cr = f->cb + (size_t)sc * (h / 2);
    f->stride_y = sy;
    f->stride_c = sc;
    f->surface = ~0u;
    f->used = false;
    return 0;
}

h264_decoder_t *h264_decoder_create(bc250_gpu_context_t *gpu_ctx,
                                    uint32_t width, uint32_t height)
{
    h264_decoder_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->gpu = gpu_ctx;
    d->width = (int)width;
    d->height = (int)height;
    d->mb_w = ((int)width + 15) / 16;
    d->mb_h = ((int)height + 15) / 16;
    d->mb_count = d->mb_w * d->mb_h;

    d->mbs = calloc((size_t)d->mb_count, sizeof(h264d_mb_t));
    d->slice_of_mb = calloc((size_t)d->mb_count, 1);
    d->rbsp_cap = (size_t)d->mb_count * 512 + 65536;
    d->rbsp = malloc(d->rbsp_cap);
    if (!d->mbs || !d->slice_of_mb || !d->rbsp) {
        h264_decoder_destroy(d);
        return NULL;
    }

    for (int i = 0; i < H264D_DPB_SIZE; i++) {
        if (alloca_frame(&d->dpb[i], d->mb_w * 16, d->mb_h * 16) != 0) {
            h264_decoder_destroy(d);
            return NULL;
        }
    }
    d->cur = -1;
    return d;
}

void h264_decoder_destroy(h264_decoder_t *d)
{
    if (!d) return;
    for (int i = 0; i < H264D_DPB_SIZE; i++)
        libera_frame(&d->dpb[i]);
    free(d->mbs);
    free(d->slice_of_mb);
    free(d->rbsp);
    free(d);
}

h264d_frame_t *h264_decoder_frame_for(h264_decoder_t *d, uint32_t surface)
{
    for (int i = 0; i < H264D_DPB_SIZE; i++)
        if (d->dpb[i].used && d->dpb[i].surface == surface)
            return &d->dpb[i];
    return NULL;
}

int h264_decoder_slot_of(const h264_decoder_t *d, const h264d_frame_t *f)
{
    const int i = (int)(f - d->dpb);
    return (i >= 0 && i < H264D_DPB_SIZE) ? i : 0;
}

/* The slot a surface already occupies, or a free one. */
static int slot_per(h264_decoder_t *d, uint32_t surface)
{
    for (int i = 0; i < H264D_DPB_SIZE; i++)
        if (d->dpb[i].used && d->dpb[i].surface == surface)
            return i;
    for (int i = 0; i < H264D_DPB_SIZE; i++)
        if (!d->dpb[i].used)
            return i;
    /* Everything is claimed: take the one the application has not listed as
     * a reference for the longest, which is the smallest POC. */
    int peggiore = 0;
    for (int i = 1; i < H264D_DPB_SIZE; i++)
        if (d->dpb[i].poc < d->dpb[peggiore].poc)
            peggiore = i;
    return peggiore;
}

void h264_decoder_set_references(h264_decoder_t *d, const uint32_t *refs,
                                 const int *pocs, const bool *long_term, int n)
{
    bool tenere[H264D_DPB_SIZE] = { false };
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < H264D_DPB_SIZE; i++) {
            if (d->dpb[i].used && d->dpb[i].surface == refs[k]) {
                tenere[i] = true;
                d->dpb[i].poc = pocs[k];
                d->dpb[i].is_long_term = long_term[k];
            }
        }
    }
    if (d->cur >= 0) tenere[d->cur] = true;
    for (int i = 0; i < H264D_DPB_SIZE; i++)
        if (d->dpb[i].used && !tenere[i])
            d->dpb[i].used = false;
}

/* ------------------------------------------------------------- picture */

int h264_decoder_begin_picture(h264_decoder_t *d, const h264d_pic_t *pic,
                               uint32_t surface, int poc, int frame_num)
{
    d->pic = *pic;
    d->cur = slot_per(d, surface);
    h264d_frame_t *f = &d->dpb[d->cur];
    f->surface = surface;
    f->poc = poc;
    f->frame_num = frame_num;
    f->used = true;

    memset(d->mbs, 0, (size_t)d->mb_count * sizeof(h264d_mb_t));
    for (int i = 0; i < d->mb_count; i++)
        memset(d->mbs[i].ref, -1, sizeof(d->mbs[i].ref));
    memset(d->slice_of_mb, 0xff, (size_t)d->mb_count);
    d->n_slices = 0;
    d->dequant.valid = 0;
    d->dequant_c[0].valid = d->dequant_c[1].valid = 0;
    return 0;
}

int h264_decoder_slice(h264_decoder_t *d, const h264d_slice_t *slice,
                       const uint8_t *data, size_t size, int bit_offset)
{
    if (d->cur < 0) return -1;
    if (d->n_slices >= H264D_MAX_SLICES) return -1;

    /* B slices need the direct derivation, which needs the co-located
     * picture's motion field. Until that store exists they are refused here
     * rather than decoded into something that merely looks plausible. */
    if (slice->type == 1) return -2;

    d->slice = *slice;
    const int numero = d->n_slices++;
    d->deblock[numero].disable_idc = (int8_t)slice->disable_deblocking_filter_idc;
    d->deblock[numero].alpha_offset = (int8_t)slice->alpha_c0_offset;
    d->deblock[numero].beta_offset = (int8_t)slice->beta_offset;

    /* The slice data starts at a byte boundary once the CABAC alignment bits
     * are skipped, and the emulation prevention bytes have to come out
     * before the engine ever sees them. */
    const int primo_byte = (bit_offset + 7) / 8;
    if ((size_t)primo_byte >= size) return -1;
    const size_t n = br_extract_rbsp(d->rbsp, d->rbsp_cap,
                                     data + primo_byte, size - primo_byte);

    d->cabac_mode = d->pic.entropy_coding_mode;
    if (!d->cabac_mode) return -3;          /* CAVLC not written yet */

    d->qpy = slice->qpy;
    d->last_qp_delta_nonzero = 0;
    d->mb_idx = slice->first_mb;
    if (d->mb_idx >= d->mb_count) return -1;
    d->mb_x = d->mb_idx % d->mb_w;
    d->mb_y = d->mb_idx / d->mb_w;

    h264d_cabac_init(&d->cabac, d->rbsp, n, slice->type == 2,
                     slice->cabac_init_idc, slice->qpy);

    const char *traccia = getenv("BC250_H264_TRACE");
    int quanti = 0;

    for (;;) {
        d->slice_of_mb[d->mb_idx] = (uint8_t)numero;
        const int r = h264d_decode_mb_cabac(d);
        if (r) return r;
        quanti++;
        if (traccia) {
            const h264d_mb_t *m = &d->mbs[d->mb_idx];
            fprintf(stderr, "mb %4d (%2d,%2d) tipo %d intra %d t8 %d cbp %02x qp %2d",
                    d->mb_idx, d->mb_x, d->mb_y, m->type, m->intra,
                    m->transform8x8, m->cbp, m->qpy);
            if (m->intra) {
                fprintf(stderr, " croma %d modi", m->chroma_pred_mode);
                for (int k = 0; k < 16; k++) fprintf(stderr, " %d", m->ipred[k]);
            }
            fprintf(stderr, "\n");
        }

        if (h264d_cabac_terminate(&d->cabac))
            break;
        if (h264d_cabac_overrun(&d->cabac))
            return -4;

        d->mb_idx++;
        if (d->mb_idx >= d->mb_count)
            break;
        d->mb_x = d->mb_idx % d->mb_w;
        d->mb_y = d->mb_idx / d->mb_w;
    }
    if (traccia)
        fprintf(stderr, "slice finita: %d macroblocchi, sforamento %d\n",
                quanti, (int)h264d_cabac_overrun(&d->cabac));
    return 0;
}

int h264_decoder_end_picture(h264_decoder_t *d, gpu_image_t out,
                             gpu_memory_t out_memory)
{
    if (d->cur < 0) return -1;
    h264d_frame_t *f = &d->dpb[d->cur];

    /* Any macroblock no slice covered is left as it was allocated. Marking
     * them as belonging to no slice keeps the deblocking filter from
     * treating them as part of their neighbour's. */
    h264d_deblock_picture(f->y, f->stride_y, f->cb, f->cr, f->stride_c,
                          d->mb_w, d->mb_h, d->mbs, d->slice_of_mb,
                          d->deblock, d->pic.chroma_qp_index_offset,
                          d->pic.second_chroma_qp_index_offset);

    if (!d->gpu)
        return 0;            /* the standalone harness keeps the planes */

    /* The surface wants NV12, so the two chroma planes are interleaved on
     * the way out. Written once, at the end: surface memory is
     * write-combining, which is fast to write and very slow to read. */
    const int cw = d->width / 2, ch = d->height / 2;
    uint8_t *uv = malloc((size_t)cw * 2 * ch);
    if (!uv) return -1;
    for (int y = 0; y < ch; y++) {
        const uint8_t *a = f->cb + (size_t)y * f->stride_c;
        const uint8_t *b = f->cr + (size_t)y * f->stride_c;
        uint8_t *o = uv + (size_t)y * cw * 2;
        for (int x = 0; x < cw; x++) {
            o[2 * x] = a[x];
            o[2 * x + 1] = b[x];
        }
    }
    const int r = gpu_compute_upload_nv12(d->gpu, &out, out_memory,
                                          f->y, f->stride_y,
                                          uv, cw * 2,
                                          d->width, d->height);
    free(uv);
    return r;
}
