#!/usr/bin/env bash
# bc250-encoding-decoding-fix v0.4.3 - https://github.com/simpmix/bc250-encoding-decoding-fix
#
# compile_shaders.sh - Compiles all compute shaders to SPIR-V binaries
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SHADER_SRC_DIR="$PROJECT_ROOT/approach1-compute-encoder/shaders"
OUTPUT_DIR="${1:-$PROJECT_ROOT/approach1-compute-encoder/build/shaders}"

mkdir -p "$OUTPUT_DIR"

COMPILER=""
if command -v glslangValidator &> /dev/null; then
    COMPILER="glslangValidator"
elif command -v glslc &> /dev/null; then
    COMPILER="glslc"
else
    echo "Error: Neither glslangValidator nor glslc found."
    echo "Please install glslang-tools (Debian/Ubuntu) or glslang (Fedora/Arch)."
    exit 1
fi

echo "Using shader compiler: $COMPILER"
echo "Compiling shaders from: $SHADER_SRC_DIR"
echo "Output directory:       $OUTPUT_DIR"

for shader in "$SHADER_SRC_DIR"/*.comp; do
    fname="$(basename "$shader")"
    out_spv="$OUTPUT_DIR/${fname}.spv"
    echo "  -> Compiling $fname -> ${fname}.spv"
    if [ "$COMPILER" = "glslangValidator" ]; then
        glslangValidator -V --target-env vulkan1.1 "$shader" -o "$out_spv"
    else
        glslc --target-env=vulkan1.1 -c "$shader" -o "$out_spv"
    fi
done

echo "All compute shaders compiled successfully to $OUTPUT_DIR!"
