# bc250-vaapi

A VA-API driver for the AMD BC-250. The BC-250 has no video engine: its VCN
block is present in silicon but the kernel driver never brings it up, and no
firmware for it has ever been published. So everything here runs somewhere
else — the encoders on the GPU's compute shaders, the decoder on the CPU.

It gives the board what the hardware does not:

- **H.264 and HEVC encoding** (`VAEntrypointEncSlice`), on compute shaders.
- **H.264 decoding** (`VAEntrypointVLD`), on the CPU, bit-exact and threaded.

## Where this comes from

This is a derived work of [simpmix/bc250-encoding-decoding-fix][upstream],
which is where the compute-shader encoders, the GPU layer and the VA-API
plumbing come from. It is GPL-3.0-only, and so is this.

What was added here is the H.264 decoder. The upstream tree has a file by
that name, but it is a 76-line stub that dispatches the *encode* shader:
nothing decoded. These files are new:

    src/decoder_h264.c       the picture and slice level, the frame store
    src/h264_mb_cabac.c      the macroblock layer, read with CABAC
    src/h264_mb_cavlc.c      the macroblock layer, read with CAVLC
    src/h264_cavlc_dec.c     CAVLC residual blocks, clause 9.2
    src/h264_cabac_dec.h     the arithmetic decoding engine
    src/h264_mb_motion.c     motion vector prediction, spatial and temporal direct
    src/h264_recon_mb.c      reconstruction: prediction, residual, both
    src/h264_recon.c         dequantisation and the inverse transforms
    src/h264_pred.c          intra prediction, all block sizes
    src/h264_mc.c            inter prediction, the six-tap and bilinear filters
    src/h264_deblock.c       the deblocking filter
    src/h264_threads.c       the wavefront and the slice workers
    src/h264_dec_tables.c    the normative tables
    src/va_decode.c          the VA-API decode entry point
    tools/h264dec.c          a standalone harness, for testing without a GPU

## Correctness

H.264 decoding is exact arithmetic: a conformant decoder produces the same
samples as every other conformant decoder, to the bit. So the only useful
pass mark is "identical", and any difference at all is a bug however small
it looks.

    tools/test_conformance.sh     67 streams through the harness
    tools/test_vaapi_decode.sh    23 streams through the driver, on the board

Both compare against the reference decoder byte for byte. Between them they
cover intra across the whole QP range, P and B pictures, reference
pyramids, spatial and temporal direct prediction, explicit and implicit
weighted prediction, the 8x8 transform, CABAC and CAVLC, several slices per
picture, JVT and custom quantisation matrices, and sizes that are not a
multiple of 16.

## What it will not decode

Refused outright rather than decoded into something plausible:

- 4:2:2 and 4:4:4, and anything above 8 bits
- interlaced and field coding
- flexible macroblock ordering, and more than one slice group
- I_PCM macroblocks
- HEVC — there is an HEVC *encoder* here, but no decoder

## Speed

Measured on a BC-250, 60 pictures of 1920x1080 at crf 23 with three B
pictures, decode time only:

| threads | one slice a picture | four slices | eight slices |
| ------: | ------------------: | ----------: | -----------: |
|       1 |            68.4 fps |    68.1 fps |     69.0 fps |
|       8 |           118.4 fps |   156.2 fps |    181.5 fps |

Reconstruction and deblocking run as a wavefront: row *r* may take column
*x* once row *r - 1* has finished column *x + 1*. Entropy decoding cannot
join in — a slice's arithmetic decoder can only be started at the slice's
beginning, which is why H.264 has no equivalent of HEVC's
entropy_coding_sync — so a picture's slices are decoded at the same time
instead, which is the one place its entropy decoding does parallelise.

`BC250_H264_THREADS` sets the thread count; 1 keeps everything on one core.

## Building

    cd approach1-compute-encoder
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    sudo install -m644 build/bc250_drv_video.so \
        /usr/lib/x86_64-linux-gnu/dri/bc250_drv_video.so
    LIBVA_DRIVER_NAME=bc250 vainfo

The decoder itself needs no GPU. `tools/build_h264dec.sh` builds the
harness, which decodes to a YUV file on any x86-64 machine.

## Licensing

GPL-3.0-only throughout, as upstream.

The normative tables in `src/h264_dec_tables.c` were extracted mechanically
from FFmpeg n8.1.2, which is LGPL-2.1-or-later. Clause 3 of the LGPL allows
that under the GPL-3.0, which is what is done here. `tools/gen_h264_tables.py`
is the extractor, so the provenance of every table can be checked.

Upstream's own documentation is kept as [README.upstream.md](README.upstream.md)
and under [docs/](docs/); it covers the encoders, the audio clock fix and
the rest of the project, none of which changed here.

[upstream]: https://github.com/simpmix/bc250-encoding-decoding-fix
