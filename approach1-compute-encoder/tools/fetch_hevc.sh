#!/bin/bash
# Fetch the FFmpeg files gen_hevc_tables.py reads, at the tag it was
# written against.
#
#     fetch_hevc.sh [directory]
#
# They are LGPL-2.1-or-later and are used here under GPL-3.0-only, which
# clause 3 of the LGPL permits. Nothing from them is copied into the tree
# except the normative constant tables of Rec. ITU-T H.265, which belong to
# the standard rather than to anyone's implementation of it.
set -eu
TAG=n8.1.2
DIR="${1:-hevc-rif}"
BASE="https://raw.githubusercontent.com/FFmpeg/FFmpeg/$TAG/libavcodec/hevc"

mkdir -p "$DIR"
for f in cabac.c data.c filter.c dsp.c pred_template.c; do
    curl -fsS -o "$DIR/$f" "$BASE/$f"
    printf '  %-20s %s\n' "$f" "$(wc -c < "$DIR/$f") byte"
done
echo "pronti in $DIR, da FFmpeg $TAG"
