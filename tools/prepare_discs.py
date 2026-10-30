"""Fetch pinned VideoLAN disc readers; verify official checksum manifests."""
import hashlib,json,urllib.request
from pathlib import Path
from prepare_android_media import download,extract
ROOT=Path(__file__).resolve().parents[1]
def main():
    lock=json.loads((ROOT/'dependencies.lock.json').read_text())
    for name,version,url,license_name in [
        ('libbluray','1.5.0','https://download.videolan.org/pub/videolan/libbluray/1.5.0/libbluray-1.5.0.tar.xz','LGPL-2.1-or-later'),
        ('libudfread','1.2.0','https://download.videolan.org/pub/videolan/libudfread/libudfread-1.2.0.tar.xz','LGPL-2.1-or-later'),
        ('libdvdread','7.1.1','https://download.videolan.org/pub/videolan/libdvdread/7.1.1/libdvdread-7.1.1.tar.xz','GPL-2.0-or-later')]:
        existing=next((e for e in lock['external_dependencies'] if e['name']==name+' source'),None)
        archive,record=download(url,url.rsplit('/',1)[-1],existing['sha256'] if existing else None)
        algorithm='sha512' if name=='libudfread' else 'sha256'
        manifest=urllib.request.urlopen(url+'.'+algorithm,timeout=30).read().decode().split()[0]
        assert hashlib.new(algorithm,archive.read_bytes()).hexdigest()==manifest,(name,'Official checksum mismatch')
        extract(archive,ROOT/'.deps/discs/source/subprojects'/name)
        if not existing:lock['external_dependencies'].append(dict(name=name+' source',version=version,license=license_name,url=url,archive=archive.relative_to(ROOT).as_posix(),sha256=record['sha256']))
        print(name,version,record['sha256'],flush=True)
    (ROOT/'dependencies.lock.json').write_text(json.dumps(lock,indent=2)+'\n')
if __name__=='__main__':main()
