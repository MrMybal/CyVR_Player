"""Enumerate the exact local foundation, including uncommitted build inputs."""
import hashlib
from pathlib import Path, PurePosixPath
import subprocess

EXCLUDED={'.git','.deps','.cache','build','dist','out','__pycache__','.codex','.agents','.idea','.vscode','.vs'}

def sources(root):
    root=Path(root).resolve()
    git=['git','-c','safe.directory='+root.as_posix(),'-C',str(root)]
    location=subprocess.run(git+['rev-parse','--show-toplevel'],capture_output=True,text=True)
    repository=location.returncode==0 and Path(location.stdout.strip()).resolve()==root
    result=subprocess.run(git+['ls-files','-z','--cached','--others','--exclude-standard'],capture_output=True) if repository else None
    if result is not None and result.returncode==0:
        paths=set(filter(None,result.stdout.decode('utf-8').split('\0')))
        base=subprocess.check_output(git+['rev-parse','HEAD'],text=True).strip()
    else:
        paths={p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
        base='source-archive'
    files={}
    for name in sorted(paths):
        parts=PurePosixPath(name).parts
        if not parts or '..' in parts or any(p in EXCLUDED for p in parts):continue
        if parts[:2] in {('assets','branding'),('docs','images')}:continue
        if name.startswith(('.env','.git/')) or name.endswith(('.keystore','.jks','.p12','.pfx','.pem','.key','.log','.pdb','.dmp','.user')):
            raise RuntimeError('Unexpected local configuration in foundation source list')
        path=(root/name).resolve()
        if root not in path.parents:raise RuntimeError('Foundation input escapes its root')
        if not path.is_file():raise RuntimeError('Foundation input is missing: '+name)
        files[name]=hashlib.sha256(path.read_bytes()).hexdigest()
    if not (root/'LICENSE').read_bytes().startswith(b'MIT License'):
        raise RuntimeError('The foundation must retain its MIT license')
    return {'base_commit':base,'files':files}
