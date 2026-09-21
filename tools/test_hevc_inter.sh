#!/bin/bash
# Do P and B slices land where they should.
set -u
BIN="${1:-/tmp/hevcps}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

passate=0
fallite=0

prova() {
    local nome="$1" quante="$2" sorgente="$3" par="$4"
    local guasti="" wpp out
    for wpp in 0 1; do
        ffmpeg -v error -y -f lavfi -i "$sorgente" -frames:v "$quante" \
               -c:v libx265 -x265-params "log-level=none:wpp=$wpp:$par" \
               -pix_fmt yuv420p -f hevc "$T/s.265" 2>/dev/null
        if [ ! -s "$T/s.265" ]; then
            guasti="$guasti wpp=$wpp:nessun-flusso"; continue
        fi
        out=$("$BIN" -q "$T/s.265" 2>&1)
        if ! echo "$out" | grep -q '0 saltate, 0 perse'; then
            guasti="$guasti wpp=$wpp:[$(echo "$out" | tail -1)]"
        fi
    done
    if [ -n "$guasti" ]; then
        printf '  %-40s NO:%s\n' "$nome" "$guasti"
        fallite=$((fallite + 1)); return
    fi
    printf '  %-40s percorse\n' "$nome"
    passate=$((passate + 1))
}

S1="testsrc2=size=176x144:rate=25"
S2="testsrc2=size=320x240:rate=25"

echo "struttura temporale"
prova "solo P, 8 immagini" 8 "$S1" "bframes=0:qp=28"
prova "2 B" 8 "$S1" "bframes=2:qp=28"
prova "4 B, piramide" 16 "$S1" "bframes=4:b-pyramid=1:qp=28"
prova "8 B" 16 "$S1" "bframes=8:qp=28"
prova "GOP lungo" 24 "$S1" "keyint=240:qp=28"
prova "ogni immagine intra" 8 "$S1" "keyint=1:qp=28"

echo
echo "partizioni"
prova "AMP acceso" 8 "$S1" "amp=1:rect=1:qp=28"
prova "AMP spento" 8 "$S1" "amp=0:rect=1:qp=28"
prova "senza rettangolari" 8 "$S1" "rect=0:amp=0:qp=28"
prova "CTU 16" 8 "$S1" "ctu=16:rect=1:amp=1:qp=28"
prova "CTU 32" 8 "$S1" "ctu=32:rect=1:amp=1:qp=28"
prova "min CU 8" 8 "$S1" "min-cu-size=8:rect=1:amp=1:qp=28"
prova "min CU 32" 8 "$S1" "min-cu-size=32:qp=28"

echo
echo "movimento e riferimenti"
prova "4 riferimenti" 12 "$S2" "ref=4:qp=28"
prova "1 riferimento" 12 "$S2" "ref=1:qp=28"
prova "senza tmvp" 8 "$S1" "temporal-mvp=0:qp=28"
prova "con tmvp" 8 "$S1" "temporal-mvp=1:qp=28"
prova "merge a 2" 8 "$S1" "max-merge=2:qp=28"
prova "merge a 5" 8 "$S1" "max-merge=5:qp=28"
prova "ricerca ampia" 8 "$S2" "me=star:merange=57:qp=28"
prova "pesi su P" 12 "$S2" "weightp=1:qp=28"
prova "pesi su P e B" 12 "$S2" "weightp=1:weightb=1:qp=28"

echo
echo "strumenti"
prova "transform skip" 8 "$S1" "tskip=1:qp=28"
prova "sign hide spento" 8 "$S1" "signhide=0:qp=28"
prova "crf" 12 "$S2" "crf=28"
prova "crf, gruppo 16" 12 "$S2" "crf=28:qg-size=16"
prova "bitrate fissato" 12 "$S2" "bitrate=300"
prova "senza perdite" 6 "$S1" "lossless=1"
prova "senza deblocking" 8 "$S1" "deblock=false:qp=28"
prova "senza sao" 8 "$S1" "sao=0:qp=28"
prova "qp basso" 8 "$S1" "qp=8"
prova "qp alto" 8 "$S1" "qp=45"
prova "640x480" 8 "testsrc2=size=640x480:rate=25" "qp=28"
prova "58x50, da ritagliare" 8 "testsrc2=size=58x50:rate=25" "qp=28"
prova "mandelbrot in movimento" 8 "mandelbrot=size=320x240" "qp=28"

echo
printf 'percorse %d, fallite %d\n' "$passate" "$fallite"
[ "$fallite" -eq 0 ]
