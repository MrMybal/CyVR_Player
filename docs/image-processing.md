# Image processing and browser previews

Version 0.4.4 keeps video decoding in MediaCodec and rendering in Vulkan. No new proprietary SDK, CPU readback in playback, temporal reconstruction or generated frames were added.

## Processing paths

The Android renderer retains one completed `AImage` lease so an image can be redrawn while paused. It consumes the acquisition FD once, waits for every GPU read, and transfers FOREIGN ownership around each read. Together with the three queued source images, this stays within the reader's six-image limit. Imported AHBs remain bounded to 64 and are cleared on epoch changes. The retained lease is released on source stop and device shutdown.

Native mode performs YCbCr conversion, BT.709 SDR inverse transfer, correction and per-eye digital crop in one pass. The attachment is sRGB. Neutral controls do not add an intermediate image or lower decoder resolution.

CAS/FSR/GSR first project the displayed view into an sRGB sampled texture. In 2D this selects a single eye and fits the current picture size. In VR each eye uses the runtime-recommended output dimensions and its predicted pose/FOV; a projection layer returns the result to OpenXR. The full panorama is sampled without allocating an enlarged full-panorama RGB texture. CAS uses the display target as its input; FSR/GSR choose a smaller input when the source or visible crop has less detail than the display. Zoom is included in that estimate. Input width and height are bounded independently by available detail and output dimensions. They do not share the former minimum scale ratio: that ratio discarded valid detail on the other axis (for example, 960×1080 half-SBS detail became 960×540 before a 1920×1080 output). Non-square intermediate pixels do not distort the picture because projection uses normalized UVs and explicit FOV, and the filters receive both input dimensions. Sampling that texture yields linear RGB. CAS operates in linear RGB. FSR 1 and GSR 1 operate in a perceptual gamma-2.2 domain, then return to linear RGB for corrections and the sRGB attachment. FSR uses EASU and RCAS in separate passes; GSR combines reconstruction and sharpening in one filter pass and does not allocate the EASU intermediate. All processing is spatial and SDR. PQ/HLG HDR-to-SDR tone mapping runs before these spatial filters. They do not recover missing source detail or interpolate frames.

Each mode has specialized fragment pipelines so unused filter branches are eliminated. Uniform/push-constant changes apply exposure, contrast, saturation, gamma, crop/zoom/pan and strength without pipeline recreation. Changing algorithm or scale recreates the processing resources. During continuous zoom, the previous reconstruction input is retained until the zoom has been stable for 250 ms, then the detail estimate is applied once; visual zoom itself remains immediate. Conversion/projection and filter passes are recorded in one command buffer and submitted together, eliminating the CPU wait between stages. The AImage lease remains retained until that single completion fence finishes, before releasing the XR swapchain. This remains conservative synchronization; sustained throughput/thermal qualification is still required.

## Stereo and geometry

All source taps clamp within one eye. CAS uses local eye pixel coordinates, EASU constants describe one eye, and FSR/GSR gather adapters clamp individual taps before adding the packed-eye origin. This prevents opposite-eye contamination at SBS/TB seams and preserves the source during upscaling. The view reconstruction path selects the eye before filtering, so its intermediate texture is not stereo-packed. Legacy packed filter diagnostics reject odd packed dimensions. Eye inversion selects the source eye during view projection and also redraws paused 2D frames.

In the 2D host the processed picture is aspect-fitted to the window and zoom digitally crops its selected eye. In native flat immersion the source-size quad is enlarged/moved. Enhanced flat immersion reconstructs the visible portion of that same screen, including alpha outside it. Panoramic immersion applies a digital crop. Native panoramic layers retain the complete decoded texture. The projection shader extends edge RGB beneath zero alpha at the cinema and VR180 boundaries. The alpha mask still defines the visible area; reconstruction/sharpening no longer sees an artificial black border and blends it into the video. UI textures and text are separate from the filters. Shared CyVR_UI now has independent atlas and video texture bindings, so thumbnails cannot replace playback textures.

## Bounds and saved settings

Settings sanitize non-finite and out-of-range values. Exposure is −2…+2 EV, contrast 0.5…1.5, saturation 0…2, gamma 0.5…2, zoom 0.25…4 in VR180 (1…4 in cinema/360°), pan −1…+1 and strength 0…1. Output choices are the headset/window target (default), then 1.25x, 1.5x and 2x that target. They are always selectable. There is no fixed 16,777,216-pixel filter cap; Vulkan image dimension/allocation limits still apply. A failure preserves the chosen algorithm and factor, reports an explicit error and temporarily uses native rendering; another algorithm/factor request retries processing.

