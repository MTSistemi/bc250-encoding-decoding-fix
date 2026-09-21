/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * decoder_h265.c - the decoded picture buffer, the reference lists and the
 * slice loop.
 *
 * Everything below the slice header knows nothing about where pictures
 * come from or where they go. This is the piece that does.
 */
#include "decoder_h265.h"
#include "hevc_dec_internal.h"
#include "gpu_compute.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUANTE_IMG 20

struct hevc_decoder {
    hevcd_t d;
    hevcd_img_t buffer[QUANTE_IMG];
    uintptr_t nome[QUANTE_IMG];      /* what the caller calls each picture */
    void *gpu;
    int width, height;
    hevc_sps_t sps;
    hevc_pps_t pps;
    hevc_slice_t ultima;
    bool aperta;
};

static void libera_img(hevcd_img_t *g)
{
    for (int i = 0; i < 3; i++) { free(g->piano[i]); g->piano[i] = NULL; }
    free(g->mvf);
    g->mvf = NULL;
    g->n_piano = 0;
    g->n_mvf = 0;
    g->valida = false;
}

static hevcd_img_t *trova_img(const hevcd_t *d, int poc)
{
    for (int i = 0; i < d->n_buf; i++)
        if (d->buf[i].valida && d->buf[i].poc == poc) return &d->buf[i];
    return NULL;
}

/* Everything the reference picture set no longer names can go. */
static void sfoltisci(hevcd_t *d, const hevc_slice_t *sl)
{
    if (sl->nal_type == HEVC_NAL_IDR_W_RADL || sl->nal_type == HEVC_NAL_IDR_N_LP) {
        for (int i = 0; i < d->n_buf; i++) d->buf[i].valida = false;
        return;
    }
    const hevc_st_rps_t *r = &sl->st_rps;
    const int quanti = r->num_negative + r->num_positive;
    for (int i = 0; i < d->n_buf; i++) {
        if (!d->buf[i].valida) continue;
        bool serve = false;
        for (int k = 0; k < quanti && !serve; k++)
            if (d->buf[i].poc == sl->poc + r->delta_poc[k]) serve = true;
        if (!serve) d->buf[i].valida = false;
    }
}

/* The three planes of one picture at the coded size, and the motion field
 * a later picture will read for its temporal candidate. The visible size
 * is smaller and is applied when writing out. */
static int apri_immagine(hevcd_t *d, const hevc_sps_t *sps, int poc)
{
    const int w = sps->width, h = sps->height;
    const size_t n_mvf = (size_t)(w >> 2) * (h >> 2);

    hevcd_img_t *g = NULL;
    for (int i = 0; i < d->n_buf && !g; i++)
        if (!d->buf[i].valida) g = &d->buf[i];
    if (!g) return -1;

    if (g->n_piano != (size_t)w * h) {
        libera_img(g);
        g->piano[0] = malloc((size_t)w * h);
        g->piano[1] = malloc((size_t)(w / 2) * (h / 2));
        g->piano[2] = malloc((size_t)(w / 2) * (h / 2));
        g->n_piano = (size_t)w * h;
    }
    if (g->n_mvf != n_mvf) {
        free(g->mvf);
        g->mvf = malloc(n_mvf * sizeof *g->mvf);
        g->n_mvf = n_mvf;
    }
    if (!g->piano[0] || !g->piano[1] || !g->piano[2] || !g->mvf) return -1;

    memset(g->mvf, 0, n_mvf * sizeof *g->mvf);
    g->passo[0] = w;
    g->passo[1] = g->passo[2] = w / 2;
    g->poc = poc;
    g->valida = true;
    g->n_lista[0] = g->n_lista[1] = 0;

    d->corrente = g;
    d->mvf = g->mvf;
    for (int i = 0; i < 3; i++) { d->piano[i] = g->piano[i]; d->passo[i] = g->passo[i]; }
    d->n_piano = g->n_piano;
    return 0;
}

/* 8.3.4: the lists are the pictures before this one, then the ones after,
 * repeated until the list is as long as the slice header asked for.
 *
 * ⚠️ List one starts from the other end. That is the whole point of having
 * two: a B picture with one reference each way sends the shorter index
 * for whichever direction it meant. */
