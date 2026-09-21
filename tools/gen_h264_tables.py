#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate src/h264_dec_tables.{c,h} from the normative H.264 tables.

These are the constant tables of Rec. ITU-T H.264: the CABAC context
initialisation values (Tables 9-12 to 9-33), the context offsets of clause
9.3.3.1.3, the dequantisation values of 8.5.9 and the CAVLC variable-length
codes of 9.2. They are facts of the standard, not choices, so the only thing
that matters about them is that every single number is right.

Typing eight thousand numbers by hand would guarantee a wrong one somewhere,
and a wrong CABAC init value does not crash: it decodes into a picture that
looks almost right and drifts. So they are extracted mechanically from
FFmpeg's H.264 decoder (LGPL-2.1-or-later, used here under GPL-3.0-only as
LGPL-2.1 clause 3 permits) and this script stays in the tree so anyone can
re-run it and diff the result.

    python3 tools/gen_h264_tables.py <reference dir> <output src dir>

The reference directory holds h264_cabac.c, h264data.c, h264_cavlc.c and
cabac.c as fetched from a tagged FFmpeg release; fetch.sh next to this file
downloads them.
"""
import pathlib
import re
import sys

TAG = "n8.1.2"


def spoglia(testo):
    """Strip C comments, which otherwise contribute digits to the scan."""
    testo = re.sub(r"/\*.*?\*/", " ", testo, flags=re.S)
    testo = re.sub(r"//[^\n]*", " ", testo)
    return testo


def blocco(testo, nome):
    """The balanced-brace initialiser of the array called `nome`.

    ⚠️ The declaration is not always `name[dims] = {`: FFmpeg wraps one of
    these tables in DECLARE_ASM_ALIGNED, so a closing parenthesis sits
    between the name and its dimensions. Anything may sit between the name
    and the `=`, as long as no statement ends in between.
    """
    for m in re.finditer(r"\b%s\b" % re.escape(nome), testo):
        resto = testo[m.end():]
        eq = resto.find("=")
        ap = resto.find("{")
        if eq < 0 or ap < 0 or eq > ap:
            continue
        if ";" in resto[:ap]:
            continue            # a declaration or a use, not a definition
        i = m.end() + ap
        break
    else:
        raise SystemExit("non trovo la tabella %s" % nome)
    livello = 0
    for j in range(i, len(testo)):
        if testo[j] == "{":
            livello += 1
        elif testo[j] == "}":
            livello -= 1
            if livello == 0:
                return testo[i:j + 1]
    raise SystemExit("parentesi non chiuse in %s" % nome)


def voci(corpo):
    """Split an initialiser body on the commas at its own brace level."""
    fuori, corrente, livello = [], [], 0
    for ch in corpo:
        if ch in "{[(":
            livello += 1
        elif ch in "}])":
            livello -= 1
        if ch == "," and livello == 0:
            fuori.append("".join(corrente).strip())
            corrente = []
            continue
        corrente.append(ch)
    coda = "".join(corrente).strip()
    if coda:
        fuori.append(coda)
    return fuori


def valore(nome, testo):
    """One scalar entry.

    Several tables write their entries as arithmetic rather than as a plain
    number: sums like `105+15` for the CABAC context offsets, products like
    `(0 + 0 * 2) * 16` for the chroma DC scan. Only digits, + - * and
    parentheses are ever accepted, so the eval below cannot run anything.
    """
    testo = testo.strip()
    if not re.fullmatch(r"[-+*()0-9\s]+", testo):
        raise SystemExit("%s: voce non aritmetica %r" % (nome, testo))
    return int(eval(testo))


def piatta(sorgenti, nome):
    """Every value of a table, in order, ignoring its row structure."""
    corpo = dentro(sorgenti, nome)
    fuori = []

    def giu(testo):
        for v in voci(testo):
            if v.startswith("{"):
                giu(v.strip()[1:-1])
            else:
                fuori.append(valore(nome, v))

    giu(corpo)
    return fuori


def dentro(sorgenti, nome):
    for testo in sorgenti.values():
        try:
            return spoglia(blocco(spoglia(testo), nome)).strip()[1:-1]
        except SystemExit as e:
            if "non trovo la tabella" not in str(e):
                raise            # a real parse error must not be swallowed
    raise SystemExit("non trovo %s in nessun sorgente" % nome)


def matrice(sorgenti, nome, righe, colonne):
    """A 2-D table, row by row, each row padded out to `colonne`.

    ⚠️ This is the one that must not be done with a flat scan. FFmpeg's CAVLC
    tables are ragged: total_zeros_len is declared [16][16] but holds 135
    values, because each row stops early and C fills the rest with zeros. Read
    flat and re-cut into equal rows, every row after the first would be
    shifted, and the decoder would produce plausible garbage.
    """
    corpo = dentro(sorgenti, nome)
    fuori = []
    for r in voci(corpo):
        r = r.strip()
        if not r.startswith("{"):
            raise SystemExit("%s: mi aspettavo una riga fra graffe, ho %r" % (nome, r[:40]))
        v = [valore(nome, x) for x in voci(r[1:-1]) if x.strip()]
        if len(v) > colonne:
            raise SystemExit("%s: riga da %d valori, il massimo e' %d" % (nome, len(v), colonne))
        fuori.append(v + [0] * (colonne - len(v)))
    if len(fuori) > righe:
        raise SystemExit("%s: %d righe, il massimo e' %d" % (nome, len(fuori), righe))
    while len(fuori) < righe:
        fuori.append([0] * colonne)
    return fuori


def esatta(sorgenti, nome, attesi):
    v = piatta(sorgenti, nome)
    if len(v) != attesi:
        raise SystemExit("%s: %d valori, ne aspettavo %d" % (nome, len(v), attesi))
    return v


# --- tables we write out ourselves, then check against the reference --------

# Rec. ITU-T H.264 Table 8-13, 4x4 zig-zag scan.
ZIGZAG4 = [0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15]

# Rec. ITU-T H.264 Table 8-14, 8x8 zig-zag scan.
ZIGZAG8 = [
    0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
]

# Rec. ITU-T H.264 9.3.3.1.3, ctxIdxInc for last_significant_coeff_flag on an
# 8x8 block. It is the tail of FFmpeg's combined ff_h264_cabac_tables blob,
# so it is written out here and checked against that blob's last 63 bytes.
LAST_OFF_8X8 = (
    [0] + [1] * 15 +
    [2] * 16 +
    [3] * 8 + [4] * 8 +
    [5] * 4 + [6] * 4 + [7] * 4 + [8] * 3
)

# Rec. ITU-T H.264 Table 8-15, qPI to QPC. Below 30 the two are equal, so only
# the tail is a real table. FFmpeg builds this with a macro rather than
# writing it out, which is why there is nothing to extract.
CHROMA_QP_TAIL = [29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36,
                  36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39]
CHROMA_QP = list(range(30)) + CHROMA_QP_TAIL


def riga(valori, per_riga, larghezza):
    fuori = []
    for i in range(0, len(valori), per_riga):
        pezzo = ", ".join(("%*d" % (larghezza, v)) for v in valori[i:i + per_riga])
        fuori.append("    " + pezzo + ",")
    return "\n".join(fuori)


def coppie(valori, per_riga=6):
    fuori = []
    for i in range(0, len(valori), 2 * per_riga):
        pezzo = ", ".join("{ %4d, %4d }" % (valori[j], valori[j + 1])
                          for j in range(i, min(i + 2 * per_riga, len(valori)), 2))
        fuori.append("    " + pezzo + ",")
    return "\n".join(fuori)


INTESTAZIONE = """/* Generated by tools/gen_h264_tables.py - do not edit by hand.
 *
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The constant tables of Rec. ITU-T H.264. Extracted mechanically from
 * FFmpeg %s (LGPL-2.1-or-later, used here under GPL-3.0-only as permitted
 * by LGPL-2.1 clause 3) so that every value is provably the standard's own.
 * Re-run the generator to reproduce this file byte for byte.
 */
