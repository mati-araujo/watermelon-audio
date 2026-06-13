<#
.SYNOPSIS
    Build and run the watermelon-audio host C++ test suite (dsp + effects +
    looper + usb) in a single configure / build / ctest pass.

.DESCRIPTION
    Locates a host toolchain automatically — MinGW g++ (msys64) for the
    compiler, Ninja (bundled with the Android SDK cmake) for the generator —
    and prepends the MinGW bin dir to PATH so the resulting .exe files find
    their libstdc++/libgcc/winpthread DLLs at run time.

    No Android SDK build, no NDK, no Oboe, no device required. Pure host x86_64.

.PARAMETER Filter
    ctest -R regex: run only tests whose name matches (e.g. -Filter Clock).

.PARAMETER Clean
    Delete the build directory before configuring (forces a fresh build;
    the googletest download in .deps is kept).

.EXAMPLE
    pwsh scripts/run-cpp-tests.ps1
    pwsh scripts/run-cpp-tests.ps1 -Filter UsbDescriptorParser
    pwsh scripts/run-cpp-tests.ps1 -Clean
#>
[CmdletBinding()]
param(
    [string]$Filter,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$SrcDir   = Join-Path $RepoRoot 'audio\src\main\cpp\tests'
$BuildDir = Join-Path $SrcDir 'build'
$DepsDir  = Join-Path $SrcDir '.deps'

function Find-First {
    param([string[]]$Candidates, [string]$OnPath)
    if ($OnPath) {
        $cmd = Get-Command $OnPath -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    foreach ($c in $Candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

# ---- locate cmake -------------------------------------------------------
$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME }
           elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT }
           else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }

$cmakeBin = Find-First -OnPath 'cmake' -Candidates @(
    'C:\Program Files\CMake\bin\cmake.exe',
    (Join-Path $sdkRoot 'cmake\3.22.1\bin\cmake.exe')
)
if (-not $cmakeBin) { throw "cmake not found. Install CMake or the Android SDK 'cmake;3.22.1' package." }

# ---- locate ninja (generator) ------------------------------------------
$ninjaBin = Find-First -OnPath 'ninja' -Candidates @(
    (Join-Path $sdkRoot 'cmake\3.22.1\bin\ninja.exe'),
    'C:\msys64\mingw64\bin\ninja.exe'
)
if (-not $ninjaBin) { throw "ninja not found. It ships with the Android SDK 'cmake;3.22.1' package." }

# ---- locate g++ / gcc (compiler) ---------------------------------------
$gxx = Find-First -OnPath 'g++' -Candidates @(
    'C:\msys64\mingw64\bin\g++.exe',
    'C:\msys64\ucrt64\bin\g++.exe',
    'C:\mingw64\bin\g++.exe'
)
if (-not $gxx) {
    throw "g++ (MinGW) not found. Install via MSYS2: 'pacman -S mingw-w64-x86_64-gcc' (expected at C:\msys64\mingw64\bin\g++.exe)."
}
$gcc     = Join-Path (Split-Path -Parent $gxx) 'gcc.exe'
$mingwBin = Split-Path -Parent $gxx

# MinGW exes need their runtime DLLs (libstdc++-6, libgcc_s_seh-1,
# libwinpthread-1) on PATH when ctest launches them.
$env:PATH = "$mingwBin;$(Split-Path -Parent $ninjaBin);$env:PATH"

Write-Host "cmake   : $cmakeBin"
Write-Host "ninja   : $ninjaBin"
Write-Host "g++     : $gxx"
Write-Host "build   : $BuildDir"
Write-Host ""

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir ..."
    Remove-Item -Recurse -Force $BuildDir
}

# ---- configure ----------------------------------------------------------
& $cmakeBin -S $SrcDir -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Debug `
    -DCMAKE_MAKE_PROGRAM="$ninjaBin" `
    -DCMAKE_CXX_COMPILER="$gxx" `
    -DCMAKE_C_COMPILER="$gcc" `
    -DFETCHCONTENT_BASE_DIR="$DepsDir"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }

# ---- build --------------------------------------------------------------
& $cmakeBin --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }

# ---- test ---------------------------------------------------------------
$ctestBin = Join-Path (Split-Path -Parent $cmakeBin) 'ctest.exe'
$ctestArgs = @('--test-dir', $BuildDir, '--output-on-failure')
if ($Filter) { $ctestArgs += @('-R', $Filter) }

& $ctestBin @ctestArgs
exit $LASTEXITCODE
