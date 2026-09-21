/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_hevc_transform.c - the inverse transforms against the matrices
 * they are supposed to be.
 *
 * hevcd_trasforma does not multiply by a matrix. The DCT reads one row of
 * a shared table with a stride, and the 4x4 intra luma DST is factored
 * into four multiplications instead of sixteen. Both are worth doing and
 * both are easy to get wrong in a way that leaves most of the output
 * right: a DST written with x1 where x2 belongs gives three correct
 * coefficients out of four, which does not look like a transform bug at
 * all when you meet it in a picture.
 *
 * ⚠️ The DST matrix here is typed from clause 8.6.4.2 and not derived
 * from the factored form, which is the whole point. A reference computed
 * from the same expression would agree with any mistake in it.
 */
#include "hevc_dec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int guasti;

/* Table in 8.6.4.2, transMatrix for the 4x4 luma intra case. */
static const int dst[4][4] = {
    { 29, 55, 74, 84 },
    { 74, 74,  0, -74 },
    { 84, -29, -74, 55 },
    { 55, -84, 74, -29 },
};

static int ritaglia16(int v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

/* 8.6.4.2 written the long way: columns, shift by seven, rows, shift by
 * twenty minus the bit depth. Both stages clipped to sixteen bits. */
static void riferimento(const int16_t *in, int16_t *out, int n, bool usa_dst)
{
    int16_t tmp[32 * 32];

    for (int x = 0; x < n; x++)
        for (int y = 0; y < n; y++) {
            int s = 0;
            for (int k = 0; k < n; k++) {
                const int m = usa_dst ? dst[k][y]
                                      : hevcd_dct[k * (32 / n)][y];
                s += m * in[k * n + x];
            }
            tmp[y * n + x] = (int16_t)ritaglia16((s + 64) >> 7);
        }

    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int s = 0;
            for (int k = 0; k < n; k++) {
                const int m = usa_dst ? dst[k][x]
                                      : hevcd_dct[k * (32 / n)][x];
                s += m * tmp[y * n + k];
            }
            out[y * n + x] = (int16_t)ritaglia16((s + 2048) >> 12);
        }
}

static unsigned semi = 12345;

static int prossimo(int ampiezza)
{
    semi = semi * 1103515245u + 12345u;
    return (int)((semi >> 16) % (unsigned)(2 * ampiezza + 1)) - ampiezza;
}

static void confronta(const char *cosa, const int16_t *a, const int16_t *b,
                      int quanti)
{
    int diversi = 0, peggio = 0;
    for (int i = 0; i < quanti; i++)
        if (a[i] != b[i]) {
            diversi++;
            const int e = abs(a[i] - b[i]);
            if (e > peggio) peggio = e;
        }
    if (diversi) {
        printf("  %-34s %d su %d diversi, il peggiore di %d\n",
               cosa, diversi, quanti, peggio);
        guasti++;
    } else {
        printf("  %-34s uguale\n", cosa);
    }
}

/* One impulse at a time: each coefficient on its own says which basis
 * function came out, so a swapped row shows up on its own line instead of
 * being averaged away with thirty-one others. */
static void impulsi(int log2_size, bool usa_dst)
{
    const int n = 1 << log2_size;
    int16_t nostro[32 * 32], atteso[32 * 32], ingresso[32 * 32];
    char cosa[64];
    int diversi = 0;

    for (int k = 0; k < n * n; k++) {
        memset(ingresso, 0, sizeof(int16_t) * (size_t)n * n);
        ingresso[k] = 256;
        memcpy(nostro, ingresso, sizeof(int16_t) * (size_t)n * n);
        hevcd_trasforma(nostro, log2_size, usa_dst);
        riferimento(ingresso, atteso, n, usa_dst);
        if (memcmp(nostro, atteso, sizeof(int16_t) * (size_t)n * n))
            diversi++;
    }
    snprintf(cosa, sizeof cosa, "%s %dx%d, un impulso per volta",
             usa_dst ? "DST" : "DCT", n, n);
    if (diversi) {
        printf("  %-34s %d basi su %d sbagliate\n", cosa, diversi, n * n);
        guasti++;
    } else {
        printf("  %-34s uguale\n", cosa);
    }
}

static void a_caso(int log2_size, bool usa_dst)
{
    const int n = 1 << log2_size;
    int16_t nostro[32 * 32], atteso[32 * 32], ingresso[32 * 32];
    char cosa[64];

    snprintf(cosa, sizeof cosa, "%s %dx%d, blocchi a caso",
             usa_dst ? "DST" : "DCT", n, n);

    for (int giro = 0; giro < 64; giro++) {
        for (int i = 0; i < n * n; i++) ingresso[i] = (int16_t)prossimo(4000);
        memcpy(nostro, ingresso, sizeof(int16_t) * (size_t)n * n);
        hevcd_trasforma(nostro, log2_size, usa_dst);
        riferimento(ingresso, atteso, n, usa_dst);
        if (memcmp(nostro, atteso, sizeof(int16_t) * (size_t)n * n)) {
            confronta(cosa, nostro, atteso, n * n);
            return;
        }
    }
    printf("  %-34s uguale\n", cosa);
}

int main(void)
{
    printf("le trasformate inverse contro le loro matrici\n");
    impulsi(2, true);
    a_caso(2, true);
    for (int l = 2; l <= 5; l++) {
        impulsi(l, false);
        a_caso(l, false);
    }

    printf("\n");
    if (guasti) {
        printf("%d prove fallite\n", guasti);
        return 1;
    }
    printf("tutto a posto\n");
    return 0;
}
