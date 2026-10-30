# Media and rendering architecture (0.4.5)

## Hosts

CyVR_UI `cyvr_android` and `runAndroidApp()` own the real Android 2D host. The launcher does not create an OpenXR session. Optional immersion shares the same C++ application and Vulkan UI. The render extension borrows the host's VkDevice/graphics queue. The 2D host uses Quest window controls; immersive controls and media have independent poses. State extensions retain navigation, document grants/URI, time, image settings, selected container tracks and the tracks panel across activity replacement.

## Serialized media worker

FFmpeg opens/probes/demuxes local files, authorized SAF descriptors and enabled network protocols. Cancellation interrupts opens/reads, network nested requests cannot escape to local files, TLS certificates are checked, and compressed packets are read on one worker. The APK enables built-in LGPL-compatible demuxers/parsers/decoders plus zlib; external decoder/protocol libraries are only available when explicitly bundled.

Each selected video track creates an Annex B filter for AVC/HEVC or a pass-through filter for other codecs. Android MediaCodec capabilities select hardware first. PRIVATE AImageReader images arrive as leased AHardwareBuffers, imported into Vulkan with producer fences and bounded caches. The presentation queue holds at most three images; seek creates a new decoder generation. Configure failure selects software decoding, and later hardware failure retries from the current position on seekable sources.

Software video uses FFmpeg and libswscale with a four-slot AHB pool, preserving ten-bit RGB for deep input. CPU writes complete before publication; render leases protect buffers while used. This fallback costs CPU conversion and memory bandwidth and is not a throughput promise. All heavy network/file decoding stays off the UI thread.

Audio uses FFmpeg + libswresample and AAudio shared low-latency stereo float output. A bounded SPSC block ring connects the worker and allocation-free callback. Blocks retain their timeline positions; gaps become silence and expired samples are dropped after underflow. The AAudio hardware timestamp drives video scheduling, with a monotonic fallback when no audio is present. Seek/track changes recreate decoders/clock and preserve play/pause. Track initialization errors leave video usable and remain visible in the tracks panel.

## Subtitles and container tracks

Demux metadata includes video/audio/subtitle indices, language/title, default/forced/accessibility dispositions, font attachments and chapters. A seekable-source track change cancels the old worker operation and reopens the same document/time using the new selection. External subtitles can be loaded separately. The state codec appends tagged extensions and keeps older records readable.

FFmpeg's subtitle API produces timed ASS events and/or palette bitmaps. The synchronized, bounded event queue is separate from video leases. Libass/FreeType/HarfBuzz/FriBidi rasterize text with styles, animation and embedded fonts; PGS/DVD/DVB bitmaps are composed with alpha. Raster output is cropped to its occupied rectangle and uploaded only when changed. In 2D a separate UI overlay binding samples it over the video. In VR a dedicated video-anchored quad remains present when the UI is hidden. Subtitle rendering does not process the entire source panorama.

## Colour and reconstruction

The hardware path keeps source dimensions and Android YCbCr conversion metadata. PQ/HLG are linearized to luminance, BT.2020 is converted to BT.709 primaries and a luminance-preserving tone curve maps into the SDR sRGB presentation path. Mastering peak metadata is used when present. This is not native HDR display output or Dolby Vision dynamic-metadata processing.

Native video uses source-size quad/equirectangular layers. Enhanced video projects the current predicted eye view before CAS, FSR1 EASU+RCAS or GSR1, preserving available source detail on each axis. Corrections and reconstruction use GPU passes; UI and captions remain separate. One command buffer/submit/completion wait covers projection and filtering per eye. Software fallback is explicitly the exception to the no-CPU-pixel-copy hardware path.

## Gaussian scenes

PLY/`.splat` and independent-frame `.splatseq` loading use a separate media path and the same play/pause/seek controls. A pinned MIT SplatKit backend borrows the existing Vulkan device through a small private context adapter. It performs visibility, per-eye projection, stable 32-bit GPU radix sorting and Gaussian rasterization. No Android reference view, vk-bootstrap device creation or proprietary SDK is linked. See [the implemented formats and budgets](gaussian-splatting.md).

## Controls and measurements

Immersive video starts with compact playback controls and hides them after three seconds without significant controller activity. Held gestures, active scrubbing and modal interactions postpone hiding. Waking consumes the initiating trigger gesture. A full-interface button restores the existing interface. Native-size media targets are independent of the controls panel.

The HUD separates nominal source FPS, decoded frames, distinct video submissions, app submissions and runtime refresh. Repeated VR redraws do not inflate source video cadence. GPU video timings exclude decoder/UI/compositor work; unknown values stay unknown. Gaussian synthetic GPU checks are not Quest performance measurements. See [validation](validation.md).
