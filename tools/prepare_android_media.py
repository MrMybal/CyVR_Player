"""Prepare source/build tools locally. No global installation or configuration."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
COMMIT = "611cec792aa54e2e53dbc1be95dcf5955d9a99ba"

def download(url, name, expected=None):
    output=ROOT/".deps/downloads"/name
    output.parent.mkdir(parents=True,exist_ok=True)
    if not output.exists():
        print("Downloading",name,flush=True)
        temporary=output.with_suffix(".download")
        with urllib.request.urlopen(url,timeout=60) as response, temporary.open("wb") as stream:
            shutil.copyfileobj(response,stream)
        temporary.replace(output)
    digest=hashlib.sha256(output.read_bytes()).hexdigest()
    if expected and digest!=expected:
        raise RuntimeError(f"Checksum mismatch: {name}")
    return output,{"url":url,"sha256":digest}

def extract(archive,target,strip_root=True):
    target=target.resolve()
    target.mkdir(parents=True,exist_ok=True)
    with tarfile.open(archive) as package:
        for item in package:
            parts=Path(item.name).parts[1:] if strip_root else Path(item.name).parts
            if not parts or not item.isfile():
                continue
            destination=target.joinpath(*parts).resolve()
            if target not in destination.parents:
                raise RuntimeError("Invalid archive path")
            destination.parent.mkdir(parents=True,exist_ok=True)
            with package.extractfile(item) as source:
                data=source.read()
            if destination.exists() and destination.read_bytes()!=data:
                raise RuntimeError(f"Refusing to replace modified dependency: {destination}")
            if not destination.exists(): destination.write_bytes(data)

def main():
    records={}
    ff,records["ffmpeg"]=download(f"https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/{COMMIT}",f"ffmpeg-{COMMIT}.tar.gz",
        "d6228fe2111721062178221edf9b4c449b66fc9b06c1fd15eb14c97537daa559")
    extract(ff,ROOT/".deps/ffmpeg/source")
    tls,records["mbedtls"]=download("https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2",
        "mbedtls-3.6.7.tar.bz2","a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6")
    extract(tls,ROOT/".deps/mbedtls/source")
    make,records["make"]=download("https://repo.msys2.org/msys/x86_64/make-4.4.1-2-x86_64.pkg.tar.zst","make-4.4.1-2-x86_64.pkg.tar.zst",
        "2408af61717dae87b00c855b132769a125c708907fc94a46bb16dae076113e5c")
    expanded=ROOT/".deps/tools/make-archive"
    expanded.mkdir(parents=True,exist_ok=True)
    sevenzip=Path(r"C:\Program Files\7-Zip\7z.exe")
    subprocess.run([str(sevenzip),"x",str(make),"-o"+str(expanded),"-y","-bso0","-bsp0"],check=True)
    extract(next(expanded.glob("*.tar")),ROOT/".deps/tools/make",False)
    (ROOT/".deps/android-media-sources.json").write_text(json.dumps(records,indent=2)+"\n")
    print("Android media sources and local GNU make are ready",flush=True)

if __name__=="__main__":
    main()
