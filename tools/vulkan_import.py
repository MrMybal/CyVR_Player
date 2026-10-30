"""Generate a local import library for the installed Windows Vulkan loader."""
from pathlib import Path
import re,subprocess,sys
header,out,compiler=map(Path,sys.argv[1:]);out.mkdir(parents=True,exist_ok=True)
if not header.exists():
    # CyVR_UI uses the NDK's Khronos headers on the Windows inspector too.
    import os
    sdk=Path(os.environ.get('ANDROID_HOME',Path(os.environ['LOCALAPPDATA'])/'Android/Sdk'))
    header=sdk/'ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include/vulkan/vulkan_core.h'
names=sorted(set(re.findall(r'VKAPI_CALL\s+(vk\w+)\s*\(',header.read_text())))
if len(names)<100:raise RuntimeError('Invalid Vulkan declarations')
definition=out/'vulkan-1.def';definition.write_text('LIBRARY vulkan-1.dll\nEXPORTS\n'+'\n'.join(names)+'\n')
subprocess.run([str(compiler.parent/'lld-link.exe'),'/lib','/machine:x64','/def:'+str(definition),'/out:'+str(out/'vulkan-1.lib')],check=True,stdout=subprocess.DEVNULL)
