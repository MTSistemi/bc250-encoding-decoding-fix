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



/* The three planes of one picture, at the coded size. The visible size is
 * smaller and is applied when writing out. */
static int apri_immagine(hevcd_t *d, const hevc_sps_t *sps)
{
    const int w = sps->width, h = sps->height;
    if (d->piano[0] && d->passo[0] == w && d->n_piano == (size_t)w * h)
        return 0;
    for (int i = 0; i < 3; i++) { free(d->piano[i]); d->piano[i] = NULL; }
    d->piano[0] = malloc((size_t)w * h);
    d->piano[1] = malloc((size_t)(w / 2) * (h / 2));
    d->piano[2] = malloc((size_t)(w / 2) * (h / 2));
    if (!d->piano[0] || !d->piano[1] || !d->piano[2]) return -1;
    d->passo[0] = w;
    d->passo[1] = d->passo[2] = w / 2;
    d->n_piano = (size_t)w * h;
    return 0;
}

/* Cropped on the way out: the coded picture is a whole number of smallest
 * coding blocks and the visible one is not. */
static void scrivi_immagine(FILE *f, hevcd_t *d, const hevc_sps_t *sps)
{
    /* ⚠️ The loop filters run here and not at the end of each slice: 8.7.2
     * is defined over the whole picture, and an edge between two coding
     * tree units cannot be filtered until both of them exist. */
    if (d->slice) { hevcd_deblocca(d); hevcd_sao(d); }
    if (!f) return;
    const int x0 = sps->crop_left, y0 = sps->crop_top;
    const int w = sps->width - sps->crop_left - sps->crop_right;
    const int h = sps->height - sps->crop_top - sps->crop_bottom;

    for (int y = 0; y < h; y++)
        fwrite(d->piano[0] + (size_t)(y0 + y) * d->passo[0] + x0, 1,
               (size_t)w, f);
    for (int p = 1; p < 3; p++)
        for (int y = 0; y < h / 2; y++)
            fwrite(d->piano[p] + (size_t)(y0 / 2 + y) * d->passo[p] + x0 / 2,
                   1, (size_t)(w / 2), f);
}

/* Why a slice could not be walked through. */
static const char *motivo_slice(int e)
{
    switch (e) {
    case 1: return "finita prima dell'ultimo CTU";
    case 2: return "non e' finita dove doveva";
    case 3: return "ha letto oltre la fine del NAL";
    case 4: return "tipo di slice non ancora percorribile";
    case 5: return "memoria";
    case 6: return "fine sottoinsieme non a uno";
    case 7: return "una riga non e' lunga come dice l'intestazione";
    default: return "?";
    }
}

/* Read every bin of a slice's coding tree, and check it lands.
 *
 * ⚠️ Nothing is reconstructed. What this proves is that the syntax was
 * read correctly, which CABAC makes checkable without any samples: a slice
 * read correctly ends with end_of_slice_segment_flag set exactly after the
 * last coding tree unit, and with the arithmetic decoder at the end of the
 * NAL. One bin read against the wrong context almost never lands there. */
