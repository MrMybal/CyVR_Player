"""Fetch a pinned LGPL shared FFmpeg development build into this project only."""
import hashlib
import json
from pathlib import Path
import shutil
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
NAME = "ffmpeg-n8.1.3-25-g611cec792a-win64-lgpl-shared-8.1.zip"
URL = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-28-19-47/" + NAME
SHA256 = "0448c3f3b9bdc724cc0189d5ce6e6bfa5b8937e7ec408e1e97fb08719bdb3b92"
COMMIT = "611cec792aa54e2e53dbc1be95dcf5955d9a99ba"

def main():
    cache = ROOT / ".deps/downloads"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / NAME
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        print("Downloading pinned FFmpeg LGPL shared build", flush=True)
        request = urllib.request.Request(URL, headers={"User-Agent": "CyVR-Player-development/0.1"})
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit("FFmpeg download checksum mismatch")
    target = ROOT / ".deps/ffmpeg/windows"
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        for item in package.infolist():
            parts = Path(item.filename).parts[1:]
            if not parts or item.is_dir():
                continue
            destination = target.joinpath(*parts).resolve()
            if target.resolve() not in destination.parents:
                raise SystemExit("Invalid archive path")
            destination.parent.mkdir(parents=True, exist_ok=True)
            data = package.read(item)
            if destination.exists() and destination.read_bytes() != data:
                raise SystemExit(f"Refusing to replace modified dependency: {destination.name}")
            destination.write_bytes(data)
    (target / "origin.json").write_text(json.dumps({"url": URL, "sha256": SHA256,
        "ffmpeg_commit": COMMIT, "variant": "LGPL shared development build"}, indent=2)+"\n")
    print("FFmpeg development SDK:", target)

if __name__ == "__main__":
    main()
