<#
.SYNOPSIS
    Build and run the watermelon-audio host C++ test suite (dsp + effects +
    looper + usb) in a single configure / build / ctest pass.

.DESCRIPTION
    Locates a host toolchain automatically — MSYS2 UCRT64 g++ for the
    compiler, Ninja (bundled with the Android SDK cmake) for the generator —
    and prepends the UCRT64 bin dir to PATH so the resulting .exe files find
    their libstdc++/libgcc/winpthread DLLs at run time.

    The compiler is the one pinned in .github/toolchain-pins.json
    (_windows_cross.msys2_ucrt64): it must live under the pinned root and
    report the pinned GCC version, or the script stops. It never falls back
    to MSYS2 mingw64 (msvcrt), which is a different C runtime.

    No Android SDK build, no NDK, no Oboe, no device required. Pure host x86_64.

.PARAMETER Filter
    ctest -R regex: run only tests whose name matches (e.g. -Filter Clock).

.PARAMETER Clean
    Delete the build directory before configuring (forces a fresh build;
    the googletest download in .deps is kept).

.PARAMETER Jobs
    ctest -j: tests in parallel (default: the CTEST_JOBS environment variable, else the number of logical
    processors; the same default as run-cpp-tests.sh). Serial took ~1200 s for the whole suite. Each
    test is its own process with its own temp names, so they do not share state; lower it to debug a
    flake, never raise a timeout.

.PARAMETER Fast
    Skip Stress/Slow tests. Like -Filter it runs fewer tests, so the not-run
    check tolerates listed tests that do not show up (--partial).

.EXAMPLE
    pwsh scripts/run-cpp-tests.ps1
    pwsh scripts/run-cpp-tests.ps1 -Filter UsbDescriptorParser
    pwsh scripts/run-cpp-tests.ps1 -Clean
#>
[CmdletBinding()]
param(
    [string]$Filter,
    [switch]$Clean,
    [switch]$Fast,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
if ($Jobs -le 0) {
    $Jobs = if ($env:CTEST_JOBS) { [int]$env:CTEST_JOBS } else { [int]$env:NUMBER_OF_PROCESSORS }
}
if ($Jobs -le 0) { $Jobs = 4 }

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
    'C:\msys64\ucrt64\bin\ninja.exe'
)
if (-not $ninjaBin) { throw "ninja not found. It ships with the Android SDK 'cmake;3.22.1' package." }

# ---- locate g++ / gcc (compiler) ---------------------------------------
# The toolchain is pinned in .github/toolchain-pins.json, the single source of
# the root and of the GCC version (scripts/check-dep-pins.py enforces it).
$PinsFile = Join-Path $RepoRoot '.github\toolchain-pins.json'
if (-not (Test-Path $PinsFile)) { throw "$PinsFile not found: cannot read the pinned MSYS2 UCRT64 toolchain." }
$msys2 = (Get-Content -Raw -Encoding UTF8 $PinsFile | ConvertFrom-Json)._windows_cross.msys2_ucrt64
if (-not $msys2 -or -not $msys2.root -or -not $msys2.gcc) {
    throw "$PinsFile has no _windows_cross.msys2_ucrt64 {root, gcc}."
}
$gxx = Join-Path $msys2.root 'bin\g++.exe'
if (-not (Test-Path $gxx)) {
    throw "MSYS2 UCRT64 g++ not found at $gxx. Install it from an MSYS2 shell: 'pacman -S mingw-w64-ucrt-x86_64-gcc' (package $($msys2.package) $($msys2.package_version)). mingw64 is NOT an accepted fallback: it is a different C runtime."
}
$gccVersion = (& $gxx -dumpfullversion).Trim()
if ($gccVersion -ne $msys2.gcc) {
    throw "$gxx is GCC '$gccVersion' but .github/toolchain-pins.json pins '$($msys2.gcc)' (package $($msys2.package) $($msys2.package_version)). Install that version, or change the pin deliberately."
}
$gcc      = Join-Path (Split-Path -Parent $gxx) 'gcc.exe'
$mingwBin = Split-Path -Parent $gxx

