/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_threads.c - reconstruction across several cores, as a wavefront.
 *
 * H.264 has no entropy_coding_sync: a slice's arithmetic decoder can only
 * be started at the slice's beginning, so reading the syntax is strictly
 * serial and stays on one thread. Reconstruction is a different matter.
 * Once the syntax and the residuals are in, what a macroblock needs from
 * its own picture is only the reconstructed samples its intra prediction
 * reads, and those come from three neighbours: left, above, and above
 * right.
 *
 * That is a wavefront. Row r may reconstruct column x as soon as row r - 1
 * has finished column x + 1, so the rows run two macroblocks apart and as
 * many of them are in flight as there are threads.
 *
 * ⚠️ The dependency really is x + 1 and not x. Intra prediction reads the
 * macroblock above right, so column x of row r needs column x + 1 of row
 * r - 1 finished, not merely column x. Getting that wrong gives a picture
 * that is right almost everywhere and wrong along some diagonals, which is
 * exactly the kind of bug that survives a casual look.
 *
 * Each worker takes a copy of the decoder and uses it as a cursor of its
 * own. Reconstruction reads the decoder and never writes to it, so the
 * copies share the macroblock array, the frame store and the residuals,
 * and differ only in where they are.
 */
#include "h264_dec_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

struct h264d_pool {
    pthread_t *thread;
    int n;

    pthread_mutex_t m;
    pthread_cond_t via, finito;

    /* The job being run, if any. */
    int tipo;                   /* 0 reconstruct, 1 deblock */
    h264_decoder_t *d;
    h264d_deblock_pic_t dbl;
    int primo, quanti, numero;
    int prima_riga, ultima_riga;

    unsigned generazione;       /* bumped once per job */
    int attivi;                 /* workers still inside this job */
    bool chiudi;

    atomic_int riga_prossima;   /* next row to claim */
    atomic_int *progresso;      /* per row: the first column not yet done */
    int n_righe;
};

/* How far a row has to have got before the row below may touch column x. */
static inline void attendi(const atomic_int *p, int fino_a)
{
    int giri = 0;
    while (atomic_load_explicit(p, memory_order_acquire) < fino_a) {
        /* A short spin, because the row above is usually only a macroblock
         * or two ahead and about to move. Then out of the way, because a
         * thread that spins through its slice stops the one it is waiting
         * for from being scheduled at all. */
        if (++giri < 256) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#endif
        } else {
            sched_yield();
            giri = 0;
        }
    }
}

static void lavora(struct h264d_pool *p)
{
    const int mb_w = (p->tipo == 0) ? p->d->mb_w : p->dbl.mb_w;
    const int ultimo = p->primo + p->quanti - 1;
    h264_decoder_t c;
    if (p->tipo == 0) {
        c = *p->d;
        c.slice = p->d->slices[p->numero];
    }

    for (;;) {
        const int r = atomic_fetch_add(&p->riga_prossima, 1);
        if (r > p->ultima_riga) break;

        const int x0 = (r == p->prima_riga) ? p->primo % mb_w : 0;
        const int x1 = (r == p->ultima_riga) ? ultimo % mb_w : mb_w - 1;
        const bool aspetta = (r > p->prima_riga);

        for (int x = x0; x <= x1; x++) {
            if (aspetta)
                attendi(&p->progresso[r - 1], x + 2);

            if (p->tipo == 0) {
                c.mb_idx = r * mb_w + x;
                c.mb_x = x;
                c.mb_y = r;
                c.res = &p->d->residui[c.mb_idx % p->d->n_residui];
                h264d_reconstruct_mb(&c);
            } else {
                h264d_deblock_mb(&p->dbl, x, r);
            }

            atomic_store_explicit(&p->progresso[r], x + 1,
                                  memory_order_release);
        }
        /* The row is finished: let anything still waiting on its tail
         * through, including the columns it never had. */
        atomic_store_explicit(&p->progresso[r], mb_w + 2,
                              memory_order_release);
    }
}

static void *operaio(void *arg)
{
    struct h264d_pool *p = arg;
    unsigned mia = 0;
    for (;;) {
        pthread_mutex_lock(&p->m);
        while (p->generazione == mia && !p->chiudi)
            pthread_cond_wait(&p->via, &p->m);
        if (p->chiudi) {
            pthread_mutex_unlock(&p->m);
            return NULL;
        }
        mia = p->generazione;
        pthread_mutex_unlock(&p->m);

        lavora(p);

        pthread_mutex_lock(&p->m);
        if (--p->attivi == 0)
            pthread_cond_signal(&p->finito);
        pthread_mutex_unlock(&p->m);
    }
}

/* How many threads to use. BC250_H264_THREADS overrides; 0 or 1 turns
 * threading off entirely and every picture is reconstructed in place. */
