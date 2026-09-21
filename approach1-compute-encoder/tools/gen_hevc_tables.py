#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate src/hevc_dec_tables.{c,h} from the normative H.265 tables.

Same reasoning as gen_h264_tables.py, and the same provenance. These are
the constant tables of Rec. ITU-T H.265: the CABAC context initialisation
values of clause 9.3.2.2, the coefficient scans of 6.5.3, the
interpolation filters of Tables 8-10 and 8-11, the deblocking thresholds
of Table 8-12 and the chroma QP mapping of Table 8-10. They are facts of
the standard, so the only thing that matters about them is that every
single number is right.

Typing several thousand numbers by hand would guarantee a wrong one
somewhere, and a wrong CABAC init value does not crash - it decodes into a
picture that looks almost right and drifts. So they are extracted
mechanically from FFmpeg's HEVC decoder (LGPL-2.1-or-later, used here
under GPL-3.0-only as LGPL-2.1 clause 3 permits) and this script stays in
the tree so anyone can re-run it and diff the result.

    python3 tools/gen_hevc_tables.py <reference dir> <output src dir>

The reference directory holds cabac.c, data.c, filter.c, dsp.c and
pred_template.c as fetched from a tagged FFmpeg release; fetch_hevc.sh
next to this file downloads them.

⚠️ The context states are stored the way the decoding engine wants them,
not the way the standard writes them: pStateIdx counted from the other
end, packed with valMPS into one byte. That is the same convention the
H.264 engine uses, which is what lets both share one arithmetic decoder.
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
    """The balanced-brace initialiser of the array called `nome`."""
    i = testo.find(nome)
    if i < 0:
        raise SystemExit("non trovo la tabella %s" % nome)
    i = testo.find("{", i)
    if i < 0:
        raise SystemExit("%s: non trovo la graffa" % nome)
    liv = 0
    for j in range(i, len(testo)):
        if testo[j] == "{":
            liv += 1
        elif testo[j] == "}":
            liv -= 1
            if liv == 0:
                return testo[i:j + 1]
    raise SystemExit("%s: graffe non bilanciate" % nome)


def numeri(testo):
    """Every integer in a flat initialiser, in order."""
    return [int(x) for x in re.findall(r"-?\d+", testo)]


def righe(testo):
    """A ragged initialiser, row by row: what is inside each inner brace."""
    dentro = testo.strip()
    assert dentro.startswith("{") and dentro.endswith("}")
    dentro = dentro[1:-1]
    fuori, liv, pezzo = [], 0, []
    for ch in dentro:
        if ch == "{":
            liv += 1
            if liv == 1:
                pezzo = []
                continue
        elif ch == "}":
            liv -= 1
            if liv == 0:
                fuori.append(numeri("".join(pezzo)))
                continue
        if liv >= 1:
            pezzo.append(ch)
    return fuori


# ------------------------------------------------------- the context list

def elementi(cabac):
    """The syntax elements and how many contexts each has, from the
    CABAC_ELEMS macro: the offsets are a running total of those."""
    i = cabac.find("#define CABAC_ELEMS(ELEM)")
    if i < 0:
        raise SystemExit("non trovo CABAC_ELEMS")
    j = cabac.find("/**", i)
    corpo = cabac[i:j]
    fuori = []
    for nome, quanti in re.findall(r"ELEM\((\w+),\s*(\d+)\)", corpo):
        fuori.append((nome, int(quanti)))
    if not fuori:
        raise SystemExit("CABAC_ELEMS vuoto")
    return fuori


def stati(init_value, qp):
    """Clause 9.3.2.2, then packed the way the engine reads it."""
    m = (init_value >> 4) * 5 - 45
    n = ((init_value & 15) << 3) - 16
    s = ((m * qp) >> 4) + n
    s = 1 if s < 1 else (126 if s > 126 else s)
    p = s if s < 127 - s else 127 - s
    return (p << 1) | (1 if s >= 64 else 0)


# ------------------------------------------------------------- emitting

def riempi(mat, larghezza):
    """Pad every row to `larghezza`.

    C fills the rest of a short initialiser with zeros, and FFmpeg writes
    the unfiltered position of both interpolation filters as a bare
    `{ 0 }`. Reading the rows back without padding them makes the array one
    column wide and shifts everything that follows.
    """
    return [(r + [0] * larghezza)[:larghezza] for r in mat]


def uno(nome, tipo, valori, per_riga=16):
    fuori = ["const %s %s[%d] = {" % (tipo, nome, len(valori))]
    for i in range(0, len(valori), per_riga):
        fuori.append("    " + ", ".join("%d" % v for v in valori[i:i + per_riga]) + ",")
    fuori.append("};")
    return "\n".join(fuori)


def due(nome, tipo, mat, per_riga=16):
    fuori = ["const %s %s[%d][%d] = {" % (tipo, nome, len(mat), len(mat[0]))]
    for r in mat:
        if len(r) <= per_riga:
            fuori.append("    { " + ", ".join("%d" % v for v in r) + " },")
        else:
            fuori.append("    {")
            for i in range(0, len(r), per_riga):
                fuori.append("        " + ", ".join("%d" % v for v in r[i:i + per_riga]) + ",")
            fuori.append("    },")
    fuori.append("};")
    return "\n".join(fuori)


