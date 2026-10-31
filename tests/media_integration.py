"""Exercise the real C++ media engine against generated local media and servers."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import ssl
import subprocess
import threading
import time

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--probe",type=Path,default=ROOT/"build/app-windows/cyvr_media_probe.exe")
    args=parser.parse_args()
    ffmpeg=ROOT/".deps/ffmpeg/windows/bin/ffmpeg.exe"
    fixtures=ROOT/"build/media-fixtures"
    fixtures.mkdir(parents=True,exist_ok=True)
    def encode(*command):
        subprocess.run([str(ffmpeg),"-hide_banner","-loglevel","error","-y",*map(str,command)],check=True,cwd=fixtures)
    print("Generating two-second video/audio, HLS and DASH fixtures",flush=True)
    encode("-f","lavfi","-i","testsrc2=size=160x90:rate=24","-f","lavfi","-i","sine=frequency=440:sample_rate=48000",
        "-t","2","-c:v","mpeg4","-bf","2","-g","12","-c:a","aac","-movflags","+faststart","sample.mp4")
    encode("-i","sample.mp4","-c","copy","sample.mkv")
    encode("-i","sample.mp4","-c:v","mpeg2video","-bf","2","-g","12","-c:a","aac","-hls_time","0.5","-hls_list_size","0",
        "-hls_segment_filename","part%02d.ts","stream.m3u8")
    encode("-i","sample.mp4","-c","copy","-f","dash","-seg_duration","1","stream.mpd")
    payload=(fixtures/"sample.mp4").read_bytes()
    (fixtures/"sliced.bin").write_bytes(b"prefix-padding"+payload+b"suffix-padding")
    (fixtures/"escape.m3u8").write_text("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:2,\n"+
        (fixtures/"part00.ts").as_uri()+"\n#EXT-X-ENDLIST\n")
    checks=[]
    def run(label,source,*options,success=True):
        started=time.monotonic()
        completed=subprocess.run([str(args.probe.resolve()),str(source),*map(str,options)],capture_output=True,text=True,timeout=15,cwd=ROOT)
        elapsed=time.monotonic()-started
        if success:
            if completed.returncode!=0: raise AssertionError(f"{label}: {completed.stderr}")
            data=json.loads(completed.stdout)
        else:
            if completed.returncode==0: raise AssertionError(f"{label}: unexpectedly succeeded")
            data=json.loads(completed.stderr)
        checks.append({"case":label,"passed":True,"elapsed_seconds":round(elapsed,3),"result":data})
        print(label,"passed",flush=True)
        return data,elapsed
    def complete(data,minimum_audio=96000):
        assert data["eof"] and data["decoded_video_frames"]==48,data
        assert data["decoded_audio_samples"]>=minimum_audio,data
        assert data["last_video_pts_us"]>1900000,data
    data,_=run("MP4 video + audio decode",fixtures/"sample.mp4","--decode");complete(data)
    data,_=run("MKV video + audio decode",fixtures/"sample.mkv","--decode");complete(data)
    data,_=run("Borrowed descriptor remains readable after original closes",fixtures/"sample.mp4","--fd","--decode");complete(data)
    data,_=run("Document offset and length slice",fixtures/"sliced.bin","--fd","--slice-offset","14","--slice-length",len(payload),"--decode");complete(data)
    data,_=run("Seek into local media",fixtures/"sample.mp4","--seek-us","1000000","--decode")
    assert data["eof"] and 0<data["decoded_video_frames"]<48,data
    data,_=run("Browser lists media and subdirectories",fixtures,"--list",fixtures)
    assert any(x["name"]=="sample.mp4" for x in data["entries"]),data
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self,*args): pass
        def do_GET(self):
            if self.path=="/web-only.mp4":
                # Stands in for a site that serves its video only to its own pages.
                if self.headers.get("Referer")!="https://site.test/" or "session=abc" not in self.headers.get("Cookie",""):
                    self.send_error(403);return
                self.path="/sample.mp4";super().do_GET()
            elif self.path=="/stall":
                self.send_response(200);self.send_header("Content-Type","video/mp4")
                self.send_header("Content-Length","1000000");self.end_headers()
                self.server.release_stalls.wait(5)
            else: super().do_GET()
    server=http.server.ThreadingHTTPServer(("127.0.0.1",0),functools.partial(Handler,directory=str(fixtures)))
    server.daemon_threads=True
    server.release_stalls=threading.Event()
    worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
    base=f"http://127.0.0.1:{server.server_port}"
    try:
        data,_=run("HTTP progressive decode",base+"/sample.mp4","--decode");complete(data)
        data,_=run("HLS segment decode",base+"/stream.m3u8","--decode")
        assert data["eof"] and data["decoded_video_frames"]==48 and data["decoded_audio_samples"]>=96000,data
        data,_=run("HLS VOD temporal seek",base+"/stream.m3u8","--seek-us","1000000","--decode")
        assert data["seekable"] and data["eof"] and 0<data["decoded_video_frames"]<48,data
        # The short remuxed DASH fixture contains edit/discard metadata. Compare
        # its rendered frame count with ffprobe under the same probe budgets
        # and both tracks enabled. Audio-only probing changes DASH demux policy.
        reference=subprocess.run([str(ffmpeg.with_name("ffprobe.exe")),"-v","error","-analyzeduration","3000000",
            "-probesize","4194304","-count_frames","-show_entries","stream=codec_type,nb_read_frames",
            "-of","json",base+"/stream.mpd"],capture_output=True,text=True,check=True,timeout=15)
        expected_audio=int(next(s for s in json.loads(reference.stdout)["streams"] if s["codec_type"]=="audio")["nb_read_frames"])
        assert expected_audio>85
        data,_=run("DASH manifest decode",base+"/stream.mpd","--decode");complete(data,expected_audio*1024)
        assert data["decoded_audio_frames"]==expected_audio,data
        data,_=run("DASH VOD temporal seek",base+"/stream.mpd","--seek-us","1000000","--decode")
        assert data["seekable"] and data["eof"] and 0<data["decoded_video_frames"]<48,data
        data,elapsed=run("Stalled source timeout",base+"/stall","--timeout-ms","300",success=False)
        assert "timed out" in data["error"] and elapsed<2,data
        data,elapsed=run("Cancel blocked network read",base+"/stall","--timeout-ms","5000","--cancel-after-ms","150",success=False)
        assert "cancelled" in data["error"] and elapsed<2,data
        run("HTTP 404 propagates an error",base+"/missing.mp4",success=False)
        run("Web video is refused without its page's request settings",base+"/web-only.mp4",success=False)
        data,_=run("Web video plays with Referer and scoped cookies",base+"/web-only.mp4","--decode","--http-header","Referer: https://site.test/",
            "--http-cookie",f"session=abc; path=/; domain=127.0.0.1:{server.server_port}");complete(data)
        run("Cookies stay with their domain",base+"/web-only.mp4","--http-header","Referer: https://site.test/",
            "--http-cookie","session=abc; path=/; domain=other.test",success=False)
        run("Network HLS cannot open local files",base+"/escape.m3u8",success=False)
    finally:
        server.release_stalls.set();server.shutdown();server.server_close();worker.join()
    openssl=Path(r"C:\Program Files\Git\usr\bin\openssl.exe")
    subprocess.run([str(openssl),"req","-x509","-newkey","rsa:2048","-nodes","-days","1","-subj","/CN=localhost",
        "-addext","subjectAltName=DNS:localhost,IP:127.0.0.1","-keyout",str(fixtures/"tls.key"),"-out",str(fixtures/"tls.crt")],
        check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    tls=http.server.ThreadingHTTPServer(("127.0.0.1",0),functools.partial(Handler,directory=str(fixtures)))
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(fixtures/"tls.crt",fixtures/"tls.key")
    tls.socket=context.wrap_socket(tls.socket,server_side=True)
    tls_worker=threading.Thread(target=tls.serve_forever,daemon=True);tls_worker.start()
    try:
        run("HTTPS rejects an untrusted certificate",f"https://127.0.0.1:{tls.server_port}/sample.mp4",success=False)
        # HLS opens segments with options of its own; the first request's verification does not carry over.
        (fixtures/"nested-tls.m3u8").write_text("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.5,\n"+
            f"https://127.0.0.1:{tls.server_port}/part00.ts\n#EXT-X-ENDLIST\n")
        plain=http.server.ThreadingHTTPServer(("127.0.0.1",0),functools.partial(Handler,directory=str(fixtures)))
        plain.daemon_threads=True;plain_worker=threading.Thread(target=plain.serve_forever,daemon=True);plain_worker.start()
        try:
            data,_=run("HLS segments verify their own certificate",f"http://127.0.0.1:{plain.server_port}/nested-tls.m3u8","--decode",success=False)
        finally:
            plain.shutdown();plain.server_close();plain_worker.join()
    finally:
        tls.shutdown();tls.server_close();tls_worker.join()
    report={"engine":"cyvr_media_probe","platform":"Windows x64","checks":checks,
        "limits":"Headless decode; no Quest runtime, Vulkan display or speaker output is exercised."}
    (ROOT/"build/media-integration-results.json").write_text(json.dumps(report,indent=2)+"\n")
    print(f"{len(checks)} media integration cases passed",flush=True)

if __name__=="__main__": main()