static int quanti_thread(void)
{
    const char *e = getenv("BC250_H264_THREADS");
    if (e) {
        const int n = atoi(e);
        return n < 0 ? 0 : n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    /* ⚠️ Beyond about eight the wavefront stops paying: the rows are two
     * macroblocks apart, so the ninth thread is nine rows behind the first
     * and spends most of its time waiting for the eighth. */
    if (n > 8) n = 8;
    return (int)n;
}

int h264d_pool_start(h264_decoder_t *d)
{
    if (d->pool) return 0;
    const int n = quanti_thread();
    if (n < 2) return 0;                 /* one thread: no pool at all */

    struct h264d_pool *p = calloc(1, sizeof(*p));
    if (!p) return -1;
    p->n_righe = d->mb_h;
    p->progresso = calloc((size_t)p->n_righe, sizeof(atomic_int));
    p->thread = calloc((size_t)n, sizeof(pthread_t));
    if (!p->progresso || !p->thread) {
        free(p->progresso); free(p->thread); free(p);
        return -1;
    }
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->via, NULL);
    pthread_cond_init(&p->finito, NULL);
    p->generazione = 0;

    for (int i = 0; i < n; i++) {
        if (pthread_create(&p->thread[i], NULL, operaio, p) != 0) {
            p->n = i;
            break;
        }
        p->n = i + 1;
    }
    if (p->n < 2) {
        /* Not enough threads came up to be worth the synchronisation. */
        pthread_mutex_lock(&p->m);
        p->chiudi = true;
        pthread_cond_broadcast(&p->via);
        pthread_mutex_unlock(&p->m);
        for (int i = 0; i < p->n; i++) pthread_join(p->thread[i], NULL);
        pthread_mutex_destroy(&p->m);
        pthread_cond_destroy(&p->via);
        pthread_cond_destroy(&p->finito);
        free(p->progresso); free(p->thread); free(p);
        return 0;
    }
    d->pool = p;
    return 0;
}

void h264d_pool_stop(h264_decoder_t *d)
{
    struct h264d_pool *p = d->pool;
    if (!p) return;
    pthread_mutex_lock(&p->m);
    p->chiudi = true;
    pthread_cond_broadcast(&p->via);
    pthread_mutex_unlock(&p->m);
    for (int i = 0; i < p->n; i++)
        pthread_join(p->thread[i], NULL);
    pthread_mutex_destroy(&p->m);
    pthread_cond_destroy(&p->via);
    pthread_cond_destroy(&p->finito);
    free(p->progresso);
    free(p->thread);
    free(p);
    d->pool = NULL;
}

/* One macroblock at a time, in order, on this thread. */
static void in_fila(h264_decoder_t *d, int primo, int quanti, int numero)
{
    h264_decoder_t c = *d;
    c.slice = d->slices[numero];
    for (int i = 0; i < quanti; i++) {
        c.mb_idx = primo + i;
        if (c.mb_idx >= d->mb_count) break;
        c.mb_x = c.mb_idx % d->mb_w;
        c.mb_y = c.mb_idx / d->mb_w;
        c.res = &d->residui[c.mb_idx % d->n_residui];
        h264d_reconstruct_mb(&c);
    }
}

void h264d_reconstruct_range(h264_decoder_t *d, int primo, int quanti,
                             int numero)
{
    if (quanti <= 0) return;
    if (primo + quanti > d->mb_count)
        quanti = d->mb_count - primo;

    struct h264d_pool *p = d->pool;
    const int prima_riga = primo / d->mb_w;
    const int ultima_riga = (primo + quanti - 1) / d->mb_w;

    /* ⚠️ Below a few rows the threads cost more than they save: waking
     * them, the wavefront's two-macroblock lag and the join at the end all
     * have to be paid before the first sample is any faster. */
    if (!p || ultima_riga - prima_riga < 3) {
        in_fila(d, primo, quanti, numero);
        return;
    }

    pthread_mutex_lock(&p->m);
    p->tipo = 0;
    p->d = d;
    p->primo = primo;
    p->quanti = quanti;
    p->numero = numero;
    p->prima_riga = prima_riga;
    p->ultima_riga = ultima_riga;
    atomic_store(&p->riga_prossima, prima_riga);
    for (int r = prima_riga; r <= ultima_riga; r++) {
        const int x0 = (r == prima_riga) ? primo % d->mb_w : 0;
        atomic_store(&p->progresso[r], x0);
    }
    p->attivi = p->n;
    p->generazione++;
    pthread_cond_broadcast(&p->via);
    while (p->attivi > 0)
        pthread_cond_wait(&p->finito, &p->m);
    pthread_mutex_unlock(&p->m);
}

/* The deblocking filter over a whole picture, as the same wavefront. */
void h264d_deblock_wavefront(h264_decoder_t *d, const h264d_deblock_pic_t *dp)
{
    struct h264d_pool *p = d->pool;
    if (!p || dp->mb_h < 4) {
        for (int my = 0; my < dp->mb_h; my++)
            for (int mx = 0; mx < dp->mb_w; mx++)
                h264d_deblock_mb(dp, mx, my);
        return;
    }

    pthread_mutex_lock(&p->m);
    p->tipo = 1;
    p->d = d;
    p->dbl = *dp;
    p->primo = 0;
    p->quanti = dp->mb_w * dp->mb_h;
    p->numero = 0;
    p->prima_riga = 0;
    p->ultima_riga = dp->mb_h - 1;
    atomic_store(&p->riga_prossima, 0);
    for (int r = 0; r <= p->ultima_riga; r++)
        atomic_store(&p->progresso[r], 0);
    p->attivi = p->n;
    p->generazione++;
    pthread_cond_broadcast(&p->via);
    while (p->attivi > 0)
        pthread_cond_wait(&p->finito, &p->m);
    pthread_mutex_unlock(&p->m);
}
