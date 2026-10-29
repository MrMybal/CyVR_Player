"""Actual APK input: paused filters, corrections, cinema wake and host transfer.

Uses the small original test fixture and keeps app data and SAF grants intact.
Physical controller motion, binocular appearance and sustained performance are
separate worn-headset checks.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import time
ROOT=Path(__file__).resolve().parents[1]
ADB=Path.home()/'AppData/Local/Android/Sdk/platform-tools/adb.exe'
PACKAGE='com.cyvr.player'
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--serial',required=True);parser.add_argument('--panel-only',action='store_true');args=parser.parse_args()
    def adb(*parts,binary=False):
        r=subprocess.run([str(ADB),'-s',args.serial,*map(str,parts)],capture_output=True,timeout=30)
        if r.returncode:raise RuntimeError(r.stderr.decode(errors='replace'))
        return r.stdout if binary else r.stdout.decode(errors='replace').strip()
    def wait(condition,message):
        until=time.monotonic()+18
        while time.monotonic()<until:
            value=condition()
            if value:return value
            time.sleep(.15)
        raise RuntimeError(message)
    def logs():
        pid=adb('shell','pidof',PACKAGE)
        output=adb('logcat','-d','--pid='+pid,'-s','CyVRPlayer:V','CyVR:V')
        if any(e in output for e in ('RENDER_ERROR','MEDIA_ERROR:','FATAL:')):raise RuntimeError(output[-5000:])
        return output
    def tap(x,y):adb('shell','input','tap',round(x*1600/1440),round(y*1100/990))
    def state():
        data=adb('exec-out','run-as',PACKAGE,'cat','files/player-state.bin',binary=True);assert data[:8]==b'CYVRP04\0';cursor=8
        def number():
            nonlocal cursor
            n=struct.unpack_from('<Q',data,cursor)[0];cursor+=8;return n
        def string():
            nonlocal cursor
            n=number();value=data[cursor:cursor+n].decode();cursor+=n;return value
        result=dict(zip(('directory','url','source','title'),(string() for _ in range(4))))
        result['parents']=[string() for _ in range(number())]
        result.update(zip(('position','page','offset','stereo','projection','flags'),(number() for _ in range(6))))
        assert number()==0x494d4731
        for name in ('exposure','contrast','saturation','gamma','zoom','pan_x','pan_y','sharpness'):
            result[name]=struct.unpack('<f',struct.pack('<I',number()))[0]
        result['quality']=number();result['upscale']=number();return result
    checks=[]
    def passed(name,**details):checks.append(dict(name=name,passed=True,**details));print(name,details,flush=True)
    remote='/sdcard/Android/data/com.cyvr.player/files/Exemples'
    adb('shell','mkdir','-p',remote);adb('push',ROOT/'build/media-fixtures/avc.mp4',remote+'/avc.mp4')
    adb('shell','input','keyevent','KEYCODE_WAKEUP');adb('shell','am','force-stop',PACKAGE)
    adb('shell','am','start','-W','-n',PACKAGE+'/.PanelActivity','-a','android.intent.action.VIEW','-t','video/mp4','-d',remote+'/avc.mp4')
    wait(lambda:'VIDEO_COMPLETE' in logs(),'Playback did not finish')
    tap(1220,125);tap(180,275);tap(180,416);tap(180,566) # format / mono / flat
    tap(1180,275);tap(180,792) # quality / reset
    tap(530,385)
    wait(lambda:'IMAGE_APPLIED quality=1' in logs(),'CAS was not applied to the retained paused frame')
    wait(lambda:state()['quality']==1,'CAS preference not saved');passed('paused-cas-dispatch')
    tap(875,385);tap(700,540)
    wait(lambda:'IMAGE_APPLIED quality=2 input=320x192 output=480x288' in logs(),'FSR did not allocate the requested output')
    wait(lambda:state()['quality']==2,'FSR preference not saved');passed('paused-fsr-1.5x-dispatch')
    tap(1230,385)
    wait(lambda:'IMAGE_APPLIED quality=3 input=320x192 output=480x288' in logs(),'GSR did not render the retained frame')
    wait(lambda:state()['quality']==3,'GSR preference not saved');passed('paused-gsr-1.5x-dispatch')
    tap(700,275);tap(1170,354);tap(781,626)
    before=wait(lambda:(s if abs((s:=state())['exposure']-1)<.03 and 1.9<s['zoom']<2.1 else None),'Image sliders did not update')
    passed('exposure-and-zoom-sliders',exposure=before['exposure'],zoom=before['zoom'])
    tap(530,125);tap(1180,846)
    wait(lambda:'INTERFACE hidden=1' in logs(),'Cinema mode did not hide UI')
    adb('shell','input','keyevent','KEYCODE_ENTER')
    wait(lambda:logs().rfind('INTERFACE hidden=0')>logs().rfind('INTERFACE hidden=1'),'Wake key did not reveal UI')
    passed('cinema-hide-and-wake')
    adb('shell','input','keyevent','KEYCODE_DPAD_LEFT')
    wait(lambda:'SEEK_REQUEST position_us=0' in logs(),'Keyboard seek was not forwarded')
    passed('keyboard-seek')
    if not args.panel_only:
        before=state();panels=logs().count('APP_READY host=panel');tap(1280,48)
        wait(lambda:'APP_READY host=immersive' in logs() and 'host=immersive width=480 height=288' in logs(),'Immersive processed layer not presented')
        during=state()
        for key in ('quality','upscale','exposure','zoom','source'):assert during[key]==before[key],key
        adb('shell','input','keyevent','KEYCODE_BACK')
        wait(lambda:logs().count('APP_READY host=panel')>panels,'2D return failed')
        passed('processed-video-host-transitions')
    # Finish in neutral mode and browse the local fixtures for thumbnail upload.
    tap(1220,125);tap(1180,275);tap(180,792)
    tap(530,125);tap(840,846) # stop before requesting browser previews
    tap(180,125)
    wait(lambda:bool(re.search(r'THUMBNAILS count=[1-7]',logs())),'No browser thumbnail uploaded')
    passed('browser-thumbnail-upload')
    result={'checks':checks,'limits':'ADB input and GPU diagnostics with a 320x192 fixture; physical grip/trigger motion, binocular comfort and sustained high-resolution throughput require manual measurement.'}
    (ROOT/('build/quest-image-panel-results.json' if args.panel_only else 'build/quest-image-results.json')).write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
