"""Regression for display-space enhancement, using only original synthetic media.

No private state is exported. Optional backup/restore stays inside app storage.
"""
import argparse,json,re,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ADB=Path.home()/'AppData/Local/Android/Sdk/platform-tools/adb.exe'
PACKAGE='com.cyvr.player'
def main():
    p=argparse.ArgumentParser();p.add_argument('--serial',required=True);p.add_argument('--panel-only',action='store_true');p.add_argument('--restore-state',action='store_true');args=p.parse_args()
    checks=[]
    def adb(*parts):
        r=subprocess.run([str(ADB),'-s',args.serial,*map(str,parts)],capture_output=True,text=True,timeout=30)
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
        return r.stdout.strip()
    def logs():
        pid=adb('shell','pidof',PACKAGE)
        if not pid:return ''
        out=adb('logcat','-d','--pid='+pid,'-s','CyVRPlayer:V','CyVR:V')
        if any(word in out for word in ('FILTER_ERROR','RENDER_ERROR','MEDIA_ERROR:','FATAL:')):raise RuntimeError(out[-5000:])
        return out
    def wait(condition,message,seconds=25):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            result=condition()
            if result:return result
            time.sleep(.2)
        raise RuntimeError(message+'\n'+logs()[-3000:])
    def tap(x,y):adb('shell','input','tap',round(x*1600/1440),round(y*1100/990))
    def passed(name,**details):
        checks.append(dict(name=name,passed=True,**details));print(name,details,flush=True)
        (ROOT/'build/quest-view-results.json').write_text(json.dumps({'checks':checks,'complete':False},indent=2)+'\n')
    remote='/sdcard/Android/data/com.cyvr.player/files/Exemples'
    try:
        adb('shell','mkdir','-p',remote)
        for name in ('view-sbs-hd.mp4','view-sbs-8k.mp4'):adb('push',ROOT/'build/media-fixtures'/name,remote+'/'+name)
        for name,dimensions in (('view-sbs-hd.mp4','1920x960'),('view-sbs-8k.mp4','7680x3840')):
            adb('shell','input','keyevent','KEYCODE_WAKEUP');adb('shell','am','force-stop',PACKAGE)
            adb('shell','am','start','-W','-n',PACKAGE+'/.PanelActivity','-a','android.intent.action.VIEW','-t','video/mp4','-d',remote+'/'+name)
            wait(lambda:'VIDEO_COMPLETE' in logs(),'Fixture did not play')
            tap(1220,125);tap(180,275);tap(650,416);tap(650,566);tap(1180,275)
            # Every default filter must stay active on both a modest per-eye source and 8K source.
            tap(180,540)
            for quality,x in ((1,530),(2,875),(3,1230)):
                before=len(logs());tap(x,385)
                line=wait(lambda:next((line for line in logs()[before:].splitlines() if f'IMAGE_APPLIED quality={quality} source={dimensions}' in line),''),'Panel enhancement did not apply')
                m=re.search(r'input=(\d+)x(\d+) output=(\d+)x(\d+)',line);assert m
                iw,ih,ow,oh=map(int,m.groups());assert max(ow,oh)<=1600
                passed('panel-default-filter',source=dimensions,quality=quality,input=[iw,ih],output=[ow,oh])
                if not args.panel_only:
                    tap(1361,48)
                    line=wait(lambda:next((line for line in logs().splitlines() if f'IMAGE_APPLIED quality={quality} source={dimensions}' in line and 'host=immersive' in line),''),'Cinema screen filter did not appear')
                    m=re.search(r'input=(\d+)x(\d+) output=(\d+)x(\d+)',line);assert m
                    iw,ih,ow,oh=map(int,m.groups());assert max(ow,oh)<4096
                    passed('immersive-default-filter',source=dimensions,quality=quality,input=[iw,ih],output=[ow,oh])
                    count=logs().count('APP_READY host=panel');adb('shell','input','keyevent','KEYCODE_BACK')
                    wait(lambda:logs().count('APP_READY host=panel')>count,'Panel return failed')
            # Targets are editable before selecting any enhancement, and remain selected afterwards.
            tap(180,385)
            for x in (530,875,1230,180):tap(x,540)
            tap(875,385)
            wait(lambda:'IMAGE_APPLIED quality=2 source='+dimensions in logs(),'Default target could not be selected after editing in Native')
            passed('target-buttons-accessible-in-native',source=dimensions)
        (ROOT/'build/quest-view-results.json').write_text(json.dumps({'checks':checks,'complete':True,'limits':'Synthetic HD stereo and 8K source. Actual display-space pipelines and host transitions; no subjective detail or long thermal certification.'},indent=2)+'\n')
    finally:
        if args.restore_state:
            adb('shell','am','force-stop',PACKAGE)
            adb('shell','run-as',PACKAGE,'cp','files/player-state.before-view-fix.bin','files/player-state.bin')
            adb('shell','am','start','-W','-n',PACKAGE+'/.PanelActivity')
if __name__=='__main__':main()
