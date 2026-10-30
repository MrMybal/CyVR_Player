"""Fetch the exact MIT Gaussian backend sources; never run its dependency fetchers."""
import json
from pathlib import Path
from prepare_android_media import download,extract
ROOT=Path(__file__).resolve().parents[1]
lock=json.loads((ROOT/'dependencies.lock.json').read_text())
for entry in lock['external_dependencies']:
    name=entry['name']
    if name not in ('SplatKit Vulkan source','VulkanMemoryAllocator source'):continue
    archive,_=download(entry['url'],Path(entry['archive']).name,entry['sha256'])
    extract(archive,ROOT/('.deps/splatkit/source' if name.startswith('SplatKit') else '.deps/vma/source'))
print('Pinned MIT Gaussian backend sources ready')
