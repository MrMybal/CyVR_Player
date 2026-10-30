"""Decode authored ISO fixtures, chapter seeks, SAF slices and HTTP byte ranges."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixtures', type=Path, default=ROOT / 'build/disc-fixtures')
    parser.add_argument('--probes', type=Path, default=ROOT / 'build/app-windows')
    args = parser.parse_args(); out = args.fixtures.resolve(); records = []
    def run(probe, *args, success=True):
        result = subprocess.run([str(args_path := args_probes / (probe + '.exe')), *map(str, args)], capture_output=True, text=True, timeout=45)
        assert (result.returncode == 0) == success, (args_path.name, result.stdout, result.stderr)
        if not success: return
        return result.stdout
    args_probes = args.probes.resolve()
    def check(name, expected, title=None, seek=None, extra=()):
        options = ['--decode', '--packets', '20000', *extra]
        if title is not None: options += ['--disc-title', str(title)]
        if seek is not None: options += ['--seek-us', str(seek)]
        result = json.loads(run('cyvr_media_probe', out / name, *options))
        assert result['seekable'] and result['eof'] and result['decoded_video_frames'] == expected, (name, result)
        assert result['decoded_audio_samples'] > 0 and result['last_video_pts_us'] < result['duration_us'], result
        if seek is None: assert result['first_video_pts_us'] == 0, result
        if seek is not None: assert 0 <= seek-result['first_video_pts_us'] < 1200000, result
        records.append(dict(file=name, title=title, seek_us=seek, extra=list(extra), result=result))
        return result
    dvd = check('dvd.iso', 200); assert len(dvd['disc_titles']) == 2 and len(dvd['chapters']) == 4
    assert [t['language'] for t in dvd['tracks'] if t['type']=='audio'] == ['en','fr']
    check('dvd.iso', 75, title=1); check('dvd.iso', 80, seek=5200000)
    check('dvd.iso', 200, extra=('--fd',)); check('bluray.iso', 144); check('bluray.iso', 144, extra=('--fd',))
    check('bluray-clips.iso', 288); check('bluray-clips.iso', 144, title=1); check('bluray-clips.iso', 96, title=2, seek=8500000)
    for name, expected in [('dvd-cells.iso',275), ('bluray-clips.iso',288)]:
        result = json.loads(run('cyvr_disc_probe', out / name)); assert result['streams'][0]['frames'] == expected, result
        if name.startswith('dvd'): assert result['bitmap_pixels'] > 8000, result
    for name in ('dvd.iso','bluray-clips.iso'): run('cyvr_disc_session_probe', out / name, out)
    for name, count in [('dvd.iso',200),('bluray.iso',144)]:
        data=(out / name).read_bytes(); wrapped=out / ('slice-' + name + '.bin'); wrapped.write_bytes(b'prefix'*683 + data + b'trailer')
        check(wrapped.name, count, extra=('--fd','--slice-offset','4098','--slice-length',str(len(data))))
    run('cyvr_media_probe',out/'dvd.iso','--disc-title','99',success=False)
    run('cyvr_media_probe',out/'bluray.iso','--disc-angle','9',success=False)
    (out/'truncated.iso').write_bytes((out/'bluray.iso').read_bytes()[:40000])
    run('cyvr_media_probe',out/'truncated.iso',success=False)
    # Loopback-only synthetic server. Never fetch the user's network sources.
    data=(out/'bluray.iso').read_bytes()
    class RangeServer(BaseHTTPRequestHandler):
        def log_message(self,*args): pass
        def do_GET(self):
            start,end=0,len(data)-1
            if value:=self.headers.get('Range'):
                lo,hi=value.removeprefix('bytes=').split('-'); start=int(lo); end=int(hi) if hi else end
            if start>=len(data): self.send_error(416); return
            end=min(end,len(data)-1); self.send_response(206 if self.headers.get('Range') else 200)
            self.send_header('Accept-Ranges','bytes'); self.send_header('Content-Length',str(end-start+1));self.send_header('Content-Type','application/x-iso9660-image')
            if self.headers.get('Range'): self.send_header('Content-Range',f'bytes {start}-{end}/{len(data)}')
            self.end_headers()
            try: self.wfile.write(data[start:end+1])
            except (ConnectionError, OSError): pass
    server=ThreadingHTTPServer(('127.0.0.1',0),RangeServer); thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    try:
        result=json.loads(run('cyvr_media_probe',f'http://127.0.0.1:{server.server_port}/movie.iso','--decode','--packets','20000','--seek-us','3500000'))
        assert result['decoded_video_frames'] == 72 and result['disc_type']=='Blu-ray', result
        records.append(dict(file='loopback HTTP range ISO',result=result))
    finally: server.shutdown();server.server_close();thread.join()
    (ROOT/'build/disc-integration-results.json').write_text(json.dumps(records,indent=2)+'\n')
    print('ISO decode, titles, chapters, bitmap subtitles, continuous clocks, SAF slices, HTTP ranges, host state and malformed inputs passed')

if __name__ == '__main__': main()
