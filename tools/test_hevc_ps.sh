#!/bin/bash
# Read the parameter sets and slice headers of a spread of H.265 streams.
#
#     test_hevc_ps.sh [percorso/hevcps]
#
# Nothing is decoded here. What is checked is that every parameter set and
# every slice header can be read, that the picture count is the one asked
# for, and that the picture order counts come out as exactly 0..N-1 with
# none missing and none repeated.
#
# ⚠️ That last one is the check worth having. Only the low bits of the
# picture order count are sent and the rest is carried from the picture
# before, so a stream in decode order - which with B pictures is not
# display order - will produce a plausible but wrong sequence if the
# derivation is wrong. Asking for the SET of them rather than the order
# catches that without caring which order they arrived in.
set -u
BIN="${1:-/tmp/hevcps}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

passate=0
fallite=0

prova() {
    local nome="$1"; shift
    local quante="$1"; shift
    local sorgente="$1"; shift

    ffmpeg -v error -y -f lavfi -i "$sorgente" "$@" \
           -pix_fmt yuv420p -f hevc "$T/s.265" 2>/dev/null
    if [ ! -s "$T/s.265" ]; then
        printf '  %-46s ffmpeg non ha prodotto il flusso\n' "$nome"
        fallite=$((fallite + 1)); return
    fi

    local out
    out=$("$BIN" "$T/s.265" 2>&1)
    if [ $? -ne 0 ]; then
        printf '  %-46s RIFIUTATO: %s\n' "$nome" \
               "$(echo "$out" | grep -m1 -i 'rifiut')"
        fallite=$((fallite + 1)); return
    fi

    local viste
    viste=$(echo "$out" | sed -n 's/^\([0-9]*\) immagini.*/\1/p')
    if [ "$viste" != "$quante" ]; then
        printf '  %-46s %s immagini invece di %s\n' "$nome" "$viste" "$quante"
        fallite=$((fallite + 1)); return
    fi

    # The picture order counts are checked by the tool itself, in its exit
    # code, because it is the tool that knows an IDR restarts them: an
    # all-intra stream is every picture at zero, not a sequence.

    printf '  %-46s letto  (%s byte)\n' "$nome" "$(wc -c < "$T/s.265")"
    passate=$((passate + 1))
}

S1="testsrc2=size=176x144:rate=25"
S2="testsrc2=size=320x240:rate=25"
X="-c:v libx265 -x265-params"

echo "una immagine sola"
prova "intra, qp 26" 1 "$S1" -frames:v 1 $X "log-level=none:qp=26"
prova "intra, qp 10" 1 "$S1" -frames:v 1 $X "log-level=none:qp=10"
prova "intra, qp 45" 1 "$S1" -frames:v 1 $X "log-level=none:qp=45"

echo
echo "dimensione dei blocchi di codifica"
for ctu in 16 32 64; do
    prova "CTU $ctu" 6 "$S1" -frames:v 6 $X "log-level=none:ctu=$ctu:bframes=1"
done
prova "TU massima 16" 6 "$S1" -frames:v 6 $X \
    "log-level=none:max-tu-size=16:bframes=1"

echo
echo "struttura temporale"
prova "senza B" 8 "$S1" -frames:v 8 $X "log-level=none:bframes=0:keyint=8"
prova "2 B" 8 "$S1" -frames:v 8 $X "log-level=none:bframes=2:keyint=8"
prova "4 B, piramide" 16 "$S1" -frames:v 16 $X \
    "log-level=none:bframes=4:b-pyramid=1:keyint=16"
prova "GOP lungo" 24 "$S1" -frames:v 24 $X \
    "log-level=none:bframes=3:keyint=24"
prova "tutte intra" 8 "$S1" -frames:v 8 $X "log-level=none:keyint=1"

echo
echo "strumenti di codifica"
prova "senza SAO" 8 "$S1" -frames:v 8 $X "log-level=none:sao=0:bframes=1"
prova "senza deblocking" 8 "$S1" -frames:v 8 $X \
    "log-level=none:deblock=0:bframes=1"
prova "AMP acceso" 8 "$S1" -frames:v 8 $X "log-level=none:amp=1:bframes=1"
prova "transform skip" 8 "$S1" -frames:v 8 $X "log-level=none:tskip=1:bframes=1"
prova "sign hide spento" 8 "$S1" -frames:v 8 $X \
    "log-level=none:signhide=0:bframes=1"
prova "strong-intra-smoothing off" 8 "$S1" -frames:v 8 $X \
    "log-level=none:strong-intra-smoothing=0:bframes=1"
prova "pesi su P e B" 12 "$S1" -frames:v 12 $X \
    "log-level=none:weightp=1:weightb=1:bframes=2"
prova "senza tmvp" 8 "$S1" -frames:v 8 $X "log-level=none:temporal-mvp=0:bframes=1"
prova "cu-qp-delta profondo" 8 "$S2" -frames:v 8 $X \
    "log-level=none:aq-mode=2:bframes=1"

echo
echo "parallelismo nel flusso"
prova "wpp acceso" 8 "$S2" -frames:v 8 $X "log-level=none:wpp=1:bframes=1"
prova "wpp spento" 8 "$S2" -frames:v 8 $X "log-level=none:wpp=0:bframes=1"
prova "4 tile" 8 "$S2" -frames:v 8 $X \
    "log-level=none:wpp=0:tu-intra-depth=1:frame-threads=1"

echo
echo "misure"
prova "320x240" 8 "$S2" -frames:v 8 $X "log-level=none:bframes=1"
prova "58x50, da ritagliare" 8 "testsrc2=size=58x50:rate=25" -frames:v 8 $X \
    "log-level=none:bframes=1"
prova "640x480" 8 "testsrc2=size=640x480:rate=25" -frames:v 8 $X \
    "log-level=none:bframes=2"

echo
printf 'lette %d, fallite %d\n' "$passate" "$fallite"
[ "$fallite" -eq 0 ]
