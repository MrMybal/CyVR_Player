<table width="100%">
  <tr>
    <td width="300" align="left" valign="top">
      <a href="docs/user-guide.md">Manual</a><br>
      <a href="https://github.com/MrMybal/CyVR_Player">Repository</a><br><br>
      <sub>README</sub><br>
      <a href="README.md">English</a> · <a href="README.fr.md">Français</a>
    </td>
    <td width="400" align="center" valign="middle">
      <img src="assets/branding/cyvr-player-logo-orange-coral-v1.png" alt="CyVR Player" width="128">
    </td>
    <td width="300" align="right" valign="top">
      <p><sub>Application license</sub><br><a href="LICENSE"><strong>GPL-3.0-only</strong></a></p>
      <p><sub>Version</sub><br><a href="docs/releases/0.4.15-beta.1.md"><strong>0.4.15-beta.1</strong></a><br><strong>BETA</strong></p>
      <p><a href="https://github.com/MrMybal/CyVR_Player/releases/download/v0.4.15-beta.1/CyVR_Player-0.4.15-beta.1-arm64.apk"><strong>Download</strong></a><br><sub>Meta Quest · Android ARM64</sub></p>
    </td>
  </tr>
</table>

<h1 align="center">CyVR Player</h1>

A native **C++17 / Vulkan video player for Meta Quest**, for local media, network streams, stereoscopic films and VR180/360 video. Start in a real Android 2D window and enter optional immersion when you want it.

> **BETA · 0.4.15-beta.1**
>
> Under validation before stable availability. Compatibility depends on the media and headset; check the documented limitations and report problems. Read the [version notes and known issues](docs/releases/0.4.15-beta.1.md).

[![Playback with video preview, timeline and volume](docs/images/playback.png)](docs/images/playback.png)

## About CyVR Player

Open an authorized Android folder, browse video previews in tiles or a list, or enter an HTTP(S), HLS or RTSP stream address. Playback brings together the timeline, volume and video, audio and subtitle track selection.

In immersion, position the movie independently of the controls, curve the cinema screen, choose an environment or enable Ambilight. The compact bar hides after three seconds without interaction; a deliberate press restores it. The full interface stays visible while adjusting settings. The shared interface uses **CyVR_UI 0.9.1**, licensed under MIT; English is the default language and French is available in Settings.

| Library and previews | Real-time image quality |
| :---: | :---: |
| [![Tile browser with folders and video previews](docs/images/library.png)](docs/images/library.png) | [![Native, CAS, FSR and GSR with smoothing, denoising and detail controls](docs/images/image-quality.png)](docs/images/image-quality.png) |
| **Cinema geometry** | **Subtitle appearance** |
| [![Cinema screen size, distance, curvature and rotation controls](docs/images/cinema.png)](docs/images/cinema.png) | [![Subtitle size, color, outline, shadow and style controls](docs/images/subtitles.png)](docs/images/subtitles.png) |
| **Interface window** | **Compact immersive controls** |
| [![Window movement, resizing and placement preferences](docs/images/window-controls.png)](docs/images/window-controls.png) | [![Compact toolbar with drag handle, timeline and playback controls](docs/images/compact-controls.png)](docs/images/compact-controls.png) |

These are captures of the shared C++/Vulkan interface in the Windows inspector, using demo media. The compact immersive bar is shown separately from the headset scene. See [capture details and image credits](docs/images/README.md).

## Features

- **Video formats:** flat, SBS/top-bottom stereo and equirectangular VR180/360; optional auto-detection with manual override.
- **Media sources:** files, seekable DVD/Blu-ray images, HTTP(S), HLS and RTSP. Track and chapter options depend on the source.
- **Image control:** Native, CAS, FSR 1 and GSR 1, plus smoothing, noise reduction, detail, color corrections and zoom.
- **Immersive placement:** independent movie grab/rotation, grip-based window movement/resizing and compact playback controls.
- **Subtitles:** text and supported bitmap tracks, external files, appearance and spatial placement settings.
- **Gaussian scenes:** Gaussian PLY, `.splat` and the internal `.splatseq` sequence format.

Hardware decoding is used when available, with software fallback. Supported codecs, protected-disc exclusions, HDR-to-SDR behavior and performance limits are detailed in the [user guide](docs/user-guide.md). There is no universal-format or sustained high-resolution performance guarantee.

---

## Installation

The published APK targets **Meta Quest on Android ARM64**, with Android API 32 or later and Horizon OS v69 or later for 2D/immersive transitions. Its manifest targets Quest 2, Quest 3S, Quest 3 and Quest Pro; device checks have been performed on Quest 3. The Windows inspector is a source-built development tool; this release does not include a Windows application download.

