#!/bin/bash
# Decode a spread of streams and compare against the reference decoder, byte
# for byte.
#
#     prova_conformita.sh [percorso/h264dec]
#
# H.264 decoding is exact arithmetic: a conformant decoder produces the same
# samples as every other conformant decoder, to the bit. So the only useful
# pass mark is "identical", and any difference at all is a bug, however
# small it looks.
#
# ⚠️ -x264opts no-deblock, not -x264-params deblock=0: the second is accepted
# and silently ignored, which produced two byte-identical streams and a very
# confusing half hour.
set -u
DEC="${1:-/tmp/h264dec}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

passate=0
fallite=0
saltate=0

prova() {
    local nome="$1"; shift
    local sorgente="$1"; shift
    local dim="$1"; shift

    ffmpeg -v error -y -f lavfi -i "$sorgente" "$@" \
           -pix_fmt yuv420p -f h264 "$T/s.264" 2>/dev/null
    if [ ! -s "$T/s.264" ]; then
        printf '  %-44s ffmpeg non ha prodotto il flusso\n' "$nome"
        saltate=$((saltate + 1)); return
    fi

    ffmpeg -v error -y -i "$T/s.264" -f rawvideo -pix_fmt yuv420p "$T/rif.yuv" 2>/dev/null
    local out err
    out=$("$DEC" "$T/s.264" "$T/noi.yuv" 2>&1)
    local rc=$?

    if [ $rc -eq 3 ]; then
        printf '  %-44s fuori copertura: %s\n' "$nome" "$(echo "$out" | head -1)"
        saltate=$((saltate + 1)); return
    fi
    if [ $rc -ne 0 ]; then
        printf '  %-44s RIFIUTATO (%d) %s\n' "$nome" "$rc" "$(echo "$out" | head -1)"
        fallite=$((fallite + 1)); return
    fi

    if cmp -s "$T/rif.yuv" "$T/noi.yuv"; then
        printf '  %-44s identico  (%s, %s byte)\n' "$nome" "$dim" "$(wc -c < "$T/s.264")"
        passate=$((passate + 1))
    else
        err=$(python3 - "$T/rif.yuv" "$T/noi.yuv" <<'PY'
import sys
a = open(sys.argv[1], 'rb').read()
b = open(sys.argv[2], 'rb').read()
if len(a) != len(b):
    print("lunghezze diverse: %d contro %d" % (len(a), len(b)))
else:
    d = [i for i in range(len(a)) if a[i] != b[i]]
    print("%d byte diversi su %d, errore max %d"
          % (len(d), len(a), max((abs(a[i]-b[i]) for i in d), default=0)))
PY
)
        printf '  %-44s DIVERSO: %s\n' "$nome" "$err"
        fallite=$((fallite + 1))
    fi
}

SRC1="testsrc2=size=176x144:rate=25"
SRC2="testsrc2=size=320x240:rate=25"
SRC3="testsrc2=size=64x64:rate=25"

echo "un fotogramma intra, vari QP e profili"
# qp 0 is absent on purpose: libx264 encodes it losslessly, which is
# High 4:4:4 Predictive, a profile this decoder refuses up front.
for qp in 1 10 18 26 34 44 51; do
    prova "intra qp $qp, main" "$SRC1" 176x144 \
        -frames:v 1 -c:v libx264 -profile:v main -qp $qp
done

echo
echo "trasformata 8x8 (profilo high)"
for qp in 12 22 32; do
    prova "intra 8x8 qp $qp" "$SRC1" 176x144 \
        -frames:v 1 -c:v libx264 -profile:v high -qp $qp
done

echo
echo "senza filtro di deblocking"
prova "intra no-deblock" "$SRC1" 176x144 \
    -frames:v 1 -c:v libx264 -profile:v main -qp 26 -x264opts no-deblock

echo
echo "misure non multiple di 16"
prova "58x50, bordi da ritagliare" "testsrc2=size=58x50:rate=25" 58x50 \
    -frames:v 1 -c:v libx264 -profile:v main -qp 24
prova "320x240" "$SRC2" 320x240 \
    -frames:v 1 -c:v libx264 -profile:v main -qp 24

echo
echo "piu' slice per immagine"
prova "4 slice" "$SRC1" 176x144 \
    -frames:v 1 -c:v libx264 -profile:v main -qp 26 -x264opts slices=4

echo
echo "sequenze con fotogrammi P"
for n in 2 5 15; do
    prova "$n fotogrammi, niente B" "$SRC1" 176x144         -frames:v $n -c:v libx264 -profile:v main -qp 26 -bf 0 -g 30