static void costruisci_liste(hevcd_t *d, const hevc_slice_t *sl)
{
    const hevc_st_rps_t *r = &sl->st_rps;
    const hevcd_img_t *prima[16], *dopo[16];
    int np = 0, nd = 0;

    for (int i = 0; i < r->num_negative && np < 16; i++) {
        if (!r->used[i]) continue;
        const hevcd_img_t *g = trova_img(d, sl->poc + r->delta_poc[i]);
        if (g) prima[np++] = g;
    }
    for (int i = r->num_negative; i < r->num_negative + r->num_positive
             && nd < 16; i++) {
        if (!r->used[i]) continue;
        const hevcd_img_t *g = trova_img(d, sl->poc + r->delta_poc[i]);
        if (g) dopo[nd++] = g;
    }

    for (int l = 0; l < 2; l++) {
        d->n_rif[l] = 0;
        const int quante = sl->num_ref_idx[l];
        const hevcd_img_t **a = l ? dopo : prima;
        const hevcd_img_t **b = l ? prima : dopo;
        const int na = l ? nd : np, nb = l ? np : nd;
        if (!na && !nb) continue;
        while (d->n_rif[l] < quante) {
            for (int i = 0; i < na && d->n_rif[l] < quante; i++)
                d->rif[l][d->n_rif[l]++] = a[i];
            for (int i = 0; i < nb && d->n_rif[l] < quante; i++)
                d->rif[l][d->n_rif[l]++] = b[i];
        }
    }

    /* What the indices mean, kept with the picture: a later one that takes
     * this as its collocated picture asks what it pointed at, and an index
     * means nothing outside the slice that wrote it. */
    for (int l = 0; l < 2; l++) {
        d->corrente->n_lista[l] = d->n_rif[l];
        for (int i = 0; i < d->n_rif[l]; i++)
            d->corrente->poc_lista[l][i] = d->rif[l][i]->poc;
    }

    d->col = NULL;
    if (sl->temporal_mvp_enabled) {
        const int l = sl->collocated_from_l0 ? 0 : 1;
        if (sl->collocated_ref_idx < d->n_rif[l])
            d->col = d->rif[l][sl->collocated_ref_idx];
    }
}

/* 7.4.7.1: the entry point offsets count the NAL unit's bytes, the
 * emulation prevention ones included. The decoder reads the payload with
 * those already removed, so each offset has to lose however many of them
 * it has passed.
 *
 * ⚠️ The rule for which 0x03 is an emulation prevention byte is the one
 * in br_extract_rbsp and not "every 0x03 after two zeros": a 0x03
 * followed by a byte above 0x03 is ordinary payload. Counting them any
 * other way moves the offsets by the wrong amount on exactly the streams
 * where it matters.
 */
static void sposta_entry_point(hevc_slice_t *s, const uint8_t *grezzo,
                               size_t n_grezzo, size_t primo)
{
    if (s->num_entry_point_offsets <= 0) return;

    size_t i = 0, r = 0, zeri = 0, i0 = 0, r0 = 0;
    bool partito = false;
    int k = 0;

    while (i < n_grezzo && k < s->num_entry_point_offsets) {
        if (!partito && r == primo) {
            i0 = i; r0 = r; partito = true;
        }
        if (partito && (size_t)(i - i0) == (size_t)s->entry_point[k]) {
            s->entry_point[k] = (uint32_t)(r - r0);
            k++;
            continue;
        }
        const uint8_t c = grezzo[i];
        if (zeri >= 2 && c == 0x03
            && !(i + 1 < n_grezzo && grezzo[i + 1] > 0x03)) {
            zeri = 0;
            i++;
            continue;                 /* removed, so the payload stands still */
        }
        zeri = (c == 0x00) ? zeri + 1 : 0;
        i++;
        r++;
    }
    /* An offset past the end of the NAL is a broken stream; leave the
     * rest where the payload ended and let the row check refuse it. */
    while (k < s->num_entry_point_offsets) {
        s->entry_point[k] = (uint32_t)(r - r0);
        k++;
    }
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
                          const uint8_t *rbsp, size_t n)
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
    if (!d->cbf_map || d->n_cbf < serve_pu) {
        free(d->cbf_map);
        d->cbf_map = calloc(serve_pu, 1);
        d->n_cbf = serve_pu;
    }
    if (!d->cbf_map) return 5;
    memset(d->cbf_map, 0, serve_pu);
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
    if (!d->corrente) return 5;
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

            /* Where the header says this row ends. */
            size_t salto = usati;
            if (sl->num_entry_point_offsets > 0) {
                const int riga = addr / sps->ctb_width;
                if (riga < sl->num_entry_point_offsets) {
                    const uint32_t fin = sl->entry_point[riga];
                    const uint32_t ini = riga > 0 ? sl->entry_point[riga - 1] : 0;
                    salto = (size_t)(fin - ini);
                    /* A row may stop short of what the header allows - the
                     * bytes left over are the engine's own look-ahead. It
                     * may not run past it: that is a row read wrongly. */
                    if (usati > salto) return 7;
                }
            }
            if (salto >= resto) return 3;
            base += salto;
            resto -= salto;
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

