/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_mc.c - inter prediction against a literal reading of the standard.
 *
 * Same method as test_pred.c. h264_mc.c computes the half-sample positions
 * once for a whole block and reuses them across the sixteen quarter-sample
 * cases; the version here computes every sample from scratch, straight out
 * of clause 8.4.2.2.1, reading the reference plane through one clamped
 * accessor. It is far slower and knows nothing about blocks, which is the
 * point.
 *
 * Motion vectors are drawn well outside the picture as well as inside, so
 * the edge clamping in h264d_mc_fetch_luma() is exercised too: a reference
 * block hanging off the corner of the picture is not an edge case in H.264,
 * it is normal at every picture boundary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264_mc.h"

static uint64_t seme = 0xB7E151628AED2A6Bull;
static uint32_t casuale(void)
{
    seme ^= seme >> 12;
    seme ^= seme << 25;
    seme ^= seme >> 27;
    return (uint32_t)((seme * 0x2545F4914F6CDD1Dull) >> 32);
}

#define PW 48
#define PH 40

static uint8_t piano[PH * PW];

static int clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* The reference plane, with the edge clamping H.264 requires (8.4.2.2.1:
 * the sample coordinates are clipped to the picture). */
static int S(int x, int y)
{
    x = x < 0 ? 0 : (x >= PW ? PW - 1 : x);
    y = y < 0 ? 0 : (y >= PH ? PH - 1 : y);
    return piano[y * PW + x];
}

#define TAP(a, b, c, d, e, f) ((a) - 5 * (b) + 20 * (c) + 20 * (d) - 5 * (e) + (f))

/* The horizontal six-tap intermediate at (x + 1/2, y), unrounded. */
static int b1_a(int x, int y)
{
    return TAP(S(x - 2, y), S(x - 1, y), S(x, y), S(x + 1, y), S(x + 2, y), S(x + 3, y));
}

/* The vertical one at (x, y + 1/2), unrounded. */
static int h1_a(int x, int y)
{
    return TAP(S(x, y - 2), S(x, y - 1), S(x, y), S(x, y + 1), S(x, y + 2), S(x, y + 3));
}

/* Clause 8.4.2.2.1, one sample, literally. */
static int letterale_luma(int x, int y, int xf, int yf)
{
    int G = S(x, y), H = S(x + 1, y), M = S(x, y + 1);
    int b = clip255((b1_a(x, y) + 16) >> 5);
    int h = clip255((h1_a(x, y) + 16) >> 5);
    int m = clip255((h1_a(x + 1, y) + 16) >> 5);
    int s = clip255((b1_a(x, y + 1) + 16) >> 5);

    /* j is filtered from the unrounded intermediates and shifted once. */
    int j1 = TAP(b1_a(x, y - 2), b1_a(x, y - 1), b1_a(x, y),
                 b1_a(x, y + 1), b1_a(x, y + 2), b1_a(x, y + 3));
    int j = clip255((j1 + 512) >> 10);

    switch (yf * 4 + xf) {
    case  0: return G;
    case  1: return (G + b + 1) >> 1;          /* a */
    case  2: return b;
    case  3: return (H + b + 1) >> 1;          /* c */
    case  4: return (G + h + 1) >> 1;          /* d */
    case  5: return (b + h + 1) >> 1;          /* e */
    case  6: return (b + j + 1) >> 1;          /* f */
    case  7: return (b + m + 1) >> 1;          /* g */
    case  8: return h;
    case  9: return (h + j + 1) >> 1;          /* i */
    case 10: return j;
    case 11: return (j + m + 1) >> 1;          /* k */
    case 12: return (M + h + 1) >> 1;          /* n */
    case 13: return (h + s + 1) >> 1;          /* p */
    case 14: return (j + s + 1) >> 1;          /* q */
    case 15: return (m + s + 1) >> 1;          /* r */
    default: abort();
    }
}

/* Clause 8.4.2.2.2. */
static int letterale_chroma(int x, int y, int xf, int yf)
{
    return ((8 - xf) * (8 - yf) * S(x, y)
          + xf * (8 - yf) * S(x + 1, y)
          + (8 - xf) * yf * S(x, y + 1)
          + xf * yf * S(x + 1, y + 1) + 32) >> 6;
}

/* Every block size an H.264 partition can be. */
static const int misure[][2] = {
    {16,16},{16,8},{8,16},{8,8},{8,4},{4,8},{4,4}
};

static int prova_luma(void)
{
    uint8_t padded[(16 + 6) * (16 + 6)];
    uint8_t nostro[16 * 16], suo[16 * 16];
    int guasti = 0;

    for (int giro = 0; giro < 3000; giro++) {
        for (int i = 0; i < PH * PW; i++) piano[i] = (uint8_t)casuale();

        for (unsigned mi = 0; mi < sizeof(misure) / sizeof(misure[0]); mi++) {
            int w = misure[mi][0], h = misure[mi][1];
            /* Inside the picture, straddling an edge, and entirely outside. */
            int x = (int)(casuale() % (PW + 32)) - 16;
            int y = (int)(casuale() % (PH + 32)) - 16;

            for (int yf = 0; yf < 4; yf++) {
                for (int xf = 0; xf < 4; xf++) {
                    const uint8_t *src = h264d_mc_fetch_luma(padded, piano, PW,
                                                             PW, PH, x, y, w, h);
                    h264d_mc_luma(nostro, w, src, w + 6, w, h, xf, yf);

                    for (int j = 0; j < h; j++)
                        for (int i = 0; i < w; i++)
                            suo[j * w + i] = (uint8_t)letterale_luma(x + i, y + j, xf, yf);

                    if (memcmp(nostro, suo, (size_t)w * h)) {
                        if (guasti < 5) {
                            printf("  luma %dx%d a (%d,%d), frac (%d,%d):\n",
                                   w, h, x, y, xf, yf);
                            for (int j = 0; j < (h < 4 ? h : 4); j++) {
                                printf("    noi ");
                                for (int i = 0; i < (w < 8 ? w : 8); i++)
                                    printf("%4d", nostro[j * w + i]);
                                printf("   norma ");
                                for (int i = 0; i < (w < 8 ? w : 8); i++)
                                    printf("%4d", suo[j * w + i]);
                                printf("\n");
                            }
                        }
                        guasti++;
                    }
                }
            }
        }
    }
    return guasti;
}

