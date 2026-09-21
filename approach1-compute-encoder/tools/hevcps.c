/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevcps.c - read an H.265 stream's parameter sets and slice headers, and
 * say what is in them.
 *
 *     hevcps [-q] <file.265>
 *
 * Nothing is decoded. This is the first half of a decoder's harness: it
 * proves the syntax above the slice data can be read, which is the part
 * VA-API hands over ready-made and which a file does not. It exits
 * non-zero if any of it is refused, so it can be run over a spread of
 * streams as a test.
 *
 * ⚠️ The picture order count is derived here rather than read: only its
 * low bits are sent, and the high bits come from the picture before. Get
 * it wrong and every reference list points at the wrong picture, which is
 * why it is worth checking on its own before anything depends on it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitreader.h"
#include "hevc_ps.h"
#include "decoder_h265.h"
#include "hevc_dec_internal.h"

static const char *nome_nal(int t)
{
    switch (t) {
    case HEVC_NAL_TRAIL_N: return "TRAIL_N";
    case HEVC_NAL_TRAIL_R: return "TRAIL_R";
    case HEVC_NAL_TSA_N: return "TSA_N";
    case HEVC_NAL_TSA_R: return "TSA_R";
    case HEVC_NAL_STSA_N: return "STSA_N";
    case HEVC_NAL_STSA_R: return "STSA_R";
    case HEVC_NAL_RADL_N: return "RADL_N";
    case HEVC_NAL_RADL_R: return "RADL_R";
    case HEVC_NAL_RASL_N: return "RASL_N";
    case HEVC_NAL_RASL_R: return "RASL_R";
    case HEVC_NAL_BLA_W_LP: return "BLA_W_LP";
    case HEVC_NAL_BLA_W_RADL: return "BLA_W_RADL";
    case HEVC_NAL_BLA_N_LP: return "BLA_N_LP";
    case HEVC_NAL_IDR_W_RADL: return "IDR_W_RADL";
    case HEVC_NAL_IDR_N_LP: return "IDR_N_LP";
    case HEVC_NAL_CRA: return "CRA";
    case HEVC_NAL_VPS: return "VPS";
    case HEVC_NAL_SPS: return "SPS";
    case HEVC_NAL_PPS: return "PPS";
    case HEVC_NAL_AUD: return "AUD";
    case HEVC_NAL_EOS: return "EOS";
    case HEVC_NAL_EOB: return "EOB";
    case HEVC_NAL_FD: return "FD";
    case HEVC_NAL_SEI_PREFIX: return "SEI";
    case HEVC_NAL_SEI_SUFFIX: return "SEI_SUFFIX";
    default: return "?";
    }
}

static const char *perche(int r)
{
    switch (r) {
    case -1: return "NAL troncato";
    case -2: return "non supportato";
    case -3: return "fuori dallo standard";
    default: return "?";
    }
}

static const char tipo_slice[3] = { 'B', 'P', 'I' };

/* Between one IDR and the next, the picture order counts must be exactly
 * 0..k-1: every one of them present, and none of them twice. They arrive
 * in decode order, which with B pictures is not display order, so the
 * question is about the SET and not about the sequence. */
static bool verifica_gruppo(const int *poc, int quanti)
{
    if (quanti <= 0) return true;
    for (int atteso = 0; atteso < quanti; atteso++) {
        int trovati = 0;
        for (int i = 0; i < quanti; i++)
            if (poc[i] == atteso) trovati++;
        if (trovati != 1) {
            fprintf(stderr, "poc %d visto %d volte su %d immagini\n",
                    atteso, trovati, quanti);
            return false;
        }
    }
    return true;
}



/* The decoded picture buffer.
 *
 * ⚠️ Every picture here is one another may still point at. A picture is
 * let go when the reference picture set of a later one stops naming it,
 * and not a moment before: the set is the only thing that says so, and a
 * decoder that frees on its own idea of "old" loses a reference the
 * stream was still counting on. */







/* Pictures wait here until the stream ends, because the order they come
 * out in is not the order they were decoded in. */
typedef struct {
    long ordine;
    uint8_t *dati;
    size_t n;
} fotogramma_t;

static fotogramma_t *ordine_uscita;
static int n_uscita, cap_uscita;

