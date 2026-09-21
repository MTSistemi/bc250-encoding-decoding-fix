/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * h264_simd.h - what the vector paths in the decoder are allowed to assume.
 *
 * SSE2 is part of the x86-64 ABI, so on this target it is always there and
 * needs no runtime check: the compiler defines __SSE2__ unconditionally.
 * Anything newer does need one, and gets it through __builtin_cpu_supports
 * with the function marked for that instruction set.
 *
 * ⚠️ Every vector path here has a scalar one beside it that produces the
 * same bytes, and the conformance run is byte-exact, so a vector path that
 * disagrees fails loudly rather than softening the picture. That is the
 * whole reason it is safe to write them.
 */
#ifndef BC250_H264_SIMD_H
#define BC250_H264_SIMD_H

#if defined(__SSE2__) && (defined(__x86_64__) || defined(_M_X64))
#  define BC250_H264_SSE2 1
#  include <emmintrin.h>
#else
#  define BC250_H264_SSE2 0
#endif

#endif /* BC250_H264_SIMD_H */