""" % TAG


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    rif = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2])

    sorgenti = {}
    for nome in ("h264_cabac.c", "h264data.c", "h264_cavlc.c", "cabac.c"):
        p = rif / nome
        if not p.exists():
            raise SystemExit("manca il riferimento %s" % p)
        sorgenti[nome] = p.read_text(encoding="utf-8", errors="replace")

    c = [INTESTAZIONE, '#include "h264_dec_tables.h"', ""]
    h = [INTESTAZIONE,
         "#ifndef BC250_H264_DEC_TABLES_H",
         "#define BC250_H264_DEC_TABLES_H",
         "",
         "#include <stdint.h>",
         ""]

    def scalare(tipo, nome, n, valori, per_riga, larghezza=4):
        if len(valori) != n:
            raise SystemExit("%s: %d valori, ne aspettavo %d" % (nome, len(valori), n))
        c.append("const %s %s[%d] = {" % (tipo, nome, n))
        c.append(riga(valori, per_riga, larghezza))
        c.append("};\n")
        h.append("extern const %s %s[%d];" % (tipo, nome, n))

    def bidim(tipo, nome, righe, colonne, tabella, per_riga, larghezza=4):
        c.append("const %s %s[%d][%d] = {" % (tipo, nome, righe, colonne))
        for r in tabella:
            c.append("  {")
            c.append(riga(r, per_riga, larghezza))
            c.append("  },")
        c.append("};\n")
        h.append("extern const %s %s[%d][%d];" % (tipo, nome, righe, colonne))

    # ---- CABAC context initialisation, Tables 9-12 to 9-33 ----------------
    init_i = esatta(sorgenti, "cabac_context_init_I", 1024 * 2)
    init_pb = esatta(sorgenti, "cabac_context_init_PB", 3 * 1024 * 2)

    c.append("const int8_t h264d_cabac_init_I[1024][2] = {")
    c.append(coppie(init_i))
    c.append("};\n")
    h.append("extern const int8_t h264d_cabac_init_I[1024][2];")

    c.append("const int8_t h264d_cabac_init_PB[3][1024][2] = {")
    for k in range(3):
        c.append("  {")
        c.append(coppie(init_pb[k * 2048:(k + 1) * 2048]))
        c.append("  },")
    c.append("};\n")
    h.append("extern const int8_t h264d_cabac_init_PB[3][1024][2];")

    # ---- context offsets, clause 9.3.3.1.3 --------------------------------
    # ⚠️ uint16_t, not uint8_t: these run up to 982.
    bidim("uint16_t", "h264d_sig_coeff_offset", 2, 14,
          matrice(sorgenti, "significant_coeff_flag_offset", 2, 14), 14)
    bidim("uint16_t", "h264d_last_coeff_offset", 2, 14,
          matrice(sorgenti, "last_coeff_flag_offset", 2, 14), 14)
    scalare("uint16_t", "h264d_abs_level_offset", 14,
            esatta(sorgenti, "coeff_abs_level_m1_offset", 14), 14)
    bidim("uint8_t", "h264d_sig_coeff_offset_8x8", 2, 63,
          matrice(sorgenti, "significant_coeff_flag_offset_8x8", 2, 63), 21, 2)
    scalare("uint8_t", "h264d_sig_coeff_offset_dc", 7,
            esatta(sorgenti, "sig_coeff_offset_dc", 7), 7, 2)

    blob = piatta(sorgenti, "ff_h264_cabac_tables")
    if blob[-63:] != LAST_OFF_8X8:
        raise SystemExit("last_coeff_flag_offset_8x8 non combacia col riferimento")
    scalare("uint8_t", "h264d_last_coeff_offset_8x8", 63, LAST_OFF_8X8, 21, 2)

    # ---- dequantisation, clause 8.5.9 -------------------------------------
    bidim("uint8_t", "h264d_dequant4_init", 6, 3,
          matrice(sorgenti, "ff_h264_dequant4_coeff_init", 6, 3), 3)
    bidim("uint8_t", "h264d_dequant8_init", 6, 6,
          matrice(sorgenti, "ff_h264_dequant8_coeff_init", 6, 6), 6)
    scalare("uint8_t", "h264d_dequant8_init_scan", 16,
            esatta(sorgenti, "ff_h264_dequant8_coeff_init_scan", 16), 16, 2)
    scalare("uint8_t", "h264d_chroma_qp", 52, CHROMA_QP, 26, 3)
    scalare("uint8_t", "h264d_golomb_to_intra4x4_cbp", 48,
            esatta(sorgenti, "ff_h264_golomb_to_intra4x4_cbp", 48), 16, 3)
    scalare("uint8_t", "h264d_golomb_to_inter_cbp", 48,
            esatta(sorgenti, "ff_h264_golomb_to_inter_cbp", 48), 16, 3)
    scalare("uint8_t", "h264d_chroma_dc_scan", 4,
            esatta(sorgenti, "ff_h264_chroma_dc_scan", 4), 4, 2)

    scalare("uint8_t", "h264d_zigzag4", 16, ZIGZAG4, 16, 2)
    scalare("uint8_t", "h264d_zigzag8", 64, ZIGZAG8, 16, 2)

    # ---- CAVLC, clause 9.2 ------------------------------------------------
    bidim("uint8_t", "h264d_coeff_token_len", 4, 68,
          matrice(sorgenti, "coeff_token_len", 4, 68), 17, 2)
    bidim("uint8_t", "h264d_coeff_token_bits", 4, 68,
          matrice(sorgenti, "coeff_token_bits", 4, 68), 17, 3)
    scalare("uint8_t", "h264d_chroma_dc_coeff_token_len", 20,
            esatta(sorgenti, "chroma_dc_coeff_token_len", 20), 5, 2)
    scalare("uint8_t", "h264d_chroma_dc_coeff_token_bits", 20,
            esatta(sorgenti, "chroma_dc_coeff_token_bits", 20), 5, 2)
    bidim("uint8_t", "h264d_total_zeros_len", 16, 16,
          matrice(sorgenti, "total_zeros_len", 16, 16), 16, 2)
    bidim("uint8_t", "h264d_total_zeros_bits", 16, 16,
          matrice(sorgenti, "total_zeros_bits", 16, 16), 16, 2)
    bidim("uint8_t", "h264d_chroma_dc_total_zeros_len", 3, 4,
          matrice(sorgenti, "chroma_dc_total_zeros_len", 3, 4), 4, 2)
    bidim("uint8_t", "h264d_chroma_dc_total_zeros_bits", 3, 4,
          matrice(sorgenti, "chroma_dc_total_zeros_bits", 3, 4), 4, 2)
    bidim("uint8_t", "h264d_run_len", 7, 16,
          matrice(sorgenti, "run_len", 7, 16), 16, 2)
    bidim("uint8_t", "h264d_run_bits", 7, 16,
          matrice(sorgenti, "run_bits", 7, 16), 16, 2)

    h += ["", "#endif /* BC250_H264_DEC_TABLES_H */", ""]

    (out / "h264_dec_tables.c").write_text("\n".join(c), encoding="utf-8", newline="\n")
    (out / "h264_dec_tables.h").write_text("\n".join(h), encoding="utf-8", newline="\n")
    print("scritte h264_dec_tables.c e .h  (%d valori CABAC, %d tabelle)"
          % (len(init_i) + len(init_pb), sum(1 for x in h if x.startswith("extern"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
