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
for qp in 0 10 18 26 34 44 51; do
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
echo "CAVLC (baseline)"
prova "intra CAVLC" "$SRC1" 176x144     -frames:v 1 -c:v libx264 -profile:v baseline -qp 26
prova "10 fotogrammi CAVLC" "$SRC1" 176x144     -frames:v 10 -c:v libx264 -profile:v baseline -qp 26 -g 30

echo
printf 'passate %d, fallite %d, saltate %d
' "$passate" "$fallite" "$saltate"
[ "$fallite" -eq 0 ]
