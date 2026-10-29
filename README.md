# CyVR Player

A native C++17 / Vulkan video player for Meta Quest, using the local [CyVR_UI 0.4 foundation](../CyVR_UI). Alpha version: **0.4.4-alpha.1**.

The app opens in a **real Android 2D window**, without creating an OpenXR instance or session. Horizon OS owns moving, resizing and closing the window, including multitasking. **Enter immersive** explicitly opens the optional VR host for cinema, stereoscopic and panoramic playback. **Return to 2D** or Android Back restores the panel. Both hosts use the same C++ interface, themes, English/French translations, browser and player model.

## Build

From PowerShell in this folder:

```powershell
python tools/build_app.py --target all
python tools/build_app.py --target android --no-immersive
```

The second command builds an alternative 2D-only APK with `CYVR_ENABLE_IMMERSIVE=OFF` and `AndroidAppConfig::immersiveEnabled=false`, without the OpenXR loader, XR runtime queries or immersive activity.

| Output | Purpose |
| --- | --- |
| `dist/android/CyVR_Player-debug.apk` | Native 2D launcher with optional immersive video |
| `dist/android/CyVR_Player-panel-debug.apk` | Exclusively 2D alternative |
| `dist/windows/CyVR_Player.exe` | Vulkan UI inspector and media inspection; hardware playback runs on Quest |

Both APK variants keep `com.cyvr.player` and the existing development signing key, so `adb install -r` preserves app files and grants. They replace one another. Android API 32 and Horizon OS v69 or later are required. These are development builds, not store release packages.

For the Android Alpha package, run `python tools/build_ffmpeg_android.py --alpha` then `python tools/build_app.py --target android --alpha`. This separate Release profile produces `dist/android/CyVR_Player-0.4.4-alpha.1-arm64.apk`, disables Android debugging, and retains the existing development signing identity for upgrades. Alpha builds are for testing, not store submission. Use `python tools/verify_app.py --alpha` to validate the package. Release assets include the exact source snapshot of the player and its local MIT-licensed CyVR_UI foundation, dependency sources and build instructions. `CYVR_UI_ROOT` can select that supplied foundation when rebuilding.

Prerequisites are the existing Android SDK (NDK 27.2.12479018, CMake 3.22.1, Build Tools 35.0.1, platform 34), JDK, LLVM/Windows C++ SDK, and the sibling `../CyVR_UI` checkout. `ANDROID_HOME`, `JAVA_HOME` and `CXX` can override tool locations. `CYVR_UI_ROOT` is the corresponding CMake setting. The local foundation includes the small reusable GPU/lifecycle extension hooks described in [the integration notes](docs/interface.md); retain those local changes when updating it.

For a fresh dependency cache:

```powershell
python tools/fetch_ffmpeg.py
python tools/prepare_android_media.py
python tools/build_ffmpeg_android.py
```

## Use and quality

Open **CyVR Player** from Home or Overlay. Browse the app's own external folder, use **Allow a folder** for Android SAF access, or enter an HTTP(S), HLS or RTSP address in **Open stream**. Android VIEW intents are also supported. Display settings select mono, SBS or top/bottom, half-width/height packing, eye inversion and flat/180°/360° projection. Panoramic presentation requires immersion.

On the **Playback** page, **Hide interface** enters video-only viewing without pausing playback. In 2D the picture fills the available window while preserving its aspect ratio. Tap anywhere, or press Enter, Space or Back, to restore the controls. In immersion the main panel, dock, HUD, controller/hand meshes, rays and cursors disappear; either trigger, a tracked-hand pinch, the right controller's **A** button or Back restores them. The first waking gesture is consumed, so it cannot also activate a button underneath. **F11** toggles the interface when using a physical keyboard. Hiding is temporary: a newly opened video, a media error or a replacement activity shows the controls again. Quest system window management remains available in 2D.

In immersion, the video has its own scene placement. You can move and release the controls panel to either side without moving or rotating the cinema screen or panorama. This applies to Native, CAS, FSR and GSR, including paused/video-only viewing. **Display → Video format → Recenter video** places the video in front of your current view and leaves the controls panel in place. Video zoom and position controls remain available. Placement is retained within the current immersive host; a new immersive session starts with a new placement. Initial tracking fallback may be head-relative until a valid local pose is available.

