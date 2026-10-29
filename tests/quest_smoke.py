"""Run the headless ARM64 media engine on an explicitly selected adb device.

Only generated test media and development binaries are copied to an isolated
/data/local/tmp directory. No APK, system setting, or app data is changed.
"""
import argparse
import functools
import http.server
import json
from pathlib import Path
import shutil
import ssl
import subprocess
import threading
import time

ROOT=Path(__file__).resolve().parents[1]
REMOTE="/data/local/tmp/cyvr_player_development"

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--serial",required=True)
    args=parser.parse_args()
    adb=shutil.which("adb")
    if not adb: raise SystemExit("adb must be on PATH")
    fixtures=ROOT/"build/media-fixtures"
    if not (fixtures/"tls.crt").exists(): raise SystemExit("Run tests/media_integration.py first")
    def call(*command,**kwargs):
        return subprocess.run([adb,"-s",args.serial,*map(str,command)],capture_output=True,text=True,timeout=30,**kwargs)
    def checked(*command):
        result=call(*command,check=True)
        return result.stdout.strip()
    model=checked("shell","getprop","ro.product.model")
    sdk=checked("shell","getprop","ro.build.version.sdk")
    checked("shell","mkdir","-p",REMOTE)
    files=[ROOT/"build/android-arm64/cyvr_media_probe",*sorted((ROOT/".deps/ffmpeg/android-arm64/lib").glob("*.so"))]
    files += [p for p in fixtures.iterdir() if p.suffix in (".mp4",".mkv",".bin",".ts",".m3u8",".crt")]
    checked("push",*files,REMOTE+"/")
    checked("shell","chmod","755",REMOTE+"/cyvr_media_probe")
    cases=[]
    def probe(label,source,*options,success=True):
        started=time.monotonic()
        result=call("shell","env","LD_LIBRARY_PATH="+REMOTE,REMOTE+"/cyvr_media_probe",source,*options)
        elapsed=time.monotonic()-started
        if (result.returncode==0)!=success: raise AssertionError(f"{label}: {result.stdout} {result.stderr}")
        lines=(result.stdout+result.stderr).splitlines()
        data=json.loads(next(line for line in reversed(lines) if line.startswith("{")))
        cases.append({"case":label,"passed":True,"elapsed_seconds":round(elapsed,3),"result":data})
        print(model+": "+label+" passed",flush=True)
        return data,elapsed
    def complete(data):
        assert data["eof"] and data["decoded_video_frames"]==48,data
        assert data["decoded_audio_samples"]>=96000,data
    data,_=probe("MP4 software video/audio decode",REMOTE+"/sample.mp4","--decode");complete(data)
    data,_=probe("MKV software video/audio decode",REMOTE+"/sample.mkv","--decode");complete(data)
    data,_=probe("Descriptor slice",REMOTE+"/sliced.bin","--fd","--slice-offset","14","--slice-length",str((fixtures/"sample.mp4").stat().st_size),"--decode");complete(data)
    data,_=probe("Local seek",REMOTE+"/sample.mp4","--seek-us","1000000","--decode")
    assert 0<data["decoded_video_frames"]<48,data
    data,_=probe("Native file browser",REMOTE+"/sample.mp4","--list",REMOTE)
    assert any(x["name"]=="sample.mp4" for x in data["entries"])
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self,*args): pass
        def do_GET(self):
            if self.path=="/stall":
                self.send_response(200);self.send_header("Content-Type","video/mp4")
                self.send_header("Content-Length","1000000");self.end_headers();self.server.release_stalls.wait(5)
            else: super().do_GET()
    servers=[]
    mappings=[]
    try:
        used_ports={line.split()[1] for line in checked("reverse","--list").splitlines() if len(line.split())>=2}
        def server(tls=False):
            while True:
                instance=http.server.ThreadingHTTPServer(("127.0.0.1",0),functools.partial(Handler,directory=str(fixtures)))
                if "tcp:"+str(instance.server_port) not in used_ports: break
                instance.server_close()
            instance.daemon_threads=True;instance.release_stalls=threading.Event()
            if tls:
                context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(fixtures/"tls.crt",fixtures/"tls.key")
                instance.socket=context.wrap_socket(instance.socket,server_side=True)
            worker=threading.Thread(target=instance.serve_forever,daemon=True);worker.start()
            servers.append((instance,worker))
            mapping="tcp:"+str(instance.server_port)
            checked("reverse",mapping,mapping);mappings.append(mapping);used_ports.add(mapping)
            return ("https" if tls else "http")+"://127.0.0.1:"+str(instance.server_port)
        http_base=server()
        data,_=probe("HTTP progressive stream",http_base+"/sample.mp4","--decode");complete(data)
        data,_=probe("HLS segment stream",http_base+"/stream.m3u8","--decode")
        assert data["eof"] and data["decoded_video_frames"]==48 and data["decoded_audio_samples"]>=96000,data
        data,_=probe("HLS VOD temporal seek",http_base+"/stream.m3u8","--seek-us","1000000","--decode")
        assert data["seekable"] and data["eof"] and 0<data["decoded_video_frames"]<48,data
        data,elapsed=probe("Network timeout",http_base+"/stall","--timeout-ms","300",success=False)
        assert "timed out" in data["error"] and elapsed<3,data
        data,elapsed=probe("Network cancellation",http_base+"/stall","--cancel-after-ms","150",success=False)
        assert "cancelled" in data["error"] and elapsed<3,data
        https_base=server(True)
        probe("HTTPS rejects untrusted certificate",https_base+"/sample.mp4",success=False)
        data,_=probe("HTTPS with explicit trusted test CA",https_base+"/sample.mp4","--ca-file",REMOTE+"/tls.crt","--decode");complete(data)
    finally:
        for mapping in mappings: call("reverse","--remove",mapping)
        for instance,worker in servers:
            instance.release_stalls.set();instance.shutdown();instance.server_close();worker.join()
    report={"device_model":model,"android_api":sdk,"checks":cases,
        "limits":"Headless 160x90 software decode and file/stream I/O; no MediaCodec, audio output, Vulkan or VR display benchmark."}
    (ROOT/"build/quest-smoke-results.json").write_text(json.dumps(report,indent=2)+"\n")
    print(f"{len(cases)} Quest integration cases passed",flush=True)

if __name__=="__main__": main()
