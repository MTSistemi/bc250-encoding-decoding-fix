#!/bin/bash
# Decode whole sequences and compare them with the reference decoder,
# byte for byte, every picture.
#
#     test_hevc_inter_pixel.sh [percorso/hevcps]
#
# The intra suite proves one picture. This proves the rest of them: the
# motion vectors a picture inherits from its neighbours, the samples they
# fetch from pictures already decoded, and - just as easy to get wrong -
# the order the pictures come out in, which for B frames is not the order
# they went in.
#
# Nothing is switched off in the encoder. Both loop filters are on, and so
# is everything x265 does by default.
set -u
BIN="${1:-/tmp/hevcps}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

passate=0
fallite=0

prova() {
    local nome="$1" sorgente="$2" quante="$3" par="$4"
    local guasti="" wpp

    for wpp in 0 1; do
        ffmpeg -v error -y -f lavfi -i "$sorgente" -frames:v "$quante" \
               -c:v libx265 -x265-params "log-level=none:wpp=$wpp:$par" \
               -pix_fmt yuv420p -f hevc "$T/s.265" 2>/dev/null
        if [ ! -s "$T/s.265" ]; then
            guasti="$guasti wpp=$wpp:nessun-flusso"; continue
        fi
        ffmpeg -v error -y -i "$T/s.265" -f rawvideo -pix_fmt yuv420p \
               "$T/rif.yuv" 2>/dev/null
        "$BIN" -q "$T/s.265" "$T/noi.yuv" >/dev/null 2>&1
        if [ ! -s "$T/noi.yuv" ]; then
            guasti="$guasti wpp=$wpp:niente-in-uscita"; continue
        fi
        if ! cmp -s "$T/rif.yuv" "$T/noi.yuv"; then
            guasti="$guasti wpp=$wpp:$(python3 - "$T/rif.yuv" "$T/noi.yuv" <<'PY'
import sys
a = open(sys.argv[1], 'rb').read()
b = open(sys.argv[2], 'rb').read()
if len(a) != len(b):
    print("lunghezze-%d-contro-%d" % (len(a), len(b)))
else:
    d = [i for i in range(len(a)) if a[i] != b[i]]
    print("%d-byte-max-%d" % (len(d), max((abs(a[i]-b[i]) for i in d), default=0)))
PY
)"
        fi
    done

    if [ -n "$guasti" ]; then
        printf '  %-42s DIVERSO:%s\n' "$nome" "$guasti"
        fallite=$((fallite + 1)); return
    fi
    printf '  %-42s identico\n' "$nome"
    passate=$((passate + 1))
}

S1="testsrc2=size=176x144:rate=25"
S2="testsrc2=size=320x240:rate=25"
M="mandelbrot=size=176x144"

echo "solo P"
prova "4 immagini" "$S1" 4 "bframes=0:qp=28"
prova "12 immagini" "$S1" 12 "bframes=0:qp=28"
prova "un riferimento" "$S1" 12 "bframes=0:ref=1:qp=28"
prova "quattro riferimenti" "$S1" 12 "bframes=0:ref=4:qp=28"
prova "senza tmvp" "$S1" 12 "bframes=0:temporal-mvp=0:qp=28"
prova "con tmvp" "$S1" 12 "bframes=0:temporal-mvp=1:qp=28"
prova "merge a 1" "$S1" 12 "bframes=0:max-merge=1:qp=28"
prova "merge a 5" "$S1" 12 "bframes=0:max-merge=5:qp=28"
prova "pesi su P" "$S2" 12 "bframes=0:weightp=1:qp=28"
prova "senza pesi" "$S2" 12 "bframes=0:weightp=0:qp=28"

echo
echo "con le B"
prova "2 B" "$S1" 12 "bframes=2:qp=28"
prova "3 B, piramide" "$S1" 16 "bframes=3:b-pyramid=1:qp=28"
prova "3 B, senza piramide" "$S1" 16 "bframes=3:b-pyramid=0:qp=28"
prova "8 B" "$S1" 16 "bframes=8:qp=28"
prova "pesi su B" "$S2" 16 "bframes=3:weightb=1:qp=28"
prova "GOP di 8" "$S1" 24 "keyint=8:bframes=3:qp=28"
prova "GOP lungo" "$S1" 24 "keyint=24:bframes=3:qp=28"

echo
echo "partizioni e blocchi"
prova "AMP acceso" "$S1" 12 "amp=1:rect=1:qp=28"
prova "senza rettangolari" "$S1" 12 "rect=0:amp=0:qp=28"
prova "CTU 16" "$S1" 12 "ctu=16:qp=28"
prova "CTU 32" "$S1" 12 "ctu=32:qp=28"
prova "min CU 16" "$S1" 12 "min-cu-size=16:qp=28"
prova "TU inter profonda" "$S1" 12 "tu-inter-depth=3:qp=28"

echo
echo "controllo di flusso e strumenti"
prova "crf" "$S2" 16 "crf=28"
prova "crf, gruppo 16" "$S2" 16 "crf=28:qg-size=16"
prova "bitrate fissato" "$S2" 16 "bitrate=400"
prova "senza deblocking" "$S1" 12 "deblock=false:qp=28"
prova "senza sao" "$S1" 12 "sao=0:qp=28"
prova "senza perdite" "$S1" 6 "lossless=1"
prova "qp basso" "$S1" 12 "qp=10"
prova "qp alto" "$S1" 12 "qp=44"
prova "transform skip" "$S1" 12 "tskip=1:qp=28"
prova "sign hide spento" "$S1" 12 "signhide=0:qp=28"

echo
echo "misure e contenuti"
prova "320x240" "$S2" 12 "qp=28"
prova "640x480" "testsrc2=size=640x480:rate=25" 8 "qp=28"
prova "58x50, da ritagliare" "testsrc2=size=58x50:rate=25" 12 "qp=28"
prova "mandelbrot" "$M" 12 "qp=28"
prova "immagine ferma" "color=c=gray:size=176x144:rate=25" 12 "qp=28"

echo
printf 'identiche %d, diverse %d\n' "$passate" "$fallite"
[ "$fallite" -eq 0 ]
