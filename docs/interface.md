# CyVR_UI 0.4 integration

## Shared hosts

The application consumes the sibling foundation with `add_subdirectory(CYVR_UI_ROOT)` and links `cyvr_ui`, `cyvr_gpu` and **`cyvr_android`**. It no longer compiles a private copy of `xr_app.cpp` or owns a second Quest runtime. `src/app/android_main.cpp` configures **`runAndroidApp()`**. The former `quest_main.cpp` was removed; a pre-migration source backup is in the ignored `build/migration-04-backup` folder.

`PlayerApplication` supplies `renderApplication`, `handleApplicationAction`, `applicationTick` and `applicationBack` to the same `UiModel`. Themes and language preferences come from the shared core. English source message IDs are the default; `assets/locales/fr.json` generates the French player catalogue via `tools/generate_player_locales.py`. User file names, document identifiers and URLs are never translated.

`PanelActivity` is the only launcher and declares Home, Overlay and 2D categories. It is resizable and uses `com.cyvr.player.panel`. `ImmersiveActivity` is unexported, uses the VR categories and a separate `com.cyvr.player.immersive` task affinity. Both load `libcyvr_player.so`. The legacy `.PlayerActivity` alias retains its own native-library metadata so existing external launch commands continue to work.

The Java base activity handles system services and activity transitions, not drawing widgets. It passes Back to C++ navigation. On a presentation change, the host releases its GPU resources and XR session, persists application state, and requests the next activity. Returning to 2D uses the documented Home pending-intent path. After a host returns, `android_main` continues servicing native-glue lifecycle messages until `APP_CMD_DESTROY`; otherwise Java's synchronous pause/stop callbacks could block and cause an ANR. Hosts are serialized while their application model and decoder are alive, but this final lifecycle drain does not hold the host lock.

## Native resolution and video extensions

The local 0.4 foundation received generic `AndroidRenderExtension` and optional `AndroidXrRenderExtension` hooks. They allow an application to extend Vulkan device features, attach GPU resources, submit due video frames and provide XR composition layers without copying the framework runtime. No FFmpeg, media URLs or codec dependencies were added to CyVR_UI. Existing local foundation changes, including application lifecycle hooks, remain in place.

The player enables `AndroidAppConfig::nativeResolution`. In 2D, the shared renderer draws directly into the actual Android swapchain extent and applies inverse mapping to pointer coordinates. The logical 1440 × 990 canvas is letterboxed for other aspect ratios rather than reflowed. The Quest system provides moving/resizing/closing; the app does not add competing 2D window controls.

In immersion, scene targets use each runtime-recommended eye size without the reference app's 1024-pixel cap. UI layer textures scale with the recommended eye width while retaining logical component coordinates. In native quality the video allocates its decoded source dimensions, separately from scene/UI resolution. A source that exceeds GPU limits reports an error instead of silently reducing its resolution. View-relative fallback handles temporarily unavailable tracking and recenters into LOCAL when a head pose is available.

`src/app/android_renderer.cpp` extends both hosts. In 2D it converts each due hardware image into a full-size Vulkan texture, supplied to the shared UI renderer as a borrowed image view. The preview letterboxes and uses the selected left-eye region for SBS/TB. Resizing recreates the shared renderer, then rebinds the retained video texture, including while paused. In immersion the same conversion writes a full-size OpenXR video swapchain. Per-eye quad or equirect2 layers implement mono/SBS/TB and 180°/360°.

`VideoRenderer` imports AHardwareBuffer with the driver's YCbCr conversion, imports acquisition sync FDs, transfers FOREIGN queue ownership and retains image leases until GPU completion. Imports are bounded to twelve buffers and reset on generation changes. Source queues hold at most three images. GPU waits are conservative; sustained resolution/thermal benchmarks are still needed. Pixel readback exists only in the separate diagnostic probe after playback.

## Application state and sources

