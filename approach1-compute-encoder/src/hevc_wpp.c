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
#define SPIN_ROUNDS 512

#if defined(__x86_64__) || defined(__i386__)
#define PAUSE() __builtin_ia32_pause()
#else
#define PAUSE() ((void)0)
#endif

/* One row's progress and its doorbell. ⚠️ Padded out to its own cache
 * line: sixteen of these one after another in memory would be sixteen
 * threads writing to the same line, which costs more than the mutex we
 * are trying to avoid. */
typedef struct {
    _Atomic int progress;                /* units this row has finished */
    _Atomic int waiting;               /* how many sleep on c */
    pthread_mutex_t m;
    pthread_cond_t c;
} __attribute__((aligned(128))) row_state_t;

typedef struct {
    hevcd_t model;                  /* what every worker starts from */
    const hevc_sps_t *sps;
    const hevc_pps_t *pps;
    const hevc_slice_t *sl;
    int init_type;

    const uint8_t **start;           /* each row's substream */
    size_t *length;
    uint8_t (*snapshot)[HEVCD_CTX]; /* the contexts each row leaves behind */
    row_state_t *row_state;

    int rows, columns;
    _Atomic int next_row_to_claim;             /* the next row nobody has claimed */
    _Atomic int error;               /* the first reason anything refused */
} wave_t;

/* This row has finished `count` units, and whoever is below may go on. */
static void publish(wave_t *o, int r, int count)
{
    row_state_t *s = &o->row_state[r];

    /* Release: everything this row wrote - samples, motion vectors, the
     * coded block flags - is in place before the count that says so. */
    atomic_store_explicit(&s->progress, count, memory_order_release);

    /* ⚠️ And that store has to be visible before we look at who is
     * asleep. Without the fence we can read "nobody" at the very moment
     * the row below is going to sleep on a value we have already written,
     * and it never wakes up. */
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&s->waiting, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&s->m);
        pthread_cond_signal(&s->c);
        pthread_mutex_unlock(&s->m);
    }
}

/* Nothing more will be decoded. Set the reason, if nobody else has, and
 * wake every sleeper so the picture comes apart instead of hanging. */
static void fail_all(wave_t *o, int fault)
{
    int none_yet = 0;
    atomic_compare_exchange_strong_explicit(&o->error, &none_yet, fault,
                                            memory_order_seq_cst,
                                            memory_order_relaxed);
    for (int r = 0; r < o->rows; r++) {
        pthread_mutex_lock(&o->row_state[r].m);
        pthread_cond_broadcast(&o->row_state[r].c);
        pthread_mutex_unlock(&o->row_state[r].m);
    }
}

/* Wait until row r has finished at least `serve` units. False when the
 * slice has already failed somewhere else and there is nothing to wait for. */
static bool wait_for(wave_t *o, int r, int serve)
{
    row_state_t *s = &o->row_state[r];

    for (int i = 0; i < SPIN_ROUNDS; i++) {
        if (atomic_load_explicit(&s->progress, memory_order_acquire) >= serve)
            return true;
        if (atomic_load_explicit(&o->error, memory_order_relaxed))
            return false;
        PAUSE();
    }

    pthread_mutex_lock(&s->m);
    atomic_fetch_add_explicit(&s->waiting, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&s->progress, memory_order_acquire) < serve
           && !atomic_load_explicit(&o->error, memory_order_relaxed))
        pthread_cond_wait(&s->c, &s->m);
    atomic_fetch_sub_explicit(&s->waiting, 1, memory_order_relaxed);
    const bool pronto =
        atomic_load_explicit(&s->progress, memory_order_acquire) >= serve;
    pthread_mutex_unlock(&s->m);
    return pronto;
}

/* One row, from its own substream, keeping step with the one above. */
static void row(wave_t *o, int r)
{
    hevcd_t d = o->model;           /* the maps are shared; the state is not */

    d.qp_y = o->sl->qp;
    d.qp_y_pred = o->sl->qp;
    d.qp_y_prev = o->sl->qp;
    d.qg_restarts = true;
    d.slice_end = false;
    d.cu_qp_delta_coded = false;
    d.cu_qp_delta = 0;

    hevcd_cabac_init(&d.cabac, o->start[r], o->length[r],
                     o->sl->type, o->sl->cabac_init_flag, o->sl->qp);

    for (int x = 0; x < o->columns; x++) {
        if (r > 0) {
            /* ⚠️ Unit x + 1 of the row above, except at the right edge
             * where there is no such unit and the whole row will do. */
            const int serve = (x + 2 < o->columns) ? x + 2 : o->columns;
            if (!wait_for(o, r - 1, serve)) return;

            if (x == 0) {
                /* 9.3.1: the statistics of the row above, as they were two
                 * units in. */
                if (o->sps->ctb_width >= 2)
                    memcpy(d.cabac.state, o->snapshot[r - 1], HEVCD_CTX);
                else
                    hevcd_cabac_ctx_init(d.cabac.state, o->init_type, o->sl->qp);
            }
        }

        const int px = x << o->sps->log2_ctb;
        const int py = r << o->sps->log2_ctb;
        int fault = 0;
        if (hevcd_read_ctu(&d, px, py)) fault = 4;
        else if (hevcd_overrun(&d.cabac)) fault = 3;

        if (!fault && x == 1)
            memcpy(o->snapshot[r], d.cabac.state, HEVCD_CTX);

        if (!fault) {
            const int fine = hevcd_terminate(&d.cabac);
            const bool last_one = (r == o->rows - 1) && (x == o->columns - 1);
            if (fine && !last_one) fault = 1;
            if (!fine && last_one) fault = 2;
            /* 7.3.8.1: every row but the last ends with a bit that is
             * defined to be one, and then the alignment. */
            if (!fault && !fine && x == o->columns - 1
                && !hevcd_terminate(&d.cabac))
                fault = 6;
        }

        if (fault) {
            fail_all(o, fault);
            return;
        }
        publish(o, r, x + 1);
    }
}