static int percorri_slice(hevcd_t *d, const hevc_sps_t *sps,
                          const hevc_pps_t *pps, const hevc_slice_t *sl,
                          const uint8_t *rbsp, size_t n, bool senza_escape)
{
    /* ⚠️ P and B slices are read through, not reconstructed. Their
     * samples are meaningless until motion compensation exists; what the
     * walk proves is that every bin of their syntax was read against the
     * right context. */

    const size_t serve_cb = (size_t)sps->min_cb_width * sps->min_cb_height;
    const size_t serve_pu = (size_t)(sps->width >> 2) * (sps->height >> 2);
    if (!d->ct_depth || d->n_ct_depth < serve_cb) {
        free(d->ct_depth);
        d->ct_depth = calloc(serve_cb, 1);
        d->n_ct_depth = serve_cb;
    }
    if (!d->intra_mode || d->n_intra_mode < serve_pu) {
        free(d->intra_mode);
        d->intra_mode = calloc(serve_pu, 1);
        d->n_intra_mode = serve_pu;
    }
    const size_t serve_bordi = (size_t)((sps->width + 7) >> 3)
                            * (size_t)((sps->height + 7) >> 3);
    if (!d->bordi || d->n_bordi < serve_bordi) {
        free(d->bordi);
        d->bordi = calloc(serve_bordi, 1);
        d->n_bordi = serve_bordi;
    }
    if (!d->no_filtro || d->n_no_filtro < serve_cb) {
        free(d->no_filtro);
        d->no_filtro = calloc(serve_cb, 1);
        d->n_no_filtro = serve_cb;
    }
    if (!d->sao || d->n_sao < (size_t)sps->ctb_count) {
        free(d->sao);
        d->sao = calloc((size_t)sps->ctb_count, sizeof *d->sao);
        d->n_sao = (size_t)sps->ctb_count;
    }
    if (!d->skip || d->n_skip < serve_cb) {
        free(d->skip);
        d->skip = calloc(serve_cb, 1);
        d->n_skip = serve_cb;
    }
    if (!d->bordi || !d->no_filtro || !d->sao || !d->skip) return 5;
    d->bordi_passo = (sps->width + 7) >> 3;
    if (!d->qp_y_map || d->n_qp < serve_cb) {
        free(d->qp_y_map);
        d->qp_y_map = calloc(serve_cb, 1);
        d->n_qp = serve_cb;
    }
    if (!d->ct_depth || !d->intra_mode || !d->qp_y_map) return 5;
    memset(d->ct_depth, 0, serve_cb);
    memset(d->bordi, 0, serve_bordi);
    memset(d->no_filtro, 0, serve_cb);
    memset(d->skip, 0, serve_cb);
    memset(d->intra_mode, HEVCD_INTRA_DC, serve_pu);

    d->sps = sps;
    if (apri_immagine(d, sps)) return 5;
    if (hevcd_prepara_zscan(d)) return 5;
    d->pps = pps;
    d->slice = sl;
    d->min_pu_width = sps->width >> 2;
    d->min_pu_height = sps->height >> 2;
    d->qp_y = sl->qp;
    d->qp_y_pred = sl->qp;
    d->qp_y_prev = sl->qp;
    d->qg_riparte = true;
    d->fine_slice = false;

    const size_t primo = sl->data_bit_offset >> 3;
    if (primo >= n) return 3;
    const uint8_t *base = rbsp + primo;
    size_t resto = n - primo;
    hevcd_cabac_init(&d->cabac, base, resto,
                     sl->type, sl->cabac_init_flag, sl->qp);

    const bool wpp = pps->entropy_coding_sync_enabled;
    const int init_type = hevcd_init_type(sl->type, sl->cabac_init_flag);
    uint8_t istantanea[HEVCD_CTX];
    bool ho_istantanea = false;

    const int quanti = sps->ctb_count;
    int fatti = 0;
    for (int addr = sl->segment_address; addr < quanti; addr++) {
        const int cx = addr % sps->ctb_width;
        const int x = cx << sps->log2_ctb;
        const int y = (addr / sps->ctb_width) << sps->log2_ctb;
        if (hevcd_leggi_ctu(d, x, y)) return 4;   /* refused inside */
        fatti++;

        /* 9.3.2.3: after the second unit of a row, so the row below can
         * start from here. */
        if (wpp && cx == 1) {
            memcpy(istantanea, d->cabac.state, HEVCD_CTX);
            ho_istantanea = true;
        }
        /* HEVC_TRACE: how far into the NAL each coding tree unit got.
         * When a slice does not land, this says where it stopped being
         * right - a unit that consumed implausibly little is where to
         * look, not the one that ran out of data. */
        if (getenv("HEVC_TRACE")) {
            const long letti = (long)((d->cabac.ptr - d->cabac.start) * 8
                                      - d->cabac.cache_bits);
            fprintf(stderr, "ctu %d (%d,%d): %ld bit su %ld" "\n",
                    addr, x, y, letti, (long)(n - primo) * 8);
        }
        if (hevcd_overrun(&d->cabac)) return 3;

        const int fine = hevcd_terminate(&d->cabac);
        if (fine) {
            /* ⚠️ It has to end after the LAST one, not merely end. */
            return (addr == quanti - 1) ? 0 : 1;
        }

        /* 7.3.8.1: when the next unit starts a row, this substream ends. */
        if (wpp && addr + 1 < quanti
            && (addr + 1) % sps->ctb_width == 0) {
            if (!hevcd_terminate(&d->cabac))
                return 6;                  /* the bit is defined to be one */

            const size_t usati = h264d_cabac_byte_pos(&d->cabac);
            if (usati >= resto) return 3;

            /* What the header said this row would be. */
            if (senza_escape && sl->num_entry_point_offsets > 0) {
                const int riga = addr / sps->ctb_width;
                if (riga < sl->num_entry_point_offsets) {
                    const uint32_t fin = sl->entry_point[riga];
                    const uint32_t ini = riga > 0 ? sl->entry_point[riga - 1] : 0;
                    if (usati != (size_t)(fin - ini)) return 7;
                }
            }
            base += usati;
            resto -= usati;
            h264d_cabac_init_engine(&d->cabac, base, resto);
            /* 8.6.1: a row under WPP predicts its first group from the
             * slice's parameter and not from the end of the row above. */
            d->qg_riparte = true;

            /* 9.3.1: from the snapshot of the row above when the unit
             * above right exists, and from nothing when it does not -
             * which is what a picture one unit wide always is. */
            if (ho_istantanea && sps->ctb_width >= 2)
                memcpy(d->cabac.state, istantanea, HEVCD_CTX);
            else
                hevcd_cabac_ctx_init(d->cabac.state, init_type, sl->qp);
        }
    }
    (void)fatti;
    return 2;                                 /* ran out of CTUs first */
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

    hevcd_t *dec = calloc(1, sizeof(hevcd_t));
    if (!dec) return 2;
    int slice_lette = 0, slice_perse = 0, slice_saltate = 0;
    FILE *fo = uscita ? fopen(uscita, "wb") : NULL;
    if (uscita && !fo) { perror(uscita); return 2; }
    bool immagine_aperta = false;
    hevc_slice_t ultima_slice;

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
                if (s.first_slice_in_pic && immagine_aperta) {
                    scrivi_immagine(fo, dec, sp);
                    immagine_aperta = false;
                }
                const int e = percorri_slice(dec, sp, &pps[s.pps_id], &s,
                                             rbsp, n, n == (size_t)(fine - inizio));
                if (e == 4) {
                    slice_saltate++;
                    if (immagine_aperta) scrivi_immagine(fo, dec, sp);
                    immagine_aperta = false;
                    if (fo) { fclose(fo); fo = NULL; }
                } else if (e) {
                    slice_perse++;
                    if (!zitto) printf("     ^ %s\n", motivo_slice(e));
                } else {
                    slice_lette++;
                    immagine_aperta = true;
                    ultima_slice = s;
                    dec->slice = &ultima_slice;
                }
            }
        }
    }
    if (immagini && !zitto)
        printf("     (%d slice)\n", slice_di_questa);

    if (immagine_aperta) {
        const hevc_sps_t *sp = NULL;
        for (int k = 0; k < 16; k++) if (sps[k].valid) { sp = &sps[k]; break; }
        if (sp) scrivi_immagine(fo, dec, sp);
    }
    if (fo) fclose(fo);
    if (!verifica_gruppo(poc_visti, n_visti)) gruppi_rotti++;

    printf("%d immagini, %d slice, %d rifiutate, %d gruppi con poc rotti, "
           "%d percorse, %d saltate, %d perse\n",
           immagini, slice_totali, rifiutate, gruppi_rotti,
           slice_lette, slice_saltate, slice_perse);
    free(buf); free(rbsp); free(sps); free(pps); free(poc_visti);
    free(dec->ct_depth); free(dec->intra_mode); free(dec->min_tb_addr_zs);
    free(dec->qp_y_map); free(dec->bordi); free(dec->no_filtro);
    hevcd_libera_filtri(dec); free(dec->skip);
    for (int k = 0; k < 3; k++) free(dec->piano[k]);
    free(dec);
    return (rifiutate || gruppi_rotti || slice_perse) ? 1 : 0;
}
