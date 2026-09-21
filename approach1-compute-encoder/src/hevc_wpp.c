/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_wpp.c - decoding several coding tree block rows at once.
 *
 * Wavefront parallelism is a property of the bitstream, not an
 * optimisation bolted onto it: with entropy_coding_sync_enabled every row
 * is its own arithmetic substream, starting from the statistics the row
 * above had after its second unit. The encoder gave up a little
 * compression to make that true. A decoder that reads the rows one after
 * another pays the price and takes none of the change.
 *
 * ⚠️ A row may not run away from the one above it. Intra prediction reads
 * the samples above and above right, the motion vector predictors read
 * the blocks above, and the arithmetic decoder starts from a snapshot
 * taken two units in. So row r may decode unit x once row r-1 has
 * finished unit x+1, and not before - which is the whole dependency, and
 * the reason this is a wavefront and not simply one thread per row.
 *
 * ⚠️ How the rows tell each other where they are matters as much as the
 * dependency itself. The first version of this file kept one mutex and
 * woke every thread after every unit. At five hundred units a picture and
 * sixteen threads that is eight thousand wakeups per picture, against
 * fifteen milliseconds of actual decoding: four threads bought 1.5x
 * instead of the 4x the wavefront allows. Each row now publishes its
 * progress with a plain atomic store, the row below spins on it for a
 * moment before sleeping, and a sleeper is woken by the one row above it
 * and nobody else.
 */
#include "hevc_dec_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_THREAD 16

/* How long to keep looking before going to sleep. Once the wavefront has
 * filled, a row is normally a few microseconds behind the one above it,
 * and a sleep and a wakeup cost more than the wait. */
#define GIRI_A_VUOTO 512

#if defined(__x86_64__) || defined(__i386__)
#define PAUSA() __builtin_ia32_pause()
#else
#define PAUSA() ((void)0)
#endif

/* One row's progress and its doorbell. ⚠️ Padded out to its own cache
 * line: sixteen of these one after another in memory would be sixteen
 * threads writing to the same line, which costs more than the mutex we
 * are trying to avoid. */
typedef struct {
    _Atomic int fatti;                /* units this row has finished */
    _Atomic int attesa;               /* how many sleep on c */
    pthread_mutex_t m;
    pthread_cond_t c;
} __attribute__((aligned(128))) stato_t;

typedef struct {
    hevcd_t modello;                  /* what every worker starts from */
    const hevc_sps_t *sps;
    const hevc_pps_t *pps;
    const hevc_slice_t *sl;
    int init_type;

    const uint8_t **inizio;           /* each row's substream */
    size_t *lungo;
    uint8_t (*istantanea)[HEVCD_CTX]; /* the contexts each row leaves behind */
    stato_t *stato;

    int righe, colonne;
    _Atomic int prossima;             /* the next row nobody has claimed */
    _Atomic int errore;               /* the first reason anything refused */
} onda_t;

/* This row has finished `quanti` units, and whoever is below may go on. */
static void pubblica(onda_t *o, int r, int quanti)
{
    stato_t *s = &o->stato[r];

    /* Release: everything this row wrote - samples, motion vectors, the
     * coded block flags - is in place before the count that says so. */
    atomic_store_explicit(&s->fatti, quanti, memory_order_release);

    /* ⚠️ And that store has to be visible before we look at who is
     * asleep. Without the fence we can read "nobody" at the very moment
     * the row below is going to sleep on a value we have already written,
     * and it never wakes up. */
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&s->attesa, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&s->m);
        pthread_cond_signal(&s->c);
        pthread_mutex_unlock(&s->m);
    }
}

/* Nothing more will be decoded. Set the reason, if nobody else has, and
 * wake every sleeper so the picture comes apart instead of hanging. */