static int confronta_ordine(const void *a, const void *b)
{
    const long x = ((const fotogramma_t *)a)->ordine;
    const long y = ((const fotogramma_t *)b)->ordine;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Cropped on the way out: the coded picture is a whole number of smallest
 * coding blocks and the visible one is not. */
static void scrivi_immagine(FILE *f, hevc_decoder_t *dc,
                            const hevc_sps_t *sps, long ordine)
{
    int passo[3];
    const uint8_t *piano[3];
    /* ⚠️ The loop filters run here and not at the end of each slice: 8.7.2
     * is defined over the whole picture, and an edge between two coding
     * tree units cannot be filtered until both of them exist. */
    hevc_decoder_end_picture(dc);
    for (int i = 0; i < 3; i++) piano[i] = hevc_decoder_piano(dc, i, &passo[i]);
    if (!f || !piano[0]) return;

    const int x0 = sps->crop_left, y0 = sps->crop_top;
    const int w = sps->width - sps->crop_left - sps->crop_right;
    const int h = sps->height - sps->crop_top - sps->crop_bottom;
    const size_t n = (size_t)w * h + 2 * (size_t)(w / 2) * (h / 2);

    if (n_uscita == cap_uscita) {
        const int nuovo = cap_uscita ? cap_uscita * 2 : 32;
        fotogramma_t *p = realloc(ordine_uscita, (size_t)nuovo * sizeof *p);
        if (!p) return;
        ordine_uscita = p;
        cap_uscita = nuovo;
    }
    uint8_t *dati = malloc(n);
    if (!dati) return;

    size_t o = 0;
    for (int y = 0; y < h; y++) {
        memcpy(dati + o, piano[0] + (size_t)(y0 + y) * passo[0] + x0,
               (size_t)w);
        o += (size_t)w;
    }
    for (int p = 1; p < 3; p++)
        for (int y = 0; y < h / 2; y++) {
            memcpy(dati + o,
                   piano[p] + (size_t)(y0 / 2 + y) * passo[p] + x0 / 2,
                   (size_t)(w / 2));
            o += (size_t)(w / 2);
        }

    ordine_uscita[n_uscita].ordine = ordine;
    ordine_uscita[n_uscita].dati = dati;
    ordine_uscita[n_uscita].n = n;
    n_uscita++;
}

/* Display order at last: sorted by picture order count, and by which
 * instantaneous refresh they belong to, since the count restarts at every
 * one of those. */
static void svuota_uscita(FILE *f)
{
    if (ordine_uscita) qsort(ordine_uscita, (size_t)n_uscita,
                             sizeof *ordine_uscita, confronta_ordine);
    for (int i = 0; i < n_uscita; i++) {
        if (f) fwrite(ordine_uscita[i].dati, 1, ordine_uscita[i].n, f);
        free(ordine_uscita[i].dati);
    }
    free(ordine_uscita);
    ordine_uscita = NULL;
    n_uscita = cap_uscita = 0;
}



/* The harness runs the decoder with no GPU context, so the finished
 * picture stays in memory instead of going to a surface. The upload is
 * still referenced from the object file, so it needs a body to link
 * against; reaching it would mean the null-context path had been lost,
 * which is why it says so rather than returning quietly. */
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
    fprintf(stderr, "l'harness non ha una GPU a cui dare l'immagine\n");
    abort();
}

