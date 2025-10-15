# Project Review: Top Improvement Opportunities

## 1. Establish automated test coverage
- No automated tests are currently provided (`tests/tests_here` is empty and the `Makefile` exposes only build/clean targets). Adding unit coverage for configuration parsing, CLI option precedence, and recorder enable/disable flows would lock in the stripped-down behaviour and guard against regressions.

## 2. Validate plane selection before committing modesets
- `atomic_modeset_maxhz` logs the chosen connector but immediately programs the user-specified plane ID without confirming that the plane supports the target CRTC/format (`src/drm_modeset.c` lines 168-247). Reintroducing a lightweight plane enumeration (e.g., scanning for overlay planes compatible with the resolved CRTC and `DRM_FORMAT_NV12`) would prevent hard-to-debug failures on boards whose plane numbering differs from the default.

## 3. Reduce per-packet allocations in the UDP ingress path
- The receiver thread allocates a new `GstBuffer` and copies payload bytes for every UDP datagram before pushing into the GStreamer pipeline (`src/udp_receiver.c` lines 75-83). Switching to `gst_buffer_new_wrapped_full` or maintaining a buffer pool would shrink allocation pressure and latency when ingesting high bitrate streams.

## 4. Eliminate redundant access-unit copies in the recorder
- Recording currently duplicates every appsink payload into heap memory before enqueueing it (`src/video_recorder.c` lines 372-399). Ref-counting the original `GstBuffer` (or extracting data via `gst_buffer_map` only when emitting) would lower peak memory usage and improve write throughput, especially for 4K streams.

## 5. Preserve RTP timestamps when feeding the hardware decoder
- The appsink hands raw Annex-B data to `video_decoder_feed` without forwarding the sample PTS, and the decoder synthesizes timestamps from `get_time_ms()` (`src/pipeline.c` lines 114-127; `src/video_decoder.c` lines 660-679). Threading the real RTP/GStreamer timestamps into the decoder would align display/recording timelines and simplify future AV sync diagnostics.