1. [Download the published application](https://github.com/MrMybal/CyVR_Player/releases/download/v0.4.15-beta.1/CyVR_Player-0.4.15-beta.1-arm64.apk).
2. Sideload the APK with your usual tool or `adb install -r CyVR_Player-0.4.15-beta.1-arm64.apk`. Updating with the same signing identity preserves app data and grants.
3. Open **CyVR Player** from Home or Overlay. Use **Open a folder** to grant a folder, or **Open stream** for a network address. **Enter immersive** opens VR; **Return to 2D** restores the Android window.

## Source, build and required libraries

<details>
<summary>Build from source</summary>

### Requirements

- Windows, Python 3.11+ with Pillow, JDK and Android SDK: NDK 27.2.12479018, CMake 3.22.1, Build Tools 35.0.1 and platform 34; Git for Windows and 7-Zip for dependency preparation. LLVM and the Windows SDK are needed for native build tools. NumPy is optional when regenerating the foundation's glove/PBR assets.
- [CyVR_UI 0.9.1](https://github.com/MrMybal/CyVR_UI) — required C++/Vulkan foundation, placed beside `CyVR_Player`; media dependencies are pinned in `dependencies.lock.json`.

### Get the source

```sh
git clone https://github.com/MrMybal/CyVR_Player.git
git clone https://github.com/MrMybal/CyVR_UI.git
cd CyVR_Player
```

### Build and run

```sh
python tools/prepare_android_media.py
python tools/prepare_splats.py
python tools/build_codecs.py
python tools/build_subtitles.py --target android
python tools/build_discs.py --target android
python tools/build_ffmpeg_android.py --alpha
python tools/build_app.py --target android --beta
python tools/verify_app.py --beta
```

These commands assume the SDK, foundation OpenXR loader and local signing key have been prepared. The optimized media profile named `--alpha` is shared by Alpha and Beta packages. Follow the [build guide](docs/building.md) and `SOURCE_BUILD.md` in the [Beta source archive](https://github.com/MrMybal/CyVR_Player/releases/download/v0.4.15-beta.1/CyVR_Player-0.4.15-beta.1-sources.zip) for complete preparation. Use the supplied foundation snapshot to rebuild this version. `--no-immersive` builds a strictly 2D alternative; it uses the same Android package and replaces the other variant.

</details>

---

## License

**CyVR Player uses [GPL-3.0-only](LICENSE).** Copyright © 2026 MrMybal. Original code, shaders, scripts and documentation use this license unless a file states otherwise. Third-party components retain their own terms.

Commercial use is permitted under the applicable terms. Redistribution must satisfy the corresponding-source requirements and other license obligations. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

### Third-party libraries

| Library · source | License · terms | Use / scope |
| --- | --- | --- |
| [CyVR_UI 0.9.1](https://github.com/MrMybal/CyVR_UI) | [MIT](https://github.com/MrMybal/CyVR_UI/blob/main/LICENSE) | Shared native UI and Vulkan rendering; OpenXR host used only in immersion. |
| [FFmpeg](https://github.com/FFmpeg/FFmpeg) | [LGPL-3.0-or-later](https://github.com/FFmpeg/FFmpeg/blob/611cec792aa54e2e53dbc1be95dcf5955d9a99ba/COPYING.LGPLv3) | Runtime demuxing, software codecs, audio conversion and subtitles; this Android shared-library profile disables GPL/nonfree components. |
| [Mbed TLS 3.6.7](https://github.com/Mbed-TLS/mbedtls) | [Apache-2.0](https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-3.6.7/LICENSE) | Apache license choice from upstream dual licensing; TLS for FFmpeg network playback. |
| [OpenXR SDK / loader 1.1.49](https://github.com/KhronosGroup/OpenXR-SDK) | [Apache-2.0 and component notices](https://github.com/MrMybal/CyVR_UI/blob/main/THIRD_PARTY.md) | Optional immersive runtime loader; some source files also offer MIT. Excluded from the 2D-only APK. |
| [Vulkan Headers](https://github.com/KhronosGroup/Vulkan-Headers) | [Apache-2.0 / MIT by file](https://github.com/MrMybal/CyVR_UI/blob/main/THIRD_PARTY.md) | API headers, with the Apache choice used here; the headset supplies the driver. |
| [Android NDK support](https://developer.android.com/ndk) | [Apache-2.0; Apache-2.0 WITH LLVM-exception](https://github.com/MrMybal/CyVR_UI/blob/main/THIRD_PARTY.md) | NativeActivity glue and LLVM C++ runtime/support, according to the individual NDK notices. |
| [FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) | [MIT](third_party/cas/LICENSE.txt) | Optional GPU sharpening. |
| [FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | [MIT](third_party/fsr1/LICENSE.txt) | Optional EASU + RCAS spatial reconstruction. |
| [Snapdragon GSR 1](https://github.com/SnapdragonGameStudios/snapdragon-gsr) | [BSD-3-Clause](third_party/gsr1/LICENSE) | Optional mobile spatial reconstruction. |
| [dav1d 1.5.4](https://www.videolan.org/projects/dav1d.html) | [BSD-2-Clause](https://code.videolan.org/videolan/dav1d/-/blob/1.5.4/COPYING) | Software AV1 fallback when hardware decoding is unavailable. |
| [edge264-mvc](https://github.com/jens-duttke/edge264-mvc) | [BSD-3-Clause](https://github.com/jens-duttke/edge264-mvc/blob/4555276ce692ee8abdb8f768d4e59f82b1f6590e/LICENSE_BSD.txt) | Software MVC stereo decoding. |
| [libass 0.17.4](https://github.com/libass/libass) | [ISC](https://github.com/libass/libass/blob/0.17.4/COPYING) | Runtime ASS/SSA and text subtitle rendering. |
| [FreeType 2.14.1](https://github.com/freetype/freetype) | [FTL](https://github.com/freetype/freetype/blob/VER-2-14-1/docs/FTL.TXT) | Font rasterization; the FreeType License choice is used. |
| [HarfBuzz 11.2.1](https://github.com/harfbuzz/harfbuzz) | [MIT](https://github.com/harfbuzz/harfbuzz/blob/11.2.1/COPYING) | Subtitle text shaping. |
| [FriBidi 1.0.16](https://github.com/fribidi/fribidi) | [LGPL-2.1-or-later](https://github.com/fribidi/fribidi/blob/v1.0.16/COPYING) | Bidirectional subtitle text. |
| [SplatKit](https://github.com/Xget7/splatkit) | [MIT and component notices](https://github.com/Xget7/splatkit/blob/e67526e2ae1b64d474275208639c1a1015c843b8/LICENSE) | Vulkan subset for Gaussian visibility, sorting and rendering. |
| [VulkanMemoryAllocator 3.2.1](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | [MIT](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/v3.2.1/LICENSE.txt) | GPU memory allocation in the Gaussian backend. |
| [libbluray 1.5.0](https://www.videolan.org/developers/libbluray.html) | [LGPL-2.1-or-later](https://code.videolan.org/videolan/libbluray/-/blob/1.5.0/COPYING) | Blu-ray title and playlist reading for supported images. |
| [libudfread 1.2.0](https://code.videolan.org/videolan/libudfread) | [LGPL-2.1-or-later](https://code.videolan.org/videolan/libudfread/-/blob/1.2.0/COPYING) | UDF access for disc images. |
| [libdvdread 7.1.1](https://code.videolan.org/videolan/libdvdread) | [GPL-2.0-or-later](https://code.videolan.org/videolan/libdvdread/-/blob/7.1.1/COPYING) | DVD-Video structures and data reading. |
| [Noto Sans](https://github.com/notofonts/noto-fonts) | [SIL OFL 1.1](https://github.com/notofonts/noto-fonts/blob/v20201206-phase3/LICENSE) | Bundled UI/subtitle font and derived UI atlas. |
| [stb_image](https://github.com/nothings/stb) | [MIT](https://github.com/nothings/stb/blob/013ac3beddff3dbffafd5177e7972067cd2b5083/LICENSE) | JPEG background decoding; MIT choice from the MIT/public-domain alternatives. |
| [Poly Haven environment photographs](https://github.com/MrMybal/CyVR_UI/blob/main/assets/scenes/README.md) | [CC0-1.0](https://github.com/MrMybal/CyVR_UI/blob/main/third_party/notices/CC0-1.0.txt) | Bundled cinema photographs and demo frame, credited to Greg Zaal. |
| [GNU make 4.4.1](https://github.com/mirror/make) | [GPL-3.0-or-later](https://github.com/mirror/make/blob/4.4.1/COPYING) | Build tool only; not linked into or shipped in the player. |
| [Pillow](https://github.com/python-pillow/Pillow) | [MIT-CMU](https://github.com/python-pillow/Pillow/blob/main/LICENSE) | Build-time image/font/branding preparation only. |
| [NumPy](https://github.com/numpy/numpy) | [BSD-3-Clause and component notices](https://github.com/numpy/numpy/blob/main/LICENSE.txt) | Optional glove/PBR asset generation during authoring; not part of the application runtime. |

This table indexes the main components; it does not replace the full notices shipped in `assets/licenses`. Versions and hashes are recorded in [dependencies.lock.json](dependencies.lock.json). OpenXR loader dependencies, C++ runtime, fonts and assets retain their component notices. Android APIs, hardware decoders, the Quest runtime and drivers are supplied by the platform.

See the [CyVR_UI notices](https://github.com/MrMybal/CyVR_UI/blob/main/THIRD_PARTY.md) for the foundation’s scope. Dependency source archives accompany the Beta.

## References and links

- [Detailed user guide](docs/user-guide.md).
- [Version notes and known limitations](docs/releases/0.4.15-beta.1.md).
- [Releases and downloads](https://github.com/MrMybal/CyVR_Player/releases/tag/v0.4.15-beta.1) · [Source repository](https://github.com/MrMybal/CyVR_Player) · [Report an issue](https://github.com/MrMybal/CyVR_Player/issues).
- [Build guide](docs/building.md) · [Architecture and integration](docs/interface.md).
- [Screenshot credits](docs/images/README.md) · [CyVR_UI](https://github.com/MrMybal/CyVR_UI).
