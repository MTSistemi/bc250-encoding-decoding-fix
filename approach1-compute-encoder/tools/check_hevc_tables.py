#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Check the generated H.265 tables against what the standard says they are.

The generator reads someone else's source and rewrites it. That is much
safer than typing the numbers, but it is not proof: a table read with the
wrong shape comes out full of plausible numbers in the wrong places, which
is the failure that costs the most to find later. So every table gets
asked a question only a correct one can answer.

    python3 tools/check_hevc_tables.py <src dir>

⚠️ These are properties, not copies of the tables. Comparing against a
second copy of the same numbers would only prove they were copied twice.
"""
import pathlib
import re
import sys


def read(text, nome):
    i = text.find(" " + nome + "[")
    if i < 0:
        i = text.find(nome + "[")
    if i < 0:
        raise SystemExit("non found %s" % nome)
    i = text.find("{", i)
    liv = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            liv += 1
        elif text[j] == "}":
            liv -= 1
            if liv == 0:
                break
    return [int(x) for x in re.findall(r"-?\d+", text[i:j + 1])]


def lines(text, nome, width):
    flat = read(text, nome)
    if len(flat) % width:
        raise SystemExit("%s: %d values, not a multiple of %d"
                         % (nome, len(flat), width))
    return [flat[k:k + width] for k in range(0, len(flat), width)]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    src = pathlib.Path(sys.argv[1])
    c = (src / "hevc_dec_tables.c").read_text(encoding="utf-8")
    h = (src / "hevc_dec_tables.h").read_text(encoding="utf-8")

    m = re.search(r"#define HEVCD_CTX (\d+)", h)
    n_ctx = int(m.group(1))
    faults = []

    def say(ok, what):
        print("  %-58s %s" % (what, "ok" if ok else "NO"))
        if not ok:
            faults.append(what)

    print("contexts")
    init = lines(c, "hevcd_ctx_init", n_ctx)
    say(len(init) == 3 * 52, "one row per initType and per QP (%d)" % len(init))
    # Packed as (pStateIdx << 1) | valMPS with pStateIdx in 1..63, because
    # preCtxState is clipped to 1..126 and the index is min(s, 127 - s).
    worst = max(max(r) for r in init)
    say(worst <= 127, "state maximum within 127 (%d)" % worst)
    minimum = min(min(r) >> 1 for r in init)
    say(minimum >= 1, "pStateIdx mai zero (%d)" % minimum)
    # ⚠️ A context whose m is zero does not move with QP. Most do move, and
    # a table read with the wrong stride would show far too few that do.
    moving = sum(1 for i in range(n_ctx)
                 if any(init[qp][i] != init[0][i] for qp in range(52)))
    say(moving > n_ctx // 3,
         "contexts che vary col QP: %d su %d" % (moving, n_ctx))

    print("scansioni")
    for nome, side in (("hevcd_diag4", 4), ("hevcd_diag8", 8)):
        x = read(c, nome + "_x")
        y = read(c, nome + "_y")
        say(len(x) == side * side and len(y) == side * side,
             "%s: %d positions" % (nome, len(x)))
        pairs = set(zip(x, y))
        say(len(pairs) == side * side and max(x) == side - 1
             and max(y) == side - 1, "%s is a permutation" % nome)
        # The diagonal scan walks up-right: within it, x + y never goes down.
        somme = [a + b for a, b in zip(x, y)]
        say(all(somme[k] <= somme[k + 1] for k in range(len(somme) - 1)),
             "%s proceeds per antidiagonali" % nome)

    for nome, side in (("hevcd_horiz2", 2), ("hevcd_horiz4", 4)):
        x = read(c, nome + "_x")
        y = read(c, nome + "_y")
        say(len(set(zip(x, y))) == side * side,
             "%s is a permutation" % nome)
        say(all(y[k] <= y[k + 1] for k in range(len(y) - 1)),
             "%s proceeds per lines" % nome)

    print("scansioni inverse")
    for nome, side, direct in (("hevcd_diag4_inv", 4, "hevcd_diag4"),
                                ("hevcd_diag8_inv", 8, "hevcd_diag8")):
        inv = lines(c, nome, side)
        x = read(c, direct + "_x")
        y = read(c, direct + "_y")
        ok = all(inv[y[k]][x[k]] == k for k in range(side * side))
        say(ok, "%s inverts %s" % (nome, direct))

    print("interpolation filters")
    qpel = lines(c, "hevcd_qpel", 8)
    epel = lines(c, "hevcd_epel", 4)
    say(len(qpel) == 4 and len(epel) == 8,
         "four phases luma, otto croma (%d, %d)" % (len(qpel), len(epel)))
    # Every filter has a gain of exactly 64: the shift after it is by six.
    for nome, tab in (("luma", qpel), ("croma", epel)):
        somme = [sum(r) for r in tab]
        say(all(s in (0, 64) for s in somme),
             "%s: gain 64 on every phase (%s)" % (nome, somme))
    say(sum(qpel[0]) == 0 and sum(epel[0]) == 0,
         "the whole-sample phase does not filter")
    # 8.5.3.3.3.2: the half-sample luma filter is symmetric.
    say(qpel[2] == qpel[2][::-1], "the half-sample luma filter is symmetric")

    print("deblocking and chroma QP")
    tc = read(c, "hevcd_tc")
    beta = read(c, "hevcd_beta")
    say(len(tc) == 54 and len(beta) == 52,
         "tc 54, beta 52 (%d, %d)" % (len(tc), len(beta)))
    say(all(tc[k] <= tc[k + 1] for k in range(len(tc) - 1)),
         "tc non decreases")
    say(all(beta[k] <= beta[k + 1] for k in range(len(beta) - 1)),
         "beta non decreases")
    qp_c = read(c, "hevcd_qp_c")
    # Table 8-10 covers qPi 30..43; below 30 QpC is qPi and above 43 it is
    # qPi - 6, so only the fourteen in between are a table.
    say(len(qp_c) == 14, "qp croma: fourteen entries (%d)" % len(qp_c))
    say(qp_c[0] == 29 and qp_c[-1] == 37,
         "chroma qp runs from 29 to 37 (%d..%d)" % (qp_c[0], qp_c[-1]))
    say(all(qp_c[k] <= qp_c[k + 1] for k in range(len(qp_c) - 1)),
         "qp croma non decreases")

    print("intra prediction")
    ang = read(c, "hevcd_intra_angle")
    inv = read(c, "hevcd_inv_angle")
    say(len(ang) == 33, "one angle per mode from 2 to 34 (%d)" % len(ang))
    # Mode 2 and mode 34 are the two corners, mode 18 the other diagonal.
    say(ang[0] == 32 and ang[-1] == 32 and ang[16] == -32,
         "the two extreme angles and the diagonal (%d, %d, %d)" % (ang[0], ang[-1], ang[16]))
    say(all(ang[k] == ang[32 - k] for k in range(33)),
         "the angles mirror around mode 18")
    # inv_angle covers modes 11..25, the ones that reach across the corner
    # and have to project the other edge onto their own reference line.
    say(len(inv) == 15, "fifteen angles inverses (%d)" % len(inv))
    say(all(inv[k] == inv[14 - k] for k in range(15)),
         "the inverses mirror each other too")
    # Table 8-8: invAngle is 8192 over the angle, which is what makes the
    # projection exact at eight bits of fraction.
    #
    # ⚠️ Rounded to nearest, not truncated. Two of the fifteen differ, and
    # only those two: 8192 over 17 is 481.88, and the table says 482.
    def inverse(a):
        a = abs(a)
        return -((8192 + a // 2) // a)
    out_values = [k for k in range(15)
             if ang[k + 9] and inv[k] != inverse(ang[k + 9])]
    say(not out_values, "every inverse is 8192 divided by its angle (%s)" % out_values)

    print("la matrix della transform")
    dct = lines(c, "hevcd_dct", 32)
    say(len(dct) == 32, "trentadue lines da trentadue (%d)" % len(dct))
    say(all(v == 64 for v in dct[0]), "la first riga e' whole 64")
    # 8.6.4.2: the n-point matrix is rows k * (32 / n) of this one. Both of
    # these are typed from the standard, not read from the same source the
    # generator read, so they say something the generator cannot.
    q4 = [[64, 64, 64, 64], [83, 36, -36, -83],
          [64, -64, -64, 64], [36, -83, 83, -36]]
    row4 = [[dct[k * 8][i] for i in range(4)] for k in range(4)]
    say(row4 == q4, "the four rows are the 4-point matrix")
    q8 = [[64, 64, 64, 64, 64, 64, 64, 64],
          [89, 75, 50, 18, -18, -50, -75, -89],
          [83, 36, -36, -83, -83, -36, 36, 83],
          [75, -18, -89, -50, 50, 89, 18, -75],
          [64, -64, -64, 64, 64, -64, -64, 64],
          [50, -89, 18, 75, -75, -18, 89, -50],
          [36, -83, 83, -36, -36, 83, -83, 36],
          [18, -50, 75, -89, 89, -75, 50, -18]]
    row8 = [[dct[k * 4][i] for i in range(8)] for k in range(8)]
    say(row8 == q8, "the eight rows are the 8-point matrix")
    # ⚠️ Nearly orthogonal, not orthogonal. The basis functions are cosines
    # rounded to integers, and above four points the rounding leaves a
    # residue: the worst pair of the 32-point matrix dots to 376 against a
    # norm of 131244, which is three parts in a thousand. Asking for exact
    # zeros fails on a correct table, which is how this check first read.
    # What a wrong digit does is nothing like three parts in a thousand.
    def product(a, b, n):
        s = 32 // n
        return sum(dct[a * s][i] * dct[b * s][i] for i in range(n))

    for n in (4, 8, 16, 32):
        norma = product(1, 1, n)
        worst = max(abs(product(a, b, n))
                     for a in range(n) for b in range(a + 1, n))
        limit = 0 if n == 4 else norma // 100
        say(worst <= limit,
             "%d points: lines quasi orthogonal (%d su %d)"
             % (n, worst, norma))
    # Even rows read the same backwards, odd rows read the same negated:
    # the basis functions are symmetric and antisymmetric in turn.
    mirror = [k for k in range(32)
                if any(dct[k][31 - i] != (dct[k][i] if k % 2 == 0
                                          else -dct[k][i]) for i in range(32))]
    say(not mirror, "even a mirror e odd a mirror col sign (%s)"
         % mirror)

    print("quantisation levels")
    ls = read(c, "hevcd_level_scale")
    say(ls == [40, 45, 51, 57, 64, 72], "levelScale as in 8.6.3 (%s)" % ls)

    print()
    if faults:
        print("%d checks failed_count:" % len(faults))
        for p in faults:
            print("  - %s" % p)
        return 1
    print("everything in order")
    return 0


if __name__ == "__main__":
    sys.exit(main())