static void *worker(void *arg)
{
    wave_t *o = arg;
    for (;;) {
        const int r = atomic_fetch_add_explicit(&o->next_row_to_claim, 1,
                                                memory_order_relaxed);
        if (r >= o->rows) break;
        row(o, r);
    }
    return NULL;
}

/* How many rows to run at once. One per processor is plenty: the rows are
 * a wavefront, so past a certain width the extra threads spend their time
 * waiting for the row above rather than decoding. */
static int thread_count(int rows, int columns)
{
    const char *s = getenv("BC250_HEVC_THREAD");
    int n = s ? atoi(s) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > MAX_THREAD) n = MAX_THREAD;
    if (n > rows) n = rows;

    /* ⚠️ And no more than the shape of the picture can use. The critical
     * path through the wavefront is one row plus two units for every row
     * below it, so a thread beyond total/path would never decode anything
     * it did not have to wait for first. It would only add contention. */
    if (columns >= 2) {
        const int path = columns + 2 * (rows - 1);
        int useful = (rows * columns + path - 1) / path;
        if (useful < 1) useful = 1;
        if (n > useful) n = useful;
    }
    return n;
}

int hevcd_wavefront(hevcd_t *d, const hevc_sps_t *sps, const hevc_pps_t *pps,
                    const hevc_slice_t *sl, const uint8_t *base, size_t rest,
                    int init_type)
{
    const int rows = sps->ctb_height;
    const int columns = sps->ctb_width;
    const int n_thread = thread_count(rows, columns);
    if (n_thread < 2) return -1;                  /* the caller walks it */

    wave_t o;
    memset(&o, 0, sizeof o);
    o.model = *d;
    o.sps = sps; o.pps = pps; o.sl = sl;
    o.init_type = init_type;
    o.rows = rows; o.columns = columns;

    o.start = calloc((size_t)rows, sizeof *o.start);
    o.length = calloc((size_t)rows, sizeof *o.length);
    o.snapshot = calloc((size_t)rows, sizeof *o.snapshot);
    /* ⚠️ aligned_alloc, not calloc: the padding in row_state_t is only worth
     * anything if the array starts on a cache line too. */
    o.row_state = aligned_alloc(128, (size_t)rows * sizeof *o.row_state);
    if (!o.start || !o.length || !o.snapshot || !o.row_state) {
        free(o.start); free(o.length); free(o.snapshot); free(o.row_state);
        return -1;
    }
    memset(o.row_state, 0, (size_t)rows * sizeof *o.row_state);

    /* Where each row's substream begins, straight from the slice header. */
    size_t off = 0;
    for (int r = 0; r < rows; r++) {
        const size_t fin = (r < sl->num_entry_point_offsets)
            ? sl->entry_point[r] : rest;
        if (fin <= off || fin > rest) {
            free(o.start); free(o.length); free(o.snapshot); free(o.row_state);
            return -1;                            /* not ours to split up */
        }
        o.start[r] = base + off;
        o.length[r] = fin - off;
        off = fin;
    }

    for (int r = 0; r < rows; r++) {
        pthread_mutex_init(&o.row_state[r].m, NULL);
        pthread_cond_init(&o.row_state[r].c, NULL);
    }

    pthread_t t[MAX_THREAD];
    int alive = 0;
    for (int i = 1; i < n_thread; i++)
        if (pthread_create(&t[alive], NULL, worker, &o) == 0) alive++;
    worker(&o);                                  /* this thread works too */
    for (int i = 0; i < alive; i++) pthread_join(t[i], NULL);

    for (int r = 0; r < rows; r++) {
        pthread_mutex_destroy(&o.row_state[r].m);
        pthread_cond_destroy(&o.row_state[r].c);
    }
    const int e = atomic_load_explicit(&o.error, memory_order_relaxed);
    free(o.start); free(o.length); free(o.snapshot); free(o.row_state);
    return e;
}
