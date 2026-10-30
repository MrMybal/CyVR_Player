"""Headless Quest validation with original/pinned fixtures, never app state."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
REMOTE='/data/local/tmp/cyvr_player_compat_tests'
def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--serial',required=True)
    parser.add_argument('--benchmark-splats',type=int,default=100000)
    args=parser.parse_args()
    adb=Path(os.environ.get('ANDROID_HOME',Path.home()/'AppData/Local/Android/Sdk'))/'platform-tools/adb.exe'
    def call(*parts,timeout=45):
        r=subprocess.run([str(adb),'-s',args.serial,*map(str,parts)],capture_output=True,text=True,timeout=timeout)
        if r.returncode:raise RuntimeError(r.stdout[-2000:]+r.stderr[-2000:])
        return r.stdout
    build=ROOT/'build/app-android-alpha'
    fixtures=ROOT/'build/compat-fixtures'
    inputs=[fixtures/name for name in ('av1-8.mkv','av1-10.mkv','plain.mp4','cenc.mp4','fixture.jwk','mvc_interview_reflist.mkv','mvc-two-pid.m2ts')]
    inputs+=[ROOT/'.deps/test-media/multiview.mov',ROOT/'.deps/test-media/pico-mv-hevc.mp4']
    programs=[build/name for name in ('cyvr_compat_probe','cyvr_video_gpu_probe','cyvr_splat_probe')]
    if not all(x.is_file() for x in inputs+programs):raise RuntimeError('Build Alpha/probes and generate the compatibility fixtures first')
    call('shell','mkdir','-p',REMOTE)
    call('push',build/'assets',*programs,*inputs,*sorted((ROOT/'.deps/ffmpeg/android-arm64-alpha/lib').glob('*.so')),REMOTE+'/')
    for executable in programs:
        expected=hashlib.sha256(executable.read_bytes()).hexdigest()
        if not call('shell','sha256sum',REMOTE+'/'+executable.name).startswith(expected):raise RuntimeError('Probe upload hash mismatch')
        call('shell','chmod','755',REMOTE+'/'+executable.name)
    results=[]
    def record(name,result):
        results.append({'case':name,'passed':True,'result':result})
        (ROOT/'build/quest-compat-results.json').write_text(json.dumps({'checks':results,'complete':False},indent=2)+'\n')
        print(name,result,flush=True)
    def probe(name,*parameters,timeout=60):
        output=call('shell','env','LD_LIBRARY_PATH='+REMOTE,'TMPDIR='+REMOTE,REMOTE+'/'+name,*parameters,timeout=timeout)
        return output
    for name,extra in [('av1-8.mkv',[]),('av1-10.mkv',[]),('mvc_interview_reflist.mkv',['views']),('mvc-two-pid.m2ts',['views']),('multiview.mov',['views']),('pico-mv-hevc.mp4',['views']),('cenc.mp4',[REMOTE+'/fixture.jwk'])]:
        output=probe('cyvr_compat_probe',REMOTE+'/'+name,*extra)
        result=json.loads(next(x for x in output.splitlines() if x.startswith('{')))
        host_extra=[str(fixtures/'fixture.jwk')] if name=='cenc.mp4' else extra
        original=next(x for x in inputs if x.name==name)
        host=subprocess.check_output([str(ROOT/'build/app-windows/cyvr_compat_probe.exe'),str(original),*host_extra],text=True,timeout=60)
        reference=json.loads(host.splitlines()[-1])
        for key in ('frames','stereo_frames','width','height','left_hash','right_hash'):assert result[key]==reference[key],(name,key,result,reference)
        record('arm64-decode-'+name,result)
    for name,codec,mode,minimum,key in [('av1-8.mkv','software','native',40,''),('av1-10.mkv','software','cas',40,''),
        ('mvc_interview_reflist.mkv','software','native',2,''),('mvc-two-pid.m2ts','software','native',2,''),('multiview.mov','software','native',6,''),
        ('plain.mp4','c2.qti.avc.decoder','native',40,''),('plain.mp4','software','controls',30,''),
        ('cenc.mp4','c2.qti.avc.decoder','native',40,REMOTE+'/fixture.jwk')]:
        params=[REMOTE+'/'+name,codec,REMOTE+'/assets',REMOTE+'/diagnostic.ppm',mode,str(minimum)]
        if key:params.append(key)
        output=probe('cyvr_video_gpu_probe',*params)
        result=json.loads(next(x for x in output.splitlines() if x.startswith('{')))
        if name in ('plain.mp4','cenc.mp4'):assert result['audio_started'],result
        if name in ('mvc_interview_reflist.mkv','mvc-two-pid.m2ts','multiview.mov'):assert result['stereo'],result
        record('ahb-vulkan-'+name+'-'+codec+'-'+mode,result)
    if args.benchmark_splats:
        output=probe('cyvr_splat_probe',REMOTE+'/assets',str(args.benchmark_splats),'60',timeout=120)
        for line in output.splitlines():
            if line.startswith('{'):record('gaussian-stereo-benchmark',json.loads(line))
    (ROOT/'build/quest-compat-results.json').write_text(json.dumps({'checks':results,'complete':True,
        'limits':'Own offscreen Vulkan targets and AAudio timestamps; physical binocular appearance, sound output and motion-to-photon latency need a worn headset. Synthetic Gaussian timing is CPU submission plus completion waits at the stated size, not compositor FPS or a thermal qualification.'},indent=2)+'\n')
if __name__=='__main__':main()
