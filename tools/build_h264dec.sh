#!/bin/bash
# Build the standalone H.264 decoder harness.
#
# The decoder proper is GPU-free: it reconstructs into plain system memory
# and only touches the GPU to upload the finished picture. The harness
# stubs that upload out, so it builds and runs anywhere.
set -eu
D=~/comunita/bc250-encoding-decoding-fix/approach1-compute-encoder
OUT="${1:-/tmp/h264dec}"

gcc -O2 -g -Wall -Wextra -std=gnu11 \
    -I"$D/src" -I"$D/include" \
    -o "$OUT" \
    "$D/tools/h264dec.c" \
    "$D/src/decoder_h264.c" \
    "$D/src/h264_mb_cabac.c" \
    "$D/src/h264_mb_cavlc.c" \
    "$D/src/h264_cavlc_dec.c" \
    "$D/src/h264_mb_motion.c" \
    "$D/src/h264_recon_mb.c" \
    "$D/src/h264_recon.c" \
    "$D/src/h264_pred.c" \
    "$D/src/h264_mc.c" \
    "$D/src/h264_deblock.c" \
    "$D/src/h264_dec_tables.c" \
    "$D/src/cabac.c" \
    -lm
echo "fatto: $OUT"
