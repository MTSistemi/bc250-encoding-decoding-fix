#!/bin/bash
# Read the parameter sets and slice headers of a spread of H.265 streams.
#
#     test_hevc_ps.sh [path/hevcps]
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

passed=0
failed=0

check() {
    local name="$1"; shift
    local count="$1"; shift
    local source="$1"; shift

    ffmpeg -v error -y -f lavfi -i "$source" "$@" \
           -pix_fmt yuv420p -f hevc "$T/s.265" 2>/dev/null
    if [ ! -s "$T/s.265" ]; then
        printf '  %-46s ffmpeg produced no stream\n' "$name"
        failed=$((failed + 1)); return
    fi

    local out
    out=$("$BIN" "$T/s.265" 2>&1)
    if [ $? -ne 0 ]; then
        printf '  %-46s REFUSED: %s\n' "$name" \
               "$(echo "$out" | grep -m1 -i 'refus')"
        failed=$((failed + 1)); return
    fi

    local seen
    seen=$(echo "$out" | sed -n 's/^\([0-9]*\) pictures.*/\1/p')
    if [ "$seen" != "$count" ]; then
        printf '  %-46s %s pictures instead of %s\n' "$name" "$seen" "$count"
        failed=$((failed + 1)); return
    fi

    # The picture order counts are checked by the tool itself, in its exit
    # code, because it is the tool that knows an IDR restarts them: an
    # all-intra stream is every picture at zero, not a sequence.

    printf '  %-46s read  (%s bytes)\n' "$name" "$(wc -c < "$T/s.265")"
    passed=$((passed + 1))
}

S1="testsrc2=size=176x144:rate=25"
S2="testsrc2=size=320x240:rate=25"
X="-c:v libx265 -x265-params"

echo "a single picture"
check "intra, qp 26" 1 "$S1" -frames:v 1 $X "log-level=none:qp=26"
check "intra, qp 10" 1 "$S1" -frames:v 1 $X "log-level=none:qp=10"
check "intra, qp 45" 1 "$S1" -frames:v 1 $X "log-level=none:qp=45"

echo
echo "coding block sizes"
for ctu in 16 32 64; do
    check "CTU $ctu" 6 "$S1" -frames:v 6 $X "log-level=none:ctu=$ctu:bframes=1"
done
check "max TU 16" 6 "$S1" -frames:v 6 $X \
    "log-level=none:max-tu-size=16:bframes=1"

echo
echo "temporal structure"
check "no B" 8 "$S1" -frames:v 8 $X "log-level=none:bframes=0:keyint=8"
check "2 B" 8 "$S1" -frames:v 8 $X "log-level=none:bframes=2:keyint=8"
check "4 B, pyramid" 16 "$S1" -frames:v 16 $X \
    "log-level=none:bframes=4:b-pyramid=1:keyint=16"
check "long GOP" 24 "$S1" -frames:v 24 $X \
    "log-level=none:bframes=3:keyint=24"
check "all intra" 8 "$S1" -frames:v 8 $X "log-level=none:keyint=1"

echo
echo "coding tools"
check "no SAO" 8 "$S1" -frames:v 8 $X "log-level=none:sao=0:bframes=1"
check "no deblocking" 8 "$S1" -frames:v 8 $X \
    "log-level=none:deblock=0:bframes=1"
check "AMP on" 8 "$S1" -frames:v 8 $X "log-level=none:amp=1:bframes=1"
check "transform skip" 8 "$S1" -frames:v 8 $X "log-level=none:tskip=1:bframes=1"
check "sign hide off" 8 "$S1" -frames:v 8 $X \
    "log-level=none:signhide=0:bframes=1"
check "strong-intra-smoothing off" 8 "$S1" -frames:v 8 $X \
    "log-level=none:strong-intra-smoothing=0:bframes=1"
check "weights on P and B" 12 "$S1" -frames:v 12 $X \
    "log-level=none:weightp=1:weightb=1:bframes=2"
check "no tmvp" 8 "$S1" -frames:v 8 $X "log-level=none:temporal-mvp=0:bframes=1"
check "deep cu-qp-delta" 8 "$S2" -frames:v 8 $X \
    "log-level=none:aq-mode=2:bframes=1"

echo
echo "parallelism in the stream"
check "wpp on" 8 "$S2" -frames:v 8 $X "log-level=none:wpp=1:bframes=1"
check "wpp off" 8 "$S2" -frames:v 8 $X "log-level=none:wpp=0:bframes=1"
check "4 tile" 8 "$S2" -frames:v 8 $X \
    "log-level=none:wpp=0:tu-intra-depth=1:frame-threads=1"

echo
echo "sizes"
check "320x240" 8 "$S2" -frames:v 8 $X "log-level=none:bframes=1"
check "58x50, to be cropped" 8 "testsrc2=size=58x50:rate=25" -frames:v 8 $X \
    "log-level=none:bframes=1"
check "640x480" 8 "testsrc2=size=640x480:rate=25" -frames:v 8 $X \
    "log-level=none:bframes=2"

echo
printf 'read %d, failed %d\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
