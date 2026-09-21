#!/bin/bash
# Walk the coding tree of intra slices, and check the slice lands.
#
#     test_hevc_intra.sh [percorso/hevcps]
#
# Nothing is reconstructed: no prediction, no transform, no samples. What
# is checked is that every bin of the syntax was read against the right
# context, and CABAC makes that checkable without any pixels at all.
#
# ⚠️ A slice read correctly ends exactly where it should - the
# end_of_slice_segment_flag after the last coding tree unit comes back one,
# with the arithmetic decoder at the end of the NAL. A slice read wrongly
# almost never does: it runs out of data, or finishes early with bytes to
# spare, or claims the picture ended in the middle of it. So "did it land"
# is a hard test to pass by accident, and it is available long before
# anything can be compared sample by sample.
#
# ⚠️ Every case runs twice, with wavefront parallelism off and on, because
# it changes the slice data itself: a bit and a byte alignment at the end of
# every coding tree row, and the arithmetic decoder restarted there from a
# snapshot of the row above. A decoder that only ever saw wpp=0 would read
# most real streams wrongly, since x265 turns it on by default.
set -u
BIN="${1:-/tmp/hevcps}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

passate=0
fallite=0

prova() {
    local nome="$1"; shift
    local sorgente="$1"; shift
    local parametri="$1"; shift

    local guasti="" dimensione="" wpp out perse
    for wpp in 0 1; do
        ffmpeg -v error -y -f lavfi -i "$sorgente" -frames:v 1 \
               -c:v libx265 -x265-params "log-level=none:wpp=$wpp:$parametri" \
               -pix_fmt yuv420p -f hevc "$T/s.265" 2>/dev/null
        if [ ! -s "$T/s.265" ]; then
            guasti="$guasti wpp=$wpp:nessun-flusso"
            continue
        fi
        dimensione=$(wc -c < "$T/s.265")
        out=$("$BIN" -q "$T/s.265" 2>&1)
        perse=$(echo "$out" | sed -n 's/.*, \([0-9]*\) perse.*/\1/p')
        if [ "$perse" != "0" ]; then
            guasti="$guasti wpp=$wpp:$(echo "$out" | grep -m1 '\^' \
                    | sed 's/.*\^ //' | tr ' ' '-')"
        fi
    done
    if [ -n "$guasti" ]; then
        printf '  %-44s NON ATTERRA:%s\n' "$nome" "$guasti"
        fallite=$((fallite + 1)); return
    fi
    printf '  %-44s percorsa  (%s byte)\n' "$nome" "$dimensione"
    passate=$((passate + 1))
}

S1="testsrc2=size=176x144:rate=25"
S2="testsrc2=size=320x240:rate=25"

echo "la scala dei QP"
for qp in 2 8 16 22 28 34 40 51; do
    prova "qp $qp" "$S1" "qp=$qp"
done

echo
echo "dimensione dei blocchi"
for ctu in 16 32 64; do
    prova "CTU $ctu" "$S1" "ctu=$ctu:qp=28"
done
prova "TU massima 16" "$S1" "max-tu-size=16:qp=28"
prova "TU massima 8" "$S1" "max-tu-size=8:qp=28"
prova "profondita' TU intra 1" "$S1" "tu-intra-depth=1:qp=28"
prova "profondita' TU intra 3" "$S1" "tu-intra-depth=3:qp=28"
prova "CTU 16 e TU 8" "$S1" "ctu=16:max-tu-size=8:qp=28"

echo
echo "strumenti che cambiano la sintassi"
prova "senza SAO" "$S1" "sao=0:qp=28"
prova "SAO acceso" "$S1" "sao=1:qp=28"
prova "sign hide spento" "$S1" "signhide=0:qp=28"
prova "transform skip" "$S1" "tskip=1:qp=28"
prova "strong smoothing off" "$S1" "strong-intra-smoothing=0:qp=28"
prova "rd massimo" "$S1" "rd=6:qp=28"
prova "cu-lossless" "$S1" "cu-lossless=1:qp=28"

echo
echo "qp che varia dentro l'immagine"
prova "aq forte" "$S2" "aq-mode=2:aq-strength=1.5:qp=28"
prova "crf invece di qp" "$S2" "crf=28"
prova "crf basso" "$S2" "crf=14"

echo
echo "misure"
prova "320x240" "$S2" "qp=28"
prova "640x480" "testsrc2=size=640x480:rate=25" "qp=28"
prova "58x50, da ritagliare" "testsrc2=size=58x50:rate=25" "qp=28"
prova "1920x1080" "testsrc2=size=1920x1080:rate=25" "qp=30"
prova "immagine piccolissima" "testsrc2=size=32x32:rate=25" "qp=28"

echo
echo "altri contenuti"
prova "mandelbrot" "mandelbrot=size=320x240" "qp=24"
prova "rumore" "testsrc2=size=176x144:rate=25" "qp=4"

echo
printf 'percorse %d, fallite %d\n' "$passate" "$fallite"
[ "$fallite" -eq 0 ]
