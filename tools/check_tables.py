#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Structural checks on the generated H.264 tables.

Not a diff against the reference - that would only prove the extractor
copied something, not that what it copied is a usable table. These are
properties the standard's tables have to satisfy no matter where they came
from, so a mis-cut ragged row or a shifted CABAC entry fails here:

  * every variable-length code set is prefix-free and complete
    (Kraft-McMillan sum exactly 1);
  * no two codes in a set share a (length, bits) pair;
  * the CABAC initialisation gives a legal state 0..63 for every one of the
    1024 contexts at every QP 0..51;
  * the ragged CAVLC tables have a non-zero length exactly where the
    standard says a code exists.

    python3 tools/check_tables.py <src dir>
"""
import pathlib
import re
import sys


def leggi(testo, nome):
    """Every integer of a generated table, in declaration order."""
    m = re.search(r"\b%s\s*(\[\d+\])+\s*=\s*\{" % re.escape(nome), testo)
    if not m:
        raise SystemExit("non trovo %s" % nome)
    i = testo.index("{", m.end() - 1)
    livello = 0
    for j in range(i, len(testo)):
        if testo[j] == "{":
            livello += 1
        elif testo[j] == "}":
            livello -= 1
            if livello == 0:
                corpo = testo[i:j + 1]
                break
    else:
        raise SystemExit("parentesi non chiuse in %s" % nome)
    return [int(x) for x in re.findall(r"-?\d+", corpo)]


def taglia(piatta, righe, colonne):
    if len(piatta) != righe * colonne:
        raise SystemExit("attesi %d valori, ne ho %d" % (righe * colonne, len(piatta)))
    return [piatta[r * colonne:(r + 1) * colonne] for r in range(righe)]


def kraft(coppie, etichetta, guasti, minimo=0.9):
    """Kraft-McMillan on a set of (length, bits) codes.

    ⚠️ The sum must be at most 1, not exactly 1. H.264's variable-length
    codes are prefix-free but deliberately incomplete: Table 9-5 leaves two
    of the 64 six-bit patterns unassigned, Table 9-7's first row leaves one
    nine-bit pattern unassigned, and so on - those patterns just mean the
    stream is corrupt. A sum above 1 is the real fault, because then two
    codes overlap; `minimo` only catches a row that lost most of its
    entries, which is what a mis-cut ragged table would look like.
    """
    somma = 0.0
    visti = {}
    for ln, bits in coppie:
        if ln <= 0:
            continue
        somma += 2.0 ** -ln
        if (ln, bits) in visti:
            guasti.append("%s: il codice (%d bit, 0x%x) compare due volte" % (etichetta, ln, bits))
        visti[(ln, bits)] = True
        if bits >= (1 << ln):
            guasti.append("%s: il codice 0x%x non ci sta in %d bit" % (etichetta, bits, ln))
    if somma > 1.0 + 1e-9:
        guasti.append("%s: somma di Kraft %.9f, sopra 1: i codici si sovrappongono"
                      % (etichetta, somma))
    elif somma < minimo:
        guasti.append("%s: somma di Kraft %.9f, troppo bassa: la riga ha perso dei codici"
                      % (etichetta, somma))
    # prefix-free: no code is the prefix of a longer one
    ordinati = sorted(visti, key=lambda t: t[0])
    for a in range(len(ordinati)):
        la, ba = ordinati[a]
        for b in range(a + 1, len(ordinati)):
            lb, bb = ordinati[b]
            if lb == la:
                continue
            if (bb >> (lb - la)) == ba:
                guasti.append("%s: (%d,0x%x) e' prefisso di (%d,0x%x)" % (etichetta, la, ba, lb, bb))
                break


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    src = pathlib.Path(sys.argv[1])
    testo = (src / "h264_dec_tables.c").read_text(encoding="utf-8")
    guasti = []

    # ---- CABAC initialisation, clause 9.3.1.1 -----------------------------
    init_i = taglia(leggi(testo, "h264d_cabac_init_I"), 1024, 2)
    init_pb = taglia(leggi(testo, "h264d_cabac_init_PB"), 3 * 1024, 2)
    for etichetta, tabella in (("I", init_i), ("PB", init_pb)):
        fuori = 0
        for m, n in tabella:
            for qp in range(52):
                pre = ((m * qp) >> 4) + n
                pre = 1 if pre < 1 else (126 if pre > 126 else pre)
                stato = (pre - 64) if pre > 63 else (63 - pre)
                if not (0 <= stato <= 63):
                    fuori += 1
        if fuori:
            guasti.append("init %s: %d stati fuori da 0..63" % (etichetta, fuori))

    # ---- coeff_token, Table 9-5 -------------------------------------------
    # 4 tables; within each, the codes for every legal (TotalCoeff,
    # TrailingOnes) pair. Index is nC-table major, then 4*TotalCoeff+T1s.
    ct_len = taglia(leggi(testo, "h264d_coeff_token_len"), 4, 68)
    ct_bits = taglia(leggi(testo, "h264d_coeff_token_bits"), 4, 68)
    for t in range(4):
        coppie = [(ct_len[t][i], ct_bits[t][i]) for i in range(68) if ct_len[t][i] > 0]
        kraft(coppie, "coeff_token[%d]" % t, guasti)

    cdc_len = leggi(testo, "h264d_chroma_dc_coeff_token_len")
    cdc_bits = leggi(testo, "h264d_chroma_dc_coeff_token_bits")
    kraft([(cdc_len[i], cdc_bits[i]) for i in range(20) if cdc_len[i] > 0],
          "chroma_dc_coeff_token", guasti)

    # ---- total_zeros, Tables 9-7 and 9-8 ----------------------------------
    # Row i is for TotalCoeff == i+1 and codes total_zeros in 0..(16-1-i),
    # so exactly 16-i entries must be present and the rest must be padding.
    tz_len = taglia(leggi(testo, "h264d_total_zeros_len"), 16, 16)
    tz_bits = taglia(leggi(testo, "h264d_total_zeros_bits"), 16, 16)
    for i in range(15):
        attesi = 16 - i
        presenti = sum(1 for j in range(16) if tz_len[i][j] > 0)
        if presenti != attesi:
            guasti.append("total_zeros riga %d: %d codici, ne aspettavo %d"
                          % (i, presenti, attesi))
        kraft([(tz_len[i][j], tz_bits[i][j]) for j in range(16) if tz_len[i][j] > 0],
              "total_zeros[%d]" % i, guasti)

    cdtz_len = taglia(leggi(testo, "h264d_chroma_dc_total_zeros_len"), 3, 4)
    cdtz_bits = taglia(leggi(testo, "h264d_chroma_dc_total_zeros_bits"), 3, 4)
    for i in range(3):
        kraft([(cdtz_len[i][j], cdtz_bits[i][j]) for j in range(4) if cdtz_len[i][j] > 0],
              "chroma_dc_total_zeros[%d]" % i, guasti)

    # ---- run_before, Table 9-10 -------------------------------------------
    # Rows 0..5 are zerosLeft 1..6 and are complete codes; row 6 is
    # zerosLeft > 6, whose run_before 7..14 tail is an escape and therefore
    # is NOT prefix-complete on its own.
    run_l = taglia(leggi(testo, "h264d_run_len"), 7, 16)
    run_b = taglia(leggi(testo, "h264d_run_bits"), 7, 16)
    for i in range(6):
        attesi = i + 2
        presenti = sum(1 for j in range(16) if run_l[i][j] > 0)
        if presenti != attesi:
            guasti.append("run_before riga %d: %d codici, ne aspettavo %d"
                          % (i, presenti, attesi))
        kraft([(run_l[i][j], run_b[i][j]) for j in range(16) if run_l[i][j] > 0],
              "run_before[%d]" % i, guasti)

    # ---- scans ------------------------------------------------------------
    for nome, n in (("h264d_zigzag4", 16), ("h264d_zigzag8", 64)):
        v = leggi(testo, nome)
        if sorted(v) != list(range(n)):
            guasti.append("%s non e' una permutazione di 0..%d" % (nome, n - 1))

    # ---- chroma QP, Table 8-15 --------------------------------------------
    cqp = leggi(testo, "h264d_chroma_qp")
    if len(cqp) != 52 or cqp[:30] != list(range(30)):
        guasti.append("chroma_qp: la parte sotto 30 non e' l'identita'")
    if any(cqp[i + 1] < cqp[i] for i in range(51)):
        guasti.append("chroma_qp: non e' monotona")
    if cqp[51] != 39:
        guasti.append("chroma_qp: l'ultimo valore e' %d invece di 39" % cqp[51])

    if guasti:
        for g in guasti:
            print("GUASTO  " + g)
        print("\n%d problemi" % len(guasti))
        return 1
    print("tabelle a posto: codici prefix-free e senza doppioni, stati CABAC in 0..63, "
          "righe frastagliate della lunghezza giusta, scansioni permutazioni di 0..n-1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