The **Playback timeline** shows the current position and total duration. Click or drag the thumb to select a time; the displayed time previews the selection while dragging, and one seek is committed on release. Playback keeps its previous play/pause state. Losing focus, hiding controls or cancelling the gesture discards the unfinished selection. Live/unseekable sources and sources without a known duration show a disabled track. The native decoder retains the final preroll frame when a seek reaches the end, so the last available picture can still be presented.

In **Native** mode video stays at its **decoded source dimensions** through the MediaCodec → PRIVATE AImageReader → AHardwareBuffer → Vulkan path; pixels are not copied through the CPU. VR uses dedicated native-size compositor video layers. The 2D preview fits the window while retaining its full-size source texture. Enhancement reconstructs the displayed window or eye view; it never reduces decoder resolution. The panel renders at the Android surface size. Immersive scene targets use the runtime's recommended resolution, with no former 1024-pixel reference cap. This does not promise a one-to-one mapping to physical display pixels through the headset optics, or recover detail absent from a source.

Navigation, document URI, URL draft, stereo/projection settings, playback position, image corrections, filter, scale and zoom settings, and pause state are saved in private app storage across activity replacement and process restart. Shared CyVR_UI preferences preserve language and theme. Existing SAF grants and app files are retained.

## Image controls and previews

The library shows **static 160 x 90 video thumbnails**, decoded on one background worker with one software thread. The worker only requests the current seven rows, keeps at most 32 entries in memory (including failed files), cancels obsolete requests and pauses thumbnail requests during playback. It supports local files and authorized SAF descriptors. It does not auto-fetch stream URLs. Unsupported or over-budget files keep a type placeholder. No continuous animated preview is decoded.

**Display → Image corrections** provides exposure (−2 to +2 EV), contrast, saturation, gamma, zoom (1–4x), and horizontal/vertical position. Reset restores neutral values. In 2D, zoom crops within the video, preserving the aspect ratio and eye boundaries. In immersive flat cinema, zoom enlarges the physical screen while retaining the complete picture; position moves that screen. Panorama zoom is a digital per-eye crop. Changes also redraw an already-paused frame without resuming decoding.

Controls remain available through the on-screen sliders/buttons. In immersion, the **left thumbstick** seeks ±10 seconds once per deflection; the **right thumbstick up/down** adjusts zoom. Hold **grip + trigger**, then move the hand up/down to zoom directly, including with the interface hidden. A trigger alone still restores hidden controls. On a physical keyboard, left/right seek ±10 seconds and up/down zoom; the first key restores hidden controls. The Windows inspector also accepts the mouse wheel for zoom.

**Display → Image quality** offers:

| Mode | Processing | Output |
| --- | --- | --- |
| Native (default) | Hardware YCbCr conversion, optional corrections in the same pass | Source dimensions |
| FidelityFX CAS | AMD contrast-adaptive sharpening in linear RGB | Display target |
| FSR 1 | AMD EASU followed by RCAS, in perceptual RGB | Display target |
| Snapdragon GSR 1 | Qualcomm spatial upscaling/sharpening in one filter pass, perceptual RGB | Display target |

