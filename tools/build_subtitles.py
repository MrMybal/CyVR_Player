"""Build pinned libass, FreeType, HarfBuzz and FriBidi locally, without global installs."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
from prepare_android_media import download, extract

ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--target',choices=['android','windows'],default='android');args=parser.parse_args()
    source=ROOT/'.deps/subtitles/source';source.mkdir(parents=True,exist_ok=True)
    lock=json.loads((ROOT/'dependencies.lock.json').read_text())
    for entry in lock['external_dependencies']:
        if entry['name'].startswith('Subtitle source: '):
            name=entry['name'].split(': ')[1]
            archive,_=download(entry['url'],Path(entry['archive']).name,entry['sha256'])
            # HarfBuzz's Meson generator rewrites only path separators in two amalgamation files.
            if name=='harfbuzz':
                with tarfile.open(archive) as package:
                    for member in package:
                        if member.name.endswith(('/src/harfbuzz.cc','/src/harfbuzz-subset.cc')):
                            original=package.extractfile(member).read()
                            path=source/'subprojects/harfbuzz/src'/Path(member.name).name
                            if path.exists() and path.read_bytes()!=original and path.read_bytes().replace(b'\\',b'/')==original:path.write_bytes(original)
            extract(archive,source/'subprojects'/('freetype2' if name=='freetype' else name))
    modules=ROOT/'.deps/tools/python'
    if not (modules/'mesonbuild').exists():
        subprocess.run([sys.executable,'-m','pip','install','--target',str(modules),'meson==1.9.1'],check=True)
    sdk=Path(os.environ.get('ANDROID_HOME',Path(os.environ['LOCALAPPDATA'])/'Android/Sdk'))
    ndk=sdk/'ndk/27.2.12479018';ninja=sdk/'cmake/3.22.1/bin/ninja.exe'
    host=Path(os.environ.get('CYVR_HOST_CC',r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang.exe'))
    build=ROOT/('build/subtitles-native-tools-'+args.target);build.mkdir(parents=True,exist_ok=True)
    prefix=ROOT/('.deps/subtitles/'+args.target)
    def quote(value):return repr(str(value).replace('\\','/'))
    native=build/'native.ini'
    # FriBidi's table generator probes the target headers even for native tools.
    # Supply the equivalent Windows string declarations to native tools only.
    compat=build/'native-include';compat.mkdir(exist_ok=True)
    (compat/'strings.h').write_text('#include <string.h>\n#define strcasecmp _stricmp\n#define strncasecmp _strnicmp\n')
    native.write_text('[binaries]\nc = ['+quote(host)+', "-fuse-ld=lld"]\ncpp = ['+quote(host.with_name('clang++.exe'))+', "-fuse-ld=lld"]\nar = '+quote(host.with_name('llvm-ar.exe'))+'\n',encoding='utf-8')
    native.write_text(native.read_text().replace(chr(34), chr(39)).replace("'-fuse-ld=lld'", "'-fuse-ld=lld', "+quote('-I'+str(compat))))
    options=['--native-file',str(native)]
    if args.target=='windows':options+=['-Dc_args=-fms-runtime-lib=dll','-Dcpp_args=-fms-runtime-lib=dll']
    if args.target=='android':
        cross=build/'android.ini';tool=ndk/'toolchains/llvm/prebuilt/windows-x86_64'
        flags=['--target=aarch64-linux-android29','--sysroot='+str(tool/'sysroot'),'-ffile-prefix-map='+str(ROOT)+'=/cyvr-player','-ffile-prefix-map='+str(ndk)+'=/android-ndk']
        cross.write_text('[binaries]\nc = '+json.dumps([str(tool/'bin/clang.exe'),*flags]).replace('\\\\','/')+'\ncpp = '+json.dumps([str(tool/'bin/clang++.exe'),*flags]).replace('\\\\','/')+'\nar = '+quote(tool/'bin/llvm-ar.exe')+'\nstrip = '+quote(tool/'bin/llvm-strip.exe')+'\n[host_machine]\nsystem = "android"\ncpu_family = "aarch64"\ncpu = "aarch64"\nendian = "little"\n',encoding='utf-8')
        cross.write_text(cross.read_text().replace(chr(34), chr(39)))
        options+=['--cross-file',str(cross)]
    (source/'meson.build').write_text('''project('cyvr-subtitle-dependencies', 'c', 'cpp', default_options: ['default_library=static', 'buildtype=release', 'b_ndebug=true', 'b_staticpic=true', 'auto_features=disabled'])
subproject('freetype2', default_options: ['harfbuzz=disabled', 'brotli=disabled', 'bzip2=disabled', 'png=disabled', 'zlib=disabled', 'tests=disabled'])
subproject('fribidi', default_options: ['bin=false', 'tests=false', 'docs=false'])
subproject('harfbuzz', default_options: ['freetype=disabled', 'utilities=disabled', 'tests=disabled', 'benchmark=disabled', 'docs=disabled'])
subproject('libass', default_options: ['asm=enabled', 'require-system-font-provider=false', 'fontconfig=disabled', 'directwrite=disabled', 'coretext=disabled', 'test=disabled', 'compare=disabled', 'profile=disabled', 'fuzz=disabled', 'checkasm=disabled'])
'''.replace("'asm=enabled'","'asm=disabled'" if args.target=='windows' else "'asm=enabled'"),encoding='utf-8')
    env=os.environ.copy();env['PYTHONPATH']=str(modules);env['PATH']=str(ninja.parent)+os.pathsep+env['PATH']
    log=build/'build.log'
    with log.open('w',encoding='utf-8') as output:
        def run(*command):subprocess.run([sys.executable,'-m','mesonbuild.mesonmain',*map(str,command)],stdout=output,stderr=subprocess.STDOUT,check=True,env=env,cwd=ROOT)
        run('setup',build,source,'--prefix',prefix,'--libdir','lib','--wrap-mode=nofallback','-Db_vscrt=md',*(['--reconfigure'] if (build/'meson-private/coredata.dat').exists() else []),*options)
        run('compile','-C',build,'-j','8');run('install','-C',build,'--no-rebuild')
    print('Subtitle libraries built for',args.target)
if __name__=='__main__':main()
