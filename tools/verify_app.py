"""Validate APK native architecture, 16 KiB ELF/ZIP alignment and notices."""
import json
import argparse
import os
import re
import subprocess
from pathlib import Path
import struct
import zipfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--panel-only',action='store_true');args=parser.parse_args()
    apk=ROOT/'dist/android'/('CyVR_Player-panel-debug.apk' if args.panel_only else 'CyVR_Player-debug.apk')
    sdk=Path(os.environ.get('ANDROID_HOME',Path(os.environ['LOCALAPPDATA'])/'Android/Sdk'))
    manifest=subprocess.check_output([str(sdk/'build-tools/35.0.1/aapt2.exe'),'dump','xmltree',str(apk),'--file','AndroidManifest.xml'],text=True)
    activities=re.split(r'(?m)^            E: activity(?:-alias)? ',manifest)[1:]
    assert len(activities)==(2 if args.panel_only else 3)
    assert 'com.cyvr.player.PanelActivity' in activities[0]
    for category in ('android.intent.category.LAUNCHER','com.oculus.intent.category.2D','com.oculus.intent.category.VR_HOME_LAUNCHER','com.oculus.intent.category.OVERLAY_LAUNCHER'):
        assert category in activities[0] and all(category not in activity for activity in activities[1:]),category
    assert all('android.app.lib_name' in activity and '="cyvr_player"' in activity for activity in activities)
    assert ('com.cyvr.player.ImmersiveActivity' in manifest)==(not args.panel_only)
    assert ('org.khronos.openxr.runtime_broker' in manifest)==(not args.panel_only)
    libraries=[]
    with zipfile.ZipFile(apk) as archive,apk.open('rb') as raw:
        names=set(archive.namelist())
        assert ('lib/arm64-v8a/libopenxr_loader.so' in names)==(not args.panel_only)
        if args.panel_only:
            native=archive.read('lib/arm64-v8a/libcyvr_player.so')
            assert b'xrCreateInstance' not in native and b'libopenxr_loader.so' not in native
        for required in ['classes.dex','assets/video.vert.spv','assets/video.frag.spv','assets/video_filter.frag.spv',
                'assets/font_atlas.bin','assets/hand_studio.bin','assets/ui.vert.spv','assets/ui.frag.spv',
                'assets/glass.frag.spv','assets/environment.vert.spv','assets/environment.frag.spv',
                'assets/spatial.vert.spv','assets/spatial.frag.spv',
                'assets/licenses/CyVR_UI-MIT.txt','assets/licenses/FFmpeg-LGPLv3.txt','assets/licenses/MbedTLS-Apache-2.0.txt',
                'assets/licenses/third_party_gsr1_LICENSE']:
            assert required in names,required
        for item in archive.infolist():
            if not item.filename.endswith('.so'):continue
            data=archive.read(item)
            assert data[:6]==b'\x7fELF\x02\x01',item.filename
            assert struct.unpack_from('<H',data,18)[0]==183,item.filename
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
    result={'apk':str(apk),'immersive':not args.panel_only,'native_libraries':len(libraries),'arm64':True,'elf_load_alignment':16384,'zip_library_alignment':16384,'licenses_present':True}
    (ROOT/('build/apk-validation-panel.json' if args.panel_only else 'build/apk-validation.json')).write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))
if __name__=='__main__':main()