The player uses the shared `UiModel::interfaceHidden` flag for video-only viewing. `setInterfaceHidden()` clears captures/widgets, and `revealInterface()` consumes the first waking action. The XR host keeps the application video layers while omitting panel, dock, HUD and cursor layers and clearing hand/ray geometry. A dedicated Touch A action and either trigger restore the UI without requiring a ray hit on an invisible panel. Android Back first reveals a hidden interface before navigating or changing hosts. In 2D the player draws only the aspect-fitted video/black bars, and the host maps that logical drawing across the full physical viewport instead of the normal letterboxed UI canvas. `viewportSize` keeps the video's geometry correct after resize. Enter/Space and the F11 toggle pass through the Java event bridge; system Home/volume commands are unchanged. The visibility flag is deliberately transient rather than saved with documents.

`PlayerSession` owns a serialized media worker. File/network I/O, Android provider queries and decoding run outside the UI/render thread. Pausing stops feeding the decoder. Seeking recreates the decoder/reader with a new epoch and discards preroll before the requested position. A paused restoration decodes one still frame, then stays paused. When an audio track is active, video scheduling follows the AAudio presentation clock; silent playback uses a monotonic clock. PCM starvation freezes media time while output silence is inserted.

The timeline uses the shared `handleApplicationSlider` callback with `SliderPhase::Begin`, `Preview`, `Commit` and `Cancel`. Pointer capture retains dragging beyond the track and clamps the final selection. Preview changes only the thumb/time label; a single committed seek reaches the media worker on release. Cancellation on focus loss, navigation or hidden controls does not seek, and a cancelled gesture cannot restart until a fresh press. Pending seeks expose their target until a new-epoch image is presented, preserving the requested position when activities change. Seeking preserves playback/pause state, including a paused preview after end-of-stream. A final preroll image is retained for a target beyond the last frame's timestamp. No pixel thumbnail decoding was added while dragging.

`PlayerApplication` writes a bounded versioned state file to `internalDataPath/player-state.bin`, replacing the previous file atomically. It preserves the current folder/history, URL draft, source URI/title, page, scroll offset, projection/stereo flags, position and intended play/pause state. Resume/pause and host release commit state. Settings remain in the original external `preferences.txt`. Java moves the former `PlayerActivity` tree preference into the common `player` preference store while retaining existing URI grants. No app data is cleared during migration or installation.

The browser keeps SAF documents as `content://` URIs; ContentResolver supplies duplicated FDs with their offsets/lengths. Lists are bounded to 512 entries, and provider requests are cancellable. Network startup preserves the existing TLS checks and bounded in-band SPS/PPS discovery needed for HLS/MPEG-TS. An incoming VIEW intent supersedes saved media before restoration starts, avoiding an unnecessary connection to a previous stream.

## Build and scope

`CYVR_ENABLE_IMMERSIVE=OFF` excludes the XR host and loader; the entry point also sets `immersiveEnabled=false`. `tools/build_app.py --target android --no-immersive` strips the immersive activity and runtime-broker queries from its generated manifest. Both variants keep the original package and signing key. Android native/ZIP alignment is 16 KiB; license notices are packaged with the APK.

Current playback includes device-supported hardware codecs and FFmpeg software fallback, including AV1 and tested two-view MVC/MV-HEVC paths. PQ/HLG tone mapping and SDR transfer handling run in the video shaders; full colorimetric qualification, rotation and dynamic format changes remain pending. Equirectangular projections do not imply fisheye/cubemap/proprietary format support. The current media, subtitle and Gaussian scope is described in the README; older validation paragraphs here apply only to their named versions. See [validation](validation.md) for tests actually run and remaining device checks.


## Image processing and thumbnails (0.4.0)

The player now uses `DrawList::video()` and `VulkanRenderer::setVideoTexture()` for its borrowed playback texture. The original image-atlas binding remains independent for the browser's static thumbnails. `UiModel::applicationController` exposes generic thumbstick/trigger/grip/aim state from the shared XR host; a consumed application zoom gesture cannot also select a widget or reveal hidden chrome. The core contains no player or FFmpeg dependency. See [image processing](image-processing.md) for filter passes, budgets, paused-frame ownership and persisted controls.


### Player performance hooks (0.4.2)