static void fallisci(onda_t *o, int guasto)
{
    int nessuno = 0;
    atomic_compare_exchange_strong_explicit(&o->errore, &nessuno, guasto,
                                            memory_order_seq_cst,
                                            memory_order_relaxed);
    for (int r = 0; r < o->righe; r++) {
        pthread_mutex_lock(&o->stato[r].m);
        pthread_cond_broadcast(&o->stato[r].c);
        pthread_mutex_unlock(&o->stato[r].m);
    }
}

/* Wait until row r has finished at least `serve` units. False when the
 * slice has already failed somewhere else and there is nothing to wait for. */
static bool aspetta(onda_t *o, int r, int serve)
{
    stato_t *s = &o->stato[r];

    for (int i = 0; i < GIRI_A_VUOTO; i++) {
        if (atomic_load_explicit(&s->fatti, memory_order_acquire) >= serve)
            return true;
        if (atomic_load_explicit(&o->errore, memory_order_relaxed))
            return false;
        PAUSA();
    }

    pthread_mutex_lock(&s->m);
    atomic_fetch_add_explicit(&s->attesa, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&s->fatti, memory_order_acquire) < serve
           && !atomic_load_explicit(&o->errore, memory_order_relaxed))
        pthread_cond_wait(&s->c, &s->m);
    atomic_fetch_sub_explicit(&s->attesa, 1, memory_order_relaxed);
    const bool pronto =
        atomic_load_explicit(&s->fatti, memory_order_acquire) >= serve;
    pthread_mutex_unlock(&s->m);
    return pronto;
}

/* One row, from its own substream, keeping step with the one above. */
static void riga(onda_t *o, int r)
{
    hevcd_t d = o->modello;           /* the maps are shared; the state is not */

    d.qp_y = o->sl->qp;
    d.qp_y_pred = o->sl->qp;
    d.qp_y_prev = o->sl->qp;
    d.qg_riparte = true;
    d.fine_slice = false;
    d.cu_qp_delta_coded = false;
    d.cu_qp_delta = 0;

    hevcd_cabac_init(&d.cabac, o->inizio[r], o->lungo[r],
                     o->sl->type, o->sl->cabac_init_flag, o->sl->qp);

    for (int x = 0; x < o->colonne; x++) {
        if (r > 0) {
            /* ⚠️ Unit x + 1 of the row above, except at the right edge
             * where there is no such unit and the whole row will do. */
            const int serve = (x + 2 < o->colonne) ? x + 2 : o->colonne;
            if (!aspetta(o, r - 1, serve)) return;

            if (x == 0) {
                /* 9.3.1: the statistics of the row above, as they were two
                 * units in. */
                if (o->sps->ctb_width >= 2)
                    memcpy(d.cabac.state, o->istantanea[r - 1], HEVCD_CTX);
                else
                    hevcd_cabac_ctx_init(d.cabac.state, o->init_type, o->sl->qp);
            }
        }

        const int px = x << o->sps->log2_ctb;
        const int py = r << o->sps->log2_ctb;
        int guasto = 0;
        if (hevcd_leggi_ctu(&d, px, py)) guasto = 4;
        else if (hevcd_overrun(&d.cabac)) guasto = 3;

        if (!guasto && x == 1)
            memcpy(o->istantanea[r], d.cabac.state, HEVCD_CTX);

        if (!guasto) {
            const int fine = hevcd_terminate(&d.cabac);
            const bool ultima = (r == o->righe - 1) && (x == o->colonne - 1);
            if (fine && !ultima) guasto = 1;
            if (!fine && ultima) guasto = 2;
            /* 7.3.8.1: every row but the last ends with a bit that is
             * defined to be one, and then the alignment. */
            if (!guasto && !fine && x == o->colonne - 1
                && !hevcd_terminate(&d.cabac))
                guasto = 6;
        }

        if (guasto) {
            fallisci(o, guasto);
            return;
        }
        pubblica(o, r, x + 1);
    }
}

