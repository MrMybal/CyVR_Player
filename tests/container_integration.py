"""Original multi-track MKV fixtures; no user media or network services."""
from pathlib import Path
import subprocess
import struct
ROOT=Path(__file__).resolve().parents[1]
fixtures=ROOT/'build/container-fixtures';fixtures.mkdir(parents=True,exist_ok=True)
(fixtures/'captions.srt').write_text('1\n00:00:00,250 --> 00:00:01,000\nBonjour, le Quest !\n\n2\n00:00:01,250 --> 00:00:01,900\nDeuxième sous-titre\n',encoding='utf-8')
(fixtures/'captions.ass').write_text('''[Script Info]
ScriptType: v4.00+
PlayResX: 320
PlayResY: 180
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Noto Sans,20,&H000000FF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.25,0:00:01.00,Default,,0,0,0,,{\\b1}Styled ASS{\\b0}
Dialogue: 0,0:00:01.25,0:00:01.90,Default,,0,0,0,,{\\move(80,140,240,140)}Moving subtitle
''',encoding='utf-8')
(fixtures/'chapters.txt').write_text(';FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=1000\ntitle=First chapter\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=1000\nEND=2000\ntitle=Second chapter\n')
def segment(pts,kind,data):return b'PG'+struct.pack('>IIBH',int(pts*90000),0,kind,len(data))+data
def composition(number,objects):return struct.pack('>HHBHB BBB',320,180,0x10,number,0x80 if number==0 else 0,0,0,objects)+(struct.pack('>HBBHH',0,0,0,100,140) if objects else b'')
sup=segment(.25,0x16,composition(0,1))+segment(.25,0x17,struct.pack('>BBHHHH',1,0,0,0,320,180))+segment(.25,0x14,bytes([0,0,1,235,128,128,255]))
rle=bytes([1,1,0,0,1,1,0,0]);obj=struct.pack('>HBB',0,0,0xc0)+(len(rle)+4).to_bytes(3,'big')+struct.pack('>HH',2,2)+rle
sup+=segment(.25,0x15,obj)+segment(.25,0x80,b'')+segment(1,0x16,composition(1,0))+segment(1,0x80,b'')
(fixtures/'captions.sup').write_bytes(sup)
ffmpeg=ROOT/'.deps/ffmpeg/windows/bin/ffmpeg.exe'
command=[ffmpeg,'-hide_banner','-loglevel','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=24','-f','lavfi','-i','color=blue:size=320x180:rate=24','-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-f','lavfi','-i','sine=frequency=880:sample_rate=48000','-i',fixtures/'captions.srt','-i',fixtures/'captions.ass','-f','ffmetadata','-i',fixtures/'chapters.txt','-map','0:v','-map','1:v','-map','2:a','-map','3:a','-map','4:s','-map','5:s','-map_metadata','6','-map_chapters','6','-t','2','-c:v:0','mpeg4','-c:v:1','ffv1','-c:a:0','aac','-c:a:1','flac','-c:s:0','srt','-c:s:1','ass','-metadata:s:a:0','language=eng','-metadata:s:a:1','language=fra','-metadata:s:s:0','language=fra','-metadata:s:s:1','language=eng','-disposition:s:0','default+forced','-attach',ROOT.parent/'CyVR_UI/assets/fonts/NotoSans-Regular.ttf','-metadata:s:t','mimetype=application/x-truetype-font','-metadata:s:t','filename=NotoSans-Regular.ttf',fixtures/'tracks.mkv']
subprocess.run(list(map(str,command)),check=True)
subprocess.run([str(ROOT/'build/app-windows/cyvr_container_probe.exe'),str(fixtures/'tracks.mkv'),str(ROOT/'build/app-windows/assets')],check=True)
