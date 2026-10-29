"""Build against the sibling CyVR_UI without changing its sources or APK."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import zipfile
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]
UI=Path(os.environ.get('CYVR_UI_ROOT',ROOT.parent/'CyVR_UI')).resolve()
SDK=Path(os.environ.get('ANDROID_HOME',Path(os.environ['LOCALAPPDATA'])/'Android/Sdk'))
NDK=SDK/'ndk/27.2.12479018'
CMAKE=SDK/'cmake/3.22.1/bin/cmake.exe'
NINJA=CMAKE.with_name('ninja.exe')
JAVA=Path(os.environ.get('JAVA_HOME',r'C:\Program Files\Android\Android Studio\jbr'))/'bin'
BUILD_TOOLS=SDK/'build-tools/35.0.1'
ANDROID_JAR=SDK/'platforms/android-34/android.jar'
def run(*args):subprocess.run([str(a) for a in args],check=True,cwd=ROOT)
def configure(target,immersive=True,alpha=False):
    directory=ROOT/'build'/('app-'+target+('-panel' if target=='android' and not immersive else '')+('-alpha' if alpha else ''))
    common=[CMAKE,'-S',ROOT,'-B',directory,'-G','Ninja',f'-DCMAKE_MAKE_PROGRAM={NINJA}',
        '-DCMAKE_BUILD_TYPE='+('Release' if alpha else 'RelWithDebInfo'),'-DCYVR_PLAYER_WITH_FFMPEG=ON','-DCYVR_PLAYER_BUILD_APP=ON',
        f'-DCYVR_UI_ROOT={UI}',
        '-DCYVR_ENABLE_IMMERSIVE='+('ON' if immersive else 'OFF'),
        f'-DCYVR_APP_GLSLC={NDK / "shader-tools/windows-x86_64/glslc.exe"}',
        f'-DCYVR_PLAYER_GLSLC={NDK / "shader-tools/windows-x86_64/glslc.exe"}',
        f'-DCYVR_PLAYER_SPIRV_VAL={NDK / "shader-tools/windows-x86_64/spirv-val.exe"}']
    if target=='android':common += [f'-DCMAKE_TOOLCHAIN_FILE={NDK / "build/cmake/android.toolchain.cmake"}',
        '-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-32','-DANDROID_STL=c++_static','-DCYVR_PLAYER_BUILD_TESTS=OFF',
        '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g -DNDEBUG','-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g -DNDEBUG']
    if target=='android' and alpha:
        flags=f'-O3 -g0 -DNDEBUG -ffile-prefix-map={ROOT.as_posix()}=/cyvr-player -ffile-prefix-map={NDK.as_posix()}=/android-ndk'
        common += [f'-DCYVR_PLAYER_FFMPEG_ROOT={ROOT / ".deps/ffmpeg/android-arm64-alpha"}',
            f'-DCMAKE_C_FLAGS_RELEASE={flags}',f'-DCMAKE_CXX_FLAGS_RELEASE={flags}',
            '-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-Wl,--strip-all']
    if target!='android':
        clang=Path(os.environ.get('CXX',r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'))
        rc=sorted((Path(os.environ['ProgramFiles(x86)'])/'Windows Kits/10/bin').glob('*/x64/rc.exe'))[-1]
        common += [f'-DCMAKE_CXX_COMPILER:FILEPATH={clang.as_posix()}',f'-DCMAKE_C_COMPILER:FILEPATH={clang.with_name("clang.exe").as_posix()}',f'-DCMAKE_RC_COMPILER:FILEPATH={rc.as_posix()}']
    run(*common);run(CMAKE,'--build',directory,'--parallel','8')
    return directory
def notices(destination):
    destination.mkdir(parents=True,exist_ok=True)
    for name,source in [('CyVR_Player-GPLv3.txt',ROOT/'LICENSE'),('CyVR_Player-NOTICE.txt',ROOT/'NOTICE'),
            ('CyVR_UI-MIT.txt',UI/'LICENSE'),('CyVR_UI-THIRD_PARTY.md',UI/'THIRD_PARTY.md')]:shutil.copyfile(source,destination/name)
    for name,source in [('FFmpeg-LGPLv3.txt',ROOT/'.deps/ffmpeg/source/COPYING.LGPLv3'),
            ('FFmpeg-GPLv3-terms.txt',ROOT/'.deps/ffmpeg/source/COPYING.GPLv3'),
            ('MbedTLS-Apache-2.0.txt',ROOT/'.deps/mbedtls/source/LICENSE'),
            ('dependencies.lock.json',ROOT/'dependencies.lock.json')]:
        if source.exists():shutil.copyfile(source,destination/name)
    for folder in [ROOT/'third_party',ROOT/'.deps/ffmpeg/android-arm64/licenses',ROOT/'.deps/ffmpeg/windows/licenses',UI/'third_party',UI/'assets/fonts']:
        if folder.exists():
            for file in folder.rglob('*'):
                if file.is_file() and (file.name.lower().startswith(('license','copying','ofl','notice'))):
                    relative=str(file.relative_to(folder)).replace('\\','_').replace('/','_')
                    shutil.copyfile(file,destination/(folder.name+'_'+relative))
def android(immersive=True,alpha=False):
    directory=configure('android',immersive,alpha);classes=directory/'java';classes.mkdir(exist_ok=True)
    run(JAVA/'javac.exe','-source','8','-target','8','-Xlint:-options','-classpath',ANDROID_JAR,'-d',classes,*sorted((ROOT/'android/java').rglob('*.java')))
    jar=directory/'classes.jar'
    with zipfile.ZipFile(jar,'w') as archive:
        for file in classes.rglob('*.class'):archive.write(file,file.relative_to(classes).as_posix())
    run(JAVA/'java.exe','-cp',BUILD_TOOLS/'lib/d8.jar','com.android.tools.r8.D8','--min-api','32','--lib',ANDROID_JAR,'--output',directory,jar)
    notices(directory/'assets/licenses')
    resources=directory/'resources.apk'
    manifest=ROOT/'android/AndroidManifest.xml'
    if not immersive or alpha:
        ET.register_namespace('android','http://schemas.android.com/apk/res/android');ET.register_namespace('metavr','http://schemas.meta.com/metavr-sdk')
        tree=ET.parse(manifest);application=tree.getroot().find('application')
        if alpha:application.set('{http://schemas.android.com/apk/res/android}debuggable','false')
        if not immersive:
            for activity in list(application.findall('activity')):
                if activity.get('{http://schemas.android.com/apk/res/android}name','').endswith('.ImmersiveActivity'):application.remove(activity)
            for query in list(tree.getroot().findall('queries')):tree.getroot().remove(query)
        manifest=directory/'AndroidManifest.xml';tree.write(manifest,encoding='utf-8',xml_declaration=True)
    run(BUILD_TOOLS/'aapt2.exe','link','-o',resources,'-I',ANDROID_JAR,'--manifest',manifest,'-A',directory/'assets')
    unsigned=directory/'unsigned.apk'
    with zipfile.ZipFile(resources) as source,zipfile.ZipFile(unsigned,'w') as archive:
        for item in source.infolist():archive.writestr(item.filename,source.read(item.filename),compress_type=item.compress_type)
        archive.write(directory/'classes.dex','classes.dex')
        media=ROOT/'.deps/ffmpeg'/('android-arm64-alpha' if alpha else 'android-arm64')
        libs=[directory/'libcyvr_player.so',*sorted((media/'lib').glob('*.so'))]
        if immersive:libs.append(UI/'.deps/openxr/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so')
        for file in libs:
            if alpha:
                staged=directory/'package-libs'/file.name;staged.parent.mkdir(exist_ok=True);shutil.copyfile(file,staged)
                run(NDK/'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe','--strip-unneeded',staged)
                file=staged
            archive.write(file,'lib/arm64-v8a/'+file.name,compress_type=zipfile.ZIP_STORED)
    aligned=directory/'aligned.apk';run(BUILD_TOOLS/'zipalign.exe','-P','16','-f','4',unsigned,aligned)
    key=ROOT/'.cache/debug.keystore';key.parent.mkdir(exist_ok=True)
    if alpha and not key.exists():raise RuntimeError('Alpha updates require the existing development signing key; it is never included in release assets.')
    if not key.exists():run(JAVA/'keytool.exe','-genkeypair','-keystore',key,'-storepass','android','-keypass','android','-alias','androiddebugkey','-keyalg','RSA','-keysize','2048','-validity','10000','-dname','CN=CyVR Player Development,O=CyVR,C=FR')
    version=ET.parse(manifest).getroot().get('{http://schemas.android.com/apk/res/android}versionName')
    name=f'CyVR_Player-{version}-'+('' if immersive else 'panel-')+'arm64.apk' if alpha else ('CyVR_Player-debug.apk' if immersive else 'CyVR_Player-panel-debug.apk')
    output=ROOT/'dist/android'/name;output.parent.mkdir(parents=True,exist_ok=True)
    signer=BUILD_TOOLS/'lib/apksigner.jar'
    run(JAVA/'java.exe','-jar',signer,'sign','--ks',key,'--ks-pass','pass:android','--key-pass','pass:android','--out',output,aligned)
    run(JAVA/'java.exe','-jar',signer,'verify','--verbose',output);run(BUILD_TOOLS/'zipalign.exe','-c','-P','16','4',output)
    print('Quest APK:',output)
def windows():
    directory=configure('windows');run(CMAKE.with_name('ctest.exe'),'--test-dir',directory,'--output-on-failure')
    output=ROOT/'dist/windows';output.mkdir(parents=True,exist_ok=True);shutil.copyfile(directory/'CyVR_Player.exe',output/'CyVR_Player.exe');shutil.copytree(directory/'assets',output/'assets',dirs_exist_ok=True)
    for file in (ROOT/'.deps/ffmpeg/windows/bin').glob('*.dll'):shutil.copyfile(file,output/file.name)
    notices(output/'assets/licenses');print('Windows preview:',output/'CyVR_Player.exe')
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--target',choices=['windows','android','all'],default='all');parser.add_argument('--no-immersive',action='store_true');parser.add_argument('--alpha',action='store_true');args=parser.parse_args()
    if args.alpha and args.target!='android':parser.error('--alpha requires --target android')
    run('python',ROOT/'tools/generate_player_locales.py')
    run('python',ROOT/'tools/adapt_gsr1.py','--check')
    if args.target in ('windows','all'):windows()
    if args.target in ('android','all'):android(not args.no_immersive,args.alpha)