The IMG2 tagged settings extension in `player-state.bin` preserves existing base records. Older IMG1 records retain corrections/algorithm but reset their source-relative factor to the new default display target. New settings survive process/activity replacement. Hiding the interface remains transient. Position-only checkpoints are now spaced five seconds apart to reduce render-thread file I/O; user changes and lifecycle transitions still save promptly.

## Performance measurements

`Playback → Performance` opens the player HUD in either host. Source nominal FPS comes from FFmpeg's frame-rate estimate and is only metadata (especially for VFR). Decoder output counts usable queued frames after preroll. Video-to-GPU counts a distinct `(decoder epoch, PTS)` only after successful conversion/filter completion; repeated VR projections and paused corrections do not create new video frames. Scheduled/dequeued frames are counted separately from successful GPU work. Late drops count obsolete frames replaced by newer due PTS; they do not include all decoder/compositor losses. App submissions count successful Android presents or OpenXR frame ends with nonempty layers, not physical display events.

Rates use a two-second timestamp history with a minimum warmup, become zero after half a second without events, and are reset on pause/seek. Unknown rates show `Not measured`. The deterministic core tests distinguish 24 fps video from 72 Hz redraws, invalidate stale/invalid histories and retain long timing stalls.

Optional GPU timestamps span conversion/projection and filtering and use the device's timestamp valid bits and period. Query availability is read only after the existing completion fence; telemetry adds no wait-for-query call. Unsupported queries remain unknown. The HUD reports the rolling mean and p95 of the last 120 successful video operations, summing both eyes for VR. Timing history resets when the decoder epoch or filter/scale request changes. It excludes UI, decoding, compositor work and actual display visibility. CPU work includes setup, submission and fence waits. See [Vulkan timestamp queries](https://docs.vulkan.org/spec/latest/chapters/queries.html#queries-timestamps).

`PERFORMANCE` Android log records distinguish these counters every two seconds without logging source paths/URLs; unavailable values use −1. `VIDEO_COMPLETE` reports `decoded`, `scheduled`, `gpu_submitted` and `late_drops` separately. These counters cannot certify actual Quest compositor cadence.

## Thumbnails

One asynchronous worker decodes the first available video frame of visible local/SAF files, using one software thread and a decoder pixel limit. It scales to a letterboxed 160×90 RGBA thumbnail. A maximum of seven visible rows is requested; the LRU cache holds at most 32 successful/failed entries. Its 160×630 atlas uses less than 0.4 MiB, and cached pixels use at most 1.8 MiB, excluding FFmpeg's temporary decode/reference buffers. Requests are cancelled when navigating or starting playback. No continuous animated preview runs and network URLs are never automatically requested. SAF providers may themselves perform remote I/O and still need provider-specific qualification.

## Open-source provenance

AMD CAS and FSR 1 remain MIT dependencies. GSR 1 comes from `SnapdragonGameStudios/snapdragon-gsr`, commit `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e`, under BSD-3-Clause. The original shader/license are byte-identical to the upstream Git blobs and SHA-256 locked. `tools/adapt_gsr1.py` reproducibly adapts only shader I/O, entry point, texture sampling and controls; the original kernel is preserved separately. The VR edge threshold is 4/255 and the adjustable edge sharpness maps to 1…2. Original notices are packaged in both APKs and the Windows distribution.

