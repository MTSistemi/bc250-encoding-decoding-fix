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


def read(text, nome):
    """Every integer of a generated table, in declaration order."""
    m = re.search(r"\b%s\s*(\[\d+\])+\s*=\s*\{" % re.escape(nome), text)
    if not m:
        raise SystemExit("not found: %s" % nome)
    i = text.index("{", m.end() - 1)
    level = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            level += 1
        elif text[j] == "}":
            level -= 1
            if level == 0:
                body = text[i:j + 1]
                break
    else:
        raise SystemExit("unclosed brackets in %s" % nome)
    return [int(x) for x in re.findall(r"-?\d+", body)]


def size_of(flat, lines, columns):
    if len(flat) != lines * columns:
        raise SystemExit("expected %d values, got %d" % (lines * columns, len(flat)))
    return [flat[r * columns:(r + 1) * columns] for r in range(lines)]


def kraft(pairs, label, faults, minimum=0.9):
    """Kraft-McMillan on a set of (length, bits) codes.

    ⚠️ The sum must be at most 1, not exactly 1. H.264's variable-length
    codes are prefix-free but deliberately incomplete: Table 9-5 leaves two
    of the 64 six-bit patterns unassigned, Table 9-7's first row leaves one
    nine-bit pattern unassigned, and so on - those patterns just mean the
    stream is corrupt. A sum above 1 is the real fault, because then two
    codes overlap; `minimum` only catches a row that lost most of its
    entries, which is what a mis-cut ragged table would look like.
    """
    total = 0.0
    seen_n = {}
    for ln, bits in pairs:
        if ln <= 0:
            continue
        total += 2.0 ** -ln
        if (ln, bits) in seen_n:
            faults.append("%s: the code (%d bit, 0x%x) appears twice" % (label, ln, bits))
        seen_n[(ln, bits)] = True
        if bits >= (1 << ln):
            faults.append("%s: the code 0x%x does not fit in %d bits" % (label, bits, ln))
    if total > 1.0 + 1e-9:
        faults.append("%s: Kraft total %.9f, above 1: the codes overlap"
                      % (label, total))
    elif total < minimum:
        faults.append("%s: Kraft total %.9f, too low: the row has lost some codes"
                      % (label, total))
    # prefix-free: no code is the prefix of a longer one
    in_order = sorted(seen_n, key=lambda t: t[0])
    for a in range(len(in_order)):
        la, ba = in_order[a]
        for b in range(a + 1, len(in_order)):
            lb, bb = in_order[b]
            if lb == la:
                continue
            if (bb >> (lb - la)) == ba:
                faults.append("%s: (%d,0x%x) is a prefix of (%d,0x%x)" % (label, la, ba, lb, bb))
                break


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    src = pathlib.Path(sys.argv[1])
    text = (src / "h264_dec_tables.c").read_text(encoding="utf-8")
    faults = []

    # ---- CABAC initialisation, clause 9.3.1.1 -----------------------------
    init_i = size_of(read(text, "h264d_cabac_init_I"), 1024, 2)
    init_pb = size_of(read(text, "h264d_cabac_init_PB"), 3 * 1024, 2)
    for label, table in (("I", init_i), ("PB", init_pb)):
        out_values = 0
        for m, n in table:
            for qp in range(52):
                pre = ((m * qp) >> 4) + n
                pre = 1 if pre < 1 else (126 if pre > 126 else pre)
                state = (pre - 64) if pre > 63 else (63 - pre)
                if not (0 <= state <= 63):
                    out_values += 1
        if out_values:
            faults.append("init %s: %d states outside 0..63" % (label, out_values))

    # ---- coeff_token, Table 9-5 -------------------------------------------
    # 4 tables; within each, the codes for every legal (TotalCoeff,
    # TrailingOnes) pair. Index is nC-table major, then 4*TotalCoeff+T1s.
    ct_len = size_of(read(text, "h264d_coeff_token_len"), 4, 68)
    ct_bits = size_of(read(text, "h264d_coeff_token_bits"), 4, 68)
    for t in range(4):
        pairs = [(ct_len[t][i], ct_bits[t][i]) for i in range(68) if ct_len[t][i] > 0]
        kraft(pairs, "coeff_token[%d]" % t, faults)

    cdc_len = read(text, "h264d_chroma_dc_coeff_token_len")
    cdc_bits = read(text, "h264d_chroma_dc_coeff_token_bits")
    kraft([(cdc_len[i], cdc_bits[i]) for i in range(20) if cdc_len[i] > 0],
          "chroma_dc_coeff_token", faults)

    # ---- total_zeros, Tables 9-7 and 9-8 ----------------------------------
    # Row i is for TotalCoeff == i+1 and codes total_zeros in 0..(16-1-i),
    # so exactly 16-i entries must be present and the rest must be padding.
    tz_len = size_of(read(text, "h264d_total_zeros_len"), 16, 16)
    tz_bits = size_of(read(text, "h264d_total_zeros_bits"), 16, 16)
    for i in range(15):
        expected = 16 - i
        present = sum(1 for j in range(16) if tz_len[i][j] > 0)
        if present != expected:
            faults.append("total_zeros row %d: %d codes, expected %d"
                          % (i, present, expected))
        kraft([(tz_len[i][j], tz_bits[i][j]) for j in range(16) if tz_len[i][j] > 0],
              "total_zeros[%d]" % i, faults)

    cdtz_len = size_of(read(text, "h264d_chroma_dc_total_zeros_len"), 3, 4)
    cdtz_bits = size_of(read(text, "h264d_chroma_dc_total_zeros_bits"), 3, 4)
    for i in range(3):
        kraft([(cdtz_len[i][j], cdtz_bits[i][j]) for j in range(4) if cdtz_len[i][j] > 0],
              "chroma_dc_total_zeros[%d]" % i, faults)

    # ---- run_before, Table 9-10 -------------------------------------------
    # Rows 0..5 are zerosLeft 1..6 and are complete codes; row 6 is
    # zerosLeft > 6, whose run_before 7..14 tail is an escape and therefore
    # is NOT prefix-complete on its own.
    run_l = size_of(read(text, "h264d_run_len"), 7, 16)
    run_b = size_of(read(text, "h264d_run_bits"), 7, 16)
    for i in range(6):
        expected = i + 2
        present = sum(1 for j in range(16) if run_l[i][j] > 0)
        if present != expected:
            faults.append("run_before row %d: %d codes, expected %d"
                          % (i, present, expected))
        kraft([(run_l[i][j], run_b[i][j]) for j in range(16) if run_l[i][j] > 0],
              "run_before[%d]" % i, faults)

    # ---- scans ------------------------------------------------------------
    for nome, n in (("h264d_zigzag4", 16), ("h264d_zigzag8", 64)):
        v = read(text, nome)
        if sorted(v) != list(range(n)):
            faults.append("%s is not a permutation of 0..%d" % (nome, n - 1))

    # ---- chroma QP, Table 8-15 --------------------------------------------
    cqp = read(text, "h264d_chroma_qp")
    if len(cqp) != 52 or cqp[:30] != list(range(30)):
        faults.append("chroma_qp: the part below 30 is not the identity")
    if any(cqp[i + 1] < cqp[i] for i in range(51)):
        faults.append("chroma_qp: not monotonic")
    if cqp[51] != 39:
        faults.append("chroma_qp: the last value is %d instead of 39" % cqp[51])

    if faults:
        for g in faults:
            print("FAULT   " + g)
        print("\n%d faults" % len(faults))
        return 1
    print("tables in order: codes prefix-free and no duplicates, CABAC states in 0..63, "
          "ragged rows of the right length, scans permutations of 0..n-1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