INTESTAZIONE = """/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * %s - generated by tools/gen_hevc_tables.py; do not edit.
 *
 * The normative constant tables of Rec. ITU-T H.265, extracted from FFmpeg
 * %s (LGPL-2.1-or-later, used here under GPL-3.0-only as clause 3 of the
 * LGPL permits). Re-run the generator to check any of them.
 */
"""


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    rif = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2])

    cabac = spoglia((rif / "cabac.c").read_text(encoding="utf-8", errors="replace"))
    # ⚠️ The initialisation table writes 154 as CNU, "context not used".
    # It is a number like any other once the macro is gone, and without
    # this a fifth of the table quietly goes missing.
    cnu = re.search(r"#define\s+CNU\s+(\d+)", cabac)
    if not cnu:
        raise SystemExit("non trovo la definizione di CNU")
    cabac = re.sub(r"\bCNU\b", cnu.group(1), cabac)
    cabac_raw = (rif / "cabac.c").read_text(encoding="utf-8", errors="replace")
    data = spoglia((rif / "data.c").read_text(encoding="utf-8", errors="replace"))
    filt = spoglia((rif / "filter.c").read_text(encoding="utf-8", errors="replace"))
    dsp = spoglia((rif / "dsp.c").read_text(encoding="utf-8", errors="replace"))
    pred = spoglia((rif / "pred_template.c").read_text(encoding="utf-8", errors="replace"))

    # --- the contexts ------------------------------------------------
    elenco = elementi(cabac_raw)
    offset, tot = [], 0
    for nome, quanti in elenco:
        offset.append((nome, tot))
        tot += quanti
    # ⚠️ An element with no contexts of its own - one that is coded in
    # bypass - does not advance the offset. FFmpeg's enum spells that as
    # END = OFFSET + NUM_BINS - 1, so with NUM_BINS zero the next element
    # starts where this one did, and several of them share an index.
    init = righe(blocco(cabac, "init_values[3][HEVC_CONTEXTS]"))
    n_ctx = len(init[0])
    if tot != n_ctx:
        raise SystemExit("offset %d contro %d contesti" % (tot, n_ctx))
    for r in init:
        if len(r) != n_ctx:
            raise SystemExit("init_values: righe di lunghezza diversa")

    # Every state, for every QP: the decoder indexes this directly instead
    # of doing the arithmetic at every slice header.
    tabella = [[stati(init[t][i], qp) for i in range(n_ctx)]
               for t in range(3) for qp in range(52)]

    # --- everything else ---------------------------------------------
    def piatta(testo, nome):
        return numeri(blocco(testo, nome))

    diag4_x = piatta(data, "ff_hevc_diag_scan4x4_x")
    diag4_y = piatta(data, "ff_hevc_diag_scan4x4_y")
    diag8_x = piatta(data, "ff_hevc_diag_scan8x8_x")
    diag8_y = piatta(data, "ff_hevc_diag_scan8x8_y")

    diag2_x = piatta(cabac, "diag_scan2x2_x")
    diag2_y = piatta(cabac, "diag_scan2x2_y")
    horiz2_x = piatta(cabac, "horiz_scan2x2_x")
    horiz2_y = piatta(cabac, "horiz_scan2x2_y")
    horiz4_x = piatta(cabac, "horiz_scan4x4_x")
    horiz4_y = piatta(cabac, "horiz_scan4x4_y")

    diag2_inv = righe(blocco(cabac, "diag_scan2x2_inv[2][2]"))
    diag4_inv = righe(blocco(cabac, "diag_scan4x4_inv[4][4]"))
    diag8_inv = righe(blocco(cabac, "diag_scan8x8_inv[8][8]"))
    horiz8_inv = righe(blocco(cabac, "horiz_scan8x8_inv[8][8]"))

    level_scale = piatta(cabac, "level_scale[]")
    ctx_idx_map = piatta(cabac, "ctx_idx_map[]")

    tc = piatta(filt, "tctable[54]")
    beta = piatta(filt, "betatable[52]")
    sao_tab = piatta(filt, "sao_tab[8]")
    qp_c = piatta(filt, "qp_c[]")

    epel = righe(blocco(dsp, "ff_hevc_epel_filters)[8][4]"))
    qpel = righe(blocco(dsp, "ff_hevc_qpel_filters)[4][16]"))

    angolo = piatta(pred, "intra_pred_angle[]")
    inv_angolo = piatta(pred, "inv_angle[]")

    # --- the header ---------------------------------------------------
    h = [INTESTAZIONE % ("hevc_dec_tables.h", TAG),
         "#ifndef BC250_HEVC_DEC_TABLES_H",
         "#define BC250_HEVC_DEC_TABLES_H", "",
         "#include <stdint.h>", "",
         "#define HEVCD_CTX %d" % n_ctx, "",
         "/* Where each syntax element's contexts start, clause 9.3.4.2. */",
         "enum {"]
    for nome, o in offset:
        h.append("    HEVCD_CTX_%s = %d," % (nome, o))
    h.append("};")
    h.append("")
    h.append("/* Every context's initial state, by [initType * 52 + SliceQpY].")
    h.append(" * Packed as the engine reads it: (pStateIdx << 1) | valMPS with")
    h.append(" * pStateIdx counted from the other end, the same convention the")
    h.append(" * H.264 engine uses. */")
    h.append("extern const uint8_t hevcd_ctx_init[3 * 52][HEVCD_CTX];")
    h.append("")
    for nome, v in (("hevcd_diag4_x", diag4_x), ("hevcd_diag4_y", diag4_y),
                    ("hevcd_diag8_x", diag8_x), ("hevcd_diag8_y", diag8_y),
                    ("hevcd_diag2_x", diag2_x), ("hevcd_diag2_y", diag2_y),
                    ("hevcd_horiz2_x", horiz2_x), ("hevcd_horiz2_y", horiz2_y),
                    ("hevcd_horiz4_x", horiz4_x), ("hevcd_horiz4_y", horiz4_y),
                    ("hevcd_level_scale", level_scale),
                    ("hevcd_ctx_idx_map", ctx_idx_map),
                    ("hevcd_tc", tc), ("hevcd_beta", beta),
                    ("hevcd_sao_tab", sao_tab), ("hevcd_qp_c", qp_c)):
        h.append("extern const uint8_t %s[%d];" % (nome, len(v)))
    for nome, m in (("hevcd_diag2_inv", diag2_inv), ("hevcd_diag4_inv", diag4_inv),
                    ("hevcd_diag8_inv", diag8_inv), ("hevcd_horiz8_inv", horiz8_inv)):
        h.append("extern const uint8_t %s[%d][%d];" % (nome, len(m), len(m[0])))
    h.append("extern const int8_t hevcd_epel[%d][%d];" % (len(epel), 4))
    h.append("extern const int8_t hevcd_qpel[%d][%d];" % (len(qpel), 8))
    h.append("extern const int16_t hevcd_intra_angle[%d];" % len(angolo))
    h.append("extern const int16_t hevcd_inv_angle[%d];" % len(inv_angolo))
    h.append("")
    h.append("#endif /* BC250_HEVC_DEC_TABLES_H */")

    # --- the source ---------------------------------------------------
    c = [INTESTAZIONE % ("hevc_dec_tables.c", TAG),
         '#include "hevc_dec_tables.h"', ""]
    c.append(due("hevcd_ctx_init", "uint8_t", tabella, 24))
    c.append("")
    for nome, v in (("hevcd_diag4_x", diag4_x), ("hevcd_diag4_y", diag4_y),
                    ("hevcd_diag8_x", diag8_x), ("hevcd_diag8_y", diag8_y),
                    ("hevcd_diag2_x", diag2_x), ("hevcd_diag2_y", diag2_y),
                    ("hevcd_horiz2_x", horiz2_x), ("hevcd_horiz2_y", horiz2_y),
                    ("hevcd_horiz4_x", horiz4_x), ("hevcd_horiz4_y", horiz4_y),
                    ("hevcd_level_scale", level_scale),
                    ("hevcd_ctx_idx_map", ctx_idx_map),
                    ("hevcd_tc", tc), ("hevcd_beta", beta),
                    ("hevcd_sao_tab", sao_tab), ("hevcd_qp_c", qp_c)):
        c.append(uno(nome, "uint8_t", v))
        c.append("")
    for nome, m in (("hevcd_diag2_inv", diag2_inv), ("hevcd_diag4_inv", diag4_inv),
                    ("hevcd_diag8_inv", diag8_inv), ("hevcd_horiz8_inv", horiz8_inv)):
        c.append(due(nome, "uint8_t", m))
        c.append("")
    # ⚠️ FFmpeg keeps the eight-tap filters in rows of sixteen, the taps
    # followed by eight zeros for its own SIMD. Only the taps are the
    # table, and the unfiltered row is written as a bare zero.
    c.append(due("hevcd_qpel", "int8_t", riempi(qpel, 8), 8))
    c.append("")
    c.append(due("hevcd_epel", "int8_t", riempi(epel, 4), 4))
    c.append("")
    c.append(uno("hevcd_intra_angle", "int16_t", angolo))
    c.append("")
    c.append(uno("hevcd_inv_angle", "int16_t", inv_angolo))

    (out / "hevc_dec_tables.h").write_text("\n".join(h) + "\n",
                                           encoding="utf-8", newline="")
    (out / "hevc_dec_tables.c").write_text("\n".join(c) + "\n",
                                           encoding="utf-8", newline="")
    print("%d contesti, %d elementi di sintassi" % (n_ctx, len(elenco)))
    print("scritti hevc_dec_tables.c e .h in %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
