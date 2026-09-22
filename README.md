# bc250-vaapi

A VA-API driver for the AMD BC-250. The BC-250 has no video engine: its VCN
block is present in silicon but the kernel driver never brings it up, and no
firmware for it has ever been published. So everything here runs somewhere
else — the encoders on the GPU's compute shaders, the decoders on the CPU.

It gives the board what the hardware does not:

- **H.264 and HEVC encoding** (`VAEntrypointEncSlice`), on compute shaders.
- **H.264 and H.265 decoding** (`VAEntrypointVLD`), on the CPU, bit-exact
  and threaded.

An application that asks for VA-API finds it, where otherwise it would find
nothing at all: no other VA-API driver initialises on this hardware.

## Where this comes from

This is a derived work of [simpmix/bc250-encoding-decoding-fix][upstream],
which is where the compute-shader encoders, the GPU layer and the VA-API
plumbing come from. It is GPL-3.0-only, and so is this.

What was added here are the two decoders. The upstream tree has a file
named for the H.264 one, but it is a 76-line stub that dispatches the
*encode* shader: nothing decoded.

H.264:

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

H.265:

    src/decoder_h265.c       the decoder as a library: DPB, lists, output order
    src/hevc_ps.c            parameter sets and slice segment headers
    src/hevc_cu.c            the coding tree and the coding unit syntax
    src/hevc_residual.c      residual coding
    src/hevc_pred.c          intra prediction, 35 modes
    src/hevc_transform.c     the inverse transforms, DCT and DST
    src/hevc_mv.c            merge and AMVP, clause 8.5.3.2
    src/hevc_mc.c            interpolation, clause 8.5.3.3
    src/hevc_filter.c        deblocking (8.7.2) and SAO (8.7.3)
    src/hevc_wpp.c           wavefront parallelism across coding tree rows
    src/hevc_dec_tables.c    the normative tables
    src/va_decode_hevc.c     the VA-API decode entry point
    tools/hevcps.c           a standalone harness, for testing without a GPU

## Correctness

Decoding is exact arithmetic: a conformant decoder produces the same
samples as every other conformant decoder, to the bit. So the only useful
pass mark is "identical", and any difference at all is a bug however small
it looks.

    tools/test_conformance.sh       67  H.264 streams through the harness
    tools/test_vaapi_decode.sh      23  H.264 streams through the driver, on the board
    tools/test_hevc_ps.sh           27  parameter sets and slice headers
    tools/test_hevc_intra.sh        33  the intra walk, wavefront on and off
    tools/test_hevc_pixel.sh        61  intra pictures, sample by sample
    tools/test_hevc_inter.sh        35  the inter walk
    tools/test_hevc_inter_pixel.sh  38  whole sequences, sample by sample
    tools/test_vaapi_hevc.sh        18  H.265 streams through the driver, on the board

All 302 pass, and every one of them compares against the reference decoder
byte for byte. Between them they cover intra across the whole QP range, P
and B pictures, reference pyramids, spatial and temporal direct prediction,
explicit and implicit weighted prediction, CABAC and CAVLC, several slices
per picture, wavefront parallelism on and off, both loop filters, custom
quantisation matrices, and sizes that are not a multiple of the block size.

`tools/check_tables.py` and `approach1-compute-encoder/tools/check_hevc_tables.py`
check the generated tables structurally — prefix-free codes, Kraft sums,
scans that are permutations — rather than against a copy of themselves.

## What it will not decode

Refused outright rather than decoded into something plausible.

H.264: 4:2:2 and 4:4:4 and anything above 8 bits, interlaced and field
coding, flexible macroblock ordering and more than one slice group, I_PCM
macroblocks.

H.265: tiles, quantisation matrices, PCM samples, long-term references,
anything but 4:2:0 at 8 bits.

## Speed

Measured on a BC-250. H.264, 60 pictures of 1920x1080 at crf 23 with three
B pictures, decode time only:

| threads | one slice a picture | four slices | eight slices |
| ------: | ------------------: | ----------: | -----------: |
|       1 |            68.4 fps |    68.1 fps |     69.0 fps |
|       8 |           118.4 fps |   156.2 fps |    181.5 fps |

H.265, 120 pictures of 1920x1080, best of three:

| threads | frames per second |
| ------: | ----------------: |
|       1 |          66.3 fps |
|       6 |          97.6 fps |

Through the driver, end to end, `ffmpeg -hwaccel vaapi` at 1920x1080:
H.264 reaches 119 frames per second and H.265 96.

**Faster than nothing, not faster than everything.** ffmpeg's own threaded
software decoder reaches about 360 frames per second on the same processor,
so an application that already decodes in software should keep doing that.
What this is for is the one that asks for VA-API and is told there is none.

Reconstruction and deblocking run as a wavefront: row *r* may take column
*x* once row *r - 1* has finished column *x + 1*. For H.264 entropy
decoding cannot join in — a slice's arithmetic decoder can only be started
at the slice's beginning, which is why H.264 has no equivalent of HEVC's
entropy_coding_sync — so a picture's slices are decoded at the same time
instead. H.265 written with wavefront parallelism gives every coding tree
block row its own arithmetic substream, and those really do run at once.

`BC250_H264_THREADS` and `BC250_HEVC_THREAD` set the thread counts; 1 keeps
everything on one core.

## Installing

There are packages for the distributions a BC-250 usually runs:

| | |
| --- | --- |
| CachyOS, Arch, SteamOS, HoloISO | `packaging/arch/PKGBUILD` |
| Bazzite, Silverblue, Kinoite, Fedora | `packaging/fedora/bc250-vaapi.spec` |
| Debian, Ubuntu | `packaging/debian/` |

`packaging/build.sh` builds each one inside a container of its own
distribution, and `packaging/verify.sh` installs the result in a clean
container and checks it. See [packaging/README.md](packaging/README.md).

Installing a package switches nothing on by itself: a systemd user
environment generator reads the PCI identifier of the Cyan Skillfish APU
out of sysfs at session start and sets `LIBVA_DRIVER_NAME` only if it finds
one. On a machine that is not a BC-250 the package is inert, which is the
point — there radeonsi or iHD run a VA-API on a graphics chip, and this
one, which decodes on the processor, would take their place and give back
less.

## Building it yourself

    cd approach1-compute-encoder
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build -j
    sudo install -m644 build/bc250_drv_video.so \
        /usr/lib/x86_64-linux-gnu/dri/bc250_drv_video.so
    LIBVA_DRIVER_NAME=bc250 vainfo

⚠️ The prefix is not a detail: the shaders are looked for at a path
compiled into the library, which CMake derives from `CMAKE_INSTALL_PREFIX`.

Neither decoder needs a GPU. `tools/build_h264dec.sh` and
`tools/build_hevcps.sh` build the harnesses, which decode to a YUV file on
any x86-64 machine.

## Licensing

GPL-3.0-only throughout, as upstream.

The normative tables in `src/h264_dec_tables.c` and
`src/hevc_dec_tables.c` were extracted mechanically from FFmpeg n8.1.2,
which is LGPL-2.1-or-later. Clause 3 of the LGPL allows that under the
GPL-3.0, which is what is done here. `tools/gen_h264_tables.py` and
`approach1-compute-encoder/tools/gen_hevc_tables.py` are the extractors, so
the provenance of every table can be checked.

Upstream's own documentation is kept as [README.upstream.md](README.upstream.md)
and under [docs/](docs/); it covers the encoders, the audio clock fix and
the rest of the project, none of which changed here.

[upstream]: https://github.com/simpmix/bc250-encoding-decoding-fix