done
prova "10 fotogrammi, 3 riferimenti" "$SRC1" 176x144     -frames:v 10 -c:v libx264 -profile:v main -qp 26 -bf 0 -refs 3 -g 30
prova "10 fotogrammi, high 8x8" "$SRC1" 176x144     -frames:v 10 -c:v libx264 -profile:v high -qp 26 -bf 0 -g 30
prova "10 fotogrammi, 4 slice" "$SRC1" 176x144     -frames:v 10 -c:v libx264 -profile:v main -qp 26 -bf 0 -g 30 -x264opts slices=4
prova "20 fotogrammi, crf e preset lento" "$SRC1" 176x144     -frames:v 20 -c:v libx264 -profile:v high -preset slow -crf 26 -bf 0 -g 8
prova "12 fotogrammi 320x240, crf" "$SRC2" 320x240     -frames:v 12 -c:v libx264 -profile:v high -crf 24 -bf 0 -g 6

echo
echo "fotogrammi B"
prova "9 fotogrammi, 2 B, niente piramide" "$SRC1" 176x144     -frames:v 9 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 30 -x264opts b-pyramid=none
prova "16 fotogrammi, 3 B, niente piramide" "$SRC1" 176x144     -frames:v 16 -c:v libx264 -profile:v main -qp 24 -bf 3 -g 8 -x264opts b-pyramid=none
prova "12 fotogrammi B, high 8x8" "$SRC1" 176x144     -frames:v 12 -c:v libx264 -profile:v high -qp 26 -bf 2 -g 6 -x264opts b-pyramid=none
prova "12 fotogrammi B, 320x240" "$SRC2" 320x240     -frames:v 12 -c:v libx264 -profile:v high -qp 24 -bf 2 -g 6 -x264opts b-pyramid=none
prova "10 fotogrammi, piramide B" "$SRC1" 176x144     -frames:v 10 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 30
prova "20 fotogrammi, tutti i default di x264" "$SRC1" 176x144     -frames:v 20 -c:v libx264 -profile:v high -preset slow -crf 25 -g 10

echo
echo "CAVLC"
NC="-x264opts cabac=0"
prova "intra CAVLC" "$SRC1" 176x144 \
    -frames:v 1 -c:v libx264 -profile:v baseline -qp 26
prova "10 fotogrammi CAVLC" "$SRC1" 176x144 \
    -frames:v 10 -c:v libx264 -profile:v baseline -qp 26 -g 30

# Low QP means big coefficients, and big coefficients are the only way to
# reach the level escape codes: level_prefix 15 and up, where the suffix
# length stops following suffixLength and starts following the prefix.
for qp in 1 8 16 34 51; do
    prova "intra CAVLC qp $qp" "$SRC1" 176x144 \
        -frames:v 1 -c:v libx264 -profile:v main -qp $qp $NC
done

prova "intra 8x8 CAVLC" "$SRC1" 176x144 \
    -frames:v 1 -c:v libx264 -profile:v high -qp 18 $NC
prova "58x50 CAVLC" "testsrc2=size=58x50:rate=25" 58x50 \
    -frames:v 1 -c:v libx264 -profile:v main -qp 24 $NC
prova "4 slice CAVLC" "$SRC1" 176x144 \
    -frames:v 1 -c:v libx264 -profile:v main -qp 26 -x264opts cabac=0:slices=4
prova "intra CAVLC no-deblock" "$SRC1" 176x144 \
    -frames:v 1 -c:v libx264 -profile:v main -qp 26 -x264opts cabac=0:no-deblock

prova "10 fotogrammi CAVLC, 3 riferimenti" "$SRC1" 176x144 \
    -frames:v 10 -c:v libx264 -profile:v main -qp 26 -bf 0 -refs 3 -g 30 $NC
prova "12 fotogrammi CAVLC, high 8x8" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v high -qp 24 -bf 0 -g 6 $NC
prova "20 fotogrammi CAVLC, preset lento" "$SRC1" 176x144 \
    -frames:v 20 -c:v libx264 -profile:v high -preset slow -crf 26 -bf 0 -g 8 $NC
prova "12 fotogrammi CAVLC 320x240" "$SRC2" 320x240 \
    -frames:v 12 -c:v libx264 -profile:v high -crf 24 -bf 0 -g 6 $NC

prova "9 fotogrammi CAVLC, 2 B" "$SRC1" 176x144 \
    -frames:v 9 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 30 \
    -x264opts cabac=0:b-pyramid=none
prova "12 fotogrammi CAVLC B, high 8x8" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v high -qp 26 -bf 2 -g 6 \
    -x264opts cabac=0:b-pyramid=none
prova "10 fotogrammi CAVLC, piramide B" "$SRC1" 176x144 \
    -frames:v 10 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 30 $NC
prova "12 fotogrammi CAVLC B, 320x240" "$SRC2" 320x240 \
    -frames:v 12 -c:v libx264 -profile:v high -qp 24 -bf 2 -g 6 \
    -x264opts cabac=0:b-pyramid=none
prova "8 fotogrammi CAVLC, pesi espliciti" "$SRC1" 176x144 \
    -frames:v 8 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 4 \
    -x264opts cabac=0:weightp=2:weightb=1:b-pyramid=none

