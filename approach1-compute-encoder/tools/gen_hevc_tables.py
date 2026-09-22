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


def strip(text):
    """Strip C comments, which otherwise contribute digits to the scan."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def block(text, nome):
    """The balanced-brace initialiser of the array called `name`."""
    i = text.find(nome)
    if i < 0:
        raise SystemExit("non found la table %s" % nome)
    i = text.find("{", i)
    if i < 0:
        raise SystemExit("%s: non found la graffa" % nome)
    liv = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            liv += 1
        elif text[j] == "}":
            liv -= 1
            if liv == 0:
                return text[i:j + 1]
    raise SystemExit("%s: braces non bilanciate" % nome)


def numbers(text):
    """Every integer in a flat initialiser, in order."""
    return [int(x) for x in re.findall(r"-?\d+", text)]


def lines(text):
    """A ragged initialiser, row by row: what is inside each inner brace."""
    inside = text.strip()
    assert inside.startswith("{") and inside.endswith("}")
    inside = inside[1:-1]
    out_values, liv, chunk = [], 0, []
    for ch in inside:
        if ch == "{":
            liv += 1
            if liv == 1:
                chunk = []
                continue
        elif ch == "}":
            liv -= 1
            if liv == 0:
                out_values.append(numbers("".join(chunk)))
                continue
        if liv >= 1:
            chunk.append(ch)
    return out_values


# ------------------------------------------------------- the context list

def elements(cabac):
    """The syntax elements and how many contexts each has, from the
    CABAC_ELEMS macro: the offsets are a running total of those."""
    i = cabac.find("#define CABAC_ELEMS(ELEM)")
    if i < 0:
        raise SystemExit("non found CABAC_ELEMS")
    j = cabac.find("/**", i)
    body = cabac[i:j]
    out_values = []
    for nome, howmany in re.findall(r"ELEM\((\w+),\s*(\d+)\)", body):
        out_values.append((nome, int(howmany)))
    if not out_values:
        raise SystemExit("CABAC_ELEMS empty")
    return out_values


def states(init_value, qp):
    """Clause 9.3.2.2, then packed the way the engine reads it."""
    m = (init_value >> 4) * 5 - 45
    n = ((init_value & 15) << 3) - 16
    s = ((m * qp) >> 4) + n
    s = 1 if s < 1 else (126 if s > 126 else s)
    p = s if s < 127 - s else 127 - s
    return (p << 1) | (1 if s >= 64 else 0)


# ------------------------------------------------------------- emitting

def fill(mat, width):
    """Pad every row to `width`.

    C fills the rest of a short initialiser with zeros, and FFmpeg writes
    the unfiltered position of both interpolation filters as a bare
    `{ 0 }`. Reading the rows back without padding them makes the array one
    column wide and shifts everything that follows.
    """
    return [(r + [0] * width)[:width] for r in mat]


def uno(nome, kind, values, per_riga=16):
    out_values = ["const %s %s[%d] = {" % (kind, nome, len(values))]
    for i in range(0, len(values), per_riga):
        out_values.append("    " + ", ".join("%d" % v for v in values[i:i + per_riga]) + ",")
    out_values.append("};")
    return "\n".join(out_values)


def due(nome, kind, mat, per_riga=16):
    out_values = ["const %s %s[%d][%d] = {" % (kind, nome, len(mat), len(mat[0]))]
    for r in mat:
        if len(r) <= per_riga:
            out_values.append("    { " + ", ".join("%d" % v for v in r) + " },")
        else:
            out_values.append("    {")
            for i in range(0, len(r), per_riga):
                out_values.append("        " + ", ".join("%d" % v for v in r[i:i + per_riga]) + ",")
            out_values.append("    },")
    out_values.append("};")
    return "\n".join(out_values)


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

    cabac = strip((rif / "cabac.c").read_text(encoding="utf-8", errors="replace"))
    # ⚠️ The initialisation table writes 154 as CNU, "context not used".
    # It is a number like any other once the macro is gone, and without
    # this a fifth of the table quietly goes missing.
    cnu = re.search(r"#define\s+CNU\s+(\d+)", cabac)
    if not cnu:
        raise SystemExit("non found la definition di CNU")
    cabac = re.sub(r"\bCNU\b", cnu.group(1), cabac)
    cabac_raw = (rif / "cabac.c").read_text(encoding="utf-8", errors="replace")
    data = strip((rif / "data.c").read_text(encoding="utf-8", errors="replace"))
    filt = strip((rif / "filter.c").read_text(encoding="utf-8", errors="replace"))
    dsp = strip((rif / "dsp.c").read_text(encoding="utf-8", errors="replace"))
    pred = strip((rif / "pred_template.c").read_text(encoding="utf-8", errors="replace"))

    # --- the contexts ------------------------------------------------
    listing = elements(cabac_raw)
    offset, tot = [], 0
    for nome, howmany in listing:
        offset.append((nome, tot))
        tot += howmany
    # ⚠️ An element with no contexts of its own - one that is coded in
    # bypass - does not advance the offset. FFmpeg's enum spells that as
    # END = OFFSET + NUM_BINS - 1, so with NUM_BINS zero the next element
    # starts where this one did, and several of them share an index.
    init = lines(block(cabac, "init_values[3][HEVC_CONTEXTS]"))
    n_ctx = len(init[0])
    if tot != n_ctx:
        raise SystemExit("offset %d against %d contexts" % (tot, n_ctx))
    for r in init:
        if len(r) != n_ctx:
            raise SystemExit("init_values: lines di length different")

    # Every state, for every QP: the decoder indexes this directly instead
    # of doing the arithmetic at every slice header.
    table = [[states(init[t][i], qp) for i in range(n_ctx)]
               for t in range(3) for qp in range(52)]

    # --- everything else ---------------------------------------------
    def flat(text, nome):
        return numbers(block(text, nome))

    diag4_x = flat(data, "ff_hevc_diag_scan4x4_x")
    diag4_y = flat(data, "ff_hevc_diag_scan4x4_y")
    diag8_x = flat(data, "ff_hevc_diag_scan8x8_x")
    diag8_y = flat(data, "ff_hevc_diag_scan8x8_y")

    diag2_x = flat(cabac, "diag_scan2x2_x")
    diag2_y = flat(cabac, "diag_scan2x2_y")
    horiz2_x = flat(cabac, "horiz_scan2x2_x")
    horiz2_y = flat(cabac, "horiz_scan2x2_y")
    horiz4_x = flat(cabac, "horiz_scan4x4_x")
    horiz4_y = flat(cabac, "horiz_scan4x4_y")

    diag2_inv = lines(block(cabac, "diag_scan2x2_inv[2][2]"))
    diag4_inv = lines(block(cabac, "diag_scan4x4_inv[4][4]"))
    diag8_inv = lines(block(cabac, "diag_scan8x8_inv[8][8]"))
    horiz8_inv = lines(block(cabac, "horiz_scan8x8_inv[8][8]"))

    level_scale = flat(cabac, "level_scale[]")
    ctx_idx_map = flat(cabac, "ctx_idx_map[]")

    tc = flat(filt, "tctable[54]")
    beta = flat(filt, "betatable[52]")
    sao_tab = flat(filt, "sao_tab[8]")
    qp_c = flat(filt, "qp_c[]")

    epel = lines(block(dsp, "ff_hevc_epel_filters)[8][4]"))
    qpel = lines(block(dsp, "ff_hevc_qpel_filters)[4][16]"))

    angle = flat(pred, "intra_pred_angle[]")
    inv_angle_values = flat(pred, "inv_angle[]")

    # The DCT-II matrix of clause 8.6.4.2. One 32x32 table holds all four
    # sizes: the 16-point transform is its even rows, the 8-point the even
    # rows of those, and so on, which is why the standard writes only one.
    dct = lines(block(dsp, "transform[32][32]"))

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
    h.append("extern const int8_t hevcd_dct[%d][%d];" % (len(dct), len(dct[0])))
    h.append("extern const int16_t hevcd_intra_angle[%d];" % len(angle))
    h.append("extern const int16_t hevcd_inv_angle[%d];" % len(inv_angle_values))
    h.append("")
    h.append("#endif /* BC250_HEVC_DEC_TABLES_H */")

    # --- the source ---------------------------------------------------
    c = [INTESTAZIONE % ("hevc_dec_tables.c", TAG),
         '#include "hevc_dec_tables.h"', ""]
    c.append(due("hevcd_ctx_init", "uint8_t", table, 24))
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
    c.append(due("hevcd_qpel", "int8_t", fill(qpel, 8), 8))
    c.append("")
    c.append(due("hevcd_epel", "int8_t", fill(epel, 4), 4))
    c.append("")
    c.append(due("hevcd_dct", "int8_t", dct, 16))
    c.append("")
    c.append(uno("hevcd_intra_angle", "int16_t", angle))
    c.append("")
    c.append(uno("hevcd_inv_angle", "int16_t", inv_angle_values))

    (out / "hevc_dec_tables.h").write_text("\n".join(h) + "\n",
                                           encoding="utf-8", newline="")
    (out / "hevc_dec_tables.c").write_text("\n".join(c) + "\n",
                                           encoding="utf-8", newline="")
    print("%d contexts, %d elements di syntax" % (n_ctx, len(listing)))
    print("written_files hevc_dec_tables.c e .h in %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