# UCRT64 exes need their runtime DLLs (libstdc++-6, libgcc_s_seh-1,
# libwinpthread-1) on PATH when ctest launches them.
$env:PATH = "$mingwBin;$(Split-Path -Parent $ninjaBin);$env:PATH"

Write-Host "cmake   : $cmakeBin"
Write-Host "ninja   : $ninjaBin"
Write-Host "g++     : $gxx (GCC $gccVersion, pinned)"
Write-Host "build   : $BuildDir"
Write-Host ""

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir ..."
    Remove-Item -Recurse -Force $BuildDir
}

# ---- configure ----------------------------------------------------------
# CMake writes these paths verbatim into CMakeCXXCompiler.cmake, where a
# backslash is an escape ('C:\msys64\mingw64' -> "Invalid character escape
# '\m'"). Forward slashes are valid Windows paths and survive that.
$ninjaBin, $gxx, $gcc, $DepsDir = @($ninjaBin, $gxx, $gcc, $DepsDir) | ForEach-Object { $_ -replace '\\', '/' }
# Each gtest writes its own XML under $ResultsDir/<target>/ (wma_gtest_discover_tests, XML_OUTPUT_DIR): the
# results scripts/check-windows-not-run.py reads (AC-061.1). Set at configure time because ctest bakes it
# into the per-test command line.
$ResultsDir = Join-Path $BuildDir 'gtest-xml'
& $cmakeBin -S $SrcDir -B $BuildDir -G Ninja `
    -DWMA_GTEST_XML_DIR="$($ResultsDir -replace '\\', '/')" `
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
# Cada TEST es un test de ctest (gtest_discover_tests) y por tanto un PROCESO que escribe su propio XML
# (<ResultsDir>/<target>/<Suite>.<Test>.xml, nombre propio: ctest en paralelo no los pisa). Esos XML son lo
# que lee scripts/check-windows-not-run.py (AC-061.1); se borran antes para que no quede nada de otra corrida.
if (Test-Path $ResultsDir) { Remove-Item -Recurse -Force $ResultsDir }
New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

$ctestBin = Join-Path (Split-Path -Parent $cmakeBin) 'ctest.exe'
$ctestArgs = @('--test-dir', $BuildDir, '--output-on-failure', '-j', $Jobs)
if ($Filter) { $ctestArgs += @('-R', $Filter) }
# -Fast: skip slow stress tests (name contains Stress/Slow) for a tight
# edit -> test loop. They still run in CI and in an unflagged local run.
if ($Fast) { $ctestArgs += @('-E', 'Stress|Slow') }

Write-Host "ctest -j $Jobs"
$ctestWatch = [System.Diagnostics.Stopwatch]::StartNew()
& $ctestBin @ctestArgs
$ctestExit = $LASTEXITCODE
Write-Host ("ctest wall time: {0:N0} s" -f $ctestWatch.Elapsed.TotalSeconds)

# ---- el trinquete de lo que no corre en Windows --------------------------
# Corre SIEMPRE, tambien con ctest en rojo: un rojo no puede ocultar que el listado se desfaso.
# -Filter/-Fast corren menos tests a proposito: ahi un test del listado puede no aparecer (--partial);
# un test que corrio y fallo, o un skip sin declarar, falla igual.
$python = Find-First -OnPath 'python' -Candidates @()
if (-not $python) { $python = Find-First -OnPath 'python3' -Candidates @() }
if (-not $python) { throw "python not found: scripts/check-windows-not-run.py needs it (AC-061.1)." }
$checkArgs = @((Join-Path $RepoRoot 'scripts\check-windows-not-run.py'), '--results', $ResultsDir)
if ($Filter -or $Fast) { $checkArgs += '--partial' }
& $python @checkArgs
$checkExit = $LASTEXITCODE

if ($ctestExit -ne 0) { exit $ctestExit }
exit $checkExit
