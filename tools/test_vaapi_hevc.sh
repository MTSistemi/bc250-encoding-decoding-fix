#!/bin/bash
# Decode H.265 through the driver, on the board, and compare with the
# software decoder byte for byte.
#
#     test_vaapi_hevc.sh [directory del driver]
#
# The harness suites prove the decoder. This proves the road to it: the
# translation from VA's structures into the decoder's own, the surfaces,
# and the picture on its way back out as NV12. A decoder that is right and
# a driver that hands it the wrong parameters look the same from outside.
set -u
DRI="${1:-/tmp/dri}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

export LIBVA_DRIVERS_PATH="$DRI"
export LIBVA_DRIVER_NAME=bc250

passate=0
fallite=0

prova() {
    local nome="$1" sorgente="$2" quante="$3" par="$4" w="$5" h="$6"

    ffmpeg -v error -y -f lavfi -i "$sorgente" -frames:v "$quante" \
           -c:v libx265 -x265-params "log-level=none:$par" \
           -pix_fmt yuv420p -f hevc "$T/s.265" 2>/dev/null
    if [ ! -s "$T/s.265" ]; then
        printf '  %-40s ffmpeg non ha prodotto il flusso\n' "$nome"
        fallite=$((fallite + 1)); return
    fi

    ffmpeg -v error -y -i "$T/s.265" -f rawvideo -pix_fmt yuv420p \
           "$T/sw.yuv" 2>/dev/null
    ffmpeg -v error -y -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
           -hwaccel_output_format nv12 -i "$T/s.265" \
           -f rawvideo -pix_fmt yuv420p "$T/hw.yuv" 2>"$T/err"

    if [ ! -s "$T/hw.yuv" ]; then
        printf '  %-40s niente in uscita: %s\n' "$nome" \
               "$(grep -v bc250-gpu "$T/err" | tail -1)"
        fallite=$((fallite + 1)); return
    fi
    if ! cmp -s "$T/sw.yuv" "$T/hw.yuv"; then
        printf '  %-40s DIVERSO: %s\n' "$nome" \
               "$(python3 - "$T/sw.yuv" "$T/hw.yuv" <<'PY'
import sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
if len(a) != len(b):
    print("lunghezze %d contro %d" % (len(a), len(b)))
else:
    d = [i for i in range(len(a)) if a[i] != b[i]]
    print("%d byte, massimo %d" % (len(d), max((abs(a[i]-b[i]) for i in d), default=0)))
PY
)"
        fallite=$((fallite + 1)); return
    fi
    printf '  %-40s identico\n' "$nome"
    passate=$((passate + 1))
}

S1="testsrc2=size=176x144:rate=25"
S2="testsrc2=size=320x240:rate=25"

echo "una immagine sola"
prova "intra, qp 28" "$S1" 1 "qp=28" 176 144
prova "intra, qp 12" "$S1" 1 "qp=12" 176 144
prova "intra, qp 44" "$S1" 1 "qp=44" 176 144

echo
echo "sequenze"
prova "solo P" "$S1" 8 "bframes=0:qp=28" 176 144
prova "con le B" "$S1" 12 "bframes=3:qp=28" 176 144
prova "piramide di B" "$S1" 16 "bframes=3:b-pyramid=1:qp=28" 176 144
prova "quattro riferimenti" "$S1" 12 "ref=4:qp=28" 176 144
prova "pesi" "$S2" 12 "weightp=1:weightb=1:qp=28" 320 240
prova "crf" "$S2" 12 "crf=28" 320 240
prova "wpp acceso" "$S1" 12 "wpp=1:qp=28" 176 144
prova "wpp spento" "$S1" 12 "wpp=0:qp=28" 176 144
prova "senza deblocking" "$S1" 8 "deblock=false:qp=28" 176 144
prova "senza sao" "$S1" 8 "sao=0:qp=28" 176 144
prova "CTU 16" "$S1" 8 "ctu=16:qp=28" 176 144
prova "CTU 32" "$S1" 8 "ctu=32:qp=28" 176 144

echo
echo "misure"
prova "320x240" "$S2" 8 "qp=28" 320 240
prova "640x480" "testsrc2=size=640x480:rate=25" 6 "qp=28" 640 480
prova "1280x720" "testsrc2=size=1280x720:rate=25" 4 "qp=30" 1280 720

echo
printf 'identiche %d, diverse %d\n' "$passate" "$fallite"
[ "$fallite" -eq 0 ]
