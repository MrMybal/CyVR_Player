"""AAudio playback and PCM reference validation using generated files only."""
import argparse
import json
import os
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
REMOTE='/data/local/tmp/cyvr_player_audio_tests'
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--serial',required=True);args=parser.parse_args()
    adb=Path(os.environ.get('ANDROID_HOME',Path.home()/'AppData/Local/Android/Sdk'))/'platform-tools/adb.exe'
    def call(*parts,timeout=45):
        r=subprocess.run([str(adb),'-s',args.serial,*map(str,parts)],capture_output=True,text=True,timeout=timeout)
        if r.returncode:raise RuntimeError(r.stdout[-2000:]+r.stderr[-2000:])
        return r.stdout
    build=ROOT/'build/app-android-alpha';fixtures=ROOT/'build/audio-fixtures'
    call('shell','mkdir','-p',REMOTE)
    call('push',build/'assets',build/'cyvr_audio_probe',build/'cyvr_video_gpu_probe',*sorted(fixtures.glob('*.mkv')),
        *sorted((ROOT/'.deps/ffmpeg/android-arm64-alpha/lib').glob('*.so')),REMOTE+'/')
    call('shell','chmod','755',REMOTE+'/cyvr_audio_probe',REMOTE+'/cyvr_video_gpu_probe')
    def probe(program,*parts):
        output=call('shell','env','LD_LIBRARY_PATH='+REMOTE,'TMPDIR='+REMOTE,REMOTE+'/'+program,*parts)
        return json.loads(next(line for line in output.splitlines() if line.startswith('{')))
    results=[]
    def record(case,result):
        results.append({'case':case,'result':result});print(case,result,flush=True)
        (ROOT/'build/quest-audio-results.json').write_text(json.dumps(results,indent=2)+'\n')
    for name in ('stereo-441','chunked-stereo','surround-51','surround-71'):
        original=fixtures/(name+'.mkv');reference=json.loads(subprocess.check_output([str(ROOT/'build/app-windows/cyvr_audio_probe.exe'),str(original)],text=True))
        decoded=probe('cyvr_audio_probe',REMOTE+'/'+original.name)
        for key in ('frames','limited','invalid','discontinuities'):assert decoded[key]==reference[key],(name,decoded,reference)
        for key in ('peak','rms_left','rms_right'):assert abs(decoded[key]-reference[key])<1e-5,(name,decoded,reference)
        record('pcm-'+name,decoded)
        for codec in ('c2.qti.avc.decoder','software'):
            result=probe('cyvr_video_gpu_probe',REMOTE+'/'+original.name,codec,REMOTE+'/assets',REMOTE+'/diagnostic.ppm','native','130')
            assert result['audio_started'],result
            for key in ('audio_underrun_frames','audio_gap_frames','audio_expired_frames','audio_limited_samples','audio_invalid_samples','audio_xruns'):assert result[key]==0,(name,codec,key,result)
            record('playback-'+name+'-'+codec,result)
    result=probe('cyvr_video_gpu_probe',REMOTE+'/chunked-stereo.mkv','software',REMOTE+'/assets',REMOTE+'/diagnostic.ppm','controls','100')
    assert result['audio_started'] and not result['audio_underrun_frames'] and not result['audio_gap_frames'],result
    record('chunked-pause-seek-resume',result)
if __name__=='__main__':main()