static int prova_chroma(void)
{
    uint8_t padded[(8 + 1) * (8 + 1)];
    uint8_t nostro[8 * 8], suo[8 * 8];
    int guasti = 0;

    for (int giro = 0; giro < 3000; giro++) {
        for (int i = 0; i < PH * PW; i++) piano[i] = (uint8_t)casuale();

        for (unsigned mi = 0; mi < sizeof(misure) / sizeof(misure[0]); mi++) {
            int w = misure[mi][0] / 2, h = misure[mi][1] / 2;
            if (w < 2 || h < 2) continue;
            int x = (int)(casuale() % (PW + 16)) - 8;
            int y = (int)(casuale() % (PH + 16)) - 8;

            for (int yf = 0; yf < 8; yf++) {
                for (int xf = 0; xf < 8; xf++) {
                    const uint8_t *src = h264d_mc_fetch_chroma(padded, piano, PW,
                                                               PW, PH, x, y, w, h);
                    h264d_mc_chroma(nostro, w, src, w + 1, w, h, xf, yf);

                    for (int j = 0; j < h; j++)
                        for (int i = 0; i < w; i++)
                            suo[j * w + i] = (uint8_t)letterale_chroma(x + i, y + j, xf, yf);

                    if (memcmp(nostro, suo, (size_t)w * h)) {
                        if (guasti < 5)
                            printf("  croma %dx%d a (%d,%d), frac (%d,%d): diversi\n",
                                   w, h, x, y, xf, yf);
                        guasti++;
                    }
                }
            }
        }
    }
    return guasti;
}

/* Clause 8.4.2.3: the default average and the explicit weights. */
static int prova_combinazione(void)
{
    uint8_t a[256], b[256], nostro[256];
    int guasti = 0;

    for (int giro = 0; giro < 20000; giro++) {
        for (int i = 0; i < 256; i++) { a[i] = (uint8_t)casuale(); b[i] = (uint8_t)casuale(); }

        h264d_mc_average(nostro, 16, a, 16, b, 16, 16, 16);
        for (int i = 0; i < 256; i++) {
            int y = i / 16, x = i % 16;
            int atteso = (a[y * 16 + x] + b[y * 16 + x] + 1) >> 1;
            if (nostro[y * 16 + x] != atteso) { guasti++; break; }
        }

        int d = (int)(casuale() % 8);
        int w0 = (int)(casuale() % 255) - 128;
        int o0 = (int)(casuale() % 255) - 128;
        h264d_mc_weight(nostro, 16, a, 16, 16, 16, d, w0, o0);
        for (int i = 0; i < 256; i++) {
            int atteso = d ? clip255(((a[i] * w0 + (1 << (d - 1))) >> d) + o0)
                           : clip255(a[i] * w0 + o0);
            if (nostro[i] != atteso) { guasti++; break; }
        }

        int w1 = (int)(casuale() % 255) - 128;
        int o1 = (int)(casuale() % 255) - 128;
        h264d_mc_weight_bi(nostro, 16, a, 16, b, 16, 16, 16, d, w0, o0, w1, o1);
        for (int i = 0; i < 256; i++) {
            /* Clause 8.4.2.3.2, written out in its two cases. The shift
             * happens first and the offset is added to the result; the
             * earlier version of this line folded the offset into the
             * rounding term, which is a different number and, being the same
             * mistake the implementation made, agreed with it perfectly. */
            int atteso;
            if (d >= 1)
                atteso = clip255(((a[i] * w0 + b[i] * w1 + (1 << d)) >> (d + 1))
                                 + ((o0 + o1 + 1) >> 1));
            else
                atteso = clip255(a[i] * w0 + b[i] * w1 + ((o0 + o1 + 1) >> 1));
            if (nostro[i] != atteso) { guasti++; break; }
        }
    }
    return guasti;
}

int main(void)
{
    printf("1. luma, sedici posizioni a un quarto di campione\n");
    int gl = prova_luma();
    if (gl) { printf("   %d blocchi diversi\n", gl); return 1; }
    printf("   3000 giri x 7 misure x 16 posizioni: identici\n");

    printf("2. croma, sessantaquattro posizioni a un ottavo\n");
    int gc = prova_chroma();
    if (gc) { printf("   %d blocchi diversi\n", gc); return 1; }
    printf("   3000 giri x 5 misure x 64 posizioni: identici\n");

    printf("3. media e pesi espliciti\n");
    int gw = prova_combinazione();
    if (gw) { printf("   %d blocchi diversi\n", gw); return 1; }
    printf("   20000 giri: identici\n");

    printf("\nOK\n");
    return 0;
}
