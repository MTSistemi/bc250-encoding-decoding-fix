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


def strip(text):
    """Strip C comments, which otherwise contribute digits to the scan."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def block(text, nome):
    """The balanced-brace initialiser of the array called `nome`.

    ⚠️ The declaration is not always `name[dims] = {`: FFmpeg wraps one of
    these tables in DECLARE_ASM_ALIGNED, so a closing parenthesis sits
    between the name and its dimensions. Anything may sit between the name
    and the `=`, as long as no statement ends in between.
    """
    for m in re.finditer(r"\b%s\b" % re.escape(nome), text):
        rest = text[m.end():]
        eq = rest.find("=")
        ap = rest.find("{")
        if eq < 0 or ap < 0 or eq > ap:
            continue
        if ";" in rest[:ap]:
            continue            # a declaration or a use, not a definition
        i = m.end() + ap
        break
    else:
        raise SystemExit("non found la table %s" % nome)
    level = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            level += 1
        elif text[j] == "}":
            level -= 1
            if level == 0:
                return text[i:j + 1]
    raise SystemExit("unclosed brackets in %s" % nome)


def entries(body):
    """Split an initialiser body on the commas at its own brace level."""
    out_values, current, level = [], [], 0
    for ch in body:
        if ch in "{[(":
            level += 1
        elif ch in "}])":
            level -= 1
        if ch == "," and level == 0:
            out_values.append("".join(current).strip())
            current = []
            continue
        current.append(ch)
    coda = "".join(current).strip()
    if coda:
        out_values.append(coda)
    return out_values


def value(nome, text):
    """One scalar entry.

    Several tables write their entries as arithmetic rather than as a plain
    number: sums like `105+15` for the CABAC context offsets, products like
    `(0 + 0 * 2) * 16` for the chroma DC scan. Only digits, + - * and
    parentheses are ever accepted, so the eval below cannot run anything.
    """
    text = text.strip()
    if not re.fullmatch(r"[-+*()0-9\s]+", text):
        raise SystemExit("%s: entry non arithmetic %r" % (nome, text))
    return int(eval(text))


def flat(sources, nome):
    """Every value of a table, in order, ignoring its row structure."""
    body = inside(sources, nome)
    out_values = []

    def giu(text):
        for v in entries(text):
            if v.startswith("{"):
                giu(v.strip()[1:-1])
            else:
                out_values.append(value(nome, v))

    giu(body)
    return out_values


def inside(sources, nome):
    for text in sources.values():
        try:
            return strip(block(strip(text), nome)).strip()[1:-1]
        except SystemExit as e:
            if "non found la table" not in str(e):
                raise            # a real parse error must not be swallowed
    raise SystemExit("non found %s in no source" % nome)


def matrix(sources, nome, lines, columns):
    """A 2-D table, row by row, each row padded out to `columns`.

    ⚠️ This is the one that must not be done with a flat scan. FFmpeg's CAVLC
    tables are ragged: total_zeros_len is declared [16][16] but holds 135
    values, because each row stops early and C fills the rest with zeros. Read
    flat and re-cut into equal rows, every row after the first would be
    shifted, and the decoder would produce plausible garbage.
    """
    body = inside(sources, nome)
    out_values = []
    for r in entries(body):
        r = r.strip()
        if not r.startswith("{"):
            raise SystemExit("%s: mi expected a riga between braces, ho %r" % (nome, r[:40]))
        v = [value(nome, x) for x in entries(r[1:-1]) if x.strip()]
        if len(v) > columns:
            raise SystemExit("%s: riga da %d values, il maximum e' %d" % (nome, len(v), columns))
        out_values.append(v + [0] * (columns - len(v)))
    if len(out_values) > lines:
        raise SystemExit("%s: %d lines, il maximum e' %d" % (nome, len(out_values), lines))
    while len(out_values) < lines:
        out_values.append([0] * columns)
    return out_values


def exact(sources, nome, expected):
    v = flat(sources, nome)
    if len(v) != expected:
        raise SystemExit("%s: %d values, ne expected %d" % (nome, len(v), expected))
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


def riga(values, per_riga, width):
    out_values = []
    for i in range(0, len(values), per_riga):
        chunk = ", ".join(("%*d" % (width, v)) for v in values[i:i + per_riga])
        out_values.append("    " + chunk + ",")
    return "\n".join(out_values)


def pairs(values, per_riga=6):
    out_values = []
    for i in range(0, len(values), 2 * per_riga):
        chunk = ", ".join("{ %4d, %4d }" % (values[j], values[j + 1])
                          for j in range(i, min(i + 2 * per_riga, len(values)), 2))
        out_values.append("    " + chunk + ",")
    return "\n".join(out_values)


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

    sources = {}
    for nome in ("h264_cabac.c", "h264data.c", "h264_cavlc.c", "cabac.c",
                 "h264_loopfilter.c"):
        p = rif / nome
        if not p.exists():
            raise SystemExit("missing il reference %s" % p)
        sources[nome] = p.read_text(encoding="utf-8", errors="replace")

    c = [INTESTAZIONE, '#include "h264_dec_tables.h"', ""]
    h = [INTESTAZIONE,
         "#ifndef BC250_H264_DEC_TABLES_H",
         "#define BC250_H264_DEC_TABLES_H",
         "",
         "#include <stdint.h>",
         ""]

    def scalar(kind, nome, n, values, per_riga, width=4):
        if len(values) != n:
            raise SystemExit("%s: %d values, ne expected %d" % (nome, len(values), n))
        c.append("const %s %s[%d] = {" % (kind, nome, n))
        c.append(riga(values, per_riga, width))
        c.append("};\n")
        h.append("extern const %s %s[%d];" % (kind, nome, n))

    def bidim(kind, nome, lines, columns, table, per_riga, width=4):
        c.append("const %s %s[%d][%d] = {" % (kind, nome, lines, columns))
        for r in table:
            c.append("  {")
            c.append(riga(r, per_riga, width))
            c.append("  },")
        c.append("};\n")
        h.append("extern const %s %s[%d][%d];" % (kind, nome, lines, columns))

    # ---- CABAC context initialisation, Tables 9-12 to 9-33 ----------------
    init_i = exact(sources, "cabac_context_init_I", 1024 * 2)
    init_pb = exact(sources, "cabac_context_init_PB", 3 * 1024 * 2)

    c.append("const int8_t h264d_cabac_init_I[1024][2] = {")
    c.append(pairs(init_i))
    c.append("};\n")
    h.append("extern const int8_t h264d_cabac_init_I[1024][2];")

    c.append("const int8_t h264d_cabac_init_PB[3][1024][2] = {")
    for k in range(3):
        c.append("  {")
        c.append(pairs(init_pb[k * 2048:(k + 1) * 2048]))
        c.append("  },")
    c.append("};\n")
    h.append("extern const int8_t h264d_cabac_init_PB[3][1024][2];")

    # ---- context offsets, clause 9.3.3.1.3 --------------------------------
    # ⚠️ uint16_t, not uint8_t: these run up to 982.
    bidim("uint16_t", "h264d_sig_coeff_offset", 2, 14,
          matrix(sources, "significant_coeff_flag_offset", 2, 14), 14)
    bidim("uint16_t", "h264d_last_coeff_offset", 2, 14,
          matrix(sources, "last_coeff_flag_offset", 2, 14), 14)
    scalar("uint16_t", "h264d_abs_level_offset", 14,
            exact(sources, "coeff_abs_level_m1_offset", 14), 14)
    bidim("uint8_t", "h264d_sig_coeff_offset_8x8", 2, 63,
          matrix(sources, "significant_coeff_flag_offset_8x8", 2, 63), 21, 2)
    scalar("uint8_t", "h264d_sig_coeff_offset_dc", 7,
            exact(sources, "sig_coeff_offset_dc", 7), 7, 2)

    blob = flat(sources, "ff_h264_cabac_tables")
    if blob[-63:] != LAST_OFF_8X8:
        raise SystemExit("last_coeff_flag_offset_8x8 non combacia col reference")
    scalar("uint8_t", "h264d_last_coeff_offset_8x8", 63, LAST_OFF_8X8, 21, 2)

    # ---- dequantisation, clause 8.5.9 -------------------------------------
    bidim("uint8_t", "h264d_dequant4_init", 6, 3,
          matrix(sources, "ff_h264_dequant4_coeff_init", 6, 3), 3)
    bidim("uint8_t", "h264d_dequant8_init", 6, 6,
          matrix(sources, "ff_h264_dequant8_coeff_init", 6, 6), 6)
    scalar("uint8_t", "h264d_dequant8_init_scan", 16,
            exact(sources, "ff_h264_dequant8_coeff_init_scan", 16), 16, 2)
    scalar("uint8_t", "h264d_chroma_qp", 52, CHROMA_QP, 26, 3)
    scalar("uint8_t", "h264d_golomb_to_intra4x4_cbp", 48,
            exact(sources, "ff_h264_golomb_to_intra4x4_cbp", 48), 16, 3)
    scalar("uint8_t", "h264d_golomb_to_inter_cbp", 48,
            exact(sources, "ff_h264_golomb_to_inter_cbp", 48), 16, 3)
    scalar("uint8_t", "h264d_chroma_dc_scan", 4,
            exact(sources, "ff_h264_chroma_dc_scan", 4), 4, 2)

    scalar("uint8_t", "h264d_zigzag4", 16, ZIGZAG4, 16, 2)
    scalar("uint8_t", "h264d_zigzag8", 64, ZIGZAG8, 16, 2)

    # ---- CAVLC, clause 9.2 ------------------------------------------------
    bidim("uint8_t", "h264d_coeff_token_len", 4, 68,
          matrix(sources, "coeff_token_len", 4, 68), 17, 2)
    bidim("uint8_t", "h264d_coeff_token_bits", 4, 68,
          matrix(sources, "coeff_token_bits", 4, 68), 17, 3)
    scalar("uint8_t", "h264d_chroma_dc_coeff_token_len", 20,
            exact(sources, "chroma_dc_coeff_token_len", 20), 5, 2)
    scalar("uint8_t", "h264d_chroma_dc_coeff_token_bits", 20,
            exact(sources, "chroma_dc_coeff_token_bits", 20), 5, 2)
    bidim("uint8_t", "h264d_total_zeros_len", 16, 16,
          matrix(sources, "total_zeros_len", 16, 16), 16, 2)
    bidim("uint8_t", "h264d_total_zeros_bits", 16, 16,
          matrix(sources, "total_zeros_bits", 16, 16), 16, 2)
    bidim("uint8_t", "h264d_chroma_dc_total_zeros_len", 3, 4,
          matrix(sources, "chroma_dc_total_zeros_len", 3, 4), 4, 2)
    bidim("uint8_t", "h264d_chroma_dc_total_zeros_bits", 3, 4,
          matrix(sources, "chroma_dc_total_zeros_bits", 3, 4), 4, 2)
    bidim("uint8_t", "h264d_run_len", 7, 16,
          matrix(sources, "run_len", 7, 16), 16, 2)
    bidim("uint8_t", "h264d_run_bits", 7, 16,
          matrix(sources, "run_bits", 7, 16), 16, 2)


    # ---- deblocking, Tables 8-16 and 8-17 ---------------------------------
    # ⚠️ FFmpeg pads these with 52 entries of 0 in front and 52 of 255 behind,
    # so that indexA and indexB can run out of range without a bounds check.
    # Only the middle 52 are the standard's table.
    alpha = flat(sources, "alpha_table")
    beta = flat(sources, "beta_table")
    if len(alpha) != 52 * 3 or len(beta) != 52 * 3:
        raise SystemExit("alpha/beta: %d e %d values, ne expected 156" % (len(alpha), len(beta)))
    alpha = alpha[52:104]
    beta = beta[52:104]
    if alpha[15] or alpha[16] != 4 or alpha[51] != 255 or beta[51] != 18:
        raise SystemExit("alpha/beta: the known values do not match, the cut is wrong")
    scalar("uint8_t", "h264d_alpha", 52, alpha, 13, 3)
    scalar("uint8_t", "h264d_beta", 52, beta, 13, 3)

    tc0 = matrix(sources, "tc0_table", 52 * 3, 4)
    # column 0 is bS = 0, which never filters; keep bS 1..3
    tc0 = [r[1:4] for r in tc0[52:104]]
    bidim("uint8_t", "h264d_tc0", 52, 3, tc0, 3, 3)

    h += ["", "#endif /* BC250_H264_DEC_TABLES_H */", ""]

    (out / "h264_dec_tables.c").write_text("\n".join(c), encoding="utf-8", newline="\n")
    (out / "h264_dec_tables.h").write_text("\n".join(h), encoding="utf-8", newline="\n")
    print("written h264_dec_tables.c e .h  (%d values CABAC, %d tables)"
          % (len(init_i) + len(init_pb), sum(1 for x in h if x.startswith("extern"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
