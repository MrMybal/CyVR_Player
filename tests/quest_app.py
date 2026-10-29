"""Real APK playback via a local HTTP origin; restores only our adb reverse."""
import argparse
import functools
import http.server
import json
import re
from pathlib import Path
import subprocess
import threading
import time

ROOT=Path(__file__).resolve().parents[1]
ADB=Path.home()/'AppData/Local/Android/Sdk/platform-tools/adb.exe'
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self,*args):pass
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--serial',required=True);args=parser.parse_args()
    fixtures=ROOT/'build/media-fixtures'
    ffmpeg=ROOT/'.deps/ffmpeg/windows/bin/ffmpeg.exe'
    for name in ('avc','hevc'):
        subprocess.run([str(ffmpeg),'-hide_banner','-loglevel','error','-y','-i',str(fixtures/(name+'.mp4')),
            '-c','copy','-movflags','+faststart',str(fixtures/('app_'+name+'.mp4'))],check=True)
    subprocess.run([str(ffmpeg),'-hide_banner','-loglevel','error','-y','-i',str(fixtures/'avc.mp4'),'-c','copy',
        '-hls_time','0.5','-hls_list_size','0','-hls_segment_filename',str(fixtures/'app-part%02d.ts'),str(fixtures/'app-stream.m3u8')],check=True)
    def adb(*parts):
        r=subprocess.run([str(ADB),'-s',args.serial,*map(str,parts)],capture_output=True,text=True,timeout=30)
        if r.returncode and parts[:2]!=('shell','pidof'):raise RuntimeError(r.stdout+r.stderr)
        return r.stdout.strip()
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=ROOT/'build/media-fixtures'))
    port=server.server_port
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    adb('reverse',f'tcp:{port}',f'tcp:{port}')
    results=[]
    try:
        for name,file in (('avc','app_avc.mp4'),('hevc','app_hevc.mp4'),('HLS AVC','app-stream.m3u8')):
            adb('shell','am','force-stop','com.cyvr.player')
            adb('shell','am','start','-n','com.cyvr.player/.PlayerActivity','-a','android.intent.action.VIEW','-t','video/mp4','-d',f'http://127.0.0.1:{port}/{file}')
            pid='';output='';start=time.monotonic()
            while time.monotonic()-start<20:
                pid=adb('shell','pidof','com.cyvr.player')
                if pid:
                    output=adb('logcat','-d',f'--pid={pid}','-s','CyVRPlayer:V')
                    if 'FATAL:' in output:raise RuntimeError(output)
                    if 'MEDIA_ERROR:' in output or 'RENDER_ERROR' in output:raise RuntimeError(output)
                    if 'VIDEO_COMPLETE ' in output:break
                time.sleep(.25)
            counts=re.search(r'VIDEO_COMPLETE decoded=(\d+) scheduled=(\d+) gpu_submitted=(\d+) late_drops=(\d+)',output)
            stats=dict(zip(('decoded','scheduled','gpu_submitted','late_drops'),map(int,counts.groups()))) if counts else {}
            success=bool(counts and stats['decoded']==48 and stats['scheduled']>=24 and stats['scheduled']+stats['late_drops']==48 and stats['gpu_submitted']>=24 and 'host=panel' in output)
            results.append({'codec':name,'passed':success,**stats,'log':output})
            print(name, stats if success else 'presentation incomplete',flush=True)
        (ROOT/'build/quest-app-results.json').write_text(json.dumps({'checks':results,'limits':'320x192, 48 decoded frames per stream. Startup/resize frame drops are counted; not a sustained performance or visual quality certification.'},indent=2)+'\n')
        if not all(r['passed'] for r in results):raise SystemExit('See build/quest-app-results.json')
    finally:
        adb('reverse','--remove',f'tcp:{port}');server.shutdown();server.server_close()
if __name__=='__main__':main()
