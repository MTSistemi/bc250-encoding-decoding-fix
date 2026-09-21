#!/bin/bash
# Decode through the driver, on the board, and compare against the software
# decoder byte for byte.
#
#     prova_vaapi.sh
#
# prova_conformita.sh exercises the decoder through the standalone harness,
# which feeds it directly. This one goes the whole way round: ffmpeg parses
# the headers, fills in VAPictureParameterBufferH264 and the slice
# parameters, and hands them to the driver through libva. So it tests the
# translation layer - reference lists mapped onto frame store slots, the
# weights, the quantisation matrices, slice_data_bit_offset - which the
# harness cannot reach.
#
# âš ï¸ -hwaccel_output_format vaapi, then hwdownload. Asking for nv12 directly
# makes ffmpeg insert a scaler it cannot configure, and the run dies with
# "Error reinitializing filters" long before the driver is involved.
set -u
export LIBVA_DRIVER_NAME=bc250
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

passate=0
fallite=0

prova() {
    local nome="$1"; shift
    local sorgente="$1"; shift

    ffmpeg -v error -y -f lavfi -i "$sorgente" "$@" \
           -pix_fmt yuv420p -f h264 "$T/s.264" 2>/dev/null
    if [ ! -s "$T/s.264" ]; then
        printf '  %-44s ffmpeg non ha prodotto il flusso\n' "$nome"
        fallite=$((fallite + 1)); return
    fi

    ffmpeg -v error -y -i "$T/s.264" -f rawvideo -pix_fmt yuv420p \
           "$T/sw.yuv" 2>/dev/null

    local err
    err=$(ffmpeg -v error -y -hwaccel vaapi -hwaccel_output_format vaapi \
                 -i "$T/s.264" -vf 'hwdownload,format=nv12' \
                 -pix_fmt yuv420p -f rawvideo "$T/hw.yuv" 2>&1 \
          | grep -v '^\[bc250-gpu\]' | head -2)

    if [ ! -s "$T/hw.yuv" ]; then
        printf '  %-44s NIENTE IN USCITA: %s\n' "$nome" "$err"
        fallite=$((fallite + 1)); return
    fi
    if cmp -s "$T/sw.yuv" "$T/hw.yuv"; then
        printf '  %-44s identico  (%s byte)\n' "$nome" "$(wc -c < "$T/s.264")"
        passate=$((passate + 1))
    else
        local d
        d=$(python3 - "$T/sw.yuv" "$T/hw.yuv" <<'PY'
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
        printf '  %-44s DIVERSO: %s\n' "$nome" "$d"
        fallite=$((fallite + 1))
    fi
}

S1="testsrc2=size=176x144:rate=25"
S2="testsrc2=size=320x240:rate=25"

echo "immagini intra"
for qp in 1 18 26 40 51; do
    prova "intra qp $qp" "$S1" -frames:v 1 -c:v libx264 -profile:v main -qp $qp
done
prova "intra 8x8, high" "$S1" -frames:v 1 -c:v libx264 -profile:v high -qp 20
prova "intra CAVLC" "$S1" -frames:v 1 -c:v libx264 -profile:v baseline -qp 26
prova "intra, 4 slice" "$S1" -frames:v 1 -c:v libx264 -profile:v main -qp 26 \
    -x264opts slices=4
prova "intra, senza deblocking" "$S1" -frames:v 1 -c:v libx264 -profile:v main \
    -qp 26 -x264opts no-deblock

echo
echo "sequenze P"
prova "10 fotogrammi P" "$S1" -frames:v 10 -c:v libx264 -profile:v main -qp 26 -bf 0 -g 30
prova "10 fotogrammi P, 3 riferimenti" "$S1" -frames:v 10 -c:v libx264 \
    -profile:v main -qp 26 -bf 0 -refs 3 -g 30
prova "12 fotogrammi P, high 8x8" "$S1" -frames:v 12 -c:v libx264 \
    -profile:v high -qp 24 -bf 0 -g 6
prova "12 fotogrammi P CAVLC" "$S1" -frames:v 12 -c:v libx264 -profile:v high \
    -qp 24 -bf 0 -g 6 -x264opts cabac=0

echo
echo "sequenze B"
prova "12 fotogrammi B" "$S1" -frames:v 12 -c:v libx264 -profile:v main -qp 26 \
    -bf 2 -g 6 -x264opts b-pyramid=none
prova "12 fotogrammi B, piramide" "$S1" -frames:v 12 -c:v libx264 \
    -profile:v main -qp 26 -bf 2 -g 30
prova "12 fotogrammi B, direct temporale" "$S1" -frames:v 12 -c:v libx264 \
    -profile:v main -qp 26 -bf 2 -g 6 -x264opts direct=temporal
prova "12 fotogrammi B, pesi espliciti" "$S1" -frames:v 12 -c:v libx264 \
    -profile:v main -qp 26 -bf 2 -g 4 -x264opts weightp=2:weightb=1
prova "12 fotogrammi B CAVLC" "$S1" -frames:v 12 -c:v libx264 -profile:v high \
    -qp 24 -bf 2 -g 6 -x264opts cabac=0:b-pyramid=none

echo
echo "matrici di quantizzazione"
prova "intra, matrici JVT" "$S1" -frames:v 1 -c:v libx264 -profile:v high \
    -qp 22 -x264opts cqm=jvt
prova "12 fotogrammi, matrici JVT" "$S1" -frames:v 12 -c:v libx264 \
    -profile:v high -qp 24 -bf 2 -g 6 -x264opts cqm=jvt:b-pyramid=none

echo
echo "misure e carichi"
prova "320x240, 20 fotogrammi" "$S2" -frames:v 20 -c:v libx264 -profile:v high \
    -preset slow -crf 24 -g 10
prova "640x480, 20 fotogrammi" "testsrc2=size=640x480:rate=25" -frames:v 20 \
    -c:v libx264 -profile:v high -preset medium -crf 25 -g 10
prova "58x50, bordi da ritagliare" "testsrc2=size=58x50:rate=25" -frames:v 12 \
    -c:v libx264 -profile:v high -qp 26 -bf 2 -g 6

echo
printf 'passate %d, fallite %d\n' "$passate" "$fallite"
[ "$fallite" -eq 0 ]
