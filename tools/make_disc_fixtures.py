"""Author original DVD/Blu-ray test images, never use commercial movie images.

Requires the pinned FFmpeg SDK, tsMuxeR 2.7.0, dvdauthor/spumux 0.7.2,
mkisofs and Pillow. Test authoring tools are not linked into the application.
Pass --dvd-tools and --tsmuxer for tools installed elsewhere.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dvd-tools', type=Path)
    parser.add_argument('--tsmuxer', type=Path, default=ROOT / '.deps/tools/disc-author/tsmuxer/tsMuxeR.exe')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/disc-fixtures')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    tools = args.dvd_tools or next((ROOT / '.deps/tools/disc-author/dvdstyler').rglob('dvdauthor.exe')).parent
    ff = ROOT / '.deps/ffmpeg/windows/bin/ffmpeg.exe'
    def run(*command, **kwargs):
        result = subprocess.run(list(map(str, command)), capture_output=True, **kwargs)
        if result.returncode: raise RuntimeError(result.stderr.decode(errors='replace')[-2500:])
    for name, duration, hz in [('dvd-main', 8, 440), ('dvd-extra', 3, 880)]:
        run(ff, '-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=720x576:rate=25',
            '-f', 'lavfi', '-i', f'sine=frequency={hz}:sample_rate=48000', '-f', 'lavfi', '-i', 'sine=frequency=660:sample_rate=48000',
            '-t', duration, '-map', '0:v', '-map', '1:a', '-map', '2:a', '-target', 'pal-dvd', '-aspect', '4:3', '-c:a', 'ac3', out / (name + '.vob'))
    def dvd(name, body, subtitle=False):
        dest = out / (name + '-authored')
        if not dest.exists():
            xml = out / (name + '.xml')
            xml.write_text(f'<dvdauthor dest="{dest.as_posix()}"><vmgm/><titleset><titles><video format="pal" aspect="4:3"/><audio format="ac3" lang="en"/><audio format="ac3" lang="fr"/>' +
                           ('<subpicture lang="fr"/>' if subtitle else '') + body + '</titles></titleset></dvdauthor>')
            run(tools / 'dvdauthor.exe', '-x', xml, env=dict(os.environ, VIDEO_FORMAT='PAL'))
        run(tools / 'mkisofs.exe', '-dvd-video', '-udf', '-V', 'CYVR_ORIGINAL_TEST', '-o', out / (name + '.iso'), dest)
    dvd('dvd', f'<pgc><vob file="{(out / "dvd-main.vob").as_posix()}" chapters="0,2,4,6"/></pgc><pgc><vob file="{(out / "dvd-extra.vob").as_posix()}"/></pgc>')
    from PIL import Image, ImageDraw
    image = Image.new('P', (240, 40)); image.putpalette([0,0,0,255,255,255,255,255,0] + [0] * 759)
    draw = ImageDraw.Draw(image); draw.rectangle((2,2,238,38), fill=2); draw.text((10,12), 'CyVR original DVD caption', fill=1)
    image.save(out / 'caption.png', transparency=0)
    spu = out / 'spu.xml'
    spu.write_text(f'<subpictures><stream><spu start="00:00:00.500" end="00:00:01.500" image="{(out / "caption.png").as_posix()}" xoffset="240" yoffset="500" transparent="000000"/></stream></subpictures>')
    with (out / 'dvd-main.vob').open('rb') as source, (out / 'dvd-caption.vob').open('wb') as target:
        result = subprocess.run([str(tools / 'spumux.exe'), '-m', 'dvd', str(spu)], stdin=source, stdout=target, stderr=subprocess.PIPE)
        if result.returncode: raise RuntimeError(result.stderr.decode(errors='replace'))
    dvd('dvd-cells', f'<pgc><vob file="{(out / "dvd-caption.vob").as_posix()}" chapters="0,2,4,6"/><vob file="{(out / "dvd-extra.vob").as_posix()}"/></pgc>', True)
    run(ff, '-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=1280x720:rate=24', '-f', 'lavfi', '-i',
        'sine=frequency=440:sample_rate=48000', '-t', '6', '-c:v', 'libopenh264', '-b:v', '3M', '-g', '24', '-c:a', 'ac3', '-b:a', '192k', out / 'bd-original.mp4')
    run(ff, '-y', '-hide_banner', '-loglevel', 'error', '-i', out / 'bd-original.mp4', '-c', 'copy', '-bsf:v', 'h264_mp4toannexb,h264_metadata=tick_rate=48/1', out / 'bd-original.ts')
    meta = out / 'bd.meta'
    meta.write_text(f'MUXOPT --blu-ray --custom-chapters=00:00:00.000;00:00:02.000;00:00:04.000\nV_MPEG4/ISO/AVC, "{(out / "bd-original.ts").as_posix()}", track=256, fps=24, insertSEI, contSPS\nA_AC3, "{(out / "bd-original.ts").as_posix()}", track=257, lang=eng\n')
    run(args.tsmuxer, meta, out / 'bluray.iso'); run(args.tsmuxer, meta, out / 'bd-one')
    # Independent clips intentionally start at the same original timestamp.
    # A second playlist chains them to exercise title clock discontinuities.
    bd = out / 'bd-multi'; shutil.copytree(out / 'bd-one', bd, dirs_exist_ok=True)
    for sub, ext in [('STREAM','.m2ts'), ('CLIPINF','.clpi')]:
        shutil.copyfile(bd / 'BDMV' / sub / ('00000' + ext), bd / 'BDMV' / sub / ('00001' + ext))
    mpls = (bd / 'BDMV/PLAYLIST/00000.mpls').read_bytes()
    pl_at, mark_at = struct.unpack_from('>II', mpls, 8); pl_size = struct.unpack_from('>I', mpls, pl_at)[0]
    pl = bytearray(mpls[pl_at:pl_at+4+pl_size]); item = bytearray(pl[10:]); item[2:7] = b'00001'
    single = bytearray(mpls); single[pl_at+12:pl_at+17] = b'00001'; (bd / 'BDMV/PLAYLIST/00001.mpls').write_bytes(single)
    struct.pack_into('>H', pl, 6, 2); pl += item; struct.pack_into('>I', pl, 0, len(pl)-4)
    joined = bytearray(mpls[:pl_at]) + pl; struct.pack_into('>I', joined, 12, len(joined)); joined += mpls[mark_at:]; struct.pack_into('>I', joined, 16, 0)
    (bd / 'BDMV/PLAYLIST/00002.mpls').write_bytes(joined)
    run(tools / 'mkisofs.exe', '-udf', '-o', out / 'bluray-clips.iso', bd)
    print('Four original ISO fixtures authored in', out)

if __name__ == '__main__': main()
