"""Package the staged player and exact local MIT foundation with dependency sources."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import xml.etree.ElementTree as ET
import zipfile
from foundation_sources import sources as foundation_sources

ROOT = Path(__file__).resolve().parents[1]
UI = Path(os.environ.get('CYVR_UI_ROOT', ROOT.parent / 'CyVR_UI')).resolve()


def git(root, *arguments):
    return subprocess.check_output(['git', '-c', 'safe.directory=' + root.as_posix(), '-C', str(root), *arguments])


def main():
    subprocess.run(['git', '-c', 'safe.directory=' + ROOT.as_posix(), '-C', str(ROOT), 'diff', '--exit-code'], check=True)
    version = ET.parse(ROOT / 'android/AndroidManifest.xml').getroot().get('{http://schemas.android.com/apk/res/android}versionName')
    if not version or '-alpha.' not in version:
        raise RuntimeError('An Alpha version is required')
    output = ROOT / 'dist/releases' / version
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f'CyVR_Player-{version}-sources.zip'
    player_tree = git(ROOT, 'write-tree').decode().strip()
    files = {}
    for raw_path in git(ROOT, 'ls-files', '-z').split(b'\0'):
        if raw_path:
            path = raw_path.decode('utf-8')
            files['CyVR_Player/' + path] = git(ROOT, 'show', ':' + path)
    foundation=foundation_sources(UI)
    compiled=json.loads((ROOT/'build/app-android-alpha/foundation-inputs.json').read_text(encoding='utf-8'))
    if foundation!=compiled:
        raise RuntimeError('The foundation source differs from the compiled APK; rebuild before packaging')
    ui_paths=set(foundation['files'])
    for path in sorted(ui_paths):
        parts = PurePosixPath(path).parts
        if parts[:2] in {('assets', 'branding'), ('docs', 'images')}:
            continue
        if '..' in parts or parts[0] in {'.git', '.cache', '.deps', 'build', 'dist'}:
            raise RuntimeError('Invalid foundation source path')
        files['CyVR_UI/' + path] = (UI / path).read_bytes()
    if not files['CyVR_UI/LICENSE'].startswith(b'MIT License'):
        raise RuntimeError('The supplied foundation must retain its MIT license')
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text(encoding='utf-8'))
    dependencies = []
    for entry in lock['external_dependencies']:
        if entry['name'] not in {'FFmpeg source for Android', 'Mbed TLS 3.6.7', 'OpenXR SDK source matching Android loader 1.1.49','SplatKit Vulkan source','VulkanMemoryAllocator source','dav1d source','edge264-mvc source'} and not entry['name'].startswith('Subtitle source: '):
            continue
        source = ROOT / entry['archive']
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise RuntimeError('Dependency archive checksum mismatch: ' + source.name)
        files['dependencies/' + source.name] = data
        dependencies.append(entry['name'])
    if len(dependencies) != 11:
        raise RuntimeError('Expected media, BSD codec, subtitle, Gaussian and OpenXR source archives')
    files['build-config/ffmpeg-alpha.json'] = (ROOT / '.deps/ffmpeg/android-arm64-alpha/origin.json').read_bytes()
    metadata = {
        'version': version,
        'player_tree': player_tree,
        'ui_base_commit': foundation['base_commit'],
        'ui_license': 'MIT',
        'dependencies': dependencies,
        'files': {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())},
    }
    files['source-manifest.json'] = (json.dumps(metadata, indent=2) + '\n').encode()
    files['SOURCE_BUILD.md'] = BUILD_INSTRUCTIONS.encode()
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as package:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            package.writestr(info, data)
    print(json.dumps({'archive': archive.name, 'files': len(files), 'player_tree': player_tree, 'sha256': hashlib.sha256(archive.read_bytes()).hexdigest()}))


BUILD_INSTRUCTIONS = '''# Rebuilding the Alpha

Keep `CyVR_Player` and `CyVR_UI` as sibling directories. The foundation is the
actual source snapshot used for this Alpha, including its local media-host
extensions, under MIT. Its reference documentation illustrations are omitted;
all build inputs are included. Player code is GPL-3.0-only.

Use Windows, Python 3.11+, the Android SDK/NDK/CMake/JDK versions in the player
README, Git for Windows (bash), and 7-Zip. Configure ANDROID_HOME and JAVA_HOME.
Clang with the Windows SDK is also needed for FFmpeg's host compilation tests.
Build tools and the Android platform/runtime are installed separately.

1. Copy all supplied source archives from `dependencies` into
   `CyVR_Player/.deps/downloads` without renaming them.
2. In `CyVR_Player`, run `python tools/prepare_android_media.py`. It verifies
   and extracts these exact sources and downloads the pinned GNU make tool.
3. In `CyVR_UI`, run `powershell -NoProfile -File tools/fetch-deps.ps1` to obtain
   the hash-pinned OpenXR Android loader AAR. The matching full loader SDK source
   is supplied under `dependencies`; its version/commit/hash are in the lockfile.
4. In `CyVR_Player`, run `python tools/prepare_splats.py`,
   `python tools/build_codecs.py`,
   `python tools/build_subtitles.py --target android`, then
   `python tools/build_ffmpeg_android.py --alpha`.
   The script builds Mbed TLS and FFmpeg in separate Alpha directories. It maps
   compiler file paths and substitutes portable variables in the generated
   FFMPEG_CONFIGURATION string before compilation. Original dependency source
   archives are unchanged. The installed origin.json records configure options;
   ${CYVR_PLAYER_ROOT}, ${ANDROID_NDK} and ${CYVR_HOST_TOOLCHAIN} stand for the
   local project, NDK and host-compiler directories.
5. Supply your own local signing key as `.cache/debug.keystore`, with alias
   `androiddebugkey` and conventional development passwords `android`. To make
   a new testing key, run in PowerShell:

   ```powershell
   New-Item -ItemType Directory -Force .cache | Out-Null
   & "$env:JAVA_HOME/bin/keytool.exe" -genkeypair -keystore .cache/debug.keystore -storepass android -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Local Alpha Build"
   ```

   The original private key is not distributed. A differently signed rebuild
   cannot update the published APK in place; uninstalling removes app data, so
   use a separate test device/package or preserve data before changing signing.
6. Run `python tools/build_app.py --target android --alpha`, then
   `python tools/verify_app.py --alpha`. It builds the default 2D launcher with
   optional immersion, packages stripped native libraries, disables Android
   debugging and signs the testing APK. Override CYVR_UI_ROOT if needed.

For the OpenXR loader itself, extract the supplied SDK archive and follow its
BUILDING.md/CMake Android instructions for OpenXR SDK 1.1.49. It contains the
loader and its bundled third-party sources/licenses. Noto Sans/OFL, vendored
shader sources and remaining notices are in the supplied foundation/player.

source-manifest.json records the staged player Git tree and hashes of every
source/dependency input in this archive. Local signing keys, caches and user
preferences are not source inputs. To run the existing Windows/GPU tests, fetch
the pinned Windows FFmpeg SDK with `python tools/fetch_ffmpeg.py`, then run
`python tools/build_app.py --target windows`.
'''


if __name__ == '__main__':
    main()