GSR 2 was reviewed but not integrated: its documented color/depth/velocity/history inputs are absent from decoded video. See [GSR 1 integration](https://github.com/SnapdragonGameStudios/snapdragon-gsr/tree/d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e/sgsr/v1) and [GSR 2 inputs](https://github.com/SnapdragonGameStudios/snapdragon-gsr/tree/d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e/sgsr/v2).

## Reproducible checks

`build_app.py --target windows` runs eleven CTest suites with CyVR_UI 0.8.0. The GPU suite checks every pixel for constant-color preservation and opposite-eye isolation in native/CAS/FSR/GSR, with zoom/pan, then checks neutral/exposure transfer, grayscale conversion, center crop and effective sharpening on a detailed low-contrast pattern (1,060 cases, including spatial interpolation/noise/detail checks, VR180 zoom-out, Ambilight, and pixel-identical separate/combined submissions, actual projection before filtering in SBS/TB and all view types, transparent-edge color preservation, and anisotropic detail preservation for FSR/GSR). Application tests cover asynchronous thumbnail production, correction gestures, transform bounds, grip/trigger consumption, timeline seeking, new-state transfer and old-state migration.

```powershell
build/app-windows/cyvr_image_probe.exe build/app-windows/assets --benchmark
python tools/adapt_gsr1.py --check
python tools/verify_dependencies.py
python tests/quest_image.py --serial QUEST_SERIAL
python tests/quest_gpu.py --serial QUEST_SERIAL
```

The optional benchmark records filter-only wall time (including CPU submission/fence wait) and optional GPU timestamp time over twelve measured frames after warmup. It excludes conversion, decoding, UI and OpenXR composition. Desktop values must not be treated as Quest throughput. A worn-headset test is still needed for physical grip/trigger motion, binocular comfort, seams/poles, color reference accuracy, long high-bitrate video and thermal/battery behavior.


### 0.4.3 rendering regressions

The new RGB projection probe executes the same `video.frag` SPIR-V and shared 128-byte `VideoPush` layout used by Android before the real filter passes, with encoded UNORM fixture input and sRGB reconstruction attachments. It covers both eyes in SBS/TB, single-eye 2D views, flat cinema, front/rear 180° and 360°, alpha coverage, orientation, and neutral BT.709-to-sRGB transfer. This is GPU shader validation on the PC, not a validation of Android AHB import or OpenXR compositor presentation.

Two failures were reproduced before fixing them: FSR changed a constant cinema video's edge color by up to 7 byte levels, and horizontal-only reconstruction reduced a 128×128 source to 128×64, erasing alternating vertical detail (measured contrast 0). The corrected path retains all 128 source rows. The regression requires retained contrast for both FSR and GSR, and unchanged constant colors within visible projected edges. Headset-specific visual/performance qualification remains necessary.

### 0.4.5 HDR and Gaussian integration

PQ and HLG are linearized and tone mapped into SDR sRGB before reconstruction. Forty additional GPU reference checks compare five input levels for both transfers through Native/CAS/FSR/GSR, including finite/monotonic output. These shader checks do not certify Android ten-bit import or Quest panel colour. Gaussian per-eye targets use the same postprocessor and full runtime target dimensions; synthetic Vulkan visibility/sorting/rasterization and stereo camera translation are tested separately.

## 0.4.7 curved cinema and Ambilight

Native flat/cylinder video retains the original decoded texture. Cylinder radius is screen arc width / curvature radians, with the front midpoint anchored; per-eye filtered projection solves the same ray/cylinder intersection. Movie geometry is independent of the controls panel. Optional OpenXR cylinder layers are requested only when advertised; the video shader provides a curved fallback when absent. The current Quest 3 exposes cylinders. Caption and halo cylindrical placement use that extension; the fallback surround/caption layers remain planar.

Ambilight is optional and off by default. A separate hardware-buffer conversion/correction pass samples 64 × 36 texels, averaging both packed stereo eyes in linear light after HDR transfer/tone mapping. It ignores digital zoom/pan because cinema zoom/position change physical geometry. A small shader computes a soft 128 × 128 field from clamped, blurred edge samples, optionally detecting letterboxing and applying inset. Controls affect glow intensity, spread, saturation and exponential temporal smoothing. History is RGBA16F, preventing 8-bit accumulation from stalling on residual colours. The result is converted to the sRGB swapchain and composed with premultiplied alpha behind the unchanged movie.

Capture and halo updates are bounded to 30 Hz. A paused image needs no recapture unless its corrections/packing change; the temporal history settles for five time constants. Disabled Ambilight issues neither capture nor glow draws. No video pixels are read back by the runtime path. Optional resources are cached until host/device teardown. GPU capture and glow timings have separate technical logs and are excluded from the video-performance HUD. Test-only readback validates the actual shader output on PC and Adreno.

The effect is a composited colour surround, not physical room illumination. It applies to planar/curved cinema movies, not VR180/360 or Gaussian scenes. Automatic bar detection may misclassify very dark picture edges; disable it or adjust inset when needed. Tiny diagnostic timings do not qualify total playback throughput or thermals.

## 0.4.8 decoded-image cache

Some decoder output pools contain more than twelve hardware buffers. The previous importer cleared its complete cache at that threshold and recreated a shader pipeline for every subsequent image. The importer now retains up to 64 images, evicts only the least recently used entry, and reuses a Vulkan pipeline compilation cache. Epoch changes still discard old imported images; no video downscale, filter substitution or decoder/frame-rate cap is introduced. Native, CAS, FSR, GSR and Ambilight's capture use the same importer. Optional stage diagnostics separate import/compilation, GPU completion, native compositor acquisition and host CPU work.

The `cyvr_video_cache_probe` Android tool exercises distinct original RGBA hardware buffers, verifies image pixels, and checks generation invalidation and the 64-entry bound. Cache timings describe this diagnostic workload and do not establish full application FPS. `cyvr_video_gpu_probe` separately exercises actual MediaCodec/video-clock playback.

## 0.4.11 source filtering and CyVR_UI 0.8.0

The Original/Bilinear/Bicubic selection is implemented in the decoded-source projection shader, before SDR/HDR transfer and corrections. Manual normalized taps support external YCbCr samplers without requiring hardware linear-conversion support. Catmull–Rom uses 16 taps with ringing clamped to its sampled range. Optional edge-aware spatial denoising and detail share a Gaussian 3×3 neighborhood. Denoising weights small encoded RGB differences; detail suppresses small residuals, with at most 2% encoded overshoot. These controls are spatial, without motion estimation or temporal history. All taps clamp within the cropped eye; no opposite-eye samples or alpha extrapolation are introduced.

All optional source filters are neutral/off by default. Enabling one selects the displayed-view path, including when the upscaler is Native: no enlarged full-panorama filter allocation is required. CAS/FSR/GSR receive the filtered projection once, then apply their existing reconstruction/strength and color corrections. The new values occupy unused fields in the existing 128-byte projection push constants. Filter changes redraw paused pictures without importing the retained AHB again. A tagged FLT1 state extension preserves older checkpoints and defaults its missing fields to off.

CyVR_UI 0.8.0 retains the modeled scene in a static GPU vertex buffer, with its PBR assets and triangle-baked lighting. The player explicitly includes these resources for hybrid builds, validates their source/asset checksums before building, and excludes them in strictly 2D builds. Foundation GPU tests exercise scene parallax, floor anchoring, scene-backed glass, depth, AR alpha, materials, optional panoramas and descriptor rebinding. They are also available as `cyvr_foundation_gpu_probe` on Android. These checks establish rendering behavior, not sustained playback FPS or thermal performance.

## 0.4.12 browsing, controls and placement

The preview atlas remains seven 160×90 cells; the card page uses the first six. Atlas upload is independent of the borrowed video binding. Playback no longer clears cached previews. Visible-file requests retain successful cache entries, retry failures at bounded intervals and invalidate on Refresh. No failed empty frame occupies a success-cache slot. One background worker uses Android provider thumbnails or scaled extraction, then FFmpeg with a 64-Mpixel input limit, one decoder thread and a bounded packet/deadline loop. Decoder EAGAIN preserves the pending packet. Thumbnails are resized to 160×90; full video resolution is unaffected. Android provider extraction uses a cancellable request and a software-compatible ARGB bitmap before RGBA packing. JNI thumbnail extraction stays on the worker. Storage-location enumeration reads persisted permission records and document IDs without querying provider metadata on the UI thread.

Manual scene offsets and rotations compose with the independent movie anchor. STAGE is created as an application-owned space when supported and destroyed before the XR session. Reference changes are associated with the space that produced them, avoiding a delayed LOCAL change being applied after migrating to STAGE. A known change uses the inverse previous-origin pose at its effective time. If unavailable, physical placement needs user adjustment; no head-locking is forced. The explicit Place in front command uses the complete head orientation, while initial default placement retains the existing horizon behavior. Panorama sampling remains at infinity and uses rotations; caption orientation follows the same anchor.

Application state adds a BRS1 extension for list/card mode, favorite folders with parent navigation, manual scene controls and volume. Older checkpoints default to list view, neutral placement and unity volume. The volume target is atomic; ramping runs inside AudioQueue's callback with no allocation, lock or timeline change. The initial decoder output starts at the stored gain, so a muted new stream does not emit a unity-gain transient.