static void *operaio(void *arg)
{
    onda_t *o = arg;
    for (;;) {
        const int r = atomic_fetch_add_explicit(&o->prossima, 1,
                                                memory_order_relaxed);
        if (r >= o->righe) break;
        riga(o, r);
    }
    return NULL;
}

/* How many rows to run at once. One per processor is plenty: the rows are
 * a wavefront, so past a certain width the extra threads spend their time
 * waiting for the row above rather than decoding. */
static int quanti_thread(int righe, int colonne)
{
    const char *s = getenv("BC250_HEVC_THREAD");
    int n = s ? atoi(s) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > MAX_THREAD) n = MAX_THREAD;
    if (n > righe) n = righe;

    /* ⚠️ And no more than the shape of the picture can use. The critical
     * path through the wavefront is one row plus two units for every row
     * below it, so a thread beyond total/path would never decode anything
     * it did not have to wait for first. It would only add contention. */
    if (colonne >= 2) {
        const int percorso = colonne + 2 * (righe - 1);
        int utili = (righe * colonne + percorso - 1) / percorso;
        if (utili < 1) utili = 1;
        if (n > utili) n = utili;
    }
    return n;
}

int hevcd_wavefront(hevcd_t *d, const hevc_sps_t *sps, const hevc_pps_t *pps,
                    const hevc_slice_t *sl, const uint8_t *base, size_t resto,
                    int init_type)
{
    const int righe = sps->ctb_height;
    const int colonne = sps->ctb_width;
    const int n_thread = quanti_thread(righe, colonne);
    if (n_thread < 2) return -1;                  /* the caller walks it */

    onda_t o;
    memset(&o, 0, sizeof o);
    o.modello = *d;
    o.sps = sps; o.pps = pps; o.sl = sl;
    o.init_type = init_type;
    o.righe = righe; o.colonne = colonne;

    o.inizio = calloc((size_t)righe, sizeof *o.inizio);
    o.lungo = calloc((size_t)righe, sizeof *o.lungo);
    o.istantanea = calloc((size_t)righe, sizeof *o.istantanea);
    /* ⚠️ aligned_alloc, not calloc: the padding in stato_t is only worth
     * anything if the array starts on a cache line too. */
    o.stato = aligned_alloc(128, (size_t)righe * sizeof *o.stato);
    if (!o.inizio || !o.lungo || !o.istantanea || !o.stato) {
        free(o.inizio); free(o.lungo); free(o.istantanea); free(o.stato);
        return -1;
    }
    memset(o.stato, 0, (size_t)righe * sizeof *o.stato);

    /* Where each row's substream begins, straight from the slice header. */
    size_t off = 0;
    for (int r = 0; r < righe; r++) {
        const size_t fin = (r < sl->num_entry_point_offsets)
            ? sl->entry_point[r] : resto;
        if (fin <= off || fin > resto) {
            free(o.inizio); free(o.lungo); free(o.istantanea); free(o.stato);
            return -1;                            /* not ours to split up */
        }
        o.inizio[r] = base + off;
        o.lungo[r] = fin - off;
        off = fin;
    }

    for (int r = 0; r < righe; r++) {
        pthread_mutex_init(&o.stato[r].m, NULL);
        pthread_cond_init(&o.stato[r].c, NULL);
    }

    pthread_t t[MAX_THREAD];
    int vivi = 0;
    for (int i = 1; i < n_thread; i++)
        if (pthread_create(&t[vivi], NULL, operaio, &o) == 0) vivi++;
    operaio(&o);                                  /* this thread works too */
    for (int i = 0; i < vivi; i++) pthread_join(t[i], NULL);

    for (int r = 0; r < righe; r++) {
        pthread_mutex_destroy(&o.stato[r].m);
        pthread_cond_destroy(&o.stato[r].c);
    }
    const int e = atomic_load_explicit(&o.errore, memory_order_relaxed);
    free(o.inizio); free(o.lungo); free(o.istantanea); free(o.stato);
    return e;
}
