# Developer checks

[README](../README.md) · [Build guide](building.md)

These are the project’s existing verification commands and requirements. This guide does not record a new test run.

```powershell
python tests/container_integration.py
python tests/compat_integration.py
python tools/verify_dependencies.py
python tools/generate_player_locales.py --check
python tools/adapt_gsr1.py --check
python tools/verify_app.py
python tools/verify_app.py --panel-only
python tests/quest_app.py --serial QUEST_SERIAL
python tests/quest_hosts.py --serial QUEST_SERIAL
python tests/quest_gpu.py --serial QUEST_SERIAL
python tests/quest_compat.py --serial QUEST_SERIAL
python tests/quest_image.py --serial QUEST_SERIAL
python tests/quest_view.py --serial QUEST_SERIAL
```

`build_app.py --target windows` runs fourteen CTest suites, including application state transfer, malformed state handling, pointer navigation, translations, independent 24 fps video / 72 Hz redraw counting, stalls and the shared UI core. The GPU image suite covers 1,060 checks, including identical output from separate and combined submissions for CAS/FSR/GSR, the real projection shader before filtering in mono-eye/SBS/TB/cinema/180°/360°, transparent edges and preserved detail when only one axis needs upscaling. `quest_hosts.py` requires `Exemples/avc.mp4` in the app's external files folder; it uses real Android input to test resizing, resume and both presentation transitions. For the installed 2D-only variant, add `--panel-only`. Network tests remove their own adb reverse when complete. Device tests change the current document, navigation and UI settings; they do not clear app data.

See [validation and remaining measurements](validation.md), [host integration](interface.md), [media architecture](architecture.md), [sources and streams](sources-et-flux.md), [dependency review](bibliotheques.md) and [Gaussian Splatting playback](gaussian-splatting.md).
