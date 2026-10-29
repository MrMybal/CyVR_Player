"""Validate the FFmpeg -> Annex B -> hardware MediaCodec -> AHardwareBuffer path."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
REMOTE="/data/local/tmp/cyvr_player_development"

def main():
    parser=argparse.ArgumentParser();parser.add_argument("--serial",required=True);args=parser.parse_args()
    sdk=Path(os.environ.get("ANDROID_HOME",str(Path(os.environ["LOCALAPPDATA"])/"Android/Sdk")))
    tools=max((p for p in (sdk/"build-tools").iterdir() if p.is_dir() and (p/"d8.bat").exists()),key=lambda p:tuple(map(int,p.name.split("."))))
    platform=max((p for p in (sdk/"platforms").iterdir() if p.name.startswith("android-") and p.name[8:].isdigit()),key=lambda p:int(p.name[8:]))
    environment=os.environ.copy();environment.setdefault("JAVA_HOME",r"C:\Program Files\Android\Android Studio\jbr")
    dex=ROOT/"build/android-arm64/codec-probe.zip"
    classes=list((ROOT/"build/android-arm64/java").rglob("*.class"))
    subprocess.run([str(tools/"d8.bat"),"--min-api","29","--lib",str(platform/"android.jar"),"--output",str(dex),*map(str,classes)],check=True,env=environment)
    ffmpeg=ROOT/".deps/ffmpeg/windows/bin/ffmpeg.exe"
    fixtures=ROOT/"build/media-fixtures"
    fixtures.mkdir(parents=True,exist_ok=True)
    samples=[("avc","video/avc","libopenh264"),("hevc","video/hevc","libkvazaar")]
    for name,mime,encoder in samples:
        command=[str(ffmpeg),"-hide_banner","-loglevel","error","-y","-f","lavfi","-i","testsrc2=size=320x192:rate=24",
            "-t","2","-an","-c:v",encoder,"-b:v","1000k","-g","12"]
        if encoder=="libkvazaar":command += ["-kvazaar-params","preset=ultrafast,threads=2"]
        command += [str(fixtures/(name+".mp4"))]
        subprocess.run(command,check=True)
    adb=shutil.which("adb")
    if not adb:raise SystemExit("adb must be on PATH")
    def call(*command):
        return subprocess.run([adb,"-s",args.serial,*map(str,command)],capture_output=True,text=True,timeout=30)
    def checked(*command):
        result=call(*command)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        return result.stdout.strip()
    model=checked("shell","getprop","ro.product.model")
    checked("shell","mkdir","-p",REMOTE)
    files=[dex,ROOT/"build/android-arm64/cyvr_hardware_probe",*sorted((ROOT/".deps/ffmpeg/android-arm64/lib").glob("*.so")),
        fixtures/"avc.mp4",fixtures/"hevc.mp4"]
    checked("push",*files,REMOTE+"/");checked("shell","chmod","755",REMOTE+"/cyvr_hardware_probe")
    results=[]
    for name,mime,encoder in samples:
        text=checked("shell","env","CLASSPATH="+REMOTE+"/codec-probe.zip","app_process","/system/bin",
            "com.cyvr.player.HeadlessCodecProbe",mime,"320","192","24")
        candidates=[line.strip() for line in text.splitlines() if line.startswith(("c2.","OMX."))]
        if not candidates:raise RuntimeError("Android reports no hardware candidate for "+mime)
        output=checked("shell","env","LD_LIBRARY_PATH="+REMOTE,REMOTE+"/cyvr_hardware_probe",REMOTE+"/"+name+".mp4",candidates[0],"48")
        data=json.loads(next(line for line in output.splitlines() if line.startswith("{")))
        assert data["hardware_frames"]==48 and not data["pixels_copied_to_cpu"],data
        results.append({"codec":mime,"android_component":candidates[0],"passed":True,"result":data})
        print(mime+" hardware decoded 48 frames through "+candidates[0],flush=True)
    (ROOT/"build/quest-hardware-results.json").write_text(json.dumps({"device":model,"checks":results,
        "limits":"320x192 two-second decode smoke test; no image rendering, sustained/high-resolution performance or thermal measurement."},indent=2)+"\n")

if __name__=="__main__":main()
