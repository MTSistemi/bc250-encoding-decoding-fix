## Description
This PR introduces critical fixes, rate control enhancements, performance optimizations, and H.265 P-frame inter-prediction:

1. **H.265/HEVC Inter-Frame (P-Frame) Prediction & CU Skip Mode (DEVLOG §27)**:
   - Extends the HEVC encoder (`encoder_h265.c`) beyond intra-only by adding GOP control and low-delay P-frames (`NAL_UNIT_CODED_SLICE_TRAIL_R = 1`).
   - Normative SPS Short-Term Reference Picture Set (RPS) configuration with $\Delta\text{POC} = -1$ and DPB buffering (`max_dec_pic_buffering_minus1 = 1`).
   - Extended CABAC entropy engine (`hevc_cabac.h`/`.c`) to support both `initType = 1` (P-slices) and `initType = 2` (I-slices) per Rec. ITU-T H.265 Tables 9-5 through 9-30, adding new contexts for `cu_skip_flag`, `pred_mode_flag`, `merge_flag`, and `merge_idx`.
   - Dynamic per-8x8-CU temporal distortion evaluation: static/low-motion blocks code as zero-motion Skip CUs (`cu_skip_flag = 1`, `merge_idx = 0`), spending only ~2 bits per CU and reducing bitrate by >80-90% on typical streaming frames.
   - Comprehensive 30-frame sequence test in `test_hevc_encode.c` (IDR + static P-frames + dynamic P-frames + forced IDR), with full 30-frame verification via external FFmpeg reference decoder oracle.

2. **VA-API HEVC Configuration, Rate Control & Parameter Passing (DEVLOG §28)**:
   - Exposes `VAProfileHEVCMain` with `VAEntrypointEncSlice` in `bc250_QueryConfigProfiles` and `bc250_QueryConfigEntrypoints`.
   - Advertises HEVC-specific features and block size attributes (`VAConfigAttribEncHEVCFeatures` and `VAConfigAttribEncHEVCBlockSizes`) with 16x16 CTU and 8x8 min CB hierarchy.
   - Wires negotiated `VAConfigAttribRateControl` in `bc250_CreateContext` (`VA_RC_CQP`, `VA_RC_VBR`, `VA_RC_CBR`).
   - Demuxes `VAEncSequenceParameterBufferHEVC`, `VAEncPictureParameterBufferHEVC`, `VAEncSliceParameterBufferHEVC`, and misc rate control / framerate buffers in `bc250_RenderPicture`.
   - Wires dynamic GOP size, picture QP, force-IDR, bitrate, and framerate directly into `hevc_enc`.
   - Computes dynamic `slice_qp_delta` relative to the active PPS to ensure compliant CABAC initialization and dequantization across QP changes.
   - Added Step 12 integration test in `test_va_api.c` and dynamic QP validation in `test_hevc_encode.c`.

3. **Multi-Threaded CABAC/CAVLC Slicing via OpenMP (DEVLOG §26.8)**:
   - Slices are encoded concurrently using OpenMP (`#pragma omp parallel for schedule(static) if(num_slices > 1)`), cutting CPU entropy coding time from ~9.4 ms down to ~2.5–3.5 ms (+30% to +50% FPS throughput boost).
   - Gated macroblock context lookups guarantee zero cross-slice data races (§26.5 audit).
   - Independent slice RBSP staging with ordered sequential NAL assembly preserves exact bitstream compliance.
   - Added `h264_encoder_set_num_slices()` API, advertised 16 slices in `VAConfigAttribEncMaxSlices`, and added 4-slice parallel validation in `test_encode.c`.

4. **Thread Synchronization & Deadlock-Free Fence Wait (DEVLOG §26.6)**:
   - Implements a recursive driver mutex (`PTHREAD_MUTEX_RECURSIVE`) across all VA-API backend entry points in `va_backend.c`, eliminating TSan data races between FFmpeg's `encoder_thread` and `filter_thread`.
   - Decouples GPU fence waits in `bc250_SyncSurface()` by querying the submitted slot under lock and executing `gpu_compute_sync_slot()` unlocked to prevent cross-thread deadlocks.
   - Added Step 10 concurrent multi-threaded stress test in `test_va_api.c`.

5. **Rate Control Improvements & CQP Mode (DEVLOG §26.7, audit §4.4, §4.5)**:
   - Adds `RC_CQP` (Constant QP) mode so constant QP requests are honored without buffer fullness deviation or drift.
   - Wires negotiated `VAConfigAttribRateControl` from `bc250_CreateConfig` into `bc250_CreateContext` (`VA_RC_CQP`, `VA_RC_VBR`, and `VA_RC_CBR`).
   - Honors `initial_qp` from `VAEncMiscParameterRateControl`.
   - Feeds real GPU motion estimation SAD from `motion_estimation.comp` staging buffers into `rc_get_frame_qp()`, unblocking the VBR temporal complexity ratio adjustment.
   - Added unit and integration tests in `test_encode.c` and `test_va_api.c`.

## Type of Change
- [x] Bug fix (non-breaking change fixing an issue)
- [x] New feature (non-breaking change adding functionality)
- [x] Performance optimization (shaders, rate control, memory)
- [x] Documentation update

## Testing Checklist
- [ ] Tested on BC-250 hardware (or compatible Vulkan AMD GPU)
- [x] Unit tests pass (`ctest --test-dir approach1-compute-encoder/build`)
- [x] Multi-slice parallel test passes (`test_encode`)
- [x] Concurrent multi-threaded test passes (`test_va_api`)
- [x] H.264 bitstream verification passes with FFmpeg reference decoder
- [x] H.265 bitstream verification passes with FFmpeg reference decoder (30/30 frames)
