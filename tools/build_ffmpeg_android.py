"""Build the selected LGPL media components for Android arm64, using local tools."""
import argparse
import json
import os
from pathlib import Path
import shlex
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def posix(path):
    value=Path(path).resolve().as_posix()
    return "/"+value[0].lower()+value[2:] if len(value)>2 and value[1]==":" else value

def latest(directory,required):
    paths=[p for p in directory.iterdir() if p.is_dir() and (p/required).is_file()]
    return max(paths,key=lambda p:tuple(int(n) for n in p.name.split(".") if n.isdigit()))

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--alpha',action='store_true');args=parser.parse_args()
    sdk=Path(os.environ.get("ANDROID_HOME",str(Path(os.environ["LOCALAPPDATA"])/"Android/Sdk")))
    ndk=latest(sdk/"ndk","build/cmake/android.toolchain.cmake")
    cmake=latest(sdk/"cmake","bin/cmake.exe")/"bin/cmake.exe"
    ninja=cmake.parent/"ninja.exe"
    suffix='-alpha' if args.alpha else ''
    tls_build=ROOT/('build/mbedtls-android'+suffix)
    tls_prefix=ROOT/('.deps/mbedtls/android-arm64'+suffix)
    source=ROOT/".deps/ffmpeg/source"
    if not (source/"configure").exists():
        raise SystemExit("Run python tools/prepare_android_media.py first")
    log=ROOT/('build/android-media-build'+suffix+'.log')
    log.parent.mkdir(exist_ok=True)
    with log.open("w",encoding="utf-8") as output:
        def run(args,**kwargs):
            output.write(shlex.join([str(a) for a in args])+"\n"); output.flush()
            subprocess.run([str(a) for a in args],stdout=output,stderr=subprocess.STDOUT,check=True,**kwargs)
        print(f"Building Mbed TLS 3.6.7 for Android (log: {log.relative_to(ROOT).as_posix()})",flush=True)
        maps=f'-ffile-prefix-map={posix(ROOT)}=/cyvr-player -ffile-prefix-map={posix(ndk)}=/android-ndk'
        cmake_maps=f'-ffile-prefix-map={ROOT.as_posix()}=/cyvr-player -ffile-prefix-map={ndk.as_posix()}=/android-ndk'
        path_options=[]
        if args.alpha:
            tls_build.mkdir(parents=True,exist_ok=True)
            path_options_file=tls_build/'path-options.cmake'
            path_options_file.write_text('add_compile_options(-g0 '+ ' '.join('"'+flag+'"' for flag in cmake_maps.split()) +')\n',encoding='utf-8')
            # Mbed TLS sets its own release flags; directory options survive that.
            path_options=[f'-DCMAKE_PROJECT_INCLUDE={path_options_file.as_posix()}']
        run([cmake,"-S",ROOT/".deps/mbedtls/source","-B",tls_build,"-G","Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}",f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake",
            "-DANDROID_ABI=arm64-v8a","-DANDROID_PLATFORM=android-29","-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON","-DENABLE_TESTING=OFF","-DENABLE_PROGRAMS=OFF",
            "-DUSE_STATIC_MBEDTLS_LIBRARY=ON","-DUSE_SHARED_MBEDTLS_LIBRARY=OFF",f"-DCMAKE_INSTALL_PREFIX={tls_prefix}"]+
            path_options)
        run([cmake,"--build",tls_build,"--parallel","8"])
        run([cmake,"--install",tls_build])
        print("Configuring and compiling FFmpeg for Android",flush=True)
        build=ROOT/('build/ffmpeg-android'+suffix)
        build.mkdir(parents=True,exist_ok=True)
        prefix=ROOT/('.deps/ffmpeg/android-arm64'+suffix)
        toolbin=ndk/"toolchains/llvm/prebuilt/windows-x86_64/bin"
        compiler=posix(toolbin/"clang.exe")
        host_compiler=Path(os.environ.get("CYVR_HOST_CC",r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang.exe"))
        host_wrapper=build/"host-cc.sh"
        host_wrapper.write_text("#!/bin/sh\nexec "+shlex.quote(posix(host_compiler))+" -fuse-ld=lld \"$@\"\n",encoding="utf-8",newline="\n")
        flags=[posix(source/"configure"),f"--prefix={posix(prefix)}",
            "--target-os=android","--arch=aarch64","--enable-cross-compile","--enable-pic",
            "--enable-shared","--disable-static","--enable-version3","--disable-autodetect",
            "--disable-programs","--disable-doc","--disable-debug","--disable-everything",
            "--enable-avformat","--enable-avcodec","--enable-swresample","--enable-swscale",
            "--disable-avdevice","--disable-avfilter","--enable-network","--enable-mbedtls","--enable-zlib",
            "--enable-protocol=file,http,https,tcp,tls,udp,rtp,crypto",
            "--enable-demuxers","--enable-parsers","--enable-decoders","--enable-bsfs",
            f"--cc={compiler}",f"--ld={compiler}",f"--ar={posix(toolbin/'llvm-ar.exe')}",
            f"--host-cc=sh {posix(host_wrapper)}",
            f"--nm={posix(toolbin/'llvm-nm.exe')}",f"--ranlib={posix(toolbin/'llvm-ranlib.exe')}",
            f"--strip={posix(toolbin/'llvm-strip.exe')}","--pkg-config=false",
            f"--extra-cflags=--target=aarch64-linux-android29 -I{posix(tls_prefix)}/include"+(' '+maps if args.alpha else ''),
            f"--extra-ldflags=--target=aarch64-linux-android29 -L{posix(tls_prefix)}/lib -Wl,-z,max-page-size=16384"]
        bash=Path(os.environ.get("CYVR_BASH",r"C:\Program Files\Git\usr\bin\bash.exe"))
        make=posix(ROOT/".deps/tools/make/usr/bin/make.exe")
        script=build/"build.sh"
        script.write_text("set -eu\nexport PATH=/usr/bin:/bin:$PATH\n"+shlex.join(flags)+"\n"+
            ('' if args.alpha else shlex.join([make,"-j8"])+"\n"+shlex.join([make,"install"])+"\n"),encoding="utf-8",newline="\n")
        environment=os.environ.copy()
        environment.pop("MSYS2_ARG_CONV_EXCL",None)
        run([bash,str(script)],cwd=build,env=environment)
        def portable(value):
            for path,name in [(posix(ndk),'${ANDROID_NDK}'),(posix(host_compiler.parent),'${CYVR_HOST_TOOLCHAIN}'),(posix(ROOT),'${CYVR_PLAYER_ROOT}')]:
                value=value.replace(path,name)
            return value
        if args.alpha:
            config=build/'config.h'
            text=config.read_text(encoding='utf-8')
            text,count=re.subn(r'(?m)^#define FFMPEG_CONFIGURATION (.+)$',lambda m:'#define FFMPEG_CONFIGURATION '+json.dumps(portable(json.loads(m[1]))),text)
            if count!=1:raise RuntimeError('Expected exactly one FFmpeg build-configuration string')
            config.write_text(text,encoding='utf-8',newline='\n')
            script.write_text('set -eu\nexport PATH=/usr/bin:/bin:$PATH\n'+shlex.join([make,'-j8'])+'\n'+shlex.join([make,'install'])+'\n',encoding='utf-8',newline='\n')
            run([bash,str(script)],cwd=build,env=environment)
        prefix.mkdir(parents=True,exist_ok=True)
        (prefix/"origin.json").write_text(json.dumps({"ffmpeg_commit":"611cec792aa54e2e53dbc1be95dcf5955d9a99ba",
            "configure":[portable(f) for f in flags] if args.alpha else flags,"profile":"alpha" if args.alpha else "development",
            "ndk":ndk.name,"mbedtls":"3.6.7","license":"LGPL-3.0-or-later"},indent=2)+"\n")
        for name in ("COPYING.LGPLv3","COPYING.GPLv3","LICENSE.md"):
            (prefix/name).write_bytes((source/name).read_bytes())
    print("Android FFmpeg libraries:",prefix,flush=True)

if __name__=="__main__":
    main()
