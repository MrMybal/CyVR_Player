"""Real Matroska StereoMode and MP4 st3d/sv3d fixtures, generated locally."""
import json
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/layout-fixtures'
FFMPEG=ROOT/'.deps/ffmpeg/windows/bin/ffmpeg.exe'
PROBE=ROOT/'build/app-windows/cyvr_layout_probe.exe'
def run(*args):
    result=subprocess.run(list(map(str,args)),capture_output=True,text=True)
    if result.returncode:raise RuntimeError(result.stderr[-1500:])
    return result.stdout
def main():
    OUT.mkdir(exist_ok=True);records=[]
    def check(path,stereo,projection,half=False,swap=False,ambiguous=False):
        result=json.loads(run(PROBE,path))
        expected=dict(stereo=stereo,projection=projection,half=half,swap=swap,evidence=2,ambiguous=ambiguous)
        assert result==expected,(path.name,result,expected)
        records.append(dict(file=path.name,result=result));print(path.name,result,flush=True)
    for name,size,packing,projection,stereo,proj,half,swap in [
        ('wrong_180_LR.mkv','384x216','bottom_top','equirectangular',2,2,False,True),
        ('half-sbs.mkv','192x108','left_right','rectilinear',1,0,True,False),
        ('full-sbs.mkv','384x108','left_right','rectilinear',1,0,False,False),
        ('stereo_180.mkv','384x192','right_left','vr180',1,1,False,True),
    ]:
        path=OUT/name
        run(FFMPEG,'-hide_banner','-loglevel','error','-y','-f','lavfi','-i','testsrc2=size='+size+':rate=24','-t','0.25','-c:v','mpeg4','-metadata:s:v:0','stereo_mode='+packing,'-metadata:s:v:0','projection='+projection,path)
        check(path,stereo,proj,half,swap)
    base=OUT/'plain.mp4'
    run(FFMPEG,'-hide_banner','-loglevel','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=24','-t','0.25','-c:v','libopenh264','-b:v','200k',base)
    for name,stereo,projection in [('mp4-180.mp4',1,1),('mp4-360.mp4',2,2),('mp4-mono-360.mp4',0,2),('mp4-cube_360.mp4',1,3)]:
        path=OUT/name;run(PROBE,'--write-metadata',base,path,stereo,projection)
        check(path,stereo,projection if projection!=3 else 0,half=projection==3,ambiguous=projection==3)
    (ROOT/'build/layout-integration-results.json').write_text(json.dumps(records,indent=2)+'\n')
if __name__=='__main__':main()
