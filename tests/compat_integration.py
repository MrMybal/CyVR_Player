"""Original AV1/CENC media and pinned synthetic MVC conformance fixtures."""
import base64
import json
from pathlib import Path
import subprocess
from mvc_transport_fixture import write_fixture

ROOT=Path(__file__).resolve().parents[1]
fixtures=ROOT/'build/compat-fixtures'
fixtures.mkdir(parents=True,exist_ok=True)
ffmpeg=ROOT/'.deps/ffmpeg/windows/bin/ffmpeg.exe'
probe=ROOT/'build/app-windows/cyvr_compat_probe.exe'
def run(*command):
    result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=60)
    if result.returncode:raise RuntimeError(result.stderr[-2000:])
    return result.stdout
def decode(path,*extra):return json.loads(run(probe,path,*extra).splitlines()[-1])

for depth in ('8','10'):
    path=fixtures/('av1-'+depth+'.mkv')
    run(ffmpeg,'-hide_banner','-loglevel','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=24',
        '-t','2','-vf','format='+('yuv420p' if depth=='8' else 'yuv420p10le'),'-c:v','libaom-av1',
        '-cpu-used','8','-row-mt','1','-threads','4','-crf','35',path)
    result=decode(path)
    assert result['frames']==48 and result['width']==320 and result['height']==180,result
    print('AV1 '+depth+'-bit:',result,flush=True)

mvc=ROOT/'.deps/edge264/source/tests/conformance/mvc-synthetic'
for name,count in [('mvc_base',2),('mvc_dep_before_base',2),('mvc_interview_reflist',2),('mvc_display_ordering',8)]:
    path=fixtures/(name+'.mkv')
    if name=='mvc_display_ordering':path=mvc/(name+'.264')
    else:run(ffmpeg,'-hide_banner','-loglevel','error','-y','-f','h264','-r','24','-i',mvc/(name+'.264'),'-c:v','copy',path)
    result=decode(path,'views')
    assert result['frames']==count and result['stereo_frames']==count and result['width']==32,result
    if name=='mvc_interview_reflist':assert result['left_hash']!=result['right_hash'],result
    print('MVC '+name+':',result,flush=True)
transport=fixtures/'mvc-two-pid.m2ts'
write_fixture(mvc/'mvc_interview_reflist.264',transport)
result=decode(transport,'views')
assert result['frames']==2 and result['stereo_frames']==2 and result['left_hash']!=result['right_hash'],result
print('MVC two-PID MPEG-TS:',result,flush=True)

plain=fixtures/'plain.mp4'
run(ffmpeg,'-hide_banner','-loglevel','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=24',
    '-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t','2','-c:v','libopenh264','-b:v','400k','-c:a','aac',plain)
# Public deterministic test vectors, generated here; these are not service keys.
key=bytes(range(16));kid=bytes(range(16,32))
encoded=lambda b:base64.urlsafe_b64encode(b).decode().rstrip('=')
jwk=fixtures/'fixture.jwk'
jwk.write_text(json.dumps({'keys':[{'kty':'oct','kid':encoded(kid),'k':encoded(key)}],'type':'temporary'}))
encrypted=fixtures/'cenc.mp4'
run(ffmpeg,'-hide_banner','-loglevel','error','-y','-i',plain,'-c','copy','-encryption_scheme','cenc-aes-ctr',
    '-encryption_key',key.hex(),'-encryption_kid',kid.hex(),encrypted)
reference=decode(plain);result=decode(encrypted,jwk)
assert result['frames']==48 and result['left_hash']==reference['left_hash'],(reference,result)
print('CENC + caller-provided JWK reproduces clear video:',result,flush=True)
missing=subprocess.run([str(probe),str(encrypted)],capture_output=True,text=True,timeout=60)
assert missing.returncode!=0,'Encrypted media unexpectedly decoded without keys'
wrong=fixtures/'wrong.jwk';wrong.write_text(json.dumps({'keys':[{'kty':'oct','kid':encoded(kid),'k':encoded(bytes(16))}]}))
bad=subprocess.run([str(probe),str(encrypted),str(wrong)],capture_output=True,text=True,timeout=60)
assert bad.returncode!=0 or json.loads(bad.stdout.splitlines()[-1])['left_hash']!=reference['left_hash']
print('Missing/wrong ClearKey cannot reproduce the clear image',flush=True)
