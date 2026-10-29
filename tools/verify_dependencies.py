"""Verify the local, vendored shader sources; does not access the network."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
lock = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))
checked = 0
for dependency in lock["dependencies"]:
    for entry in dependency["files"]:
        path = (root / entry["path"]).resolve()
        if root not in path.parents:
            raise SystemExit(f"Dependency path escapes project: {entry['path']}")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != entry["sha256"]:
            raise SystemExit(f"Checksum mismatch: {entry['path']}")
        checked += 1
archives = 0
for entry in lock.get("external_dependencies", []):
    path = (root / entry["archive"]).resolve()
    if root not in path.parents:
        raise SystemExit("Dependency archive path escapes project")
    if path.exists():
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise SystemExit(f"Checksum mismatch: {entry['archive']}")
        archives += 1
print(f"Verified {checked} vendored files and {archives} cached dependency archives")