The local foundation also exposes `UiModel::renderApplicationDebug` for the player's shared video/render HUD, and `AndroidRenderExtension::frameSubmitted()` after a successful panel present or a nonempty OpenXR frame end. Neither hook equates submission to physical compositor display. The optional Vulkan query entry points are loaded with the existing device functions. Keep these small local hooks when updating the foundation.


## Independent immersive video placement (0.4.4)

The player no longer derives cinema/panorama position or orientation from `AndroidXrFrame::panel.pose`. `VideoPlacement` captures a separate placement from the current head pose. Native quad/equirect layers and enhanced per-eye projection share that placement and reference space. Panel drag, release-facing rotation, follow, hide/reveal and filter resource recreation do not update it. Action 1050 increments an application video-placement request instead of setting the host's panel recenter flag. The explicit recenter uses the current view and keeps the controls panel where the user placed it.

The local foundation forwards `XrEventDataReferenceSpaceChangePending` through the optional `AndroidXrRenderExtension::referenceSpaceChanging()` hook. The player applies a known LOCAL-origin transform at its reported change time. A VIEW-relative startup placement is transformed into LOCAL coordinates once tracking is valid, rather than adopting the newly recentered UI pose. See [the OpenXR reference-space change definition](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEventDataReferenceSpaceChangePending.html). Unknown runtime-origin deltas cannot guarantee preservation of a physical room position. Scene placement is transient and is recreated when the immersive host is replaced; no persistent spatial-anchor service was added.

## Compact controls and caption overlay (0.4.5)

`UiModel::compactInterface` omits the immersive dock, decorative glass and separate HUD while retaining application widgets. The player supplies a compact playback bar and controls inactivity with its portable `PlaybackControls` helper. Significant controller motion/input wakes it; active interactions postpone the three-second timeout. The full and compact interfaces share the same player/session.

The foundation routes a captured pointer with `isDockWidget()`: only IDs 600–699, excluding the main-panel drag handle 605, belong to the dock. Player IDs above 1000 remain on the main panel throughout trigger press/hold/release. Treating every ID above 600 as a dock control previously lost immersive clicks, including **Full interface**. The supplied MIT foundation and the local sibling include this bounded classifier; application regression tests exercise both controller pointers through compact/full transitions using real panel ray intersections.

`DrawList::overlay()` and `VulkanRenderer::setOverlayTexture()` add a borrowed RGBA overlay independent of the browser atlas and video texture. Subtitles use this binding in 2D and a separate video-anchored OpenXR layer in immersion. The reusable UI code remains MIT and contains no FFmpeg/libass/player dependency.

## 0.4.7 cinema and Ambilight

The compact Cinema action opens Display's cinema page. Screen, Environment and Ambilight subtabs share both hosts; rendering effects apply in immersion. CIN1 stores screen/environment settings, AMB1 adds optional Ambilight controls. Missing extensions receive neutral defaults without replacing existing geometry. Cinema geometry is independent of the movable controls panel.

The generic foundation adds optional XR-extension negotiation, Black/Dark backgrounds and an environment-only brightness value. Cylinder support is queried at runtime; it is not required to start the 2D host. Environment photos are 4K CC0 with packaged credits. The supplied foundation remains MIT; unrelated changes in a newer working checkout are not required by this release.

Ambilight uses one optional alpha layer behind the film and captions, following the same centre, size and orientation. Both stereo eyes see the same averaged colour field. Its controls remain editable when off; disabling removes the layer immediately. Errors report a message and leave movie playback running; toggle the option to retry.

## 0.4.8 automatic video layout

The selected video track carries independent stereo/projection hints from FFmpeg's codec-parameter side data and container tags. New opens combine them with bounded filename parsing, on the media worker/UI metadata transition only. A media-request identifier prevents a previous worker result from applying its layout to a newer file. Metadata takes priority over naming; decoded multiview has priority for actual output packing. The UI's existing layout controls set manual mode, with explicit re-detection available. No additional per-frame image analysis runs.

FMT1 appends the manual/automatic choice to the existing private checkpoint. Legacy checkpoints keep their existing selections. New files enable detection, while activity changes and process restarts preserve a manual choice for the current source. Ordinary videos receive a neutral 2D layout, rather than inheriting the previous panorama. The Android 2D entry point still creates no OpenXR session.