int main(int argc, char **argv)
{
    bool zitto = false, solo_intestazioni = false;
    const char *nome = NULL, *uscita = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) zitto = true;
        else if (strcmp(argv[i], "-h") == 0) solo_intestazioni = true;
        else if (!nome) nome = argv[i];
        else uscita = argv[i];
    }
    if (!nome) {
        fprintf(stderr, "uso: hevcps [-q] [-h] <file.265> [uscita.yuv]\n");
        return 2;
    }

    FILE *f = fopen(nome, "rb");
    if (!f) { perror(nome); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    uint8_t *rbsp = malloc((size_t)len);
    if (!buf || !rbsp || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "%s: non riesco a leggerlo\n", nome);
        return 2;
    }
    fclose(f);

    hevc_sps_t *sps = calloc(16, sizeof(hevc_sps_t));
    hevc_pps_t *pps = calloc(64, sizeof(hevc_pps_t));
    if (!sps || !pps) return 2;

    /* Clause 8.3.1, the picture order count: only its low bits are sent. */
    int prev_poc_lsb = 0, prev_poc_msb = 0;
    int immagini = 0, slice_totali = 0, rifiutate = 0;
    int slice_di_questa = 0;

    /* The picture order counts seen since the last IDR. They must come out
     * as exactly 0..k-1: every one present, none twice. An IDR restarts
     * the count, so the check is per group and not over the whole file. */
    int *poc_visti = calloc(4096, sizeof(int));
    int n_visti = 0, gruppi_rotti = 0;
    if (!poc_visti) return 2;

    hevc_decoder_t *dec = hevc_decoder_create(NULL, 0, 0);
    if (!dec) return 2;
    int slice_lette = 0, slice_perse = 0, slice_saltate = 0;
    FILE *fo = uscita ? fopen(uscita, "wb") : NULL;
    if (uscita && !fo) { perror(uscita); return 2; }
    bool immagine_aperta = false;
    long base_ordine = 0, prossimo_ordine = 0, ordine_corrente = 0;

    for (long i = 0; i + 3 < len; ) {
        /* Find the start code, then the next one. */
        if (!(buf[i] == 0 && buf[i + 1] == 0
              && (buf[i + 2] == 1 || (buf[i + 2] == 0 && buf[i + 3] == 1)))) {
            i++;
            continue;
        }
        long inizio = i + ((buf[i + 2] == 1) ? 3 : 4);
        long fine = inizio;
        while (fine + 3 < len
               && !(buf[fine] == 0 && buf[fine + 1] == 0
                    && (buf[fine + 2] == 1
                        || (buf[fine + 2] == 0 && buf[fine + 3] == 1))))
            fine++;
        if (fine + 3 >= len) fine = len;
        i = fine;
        if (inizio >= fine) continue;

        const int tipo = (buf[inizio] >> 1) & 0x3f;
        const size_t n = br_extract_rbsp(rbsp, (size_t)len, buf + inizio,
                                         (size_t)(fine - inizio));

        if (tipo == HEVC_NAL_SPS) {
            hevc_sps_t s;
            const int r = hevc_ps_leggi_sps(&s, rbsp, n);
            if (r) {
                fprintf(stderr, "SPS rifiutato: %s\n", perche(r));
                rifiutate++;
                continue;
            }
            sps[s.sps_id] = s;
            if (!zitto)
                printf("SPS %d: %dx%d, ritaglio %d/%d/%d/%d, CTB %d, "
                       "CB %d..%d, TB %d..%d, poc_lsb %d bit, "
                       "sao %d amp %d smi %d tmvp %d, %d insiemi di "
                       "riferimento\n",
                       s.sps_id, s.width, s.height,
                       s.crop_left, s.crop_right, s.crop_top, s.crop_bottom,
                       s.ctb_size, 1 << s.log2_min_cb, 1 << s.log2_ctb,
                       1 << s.log2_min_tb, 1 << s.log2_max_tb,
                       s.log2_max_poc_lsb, s.sao_enabled, s.amp_enabled,
                       s.strong_intra_smoothing, s.temporal_mvp_enabled,
                       s.num_st_rps);
        } else if (tipo == HEVC_NAL_PPS) {
            hevc_pps_t p;
            const int r = hevc_ps_leggi_pps(&p, rbsp, n);
            if (r) {
                fprintf(stderr, "PPS rifiutato: %s\n", perche(r));
                rifiutate++;
                continue;
            }
            pps[p.pps_id] = p;
            if (!zitto)
                printf("PPS %d (SPS %d): qp %d, cu_qp_delta %d/%d, "
                       "croma %+d%+d, pesi %d/%d, tile %dx%d, wpp %d, "
                       "deblk %s, liste %d/%d, merge %d\n",
                       p.pps_id, p.sps_id, p.init_qp,
                       p.cu_qp_delta_enabled, p.diff_cu_qp_delta_depth,
                       p.cb_qp_offset, p.cr_qp_offset,
                       p.weighted_pred, p.weighted_bipred,
                       p.num_tile_columns, p.num_tile_rows,
                       p.entropy_coding_sync_enabled,
                       p.deblocking_filter_disabled ? "no" : "si",
                       p.num_ref_idx_default[0], p.num_ref_idx_default[1],
                       p.log2_parallel_merge_level);
        } else if (hevc_nal_e_slice(tipo)) {
            hevc_slice_t s;
            const int r = hevc_ps_leggi_slice(&s, rbsp, n, tipo, sps, pps);
            if (r) {
                fprintf(stderr, "slice rifiutata: %s\n", perche(r));
                rifiutate++;
                continue;
            }
            if (s.dependent_slice_segment) {
                fprintf(stderr, "segmenti dipendenti: non supportati\n");
                rifiutate++;
                continue;
            }

            if (s.first_slice_in_pic) {
                if (immagini && !zitto)
                    printf("     (%d slice)\n", slice_di_questa);
                slice_di_questa = 0;
                immagini++;

                /* 8.3.1. An IRAP that starts the sequence resets it. */
                if (hevc_nal_e_idr(tipo)) {
                    /* An IDR ends one group and starts the next. */
                    if (!verifica_gruppo(poc_visti, n_visti)) gruppi_rotti++;
                    n_visti = 0;
                    s.poc = 0;
                    prev_poc_lsb = 0;
                    prev_poc_msb = 0;
                } else {
                    const hevc_sps_t *sp = &sps[pps[s.pps_id].sps_id];
                    const int max = 1 << sp->log2_max_poc_lsb;
                    int msb;
                    if (s.poc_lsb < prev_poc_lsb
                        && prev_poc_lsb - s.poc_lsb >= max / 2)
                        msb = prev_poc_msb + max;
                    else if (s.poc_lsb > prev_poc_lsb
                             && s.poc_lsb - prev_poc_lsb > max / 2)
                        msb = prev_poc_msb - max;
                    else
                        msb = prev_poc_msb;
                    s.poc = msb + s.poc_lsb;
                    prev_poc_lsb = s.poc_lsb;
                    prev_poc_msb = msb;
                }

                if (n_visti < 4096) poc_visti[n_visti++] = s.poc;

                if (!zitto)
                    printf("  %-10s %c poc %3d qp %2d rif %d/%d sao %d%d "
                           "merge %d",
                           nome_nal(tipo), tipo_slice[s.type], s.poc, s.qp,
                           s.num_ref_idx[0], s.num_ref_idx[1],
                           s.sao_luma, s.sao_chroma,
                           5 - s.five_minus_max_num_merge_cand);
                if (!zitto && s.type != 2) {
                    printf(" rps");
                    for (int k = 0; k < s.st_rps.num_negative
                                        + s.st_rps.num_positive; k++)
                        printf(" %+d%s", s.st_rps.delta_poc[k],
                               s.st_rps.used[k] ? "" : "-");
                }
                if (!zitto) printf("\n");
            }
            slice_di_questa++;
            slice_totali++;

            if (!solo_intestazioni) {
                const hevc_sps_t *sp = &sps[pps[s.pps_id].sps_id];
                hevc_decoder_sposta_entry_point(
                    &s, buf + inizio, (size_t)(fine - inizio),
                    s.data_bit_offset >> 3);
                if (s.first_slice_in_pic) {
                    if (immagine_aperta) {
                        scrivi_immagine(fo, dec, sp, ordine_corrente);
                        immagine_aperta = false;
                    }
                    if (s.nal_type == HEVC_NAL_IDR_W_RADL
                        || s.nal_type == HEVC_NAL_IDR_N_LP)
                        base_ordine = prossimo_ordine;
                    hevc_decoder_sfoltisci(dec, &s);
                    if (hevc_decoder_begin_picture(dec, sp, &pps[s.pps_id],
                                                   (uintptr_t)immagini, s.poc)) {
                        slice_perse++;
                        continue;
                    }
                    ordine_corrente = base_ordine + s.poc;
                    if (ordine_corrente >= prossimo_ordine)
                        prossimo_ordine = ordine_corrente + 1;
                }
                const int e = hevc_decoder_slice(dec, &s, rbsp, n);
                if (e == 4) {
                    slice_saltate++;
                    if (immagine_aperta)
                        scrivi_immagine(fo, dec, sp, ordine_corrente);
                    immagine_aperta = false;
                    if (fo) { fclose(fo); fo = NULL; }
                } else if (e) {
                    slice_perse++;
                    if (!zitto)
                        printf("     ^ %s\n", hevc_decoder_motivo(e));
                } else {
                    slice_lette++;
                    immagine_aperta = true;
                }
            }
        }
    }
    if (immagini && !zitto)
        printf("     (%d slice)\n", slice_di_questa);

    if (immagine_aperta) {
        const hevc_sps_t *sp = NULL;
        for (int k = 0; k < 16; k++) if (sps[k].valid) { sp = &sps[k]; break; }
        if (sp) scrivi_immagine(fo, dec, sp, ordine_corrente);
    }
    svuota_uscita(fo);
    if (fo) fclose(fo);
    if (!verifica_gruppo(poc_visti, n_visti)) gruppi_rotti++;

    printf("%d immagini, %d slice, %d rifiutate, %d gruppi con poc rotti, "
           "%d percorse, %d saltate, %d perse\n",
           immagini, slice_totali, rifiutate, gruppi_rotti,
           slice_lette, slice_saltate, slice_perse);
    free(buf); free(rbsp); free(sps); free(pps); free(poc_visti);
    hevc_decoder_destroy(dec);
    return (rifiutate || gruppi_rotti || slice_perse) ? 1 : 0;
}