/* ------------------------------------------------------------- the API */

hevc_decoder_t *hevc_decoder_create(void *gpu, int width, int height)
{
    hevc_decoder_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->gpu = gpu;
    h->width = width;
    h->height = height;
    h->d.buf = h->buffer;
    h->d.n_buf = QUANTE_IMG;
    return h;
}

void hevc_decoder_destroy(hevc_decoder_t *h)
{
    if (!h) return;
    for (int i = 0; i < QUANTE_IMG; i++) libera_img(&h->buffer[i]);
    hevcd_t *d = &h->d;
    free(d->ct_depth); free(d->intra_mode); free(d->min_tb_addr_zs);
    free(d->qp_y_map); free(d->bordi); free(d->no_filtro);
    free(d->skip); free(d->cbf_map);
    hevcd_libera_filtri(d);
    free(h);
}

void hevc_decoder_set_references(hevc_decoder_t *h, const uintptr_t *id,
                                 const int *poc, int n)
{
    for (int i = 0; i < QUANTE_IMG; i++) {
        if (!h->buffer[i].valida) continue;
        bool serve = false;
        for (int k = 0; k < n && !serve; k++)
            if (h->nome[i] == id[k] && h->buffer[i].poc == poc[k]) serve = true;
        if (!serve) h->buffer[i].valida = false;
    }
}

int hevc_decoder_begin_picture(hevc_decoder_t *h, const hevc_sps_t *sps,
                               const hevc_pps_t *pps, uintptr_t id, int poc)
{
    h->sps = *sps;
    h->pps = *pps;
    if (apri_immagine(&h->d, &h->sps, poc)) return -1;
    for (int i = 0; i < QUANTE_IMG; i++)
        if (&h->buffer[i] == h->d.corrente) h->nome[i] = id;
    h->aperta = true;
    return 0;
}

int hevc_decoder_slice(hevc_decoder_t *h, const hevc_slice_t *sl,
                       const uint8_t *rbsp, size_t n)
{
    if (!h->aperta || !h->d.corrente) return 5;
    h->ultima = *sl;
    h->d.slice = &h->ultima;
    costruisci_liste(&h->d, &h->ultima);
    return percorri_slice(&h->d, &h->sps, &h->pps, &h->ultima, rbsp, n);
}

void hevc_decoder_end_picture(hevc_decoder_t *h)
{
    if (!h->aperta || !h->d.corrente) return;
    if (h->d.slice) { hevcd_deblocca(&h->d); hevcd_sao(&h->d); }
    h->aperta = false;
}

const uint8_t *hevc_decoder_piano(const hevc_decoder_t *h, int piano,
                                  int *passo)
{
    if (!h->d.corrente || piano < 0 || piano > 2) return NULL;
    if (passo) *passo = h->d.corrente->passo[piano];
    return h->d.corrente->piano[piano];
}

void hevc_decoder_sfoltisci(hevc_decoder_t *h, const hevc_slice_t *sl)
{
    sfoltisci(&h->d, sl);
}

void hevc_decoder_sposta_entry_point(hevc_slice_t *s, const uint8_t *grezzo,
                                     size_t n_grezzo, size_t primo)
{
    sposta_entry_point(s, grezzo, n_grezzo, primo);
}

/* ⚠️ The surface wants the two chroma planes interleaved, and it is
 * written once, here, rather than plane by plane as the picture is
 * decoded: surface memory is write-combining, which is fast to write
 * straight through and very slow to read back or revisit. */
int hevc_decoder_carica(hevc_decoder_t *h, gpu_image_t out, gpu_memory_t mem)
{
    if (!h->gpu) return 0;                 /* the harness keeps the planes */
    const hevcd_img_t *g = h->d.corrente;
    if (!g || !g->piano[0]) return -1;

    const int cw = h->width / 2, ch = h->height / 2;
    uint8_t *uv = malloc((size_t)cw * 2 * ch);
    if (!uv) return -1;
    for (int r = 0; r < ch; r++) {
        const uint8_t *a = g->piano[1] + (size_t)r * g->passo[1];
        const uint8_t *b = g->piano[2] + (size_t)r * g->passo[2];
        uint8_t *o = uv + (size_t)r * cw * 2;
        for (int x = 0; x < cw; x++) { o[2 * x] = a[x]; o[2 * x + 1] = b[x]; }
    }
    const int r = gpu_compute_upload_nv12(h->gpu, &out, mem,
                                          g->piano[0], g->passo[0],
                                          uv, cw * 2, h->width, h->height);
    free(uv);
    return r;
}

const char *hevc_decoder_motivo(int e)
{
    return motivo_slice(e);
}
