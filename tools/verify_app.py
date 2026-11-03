"""Validate APK native architecture, 16 KiB ELF/ZIP alignment and notices."""
import json
import argparse
import os
import re
import subprocess
from pathlib import Path
import struct
import zipfile
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[1]
def jpeg_dimensions(data):
    assert data[:2]==bytes((255,216))
    pos=2
    while pos+4<=len(data):
        assert data[pos]==255
        marker=data[pos+1];pos+=2
        if marker==255:pos-=1;continue
        size=int.from_bytes(data[pos:pos+2],'big');assert size>=2
        if marker in (192,193,194):
            height,width=struct.unpack_from('>HH',data,pos+3);return width,height
        pos+=size
    raise AssertionError('JPEG frame dimensions missing')
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--panel-only',action='store_true')
    channel=parser.add_mutually_exclusive_group();channel.add_argument('--alpha',action='store_true');channel.add_argument('--beta',action='store_true');args=parser.parse_args()
    release=args.alpha or args.beta
    version=ET.parse(ROOT/'android/AndroidManifest.xml').getroot().get('{http://schemas.android.com/apk/res/android}versionName')
    name=f'CyVR_Player-{version}-'+('panel-' if args.panel_only else '')+'arm64.apk' if release else ('CyVR_Player-panel-debug.apk' if args.panel_only else 'CyVR_Player-debug.apk')
    apk=ROOT/'dist/android'/name
    sdk=Path(os.environ.get('ANDROID_HOME',Path(os.environ['LOCALAPPDATA'])/'Android/Sdk'))
    manifest=subprocess.check_output([str(sdk/'build-tools/35.0.1/aapt2.exe'),'dump','xmltree',str(apk),'--file','AndroidManifest.xml'],text=True)
    if release:
        debug=re.search(r'android:debuggable[^=]*=(?:\(type 0x12\))?(false|true|0x[0-9a-f]+)',manifest)
        assert debug and debug[1] in ('false','0x0'), 'Prerelease APK must not be debuggable'
        assert version in manifest and ('-beta.' if args.beta else '-alpha.') in version
    activities=re.split(r'(?m)^            E: activity(?:-alias)? ',manifest)[1:]
    browser=[activity for activity in activities if 'com.cyvr.player.BrowserActivity' in activity]
    assert len(browser)==1 and ':browser' in browser[0],'The web browser must run in its own process'
    activities=[activity for activity in activities if activity not in browser]
    assert len(activities)==(2 if args.panel_only else 3)
    assert 'com.cyvr.player.PanelActivity' in activities[0]
    for category in ('android.intent.category.LAUNCHER','com.oculus.intent.category.2D','com.oculus.intent.category.VR_HOME_LAUNCHER','com.oculus.intent.category.OVERLAY_LAUNCHER'):
        assert category in activities[0] and all(category not in activity for activity in activities[1:]),category
    assert all('android.app.lib_name' in activity and '="cyvr_player"' in activity for activity in activities)
    assert ('com.cyvr.player.ImmersiveActivity' in manifest)==(not args.panel_only)
    assert ('org.khronos.openxr.runtime_broker' in manifest)==(not args.panel_only)
    assert 'android:icon' in manifest and 'android:roundIcon' in manifest
    libraries=[]
    with zipfile.ZipFile(apk) as archive,apk.open('rb') as raw:
        names=set(archive.namelist())
        assert len(archive.read('assets/branding/player-logo.rgba'))==160*160*4
        for icon in ('ic_launcher','ic_launcher_round'):
            resource='res/mipmap-anydpi-v26/'+icon+'.xml'
            assert resource in names,resource
            xml=subprocess.check_output([str(sdk/'build-tools/35.0.1/aapt2.exe'),'dump','xmltree',str(apk),'--file',resource],text=True)
            assert 'adaptive-icon' in xml and 'background' in xml and 'foreground' in xml
        if not args.panel_only:
            expected_pbr={'basecolor','normal-rough-metal','brdf','lounge','observatory','terrace','lighting-lounge','lighting-observatory','lighting-terrace'}
            for material in expected_pbr:
                assert 'assets/pbr/'+material+'.bin' in names,material
            for photo in ('lebombo','venice_sunset'):
                assert jpeg_dimensions(archive.read('assets/scenes/'+photo+'.jpg'))==(4096,2048), 'Cinema backgrounds must retain their bundled 4K resolution'
            assert 'assets/licenses/CyVR_UI-environments-CC0.txt' in names and 'assets/licenses/CyVR_UI-environment-credits.md' in names
        assert ('lib/arm64-v8a/libopenxr_loader.so' in names)==(not args.panel_only)
        if args.panel_only:
            assert not any(name.startswith('assets/pbr/') for name in names),'2D-only must not bundle 3D scene resources'
            native=archive.read('lib/arm64-v8a/libcyvr_player.so')
            assert b'xrCreateInstance' not in native and b'libopenxr_loader.so' not in native
        for required in ['classes.dex','assets/video.vert.spv','assets/video.frag.spv','assets/video_filter.frag.spv',
                'assets/font_atlas.bin','assets/hand_studio.bin','assets/ui.vert.spv','assets/ui.frag.spv',
                'assets/subtitle-font.ttf','assets/licenses/libass-ISC.txt','assets/licenses/FriBidi-LGPLv2.1.txt',
                'assets/licenses/dav1d-BSD-2-Clause.txt','assets/licenses/edge264-mvc-BSD-3-Clause.txt',
                'assets/licenses/libbluray-COPYING.txt','assets/licenses/libdvdread-COPYING.txt','assets/licenses/libudfread-COPYING.txt',
                'assets/licenses/FreeType-FTL.txt','assets/licenses/HarfBuzz-MIT.txt','assets/licenses/SplatKit-MIT.txt','assets/licenses/VMA-MIT.txt',
                'assets/glass.frag.spv','assets/environment.vert.spv','assets/environment.frag.spv',
                'assets/spatial.vert.spv','assets/spatial.frag.spv',
                'assets/licenses/CyVR_Player-GPLv3.txt','assets/licenses/CyVR_Player-NOTICE.txt',
                'assets/licenses/CyVR_UI-MIT.txt','assets/licenses/FFmpeg-LGPLv3.txt','assets/licenses/MbedTLS-Apache-2.0.txt',
                'assets/licenses/third_party_gsr1_LICENSE']:
            assert required in names,required
        for packaged,source in [('CyVR_Player-GPLv3.txt','LICENSE'),('CyVR_Player-NOTICE.txt','NOTICE')]:
            assert archive.read('assets/licenses/'+packaged)==(ROOT/source).read_bytes(),packaged
        for item in archive.infolist():
            if not item.filename.endswith('.so'):continue
            data=archive.read(item)
            assert data[:6]==b'\x7fELF\x02\x01',item.filename
            assert struct.unpack_from('<H',data,18)[0]==183,item.filename
            if release:
                shoff=struct.unpack_from('<Q',data,40)[0]
                shsize,shcount,shstrings=struct.unpack_from('<HHH',data,58)
                strings_header=shoff+shsize*shstrings
                offset,length=struct.unpack_from('<QQ',data,strings_header+24)
                strings=data[offset:offset+length]
                for i in range(shcount):
                    start=struct.unpack_from('<I',data,shoff+shsize*i)[0]
                    section=strings[start:].split(b'\0',1)[0]
                    assert not section.startswith(b'.debug_') and section!=b'.symtab', (item.filename,section)
            phoff=struct.unpack_from('<Q',data,32)[0]
            phsize,phcount=struct.unpack_from('<HH',data,54)
            for i in range(phcount):
                offset=phoff+i*phsize
                if struct.unpack_from('<I',data,offset)[0]==1:
                    assert struct.unpack_from('<Q',data,offset+48)[0]>=16384,item.filename
            raw.seek(item.header_offset)
            header=raw.read(30)
            name_length,extra_length=struct.unpack_from('<HH',header,26)
            assert (item.header_offset+30+name_length+extra_length)%16384==0,item.filename
            libraries.append(item.filename)
    result={'apk':str(apk),'alpha':args.alpha,'beta':args.beta,'immersive':not args.panel_only,'native_libraries':len(libraries),'arm64':True,'elf_load_alignment':16384,'zip_library_alignment':16384,'licenses_present':True}
    report='build/apk-validation'+('-beta' if args.beta else '-alpha' if args.alpha else '')+('-panel' if args.panel_only else '')+'.json'
    (ROOT/report).write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))
if __name__=='__main__':main()
