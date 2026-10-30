"""Build the pinned VideoLAN DVD, Blu-ray and UDF readers as static libraries."""
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
    parser.add_argument('--target', choices=['android', 'windows'], default='android')
    args = parser.parse_args()
    source = ROOT / '.deps/discs/source'
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    for entry in lock['external_dependencies']:
        name = entry['name'].removesuffix(' source')
        if name in ('libbluray', 'libudfread', 'libdvdread'):
            archive, _ = download(entry['url'], Path(entry['archive']).name, entry['sha256'])
            extract(archive, source / 'subprojects' / name)
    modules = ROOT / '.deps/tools/python'
    if not (modules / 'mesonbuild').exists():
        subprocess.run([sys.executable, '-m', 'pip', 'install', '--target', str(modules), 'meson==1.9.1'], check=True)
    sdk = Path(os.environ.get('ANDROID_HOME', Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'))
    ndk = sdk / 'ndk/27.2.12479018'
    ninja = sdk / 'cmake/3.22.1/bin/ninja.exe'
    build = ROOT / ('build/discs-' + args.target)
    build.mkdir(parents=True, exist_ok=True)
    prefix = ROOT / ('.deps/discs/' + args.target)
    def quote(value): return repr(str(value).replace('\\', '/'))
    host = Path(os.environ.get('CYVR_HOST_CC', r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang.exe'))
    native = build / 'native.ini'
    flags = ['-fuse-ld=lld', '-ffile-prefix-map=' + str(ROOT) + '=/cyvr-player']
    native.write_text('[binaries]\nc = [' + ','.join(map(quote, [host, *flags])) + ']\nar = ' + quote(host.with_name('llvm-ar.exe')) + '\n')
    options = ['--native-file', str(native)]
    if args.target == 'windows':
        compat = build / 'compat.h'
        compat.write_text('#include <stdint.h>\n#include <sys/types.h>\n#include <stdio.h>\nint _close(int);\n#define close _close\n#ifndef ssize_t\n#define ssize_t intptr_t\n#endif\n')
        options += ['-Dc_args=-fms-runtime-lib=dll -include ' + compat.as_posix() + ' -I' + (source / 'subprojects/libdvdread/msvc/include').as_posix() + ' -I' + (source / 'subprojects/libdvdread/msvc/contrib/dirent').as_posix()]
    else:
        tool = ndk / 'toolchains/llvm/prebuilt/windows-x86_64'
        cross = build / 'android.ini'
        flags = ['--target=aarch64-linux-android29', '--sysroot=' + str(tool / 'sysroot'),
                 '-ffile-prefix-map=' + str(ROOT) + '=/cyvr-player', '-ffile-prefix-map=' + str(ndk) + '=/android-ndk']
        cross.write_text('[binaries]\nc = [' + ','.join(map(quote, [tool / 'bin/clang.exe', *flags])) + ']\nar = ' + quote(tool / 'bin/llvm-ar.exe') + '\nstrip = ' + quote(tool / 'bin/llvm-strip.exe') + "\n[host_machine]\nsystem = 'android'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n")
        options += ['--cross-file', str(cross)]
    (source / 'meson.build').write_text("""project('cyvr-disc-readers', 'c', default_options: ['default_library=static', 'buildtype=release', 'b_ndebug=true', 'b_staticpic=true', 'auto_features=disabled'])
subproject('libudfread', default_options: ['default_library=static', 'embed_udfread=false', 'enable_examples=false'])
subproject('libbluray', default_options: ['default_library=static', 'bdj_jar=disabled', 'freetype=disabled', 'fontconfig=disabled', 'libxml2=disabled', 'enable_tools=false', 'enable_examples=false', 'enable_docs=false'])
subproject('libdvdread', default_options: ['default_library=static', 'libdvdcss=disabled', 'enable_docs=false'])
""")
    env = dict(os.environ, PYTHONPATH=str(modules))
    env['PATH'] = str(ninja.parent) + os.pathsep + env['PATH']
    with (build / 'build.log').open('w', encoding='utf-8') as output:
        def run(*command):
            subprocess.run([sys.executable, '-m', 'mesonbuild.mesonmain', *map(str, command)], env=env, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, check=True)
        run('setup', build, source, '--prefix', prefix, '--libdir', 'lib', '--wrap-mode=nofallback', '-Db_vscrt=md', *['-D'+n+':default_library=static' for n in ('libudfread','libbluray','libdvdread')], *(['--reconfigure'] if (build / 'meson-private/coredata.dat').exists() else []), *options)
        run('compile', '-C', build, '-j', '8')
        run('install', '-C', build, '--no-rebuild')
    print('Disc readers built for', args.target)

if __name__ == '__main__': main()