echo
echo "direct temporale"
TD="-x264opts direct=temporal"
prova "9 fotogrammi, direct temporale" "$SRC1" 176x144 \
    -frames:v 9 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 30 \
    -x264opts direct=temporal:b-pyramid=none
prova "16 fotogrammi, 3 B, direct temporale" "$SRC1" 176x144 \
    -frames:v 16 -c:v libx264 -profile:v main -qp 24 -bf 3 -g 8 \
    -x264opts direct=temporal:b-pyramid=none
prova "12 fotogrammi temporale, high 8x8" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v high -qp 26 -bf 2 -g 6 \
    -x264opts direct=temporal:b-pyramid=none
prova "10 fotogrammi temporale, piramide B" "$SRC1" 176x144 \
    -frames:v 10 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 30 $TD
prova "20 fotogrammi temporale, piramide e 3 rif" "$SRC1" 176x144 \
    -frames:v 20 -c:v libx264 -profile:v high -qp 24 -bf 3 -refs 3 -g 10 $TD
prova "12 fotogrammi temporale, 320x240" "$SRC2" 320x240 \
    -frames:v 12 -c:v libx264 -profile:v high -crf 24 -bf 2 -g 6 $TD
prova "12 fotogrammi temporale, pesi impliciti" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 6 \
    -x264opts direct=temporal:weightb=1
prova "12 fotogrammi temporale CAVLC" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 6 \
    -x264opts direct=temporal:cabac=0
prova "16 fotogrammi temporale CAVLC, piramide" "$SRC1" 176x144 \
    -frames:v 16 -c:v libx264 -profile:v high -qp 24 -bf 3 -g 8 \
    -x264opts direct=temporal:cabac=0
prova "20 fotogrammi temporale, preset lento" "$SRC1" 176x144 \
    -frames:v 20 -c:v libx264 -profile:v high -preset slow -crf 25 -g 10 $TD
prova "12 fotogrammi temporale, 4 slice" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v main -qp 26 -bf 2 -g 6 \
    -x264opts direct=temporal:slices=4
prova "12 fotogrammi temporale, 58x50" "testsrc2=size=58x50:rate=25" 58x50 \
    -frames:v 12 -c:v libx264 -profile:v high -qp 26 -bf 2 -g 6 $TD

echo
echo "matrici di quantizzazione"

# A distinct value in every position: a list kept in the wrong order then
# cannot come out right by accident. The 4x4 lists run 8..38 and the 8x8
# ones 8..71, both in the raster order x264's file format expects.
cqm_file() {
    local f="$1"
    { for nome in INTRA4X4_LUMA INTRA4X4_CHROMAU INTRA4X4_CHROMAV \
                  INTER4X4_LUMA INTER4X4_CHROMAU INTER4X4_CHROMAV; do
          echo "$nome"
          for r in 0 1 2 3; do
              for c in 0 1 2 3; do printf ' %d' $((8 + 2 * (r * 4 + c))); done
              echo
          done
      done
      for nome in INTRA8X8_LUMA INTER8X8_LUMA; do
          echo "$nome"
          for r in 0 1 2 3 4 5 6 7; do
              for c in 0 1 2 3 4 5 6 7; do printf ' %d' $((8 + r * 8 + c)); done
              echo
          done
      done
    } > "$f"
}
cqm_file "$T/cqm.txt"

prova "intra, matrici JVT" "$SRC1" 176x144 \
    -frames:v 1 -c:v libx264 -profile:v high -qp 22 -x264opts cqm=jvt
prova "12 fotogrammi, matrici JVT" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v high -qp 24 -bf 2 -g 6 \
    -x264opts cqm=jvt:b-pyramid=none
prova "12 fotogrammi, matrici JVT CAVLC" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v high -qp 24 -bf 2 -g 6 \
    -x264opts cqm=jvt:cabac=0:b-pyramid=none
prova "intra, matrici su misura" "$SRC1" 176x144 \
    -frames:v 1 -c:v libx264 -profile:v high -qp 22 \
    -x264opts "cqmfile=$T/cqm.txt"
prova "12 fotogrammi, matrici su misura" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v high -qp 24 -bf 2 -g 6 \
    -x264opts "cqmfile=$T/cqm.txt:b-pyramid=none"
prova "12 fotogrammi su misura, CAVLC" "$SRC1" 176x144 \
    -frames:v 12 -c:v libx264 -profile:v high -qp 24 -bf 2 -g 6 \
    -x264opts "cqmfile=$T/cqm.txt:cabac=0:b-pyramid=none"
prova "20 fotogrammi su misura, preset lento" "$SRC2" 320x240 \
    -frames:v 20 -c:v libx264 -profile:v high -preset slow -crf 24 -g 10 \
    -x264opts "cqmfile=$T/cqm.txt"

echo
printf 'passate %d, fallite %d, saltate %d
' "$passate" "$fallite" "$saltate"
[ "$fallite" -eq 0 ]
