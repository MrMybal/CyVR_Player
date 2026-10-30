"""Original PCM/encoded surround references and deliberately chunked Matroska."""
import json
import math
from pathlib import Path
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]
FIXTURES=ROOT/'build/audio-fixtures'
FFMPEG=ROOT/'.deps/ffmpeg/windows/bin/ffmpeg.exe'
PROBE=ROOT/'build/app-windows/cyvr_audio_probe.exe'

def run(*args):
    r=subprocess.run(list(map(str,args)),capture_output=True,timeout=60)
    if r.returncode:raise RuntimeError(r.stderr.decode(errors='replace')[-2000:])
    return r.stdout

def vint(data,offset,identifier=False):
    first=data[offset];width=1
    while not first & (0x80>>(width-1)):
        width+=1
        if width>8:raise ValueError('Invalid EBML integer')
    raw=int.from_bytes(data[offset:offset+width],'big')
    return (raw if identifier else raw & ((1<<(width*7))-1)),width

def chunked_matroska(source,destination):
    data=bytearray(source.read_bytes());changed=0
    def visit(begin,end):
        nonlocal changed
        offset=begin
        while offset<end:
            identifier,iw=vint(data,offset,True);size,sw=vint(data,offset+iw)
            start=offset+iw+sw;stop=min(end,start+size)
            if identifier==0x18538067:visit(start,stop)
            elif identifier==0x1f43b675:
                parts=[];at=start
                while at<stop:
                    child,cw=vint(data,at,True);length,lw=vint(data,at+cw);tail=at+cw+lw+length
                    if tail>stop:raise ValueError('Truncated cluster')
                    track=vint(data,at+cw+lw)[0] if child==0xa3 else 0
                    parts.append((track,bytes(data[at:tail])));at=tail
                blocks=[(t,b) for t,b in parts if t]
                if any(t==1 for t,b in blocks) and any(t==2 for t,b in blocks):
                    # Preserve byte counts and every per-track timestamp/order.
                    ordered=iter([b for t,b in blocks if t==1]+[b for t,b in blocks if t!=1])
                    data[start:stop]=b''.join(next(ordered) if t else b for t,b in parts);changed+=1
            offset=stop
    visit(0,len(data));assert changed,'No interleaved cluster to rearrange'
    destination.write_bytes(data)

def main():
    FIXTURES.mkdir(parents=True,exist_ok=True);results=[]
    for name,rate,channels,codec in [('stereo-441',44100,2,'flac'),('surround-51',48000,6,'aac'),('surround-71',48000,8,'flac')]:
        path=FIXTURES/(name+'.mkv')
        expressions=['0.70*sin(2*PI*440*t)','0.40*sin(2*PI*880*t)'] if channels==2 else ['0.90*sin(2*PI*440*t)']*channels
        layout={2:'stereo',6:'5.1',8:'7.1'}[channels]
        run(FFMPEG,'-hide_banner','-loglevel','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=24',
            '-f','lavfi','-i','aevalsrc='+('|'.join(expressions))+':s='+str(rate)+':c='+layout,
            '-t','6','-c:v','libopenh264','-b:v','400k','-c:a',codec,path)
        result=json.loads(run(PROBE,path))
        reference=run(FFMPEG,'-hide_banner','-loglevel','error','-i',path,'-vn','-af','aresample=48000:rematrix_maxval=1:rematrix_volume='+('0.8912509381337456' if channels>2 else '1'),
            '-ac','2','-c:a','pcm_f32le','-f','f32le','pipe:1')
        samples=struct.unpack('<'+'f'*(len(reference)//4),reference)
        frames=len(samples)//2;peak=max(map(abs,samples))
        rms=[math.sqrt(sum(s*s for s in samples[c::2])/frames) for c in (0,1)]
        assert result['frames']==frames and not result['discontinuities'],result
        assert result['limited']==0 and result['invalid']==0 and abs(result['peak']-peak)<1e-5,result
        for key,ref in zip(('rms_left','rms_right'),rms):assert abs(result[key]-ref)<1e-5,(result,rms)
        results.append({'case':name,'reference':'FFmpeg normalized float stereo','result':result})
        print(name,result,flush=True)
        if channels==2:
            chunked=FIXTURES/'chunked-stereo.mkv';chunked_matroska(path,chunked)
            chunked_result=json.loads(run(PROBE,chunked));assert chunked_result==result,(result,chunked_result)
            results.append({'case':'chunked-stereo','result':chunked_result})
    (ROOT/'build/audio-reference-results.json').write_text(json.dumps(results,indent=2)+'\n')
if __name__=='__main__':main()
