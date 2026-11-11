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

.PARAMETER TimeoutSec
    ctest --timeout: seconds per test before ctest kills it and reports it failed (0 = ctest's default).
    scripts/gate-windows.ps1 passes 180, like the sanitizer runs in CLAUDE.md.

.PARAMETER Offline
    Configure disconnected from the network (FETCHCONTENT_FULLY_DISCONNECTED=ON): googletest must already be in
    the deps dir, and if it is not the script stops BEFORE configuring with how to prepare it. scripts/gate-windows.ps1
    passes it (the gate downloads nothing). Run the script once without it to populate .deps.

.PARAMETER DepsDir
    Where FetchContent keeps googletest (default: audio/src/main/cpp/tests/.deps, ignored by git).

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
    [int]$Jobs = 0,
    [int]$TimeoutSec = 0,
    [switch]$Offline,
    [string]$DepsDir
)

$ErrorActionPreference = 'Stop'
if ($Jobs -le 0) {
    $Jobs = if ($env:CTEST_JOBS) { [int]$env:CTEST_JOBS } else { [int]$env:NUMBER_OF_PROCESSORS }
}
if ($Jobs -le 0) { $Jobs = 4 }

$RepoRoot = Split-Path -Parent $PSScriptRoot
$SrcDir   = Join-Path $RepoRoot 'audio\src\main\cpp\tests'
$BuildDir = Join-Path $SrcDir 'build'
if (-not $DepsDir) { $DepsDir = Join-Path $SrcDir '.deps' }

# cmake, ninja and the pinned MSYS2 UCRT64 g++ (root + GCC version from .github/toolchain-pins.json,
# the single source; scripts/check-dep-pins.py enforces it). Shared with scripts/gate-windows.ps1.
. (Join-Path $PSScriptRoot 'windows-toolchain.ps1')
$tc = Get-WmaHostToolchain -RepoRoot $RepoRoot
$cmakeBin, $ninjaBin, $gxx, $gcc = $tc.Cmake, $tc.Ninja, $tc.Gxx, $tc.Gcc
$gccVersion = $tc.GccVersion
$mingwBin = $tc.MingwBin

# UCRT64 exes need their runtime DLLs (libstdc++-6, libgcc_s_seh-1,
# libwinpthread-1) on PATH when ctest launches them.
$env:PATH = "$mingwBin;$(Split-Path -Parent $ninjaBin);$env:PATH"

if ($Offline -and -not (Test-Path (Join-Path $DepsDir 'googletest-src\CMakeLists.txt'))) {
    Write-Host "run-cpp-tests: -Offline y falta googletest en $DepsDir."
    Write-Host "  Preparalo una vez FUERA del gate (baja googletest de GitHub): corre scripts/run-cpp-tests.ps1 sin -Offline (una corrida de la suite basta)"
    exit 2
}

# Desconectado, FetchContent no re-chequea el GIT_TAG: un bump del SHA correria contra el checkout viejo en verde.
# El SHA se lee de googletest.cmake (unico lugar del pin, scripts/check-dep-pins.py); googletest-src es un checkout git.
if ($Offline) {
    $pinFile = Join-Path $RepoRoot 'audio\src\main\cpp\thirdparty\googletest.cmake'
    $pinMatch = [regex]::Match((Get-Content -Raw -LiteralPath $pinFile), '(?m)^\s*GIT_TAG\s+([0-9a-f]{40})\b')
    if (-not $pinMatch.Success) { throw "No encuentro el GIT_TAG (SHA de 40 hex) en $pinFile." }
    $wantSha = $pinMatch.Groups[1].Value
    $gotSha = (& git -C (Join-Path $DepsDir 'googletest-src') rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $gotSha) { $gotSha = '(no es un checkout git)' }
    $gotSha = "$gotSha".Trim()
    if ($gotSha -ne $wantSha) {
        Write-Host "run-cpp-tests: -Offline y el googletest instalado en $DepsDir no es el fijado."
        Write-Host "  instalada: $gotSha"
        Write-Host "  fijada   : $wantSha (audio/src/main/cpp/thirdparty/googletest.cmake)"
        Write-Host "  Actualizalo FUERA del gate (baja de GitHub): borra $(Join-Path $DepsDir 'googletest-src') y corre scripts/run-cpp-tests.ps1 sin -Offline (una corrida basta)."
        exit 2
    }
}

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
# Explicito en los dos sentidos: la variable queda en el CMakeCache y un -Offline previo no puede pegarse a una corrida manual.
$offlineArgs = @("-DFETCHCONTENT_FULLY_DISCONNECTED=$(if ($Offline) { 'ON' } else { 'OFF' })")
& $cmakeBin -S $SrcDir -B $BuildDir -G Ninja @offlineArgs `
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

$ctestBin = $tc.Ctest
$ctestArgs = @('--test-dir', $BuildDir, '--output-on-failure', '-j', $Jobs)
if ($TimeoutSec -gt 0) { $ctestArgs += @('--timeout', $TimeoutSec) }
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
$checker = Join-Path $RepoRoot 'scripts\check-windows-not-run.py'
# El --self-test PRIMERO (scripts/CLAUDE.md): si el parser del trinquete se rompe, queda en verde para siempre.
& $python $checker --self-test
$selfTestExit = $LASTEXITCODE
# --build-dir: de ahi sale que exes registra la suite y que tests lista cada uno (`<exe> --gtest_list_tests`):
# un test registrado sin XML (ctest -E, TEST_FILTER, DISABLED) es rojo. Si Smart App Control no deja listar un
# exe, tambien es rojo: no se sabe que registra.
$checkArgs = @($checker, '--results', $ResultsDir, '--build-dir', $BuildDir)
if ($Filter -or $Fast) { $checkArgs += '--partial' }
& $python @checkArgs
$checkExit = $LASTEXITCODE
if ($selfTestExit -ne 0) { $checkExit = $selfTestExit }

if ($ctestExit -ne 0) { exit $ctestExit }
exit $checkExit
