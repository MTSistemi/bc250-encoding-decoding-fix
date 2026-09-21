#!/bin/bash
# Decode intra pictures and compare them with the reference decoder, byte
# for byte.
#
#     test_hevc_pixel.sh [percorso/hevcps]
#
# H.265 decoding is exact arithmetic, like H.264's: a conformant decoder
# produces the same samples as every other one, to the bit. So the only
# useful pass mark is "identical", and any difference at all is a bug
# however small it looks.
#
# ⚠️ deblock=false, not deblock=0. The second sets the filter's offsets to
# zero and leaves it running, which is a different thing entirely and the
# reason a first round of these comparisons looked like a prediction bug:
# every difference sat on an eight-sample boundary, which is the
# deblocking grid. x264 has the same trap with -x264-params deblock=0, so
# it is worth knowing twice.
#
# ⚠️ sao=0 as well, for now. Both filters come next; until then they are
# turned off in the streams rather than ignored in the comparison.
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

    local guasti="" dimensione="" wpp
    for wpp in 0 1; do
        ffmpeg -v error -y -f lavfi -i "$sorgente" -frames:v 1 -c:v libx265 \
               -x265-params "log-level=none:sao=0:deblock=false:wpp=$wpp:$parametri" \
               -pix_fmt yuv420p -f hevc "$T/s.265" 2>/dev/null
        if [ ! -s "$T/s.265" ]; then
            guasti="$guasti wpp=$wpp:nessun-flusso"
            continue
        fi
        dimensione=$(wc -c < "$T/s.265")
        ffmpeg -v error -y -i "$T/s.265" -f rawvideo -pix_fmt yuv420p \
               "$T/rif.yuv" 2>/dev/null
        "$BIN" -q "$T/s.265" "$T/noi.yuv" >/dev/null 2>&1
        if [ ! -s "$T/noi.yuv" ]; then
            guasti="$guasti wpp=$wpp:niente-in-uscita"
            continue
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
        printf '  %-44s DIVERSO:%s\n' "$nome" "$guasti"
        fallite=$((fallite + 1)); return
    fi
    printf '  %-44s identico  (%s byte)\n' "$nome" "$dimensione"
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
prova "TU massima 4" "$S1" "max-tu-size=4:qp=28"
prova "profondita' TU intra 1" "$S1" "tu-intra-depth=1:qp=28"
prova "profondita' TU intra 4" "$S1" "tu-intra-depth=4:qp=28"
prova "CTU 16, TU 8" "$S1" "ctu=16:max-tu-size=8:qp=28"

echo
echo "strumenti"
prova "sign hide spento" "$S1" "signhide=0:qp=28"
prova "transform skip" "$S1" "tskip=1:qp=28"
prova "strong smoothing off" "$S1" "strong-intra-smoothing=0:qp=28"
prova "strong smoothing on" "$S1" "strong-intra-smoothing=1:qp=28"
prova "rd massimo" "$S1" "rd=6:qp=28"
prova "aq forte" "$S2" "aq-mode=2:aq-strength=1.5:qp=28"
prova "rdoq spento" "$S1" "rdoq-level=0:qp=28"
prova "psy-rd forte" "$S1" "psy-rd=4.0:qp=28"
prova "senza perdite" "$S1" "lossless=1"

echo
# ⚠️ Rate control is where the quantisation parameter stops standing still.
# A decoder can be byte-exact on every constant-QP stream in the world and
# still be wrong here, because only here does the parameter get predicted
# from the neighbours rather than read from the slice header.
echo "controllo di flusso, un QP per gruppo"
prova "crf" "$S2" "crf=28"
prova "crf basso" "$S2" "crf=12"
prova "crf, gruppo 32" "$S2" "crf=28:qg-size=32"
prova "crf, gruppo 16" "$S2" "crf=28:qg-size=16"
prova "crf, CTU 32" "$S2" "crf=28:ctu=32"
prova "crf, CTU 16" "$S2" "crf=28:ctu=16"
prova "crf 1920x1080" "testsrc2=size=1920x1080:rate=25" "crf=30"
prova "crf mandelbrot" "mandelbrot=size=320x240" "crf=20"
prova "bitrate fissato" "$S2" "bitrate=300"
prova "aq 3 con crf" "$S2" "crf=28:aq-mode=3"

echo
echo "misure"
prova "320x240" "$S2" "qp=28"
prova "640x480" "testsrc2=size=640x480:rate=25" "qp=28"
prova "58x50, da ritagliare" "testsrc2=size=58x50:rate=25" "qp=28"
prova "1920x1080" "testsrc2=size=1920x1080:rate=25" "qp=30"
prova "32x32" "testsrc2=size=32x32:rate=25" "qp=28"

echo
echo "altri contenuti"
prova "mandelbrot" "mandelbrot=size=320x240" "qp=24"
prova "quasi rumore" "$S1" "qp=4"
prova "grigio piatto" "color=c=gray:size=176x144" "qp=28"

echo
printf 'identiche %d, diverse %d\n' "$passate" "$fallite"
[ "$fallite" -eq 0 ]
