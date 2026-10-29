"""Exercise the actual hybrid APK through Android input and durable state.

Does not clear application data or change other packages. Requires the small
local AVC fixture in the app's Exemples folder (quest_app.py tests networking).
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
ADB = Path.home() / 'AppData/Local/Android/Sdk/platform-tools/adb.exe'
PACKAGE = 'com.cyvr.player'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--serial', required=True)
    parser.add_argument('--panel-only', action='store_true')
    args = parser.parse_args()
    base = [str(ADB), '-s', args.serial]

    def adb(*parts, binary=False):
        r = subprocess.run(base + list(map(str, parts)), capture_output=True, timeout=30)
        if r.returncode:
            raise RuntimeError(r.stderr.decode(errors='replace'))
        return r.stdout if binary else r.stdout.decode('utf-8', errors='replace').strip()

    def wait(condition, message, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            result = condition()
            if result:
                return result
            time.sleep(.2)
        raise RuntimeError(message)

    def logs():
        pid = adb('shell', 'pidof', PACKAGE)
        output = adb('logcat', '-d', '--pid=' + pid, '-s', 'CyVRPlayer:V', 'CyVR:V')
        if 'FATAL:' in output or 'RENDER_ERROR' in output or 'MEDIA_ERROR:' in output:
            raise RuntimeError(output[-5000:])
        return output

    def state():
        b = adb('exec-out', 'run-as', PACKAGE, 'cat', 'files/player-state.bin', binary=True)
        assert b[:8] == b'CYVRP04\0'
        cursor = 8

        def number():
            nonlocal cursor
            n = struct.unpack_from('<Q', b, cursor)[0]
            cursor += 8
            return n

        def string():
            nonlocal cursor
            n = number()
            s = b[cursor:cursor+n].decode('utf-8')
            cursor += n
            return s

        s = dict(zip(('directory', 'url', 'source', 'title'), (string() for _ in range(4))))
        s['parents'] = [string() for _ in range(number())]
        s.update(zip(('position', 'page', 'offset', 'stereo', 'projection', 'flags'), (number() for _ in range(6))))
        return s

    def tap(x, y, width=1600, height=1100):
        scale = min(width / 1440, height / 990)
        adb('shell', 'input', 'tap', round((width-1440*scale)/2+x*scale),
            round((height-990*scale)/2+y*scale))

    checks = []

    def passed(name, **details):
        checks.append(dict(name=name, passed=True, **details))
        print(name, 'passed', details, flush=True)

    adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')
    adb('shell', 'am', 'force-stop', PACKAGE)
    adb('shell', 'am', 'start', '-W', '-n', PACKAGE+'/.PanelActivity',
        '-a', 'android.intent.action.VIEW', '-t', 'video/mp4', '-d',
        '/sdcard/Android/data/com.cyvr.player/files/Exemples/avc.mp4')
    wait(lambda: 'VIDEO_COMPLETE' in logs(), 'Local panel playback did not complete')
    log = logs()
    assert 'Panel2D / Vulkan Android surface 1600x1100 / OpenXR inactive' in log
    assert 'OpenXR state:' not in log
    passed('native-2d-launch-no-openxr', surface='1600x1100')

    tap(1220, 125)  # Display
    tap(180, 275)   # Format tab (retained across activity replacement)
    tap(1180, 416)  # Top/bottom stereo
    tap(820, 48)    # Language
    tap(1000, 48)   # Theme
    tap(870, 125)   # Open stream
    tap(90, 425)    # Add a character to the existing URL draft
    before = wait(lambda: (s if (s := state())['page'] == 2 and s['stereo'] == 2 else None),
                  'Pointer actions did not update the shared application')
    passed('android-pointer-navigation-and-settings')

    activities = adb('shell', 'dumpsys', 'activity', 'activities')
    task = re.search(r'Task\{[^\n]*#(\d+)[^\n]*A=\d+:com\.cyvr\.player\.panel[^\n]*rootTaskId=(\d+)', activities)
    assert task, 'Player task not found'
    root_task = task[2]
    try:
        adb('shell', 'am', 'task', 'resize', root_task, 0, 0, 1920, 1080)
        wait(lambda: 'surface 1920x1080' in logs(), 'Surface did not adopt the resized bounds')
        tap(180, 125, 1920, 1080)
        wait(lambda: state()['page'] == 0, 'Input mapping failed after resize')
        passed('resize-and-inverse-input-mapping', surface='1920x1080')
    finally:
        adb('shell', 'am', 'task', 'resize', root_task, 0, 0, 1600, 1100)
    wait(lambda: logs().rfind('surface 1600x1100') > logs().rfind('surface 1920x1080'),
         'Original size did not return')
    tap(870, 125)
    wait(lambda: state()['page'] == 2, 'Stream navigation did not return')
    before = state()

    adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
    time.sleep(.5)
    adb('shell', 'am', 'start', '-W', '-n', PACKAGE+'/.PanelActivity')
    wait(lambda: state()['page'] == 2, 'Pause/resume lost navigation')
    passed('pause-resume-preserves-navigation')

    if not args.panel_only:
        panels_before = logs().count('APP_READY host=panel')
        tap(1280, 48)
        wait(lambda: 'APP_READY host=immersive' in logs() and 'host=immersive width=' in logs(),
             'Immersive activity did not render the restored video')
        native = re.findall(r'Eye target (\d+x\d+) / native recommendation', logs())
        assert len(native) == 2
        time.sleep(.6)
        during = state()
        for key in ('source', 'directory', 'url', 'page', 'stereo', 'projection', 'parents'):
            assert during[key] == before[key], 'Immersive transfer lost '+key
        passed('panel-to-immersive-and-state-transfer', eyes=native)
        adb('shell', 'input', 'keyevent', 'KEYCODE_BACK')
        wait(lambda: logs().count('APP_READY host=panel') > panels_before,
             'Back did not return to the 2D panel')
        time.sleep(.6)
        after = state()
        for key in ('source', 'directory', 'url', 'page', 'stereo', 'projection', 'parents'):
            assert after[key] == before[key], 'Panel return lost '+key
        assert abs(after['position']-before['position']) <= 100000
        passed('immersive-to-panel-restores-document-position-and-navigation')
    else:
        assert 'immersive_enabled=0' in log
        passed('panel-only-runtime-configuration')

    adb('shell', 'am', 'force-stop', PACKAGE)
    adb('shell', 'am', 'start', '-W', '-n', PACKAGE+'/.PanelActivity')
    wait(lambda: 'APP_READY host=panel' in logs(), 'Process restart did not restore the app')
    time.sleep(.6)
    after = state()
    for key in ('source', 'directory', 'url', 'page', 'stereo', 'projection', 'parents'):
        assert after[key] == before[key], 'Process restart lost '+key
    passed('process-restart-restores-durable-state')

    suffix = '-panel' if args.panel_only else ''
    result = {'checks': checks, 'limits': 'ADB input and runtime diagnostics; binocular appearance, physical window gestures and sustained high-resolution performance need a worn-headset check.'}
    (ROOT / ('build/quest-hosts'+suffix+'-results.json')).write_text(json.dumps(result, indent=2)+'\n')


if __name__ == '__main__':
    main()
