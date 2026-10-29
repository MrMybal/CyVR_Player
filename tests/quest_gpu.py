"""Validate actual AHardwareBuffer sampling on Quest without a tracked head pose."""
import argparse
import json
import hashlib
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
ADB=Path.home()/'AppData/Local/Android/Sdk/platform-tools/adb.exe'
REMOTE='/data/local/tmp/cyvr_player_gpu_validation'
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--serial',required=True);args=parser.parse_args()
    def adb(*parts):
        result=subprocess.run([str(ADB),'-s',args.serial,*map(str,parts)],capture_output=True,text=True,timeout=40)
        if result.returncode:raise RuntimeError(f'adb failed ({result.returncode}): {parts}\n'+result.stdout+result.stderr)
        return result.stdout.strip()
    adb('shell','am','force-stop','com.cyvr.player')
    adb('shell','mkdir','-p',REMOTE)
    adb('push',ROOT/'build/app-android/assets',
        *sorted((ROOT/'.deps/ffmpeg/android-arm64/lib').glob('*.so')),
        ROOT/'build/media-fixtures/avc.mp4',ROOT/'build/media-fixtures/hevc.mp4',REMOTE+'/')
    executable=ROOT/'build/app-android/cyvr_video_gpu_probe'
    adb('push',executable,REMOTE+'/cyvr_video_gpu_probe')
    digest=hashlib.sha256(executable.read_bytes()).hexdigest()
    if not adb('shell','sha256sum',REMOTE+'/cyvr_video_gpu_probe').startswith(digest):raise RuntimeError('adb executable copy verification failed')
    adb('shell','chmod','755',REMOTE+'/cyvr_video_gpu_probe')
    results=[]
    def record(entry):
        results.append(entry)
        (ROOT/'build/quest-gpu-results.json').write_text(json.dumps({'checks':results,'complete':False,'limits':'Partial checkpoint; 320x192 source, no sustained benchmark.'},indent=2)+'\n')
    for name in ('avc','hevc'):
        codec='c2.qti.'+name+'.decoder' # specific to the connected Quest 3; capability probe validated separately
        output=adb('shell','env','LD_LIBRARY_PATH='+REMOTE,REMOTE+'/cyvr_video_gpu_probe',REMOTE+'/'+name+'.mp4',codec,REMOTE+'/assets',REMOTE+'/'+name+'.ppm')
        result=json.loads(next(line for line in output.splitlines() if line.startswith('{')))
        adb('pull',REMOTE+'/'+name+'.ppm',ROOT/'build'/('quest-'+name+'.ppm'))
        record({'codec':name,'passed':True,'result':result});print(name,result,flush=True)
        controlled=adb('shell','env','LD_LIBRARY_PATH='+REMOTE,REMOTE+'/cyvr_video_gpu_probe',REMOTE+'/'+name+'.mp4',codec,REMOTE+'/assets',REMOTE+'/'+name+'-seek.ppm','controls')
        control_result=json.loads(next(line for line in controlled.splitlines() if line.startswith('{')))
        record({'codec':name,'case':'pause-seek-resume-new-epoch','passed':True,'result':control_result});print(name,'pause / seek / resume',control_result,flush=True)
        for mode in ('cas','fsr','gsr'):
            output=adb('shell','env','LD_LIBRARY_PATH='+REMOTE,REMOTE+'/cyvr_video_gpu_probe',REMOTE+'/'+name+'.mp4',codec,REMOTE+'/assets',REMOTE+'/'+name+'-'+mode+'.ppm',mode)
            result=json.loads(next(line for line in output.splitlines() if line.startswith('{')))
            record({'codec':name,'case':mode,'passed':True,'result':result});print(name,mode,result,flush=True)
    (ROOT/'build/quest-gpu-results.json').write_text(json.dumps({'checks':results,'complete':True,'limits':'Offscreen 320x192; no OpenXR composition, headset visual confirmation or sustained 8K benchmark.'},indent=2)+'\n')
if __name__=='__main__':main()
