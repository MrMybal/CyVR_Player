"""Build the pinned BSD dav1d decoder with ARM64 assembly, locally."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from prepare_android_media import download, extract

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', choices=['android'], default='android')
    parser.parse_args()
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    for entry in lock['external_dependencies']:
        if entry['name'] in ('dav1d source', 'edge264-mvc source'):
            archive, _ = download(entry['url'], Path(entry['archive']).name, entry['sha256'])
            extract(archive, ROOT / '.deps' / ('dav1d' if entry['name']=='dav1d source' else 'edge264') / 'source')
    sdk = Path(os.environ.get('ANDROID_HOME', Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'))
    ndk = sdk / 'ndk/27.2.12479018'
    tool = ndk / 'toolchains/llvm/prebuilt/windows-x86_64'
    ninja = sdk / 'cmake/3.22.1/bin/ninja.exe'
    build = ROOT / 'build/dav1d-android'
    build.mkdir(parents=True, exist_ok=True)
    prefix = ROOT / '.deps/dav1d/android'
    modules = ROOT / '.deps/tools/python'
    if not (modules / 'mesonbuild').exists():
        subprocess.run([sys.executable, '-m', 'pip', 'install', '--target', str(modules), 'meson==1.9.1'], check=True)
    flags = ['--target=aarch64-linux-android29', '--sysroot='+str(tool/'sysroot'),
             '-ffile-prefix-map='+str(ROOT)+'=/cyvr-player', '-ffile-prefix-map='+str(ndk)+'=/android-ndk']
    def array(values): return repr([str(v).replace('\\','/') for v in values])
    cross = build/'android.ini'
    cross.write_text('[binaries]\nc = '+array([tool/'bin/clang.exe', *flags])+
        '\nar = '+repr((tool/'bin/llvm-ar.exe').as_posix())+
        '\nstrip = '+repr((tool/'bin/llvm-strip.exe').as_posix())+
        '\n[host_machine]\nsystem = \'android\'\ncpu_family = \'aarch64\'\ncpu = \'aarch64\'\nendian = \'little\'\n')
    env = os.environ.copy()
    env['PYTHONPATH'] = str(modules)
    env['PATH'] = str(ninja.parent)+os.pathsep+env['PATH']
    with (build/'build.log').open('w', encoding='utf-8') as output:
        def run(*args):
            subprocess.run([sys.executable,'-m','mesonbuild.mesonmain',*map(str,args)], env=env,
                           stdout=output, stderr=subprocess.STDOUT, check=True, cwd=ROOT)
        run('setup', build, ROOT/'.deps/dav1d/source', '--prefix',prefix,'--libdir','lib',
            '--cross-file',cross,'--default-library','static','--buildtype','release',
            '-Db_staticpic=true','-Denable_tools=false','-Denable_tests=false','-Denable_examples=false',
            '-Denable_asm=true', *(['--reconfigure'] if (build/'meson-private/coredata.dat').exists() else []))
        run('compile','-C',build,'-j','8')
        run('install','-C',build,'--no-rebuild')
    print('dav1d built with ARM64 SIMD assembly; edge264-mvc sources prepared', flush=True)

if __name__ == '__main__': main()
