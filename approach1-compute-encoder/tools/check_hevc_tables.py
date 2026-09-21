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


def leggi(testo, nome):
    i = testo.find(" " + nome + "[")
    if i < 0:
        i = testo.find(nome + "[")
    if i < 0:
        raise SystemExit("non trovo %s" % nome)
    i = testo.find("{", i)
    liv = 0
    for j in range(i, len(testo)):
        if testo[j] == "{":
            liv += 1
        elif testo[j] == "}":
            liv -= 1
            if liv == 0:
                break
    return [int(x) for x in re.findall(r"-?\d+", testo[i:j + 1])]


def righe(testo, nome, larghezza):
    piatta = leggi(testo, nome)
    if len(piatta) % larghezza:
        raise SystemExit("%s: %d valori, non multiplo di %d"
                         % (nome, len(piatta), larghezza))
    return [piatta[k:k + larghezza] for k in range(0, len(piatta), larghezza)]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    src = pathlib.Path(sys.argv[1])
    c = (src / "hevc_dec_tables.c").read_text(encoding="utf-8")
    h = (src / "hevc_dec_tables.h").read_text(encoding="utf-8")

    m = re.search(r"#define HEVCD_CTX (\d+)", h)
    n_ctx = int(m.group(1))
    problemi = []

    def dire(ok, cosa):
        print("  %-58s %s" % (cosa, "ok" if ok else "NO"))
        if not ok:
            problemi.append(cosa)

    print("contesti")
    init = righe(c, "hevcd_ctx_init", n_ctx)
    dire(len(init) == 3 * 52, "una riga per initType e per QP (%d)" % len(init))
    # Packed as (pStateIdx << 1) | valMPS with pStateIdx in 1..63, because
    # preCtxState is clipped to 1..126 and the index is min(s, 127 - s).
    peggio = max(max(r) for r in init)
    dire(peggio <= 127, "stato massimo entro 127 (%d)" % peggio)
    minimo = min(min(r) >> 1 for r in init)
    dire(minimo >= 1, "pStateIdx mai zero (%d)" % minimo)
    # ⚠️ A context whose m is zero does not move with QP. Most do move, and
    # a table read with the wrong stride would show far too few that do.
    mobili = sum(1 for i in range(n_ctx)
                 if any(init[qp][i] != init[0][i] for qp in range(52)))
    dire(mobili > n_ctx // 3,
         "contesti che variano col QP: %d su %d" % (mobili, n_ctx))

    print("scansioni")
    for nome, lato in (("hevcd_diag4", 4), ("hevcd_diag8", 8)):
        x = leggi(c, nome + "_x")
        y = leggi(c, nome + "_y")
        dire(len(x) == lato * lato and len(y) == lato * lato,
             "%s: %d posizioni" % (nome, len(x)))
        coppie = set(zip(x, y))
        dire(len(coppie) == lato * lato and max(x) == lato - 1
             and max(y) == lato - 1, "%s e' una permutazione" % nome)
        # The diagonal scan walks up-right: within it, x + y never goes down.
        somme = [a + b for a, b in zip(x, y)]
        dire(all(somme[k] <= somme[k + 1] for k in range(len(somme) - 1)),
             "%s procede per antidiagonali" % nome)

    for nome, lato in (("hevcd_horiz2", 2), ("hevcd_horiz4", 4)):
        x = leggi(c, nome + "_x")
        y = leggi(c, nome + "_y")
        dire(len(set(zip(x, y))) == lato * lato,
             "%s e' una permutazione" % nome)
        dire(all(y[k] <= y[k + 1] for k in range(len(y) - 1)),
             "%s procede per righe" % nome)

    print("scansioni inverse")
    for nome, lato, diretta in (("hevcd_diag4_inv", 4, "hevcd_diag4"),
                                ("hevcd_diag8_inv", 8, "hevcd_diag8")):
        inv = righe(c, nome, lato)
        x = leggi(c, diretta + "_x")
        y = leggi(c, diretta + "_y")
        ok = all(inv[y[k]][x[k]] == k for k in range(lato * lato))
        dire(ok, "%s inverte %s" % (nome, diretta))

    print("filtri di interpolazione")
    qpel = righe(c, "hevcd_qpel", 8)
    epel = righe(c, "hevcd_epel", 4)
    dire(len(qpel) == 4 and len(epel) == 8,
         "quattro fasi luma, otto croma (%d, %d)" % (len(qpel), len(epel)))
    # Every filter has a gain of exactly 64: the shift after it is by six.
    for nome, tab in (("luma", qpel), ("croma", epel)):
        somme = [sum(r) for r in tab]
        dire(all(s in (0, 64) for s in somme),
             "%s: guadagno 64 su ogni fase (%s)" % (nome, somme))
    dire(sum(qpel[0]) == 0 and sum(epel[0]) == 0,
         "la fase intera non filtra")
    # 8.5.3.3.3.2: the half-sample luma filter is symmetric.
    dire(qpel[2] == qpel[2][::-1], "il mezzo campione luma e' simmetrico")

    print("deblocking e QP croma")
    tc = leggi(c, "hevcd_tc")
    beta = leggi(c, "hevcd_beta")
    dire(len(tc) == 54 and len(beta) == 52,
         "tc 54, beta 52 (%d, %d)" % (len(tc), len(beta)))
    dire(all(tc[k] <= tc[k + 1] for k in range(len(tc) - 1)),
         "tc non decresce")
    dire(all(beta[k] <= beta[k + 1] for k in range(len(beta) - 1)),
         "beta non decresce")
    qp_c = leggi(c, "hevcd_qp_c")
    # Table 8-10 covers qPi 30..43; below 30 QpC is qPi and above 43 it is
    # qPi - 6, so only the fourteen in between are a table.
    dire(len(qp_c) == 14, "qp croma: quattordici voci (%d)" % len(qp_c))
    dire(qp_c[0] == 29 and qp_c[-1] == 37,
         "qp croma va da 29 a 37 (%d..%d)" % (qp_c[0], qp_c[-1]))
    dire(all(qp_c[k] <= qp_c[k + 1] for k in range(len(qp_c) - 1)),
         "qp croma non decresce")

    print("predizione intra")
    ang = leggi(c, "hevcd_intra_angle")
    inv = leggi(c, "hevcd_inv_angle")
    dire(len(ang) == 33, "un angolo per ogni modo da 2 a 34 (%d)" % len(ang))
    # Mode 2 and mode 34 are the two corners, mode 18 the other diagonal.
    dire(ang[0] == 32 and ang[-1] == 32 and ang[16] == -32,
         "i due angoli e la diagonale (%d, %d, %d)" % (ang[0], ang[-1], ang[16]))
    dire(all(ang[k] == ang[32 - k] for k in range(33)),
         "gli angoli sono a specchio attorno al modo 18")
    # inv_angle covers modes 11..25, the ones that reach across the corner
    # and have to project the other edge onto their own reference line.
    dire(len(inv) == 15, "quindici angoli inversi (%d)" % len(inv))
    dire(all(inv[k] == inv[14 - k] for k in range(15)),
         "anche gli inversi sono a specchio")
    # Table 8-8: invAngle is 8192 over the angle, which is what makes the
    # projection exact at eight bits of fraction.
    #
    # ⚠️ Rounded to nearest, not truncated. Two of the fifteen differ, and
    # only those two: 8192 over 17 is 481.88, and the table says 482.
    def inverso(a):
        a = abs(a)
        return -((8192 + a // 2) // a)
    fuori = [k for k in range(15)
             if ang[k + 9] and inv[k] != inverso(ang[k + 9])]
    dire(not fuori, "ogni inverso e' 8192 diviso il suo angolo (%s)" % fuori)

    print("la matrice della trasformata")
    dct = righe(c, "hevcd_dct", 32)
    dire(len(dct) == 32, "trentadue righe da trentadue (%d)" % len(dct))
    dire(all(v == 64 for v in dct[0]), "la prima riga e' tutta 64")
    # 8.6.4.2: the n-point matrix is rows k * (32 / n) of this one. Both of
    # these are typed from the standard, not read from the same source the
    # generator read, so they say something the generator cannot.
    q4 = [[64, 64, 64, 64], [83, 36, -36, -83],
          [64, -64, -64, 64], [36, -83, 83, -36]]
    letto4 = [[dct[k * 8][i] for i in range(4)] for k in range(4)]
    dire(letto4 == q4, "le quattro righe sono la matrice a 4 punti")
    q8 = [[64, 64, 64, 64, 64, 64, 64, 64],
          [89, 75, 50, 18, -18, -50, -75, -89],
          [83, 36, -36, -83, -83, -36, 36, 83],
          [75, -18, -89, -50, 50, 89, 18, -75],
          [64, -64, -64, 64, 64, -64, -64, 64],
          [50, -89, 18, 75, -75, -18, 89, -50],
          [36, -83, 83, -36, -36, 83, -83, 36],
          [18, -50, 75, -89, 89, -75, 50, -18]]
    letto8 = [[dct[k * 4][i] for i in range(8)] for k in range(8)]
    dire(letto8 == q8, "le otto righe sono la matrice a 8 punti")
    # ⚠️ Nearly orthogonal, not orthogonal. The basis functions are cosines
    # rounded to integers, and above four points the rounding leaves a
    # residue: the worst pair of the 32-point matrix dots to 376 against a
    # norm of 131244, which is three parts in a thousand. Asking for exact
    # zeros fails on a correct table, which is how this check first read.
    # What a wrong digit does is nothing like three parts in a thousand.
    def prodotto(a, b, n):
        s = 32 // n
        return sum(dct[a * s][i] * dct[b * s][i] for i in range(n))

    for n in (4, 8, 16, 32):
        norma = prodotto(1, 1, n)
        peggio = max(abs(prodotto(a, b, n))
                     for a in range(n) for b in range(a + 1, n))
        limite = 0 if n == 4 else norma // 100
        dire(peggio <= limite,
             "%d punti: righe quasi ortogonali (%d su %d)"
             % (n, peggio, norma))
    # Even rows read the same backwards, odd rows read the same negated:
    # the basis functions are symmetric and antisymmetric in turn.
    specchio = [k for k in range(32)
                if any(dct[k][31 - i] != (dct[k][i] if k % 2 == 0
                                          else -dct[k][i]) for i in range(32))]
    dire(not specchio, "pari a specchio e dispari a specchio col segno (%s)"
         % specchio)

    print("livelli di quantizzazione")
    ls = leggi(c, "hevcd_level_scale")
    dire(ls == [40, 45, 51, 57, 64, 72], "levelScale come 8.6.3 (%s)" % ls)

    print()
    if problemi:
        print("%d controlli falliti:" % len(problemi))
        for p in problemi:
            print("  - %s" % p)
        return 1
    print("tutto a posto")
    return 0


if __name__ == "__main__":
    sys.exit(main())
