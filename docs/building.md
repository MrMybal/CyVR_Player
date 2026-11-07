# Building CyVR Player

[README](../README.md) · [Présentation en français](../README.fr.md) · [User guide](user-guide.md) · [Developer checks](testing.md)

The published Android build is **0.4.15-beta.1**, with **CyVR_UI 0.9.1**. To rebuild that release, use the [matching source archive](https://github.com/MrMybal/CyVR_Player/releases/download/v0.4.15-beta.1/CyVR_Player-0.4.15-beta.1-sources.zip) and its `SOURCE_BUILD.md`. It includes the player, the exact MIT foundation snapshot and fourteen dependency source archives. Keep `CyVR_Player` and `CyVR_UI` as sibling directories.

## Toolchain

- Windows; Python 3.11+ and Pillow for generated branding/images. NumPy is optional when regenerating the foundation's glove/PBR assets.
- Android SDK: NDK **27.2.12479018**, CMake **3.22.1**, Build Tools **35.0.1**, platform **34**.
- A JDK; the scripts default to Android Studio's bundled JDK.
- Git for Windows with bash, 7-Zip, LLVM and the Windows C++ SDK for dependency preparation and native build tools.
- The sibling CyVR_UI checkout or `CYVR_UI_ROOT` pointing to the supplied source snapshot.

`ANDROID_HOME`, `JAVA_HOME` and `CXX` override tool locations. The application uses C++17 and Vulkan. OpenXR is required only by immersive builds. The CyVR_UI reference application and its separate editor are not built as part of the player.

## Prepare dependencies

From `CyVR_UI`, fetch the pinned OpenXR loader and foundation dependencies:

```powershell
powershell -NoProfile -File tools/fetch-deps.ps1
```

From `CyVR_Player`, prepare and build the media components:

```powershell
python tools/prepare_android_media.py
python tools/prepare_splats.py
python tools/build_codecs.py
python tools/build_subtitles.py --target android
python tools/build_discs.py --target android
python tools/build_ffmpeg_android.py --alpha
```

The dependency archives and checksums are recorded in [dependencies.lock.json](../dependencies.lock.json). Copy the supplied release archives into `.deps/downloads` before preparation to reuse them. The optimized media profile called `--alpha` is shared by Alpha and Beta application builds; the name is a build-cache convention, not the application's release stage.

The scripts generate local assets and build caches. Runtime images, shaders, fonts and dependency notices are packaged with the application.

## Signing and build

Use a local `.cache/debug.keystore` with alias `androiddebugkey` and the development signing conventions described in the source archive. The published app's signing identity must match to update it in place. A rebuild signed with a different key cannot update that installation. The source archive explains how to create a separate testing key.

```powershell
python tools/build_app.py --target android --beta
python tools/verify_app.py --beta
```

The output is `dist/android/CyVR_Player-0.4.15-beta.1-arm64.apk`. It starts with the native Android 2D host and offers optional immersive playback. The Beta profile uses optimized Release code, disables Android debugging, strips native symbols and checks 16 KiB native-library alignment.

For a strictly 2D build:

```powershell
python tools/build_app.py --target android --beta --no-immersive
python tools/verify_app.py --beta --panel-only
```

This produces `dist/android/CyVR_Player-0.4.15-beta.1-panel-arm64.apk`, with `CYVR_ENABLE_IMMERSIVE=OFF`, without the OpenXR loader or immersive activity. Both variants use `com.cyvr.player`, so installing one replaces the other. Keep app data when updating with the same signing identity.

## Windows inspector

The Windows build inspects the shared Vulkan UI and media core. It is a development tool, not a published Windows hardware-video-player download.

```powershell
python tools/fetch_ffmpeg.py
python tools/build_subtitles.py --target windows
python tools/build_discs.py --target windows
python tools/build_app.py --target windows
```

The command also runs the configured CTest suites. The executable is `dist/windows/CyVR_Player.exe`; keep its assets and FFmpeg DLLs beside it. See [developer checks](testing.md) for the existing media, shader and device checks and their requirements.

## Package corresponding sources

Stage the intended source changes before running:

```powershell
python tools/package_release.py
```

The packager verifies that the foundation matches the compiled default APK inputs, then creates `dist/releases/0.4.15-beta.1/CyVR_Player-0.4.15-beta.1-sources.zip`. It contains an input hash manifest, dependency sources, licenses and rebuilding instructions. It does not create or publish a GitHub release.
