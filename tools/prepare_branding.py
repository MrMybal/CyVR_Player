"""Derive runtime and Android icon resources from the approved master PNG."""
import argparse
from pathlib import Path
import shutil
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
MASTER = ROOT / 'assets/branding/cyvr-player-logo-orange-coral-v1.png'


def prepare(output):
    output.mkdir(parents=True, exist_ok=True)
    with Image.open(MASTER) as source:
        logo = source.convert('RGBA')
    # Shared UI atlas: nine 160x90 previews, a ten-pixel gap, then this logo.
    (output / 'player-logo.rgba').write_bytes(logo.resize((160, 160), Image.Resampling.LANCZOS).tobytes())
    resources = output / 'res'
    for density, size in [('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)]:
        folder = resources / ('mipmap-' + density)
        folder.mkdir(parents=True, exist_ok=True)
        logo.resize((size, size), Image.Resampling.LANCZOS).save(folder / 'ic_launcher.png')
        shutil.copyfile(folder / 'ic_launcher.png', folder / 'ic_launcher_round.png')
    # 108dp adaptive canvas at 4x density. The approved tile fits its safe zone.
    foreground = Image.new('RGBA', (432, 432))
    foreground.alpha_composite(logo.resize((288, 288), Image.Resampling.LANCZOS), (72, 72))
    folder = resources / 'drawable-nodpi'
    folder.mkdir(parents=True, exist_ok=True)
    foreground.save(folder / 'ic_launcher_foreground.png')
    folder = resources / 'values'
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'branding.xml').write_text('<resources><color name="launcher_background">#FF573F</color></resources>\n')
    folder = resources / 'mipmap-anydpi-v26'
    folder.mkdir(parents=True, exist_ok=True)
    adaptive = ('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '  <background android:drawable="@color/launcher_background" />\n'
                '  <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
                '</adaptive-icon>\n')
    for name in ('ic_launcher', 'ic_launcher_round'):
        (folder / (name + '.xml')).write_text(adaptive)
    (output / 'branding.stamp').write_text('CyVR Player branding resources\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    prepare(parser.parse_args().output)