Filters affect **only the video**, so the native UI stays sharp. SBS/TB taps clamp within each eye. Sharpening strength is adjustable. FSR/GSR are spatial filters; they do not invent missing source detail or generate frames. GSR 2/FSR 2 are not used because the decoded video path does not provide their required scene depth and motion data. GSR 1 is pinned from [the upstream project](https://github.com/SnapdragonGameStudios/snapdragon-gsr/tree/d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e/sgsr/v1), with BSD-3-Clause notices packaged.

The default native path keeps the complete decoded source in one conversion/correction pass. Enhancement first projects **only the displayed picture**: the fitted image in 2D, or the current predicted field of view for each eye in VR. CAS sharpens at the output resolution. FSR/GSR reconstruct from the available source detail in that view (including zoom); their input never exceeds the output. Each axis retains its available detail independently, including half-width/height stereo; the projection FOV and UV mapping preserve geometry even when the intermediate has a different aspect ratio. The output defaults to **Headset / window**, meaning the runtime's recommended eye resolution or actual window picture size. Optional 1.25x, 1.5x and 2x choices multiply that display target. All controls remain available, including with large panoramas. The previous fixed 16-megapixel filter cap is removed. A Vulkan limit/allocation failure preserves the requested settings, reports an error, and temporarily uses native rendering until a filter/scale request retries it.

Conversion/projection and all filter passes now share one command buffer, one queue submission and one completion wait per eye. CAS/GSR need one RGB input; FSR additionally needs an EASU intermediate. Playback does not read pixels through the CPU. There is no claim of sustained 4K/8K or thermal performance without headset measurements.

On **Playback → Performance**, the HUD separates the file's nominal frame rate, distinct video frames processed by the GPU, usable decoder output, app frame submissions and the XR runtime's reported refresh rate. Repeated VR redraws do not inflate the video count. It also shows late drops, queued frames, rendering errors, GPU video mean/p95 and CPU work including waits. GPU timings use Vulkan timestamps when supported, cover video conversion/projection/filter passes (summed across both VR eyes), and exclude UI, compositor and decoding. Unknown measurements remain **Not measured**. Event rates age out when stalled, and pause/seek clears old cadence history. These measurements do **not** measure frames actually displayed by the Quest compositor.

See [processing details and limits](docs/image-processing.md).

## Implemented scope

The current APK plays **AVC/HEVC SDR video without audio output**. It includes an asynchronous paginated file browser, URL keyboard, play/pause/seek, hardware decoder selection and TLS verification using Android system certificates. HTTP AVC, HTTP HEVC and HLS AVC have been exercised on Quest 3. Real RTSP/live sessions, provider-specific SAF behavior and dynamic format changes still need qualification.

Audio output/A-V synchronization, subtitle display, HDR tone mapping and Gaussian Splatting loading/rendering remain unimplemented. CAS, FSR 1 and Snapdragon GSR 1 are applied by real Vulkan fragment pipelines; see the image controls below. There is no claim of universal format support or sustained 8K performance.

## Verify

```powershell
python tools/verify_dependencies.py
python tools/generate_player_locales.py --check
python tools/adapt_gsr1.py --check
python tools/verify_app.py
python tools/verify_app.py --panel-only
python tests/quest_app.py --serial QUEST_SERIAL
python tests/quest_hosts.py --serial QUEST_SERIAL
python tests/quest_gpu.py --serial QUEST_SERIAL
python tests/quest_image.py --serial QUEST_SERIAL
python tests/quest_view.py --serial QUEST_SERIAL
```

`build_app.py --target windows` runs six CTest suites, including application state transfer, malformed state handling, pointer navigation, translations, independent 24 fps video / 72 Hz redraw counting, stalls and the shared UI core. The GPU suite covers 134 cases, including identical output from separate and combined submissions for CAS/FSR/GSR, the real projection shader before filtering in mono-eye/SBS/TB/cinema/180°/360°, transparent edges and preserved detail when only one axis needs upscaling. `quest_hosts.py` requires `Exemples/avc.mp4` in the app's external files folder; it uses real Android input to test resizing, resume and both presentation transitions. For the installed 2D-only variant, add `--panel-only`. Network tests remove their own adb reverse when complete. Device tests change the current document, navigation and UI settings; they do not clear app data.

See [validation and remaining measurements](docs/validation.md), [host integration](docs/interface.md), [media architecture](docs/architecture.md), [sources and streams](docs/sources-et-flux.md), [dependency review](docs/bibliotheques.md) and [Gaussian Splatting roadmap](docs/gaussian-splatting.md).

## License

Original CyVR Player source code, shaders, build scripts and documentation are licensed under the **GNU General Public License, version 3 only** (`GPL-3.0-only`), unless an individual file states otherwise. Copyright (C) 2026 MrMybal. See [LICENSE](LICENSE) for the full terms and [NOTICE](NOTICE) for the project notice and third-party scope. The program is provided without warranty.

Commercial use and sale are permitted. Redistributing the application, including modified versions, requires complying with GPLv3, including its corresponding-source requirements. Keeping this development repository private does not remove those obligations when binaries are distributed. The build packages the project's GPLv3 text and notice in `assets/licenses` for both Android and Windows.

Third-party components retain their own copyright notices and licenses. [CyVR_UI](https://github.com/MrMybal/CyVR_UI), CAS and FSR 1 use MIT; Snapdragon GSR uses BSD-3-Clause; the Android FFmpeg build uses shared LGPL-3.0-or-later libraries with GPL/nonfree disabled; Mbed TLS uses Apache 2.0; OpenXR uses its supplied open-source license. Pinned sources/checksums are recorded in `dependencies.lock.json`; packaged notices are under `assets/licenses`. Redistribution must retain the applicable notices and satisfy the dependencies' own obligations, including FFmpeg source/relinking requirements. No proprietary media or rendering SDK was added.
