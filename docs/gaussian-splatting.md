# Gaussian playback in 0.4.5

The player loads standard Gaussian PLY and 32-byte `.splat` scenes. PLY must be ASCII or binary little-endian and contain `x/y/z`, `scale_0..2` (log scale), `rot_0..3` (wxyz), `opacity` (logit), and `f_dc_0..2`. Optional `f_rest_0..44` supplies SH degrees 1–3. A plain XYZ point cloud is not a Gaussian scene. Compressed PLY, SPZ, SOG and deformation networks are not decoded by this profile.

The renderer uses the MIT SplatKit Vulkan backend at the commit/hash in `dependencies.lock.json`, through a player-owned borrowed-device adapter. It never creates another Vulkan instance/device or Android view. Visibility, SH evaluation, projection and stable full-32-bit depth radix sorting run on the GPU, independently for the current eye pose. Rasterization uses Gaussian ellipses and back-to-front blending on black. No LOD reduction or render-resolution reduction is requested by the player. The backend still applies its Gaussian support/opacity cutoffs and packs colour to RGBA8 and covariance/SH to half precision. These are representation approximations, not lossless rendering.

OpenXR targets use the runtime's recommended eye dimensions (or the selected output multiplier). The controls and scene have independent anchors. Recenter video, controller zoom and image position move the scene; moving the UI does not. The 2D host provides a perspective preview. Image corrections and Native/CAS/FSR/GSR use the existing Vulkan postprocessor.

## Independent-frame animation

`.splatseq` is a deliberately internal, UTF-8 sequence manifest. It is not an industry-standard Gaussian video codec. Example:

```text
CYVR_SPLAT_SEQUENCE 1
fps 30
frames/frame0000.ply
frames/frame0001.ply
frames/frame0002.splat
```

Each nonempty line after the header is a frame path relative to the manifest folder; `fps` accepts 1–120 and `#` introduces a whole-line comment. Absolute paths, traversal outside that folder and escaping symlinks are rejected. Frames must share a coordinate system. The first frame supplies a fixed centre/scale for the entire sequence; frames are not independently recentered. There is no interpolation between unrelated splat arrays.

Play/pause and timeline seek select the appropriate independent frame. Playback starts when the first rendered frame has been submitted, so pipeline preparation does not advance the animation. Loading runs on the serialized media worker; obsolete generations are cancelled and rendering retains its own frame until completion. Slow storage/decoding skips forward to the current sequence time rather than retaining an unbounded backlog.

Current limits: 1,000,000 splats/frame, 512 MiB encoded input/frame, 100,000 frames and a 1 MiB manifest. The CPU holds the newly decoded cloud and the cloud retained by rendering; GPU allocations are device-limited. Local manifests/relative files are supported. Single scenes also accept authorized Android document descriptors; SAF-relative/network manifests and sequence soundtracks are not implemented. GPU pipeline/world allocation failures are reported. Actual large-scene frame rate, startup time and thermal behaviour need Quest measurements.

## Validation

`cyvr_splat_probe` executes real Vulkan visibility, radix sorting, stereo camera translation and Native/CAS/FSR/GSR. It tests ASCII PLY, the `.splat` layout, malformed input rejection, sequence seek while paused, generation changes and compact/full/hidden controls. These synthetic fixtures establish function, not representative mobile performance. No Quest benchmark or worn-headset visual acceptance is claimed for this integration.

The original INRIA research renderer is not included. Data-format support does not import its noncommercial implementation. See the exact MIT backend, VMA and other notices packaged with the application.
