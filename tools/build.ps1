param([ValidateSet('host','android','all')][string]$Target = 'all', [switch]$CoreOnly)
$ErrorActionPreference = 'Stop'
$playerRoot = Split-Path -Parent $PSScriptRoot
$sdkRoot = $env:ANDROID_HOME
if (!$sdkRoot) { $sdkRoot = Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
function Find-VersionedTool([string]$Directory, [string]$File) {
    $candidate = Get-ChildItem -LiteralPath $Directory -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName $File) } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (!$candidate) { throw "No installed tool found: $Directory / $File" }
    return Join-Path $candidate.FullName $File
}
function Invoke-Checked([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Build command failed ($LASTEXITCODE): $Tool" }
}
$cmakePath = Find-VersionedTool (Join-Path $sdkRoot 'cmake') 'bin\cmake.exe'
$ndkToolchain = Find-VersionedTool (Join-Path $sdkRoot 'ndk') 'build\cmake\android.toolchain.cmake'
$ndkRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $ndkToolchain))
$shaderDir = Join-Path $ndkRoot 'shader-tools\windows-x86_64'
$glslcPath = Join-Path $shaderDir 'glslc.exe'
$spirvValPath = Join-Path $shaderDir 'spirv-val.exe'
$ninjaPath = Join-Path (Split-Path -Parent $cmakePath) 'ninja.exe'
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
$mediaOption = if ($CoreOnly) { '-DCYVR_PLAYER_WITH_FFMPEG=OFF' } else { '-DCYVR_PLAYER_WITH_FFMPEG=ON' }
if ($Target -in @('host','all')) {
    $hostBuild = Join-Path $playerRoot 'build\host-clang'
    $clangCandidates = @($env:CXX,
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'),
        (Join-Path $env:ProgramFiles 'LLVM\bin\clang++.exe'))
    $clangPath = $clangCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (!$clangPath) { throw 'Set CXX to clang++.exe; an installed MSVC toolchain and Windows SDK are also needed' }
    $rcPath = Find-VersionedTool (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') 'x64\rc.exe'
    Invoke-Checked $cmakePath @('-S',$playerRoot,'-B',$hostBuild,'-G','Ninja',
        "-DCMAKE_CXX_COMPILER:FILEPATH=$($clangPath.Replace('\','/'))","-DCMAKE_RC_COMPILER:FILEPATH=$($rcPath.Replace('\','/'))","-DCMAKE_MAKE_PROGRAM=$ninjaPath",
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo',"-DCYVR_PLAYER_GLSLC=$glslcPath","-DCYVR_PLAYER_SPIRV_VAL=$spirvValPath",$mediaOption)
    Invoke-Checked $cmakePath @('--build',$hostBuild,'--config','RelWithDebInfo','--parallel')
    Invoke-Checked $ctestPath @('--test-dir',$hostBuild,'-C','RelWithDebInfo','--output-on-failure')
}
if ($Target -in @('android','all')) {
    $androidBuild = Join-Path $playerRoot 'build\android-arm64'
    Invoke-Checked $cmakePath @('-S',$playerRoot,'-B',$androidBuild,'-G','Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninjaPath","-DCMAKE_TOOLCHAIN_FILE=$ndkToolchain",'-DANDROID_ABI=arm64-v8a',
        '-DANDROID_PLATFORM=android-29','-DANDROID_STL=c++_static','-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON',
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCYVR_PLAYER_BUILD_TESTS=OFF',$mediaOption)
    Invoke-Checked $cmakePath @('--build',$androidBuild,'--parallel')
    $jdkRoot = $env:JAVA_HOME
    if (!$jdkRoot) { $jdkRoot = 'C:\Program Files\Android\Android Studio\jbr' }
    $javacPath = Join-Path $jdkRoot 'bin\javac.exe'
    $platform = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'platforms') -Directory |
        Where-Object { $_.Name -match '^android-\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'android.jar')) } |
        Sort-Object { [int]($_.Name -replace '^android-','') } -Descending | Select-Object -First 1
    if (!$platform) { throw 'No Android SDK platform is installed' }
    if (!(Test-Path -LiteralPath $javacPath)) { throw 'Set JAVA_HOME to a JDK to compile CodecProbe.java' }
    $javaOutput = Join-Path $androidBuild 'java'
    New-Item -ItemType Directory -Force -Path $javaOutput | Out-Null
    $javaSources = @(Get-ChildItem -LiteralPath (Join-Path $playerRoot 'android\java') -Recurse -Filter '*.java' | ForEach-Object { $_.FullName })
    Invoke-Checked $javacPath (@('-source','8','-target','8','-Xlint:-options',
        '-classpath',(Join-Path $platform.FullName 'android.jar'),'-d',$javaOutput) + $javaSources)
}
