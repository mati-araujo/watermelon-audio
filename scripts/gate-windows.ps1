<#
.SYNOPSIS
    Gate LOCAL de la PC Windows (MSYS2 UCRT64): pin de GCC, lint de portabilidad, suite de host,
    watermelon_audio.dll con sus exports/imports, y su carga desde una JVM.

.DESCRIPTION
    No atesta (.github/local-gate.json lo escribe gate.sh y nadie mas), no escribe en el arbol, no
    baja ni ejecuta nada de la red. Los build dirs de la DLL y del launcher JVM viven en -WorkDir
    (fuera del repo). La suite usa scripts/run-cpp-tests.ps1, que construye en
    audio/src/main/cpp/tests/build (ignorado por git).

    Pasos, en orden:
      pin     el GCC de MSYS2 UCRT64 es el fijado en .github/toolchain-pins.json (AC-061.6)
      lint    check-cpp-portability.sh --self-test y despues el lint
      suite   scripts/run-cpp-tests.ps1 (ctest con timeout por test + check-windows-not-run.py --results)
      dll     build Release de audio/src/main/cpp/desktop; check-jni-symbols.py --pe y --imports
      jvm     System.loadLibrary + nativeIsEngineInitialized() == false desde una JVM (AC-061.5)
      repo    `git status --porcelain --ignored` al final == al principio

    Antes de lanzar cualquier hijo, SetErrorMode(FAILCRITICALERRORS | NOGPFAULTERRORBOX |
    NOOPENFILEERRORBOX): se hereda a los hijos y un crash/abort() de un test vuelve solo, sin dialogo
    de WER. (ctest ademas fija su propio modo; medido en la etapa, los dos caminos vuelven en < 1 s.)

.PARAMETER Only
    Corre un solo paso (pin, lint, suite, dll, jvm, repo) para iterar. NO cuenta como corrida completa.

.PARAMETER SkipJvm
    Salta el paso jvm, y lo dice en el resumen.

.PARAMETER WorkDir
    Donde van el build de la DLL y el launcher JVM. Default: %LOCALAPPDATA%\Temp\wma-gate-windows.
    La DLL queda en <WorkDir>\dll\watermelon_audio.dll.

.PARAMETER TestTimeoutSec
    ctest --timeout por test (default 180).
#>
[CmdletBinding()]
param(
    [ValidateSet('pin', 'lint', 'suite', 'dll', 'jvm', 'repo')]
    [string]$Only,
    [switch]$SkipJvm,
    [string]$WorkDir = (Join-Path $env:LOCALAPPDATA 'Temp\wma-gate-windows'),
    [int]$TestTimeoutSec = 180
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'windows-toolchain.ps1')

# Que python no deje __pycache__ en el arbol.
$env:PYTHONDONTWRITEBYTECODE = '1'

# ---- SetErrorMode: antes de lanzar cualquier hijo -----------------------------
Add-Type -Namespace Wma -Name ErrorMode -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint SetErrorMode(uint mode);
'@
$SEM_FAILCRITICALERRORS = 0x0001
$SEM_NOGPFAULTERRORBOX  = 0x0002
$SEM_NOOPENFILEERRORBOX = 0x8000
$errorModeBefore = [Wma.ErrorMode]::SetErrorMode($SEM_FAILCRITICALERRORS -bor $SEM_NOGPFAULTERRORBOX -bor $SEM_NOOPENFILEERRORBOX)
Write-Host ("SetErrorMode: 0x{0:X} -> 0x{1:X}" -f $errorModeBefore, ($SEM_FAILCRITICALERRORS -bor $SEM_NOGPFAULTERRORBOX -bor $SEM_NOOPENFILEERRORBOX))

# ---- ayudantes ------------------------------------------------------------------
$script:Tc = $null
function Get-Tc {
    if (-not $script:Tc) { $script:Tc = Get-WmaHostToolchain -RepoRoot $RepoRoot }
    $script:Tc
}

function Invoke-Native {
    param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @())
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') -> rc=$LASTEXITCODE" }
}

function Get-Bash {
    # El bash de Git, no el de WSL (System32\bash.exe).
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        $candidate = Join-Path (Split-Path -Parent (Split-Path -Parent $git.Source)) 'bin\bash.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if ($bash -and $bash.Source -notmatch '\\System32\\') { return $bash.Source }
    throw "No encuentro el bash de Git for Windows (scripts/check-cpp-portability.sh lo necesita)."
}

function Get-Python {
    $python = Find-First -OnPath 'python' -Candidates @()
    if (-not $python) { $python = Find-First -OnPath 'python3' -Candidates @() }
    if (-not $python) { throw "python not found: scripts/check-jni-symbols.py lo necesita." }
    $python
}

function Get-Jdk {
    $candidates = @()
    if ($env:JAVA_HOME) { $candidates += $env:JAVA_HOME }
    $candidates += @(Get-ChildItem 'C:\Program Files\Java' -Directory -Filter 'jdk-*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | ForEach-Object { $_.FullName })
    foreach ($c in $candidates) {
        if ((Test-Path (Join-Path $c 'include\jni.h')) -and (Test-Path (Join-Path $c 'bin\javac.exe'))) { return $c }
    }
    throw "No encuentro un JDK con include\jni.h y bin\javac.exe (JAVA_HOME='$($env:JAVA_HOME)', C:\Program Files\Java\jdk-*)."
}

function Get-RepoState {
    $s = & git -C $RepoRoot status --porcelain --ignored
    if ($LASTEXITCODE -ne 0) { throw "git status fallo (rc=$LASTEXITCODE)" }
    if ($s) { return ,[string[]]@($s) }
    return ,[string[]]@()
}

$DllDir = Join-Path $WorkDir 'dll'
$DllPath = Join-Path $DllDir 'watermelon_audio.dll'
$results = New-Object System.Collections.ArrayList

function Invoke-Step {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][scriptblock]$Body)
    Write-Host ""
    Write-Host "== $Name"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $rc = 0
    try { & $Body | Out-Host }
    catch {
        Write-Host "FALLA [$Name]: $($_.Exception.Message)"
        $rc = 1
    }
    [void]$results.Add([pscustomobject]@{ Name = $Name; Secs = $sw.Elapsed.TotalSeconds; Rc = $rc })
    return ($rc -eq 0)
}

# ---- pasos -----------------------------------------------------------------------
$stepPin = {
    $tc = Get-Tc
    Write-Host "g++     : $($tc.Gxx) (GCC $($tc.GccVersion), fijado en $($tc.PinsFile))"
    Write-Host "cmake   : $($tc.Cmake)"
    Write-Host "ninja   : $($tc.Ninja)"
}

$stepLint = {
    $bash = Get-Bash
    Push-Location $RepoRoot
    try {
        Invoke-Native $bash @('scripts/check-cpp-portability.sh', '--self-test')
        Invoke-Native $bash @('scripts/check-cpp-portability.sh')
    } finally { Pop-Location }
}

$stepSuite = {
    $null = Get-Tc
    $ps = Join-Path $PSHOME 'powershell.exe'
    Invoke-Native $ps @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $PSScriptRoot 'run-cpp-tests.ps1'), '-TimeoutSec', $TestTimeoutSec)
}

$stepDll = {
    $tc = Get-Tc
    $jdk = Get-Jdk
    $python = Get-Python
    $env:PATH = "$($tc.MingwBin);$(Split-Path -Parent $tc.Ninja);$env:PATH"
    $src = Join-Path $RepoRoot 'audio\src\main\cpp\desktop'
    $fwd = { param($p) $p -replace '\\', '/' }
    Invoke-Native $tc.Cmake @('-S', $src, '-B', $DllDir, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_MAKE_PROGRAM=$(& $fwd $tc.Ninja)",
        "-DCMAKE_CXX_COMPILER=$(& $fwd $tc.Gxx)",
        "-DWMA_JAVA_HOME=$(& $fwd $jdk)")
    Invoke-Native $tc.Cmake @('--build', $DllDir)
    if (-not (Test-Path $DllPath)) { throw "El build no dejo $DllPath." }
    Write-Host "DLL: $DllPath"

    $checker = Join-Path $PSScriptRoot 'check-jni-symbols.py'
    Invoke-Native $python @($checker, '--self-test')
    Invoke-Native $python @($checker, '--pe', $DllPath, '--imports', $DllPath, '--objdump', $tc.Objdump)

    Write-Host "objdump -p (DLL Name):"
    & $tc.Objdump -p $DllPath | Select-String 'DLL Name' | ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
    if ($LASTEXITCODE -ne 0) { throw "objdump -p -> rc=$LASTEXITCODE" }
}

$stepJvm = {
    $jdk = Get-Jdk
    if (-not (Test-Path $DllPath)) { throw "No hay DLL en ${DllPath}: corre el paso dll antes." }
    $jvmDir = Join-Path $WorkDir 'jvm'
    if (Test-Path $jvmDir) { Remove-Item -Recurse -Force $jvmDir }
    $pkgDir = Join-Path $jvmDir 'src\com\watermellonstudios\audio\internal\bridge'
    New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null
    $bridgeKt = Join-Path $RepoRoot 'audio\src\jvmSharedMain\kotlin\com\watermellonstudios\audio\internal\bridge\AudioNativeBridge.kt'
    if (-not (Test-Path $bridgeKt)) { throw "El bridge Kotlin ya no esta en ${bridgeKt}: el launcher declara su paquete y nombre." }

    # Mismo paquete y nombre que el bridge Kotlin: el simbolo JNI sale de ellos.
    Set-Content -Encoding ASCII -Path (Join-Path $pkgDir 'AudioNativeBridge.java') -Value @'
package com.watermellonstudios.audio.internal.bridge;
public class AudioNativeBridge {
    public native boolean nativeIsEngineInitialized();
}
'@
    Set-Content -Encoding ASCII -Path (Join-Path $jvmDir 'src\Launcher.java') -Value @'
import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge;
public class Launcher {
    public static void main(String[] args) {
        System.loadLibrary("watermelon_audio");
        boolean init = new AudioNativeBridge().nativeIsEngineInitialized();
        System.out.println("nativeIsEngineInitialized=" + init);
        System.exit(init ? 1 : 0);
    }
}
'@
    $classes = Join-Path $jvmDir 'classes'
    New-Item -ItemType Directory -Force -Path $classes | Out-Null
    Invoke-Native (Join-Path $jdk 'bin\javac.exe') @('-d', $classes,
        (Join-Path $pkgDir 'AudioNativeBridge.java'), (Join-Path $jvmDir 'src\Launcher.java'))

    # PATH reducido: con el de arriba (UCRT64 delante) un runtime que la DLL no lleva dentro
    # lo encontraria la JVM y el paso daria verde por casualidad.
    $savedPath = $env:PATH
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    try {
        Invoke-Native (Join-Path $jdk 'bin\java.exe') @("-Djava.library.path=$DllDir", '-cp', $classes, 'Launcher')
    } finally { $env:PATH = $savedPath }
}

# ---- corrida -----------------------------------------------------------------------
Write-Host "gate-windows: $(Get-Date -Format 's')  repo=$RepoRoot  work=$WorkDir"
if ($Only) { Write-Host "-Only $Only : PARCIAL, no cuenta como corrida completa." }

$stateBefore = Get-RepoState
$plan = @(
    @{ Name = 'pin';   Body = $stepPin },
    @{ Name = 'lint';  Body = $stepLint },
    @{ Name = 'suite'; Body = $stepSuite },
    @{ Name = 'dll';   Body = $stepDll },
    @{ Name = 'jvm';   Body = $stepJvm }
)
$ok = $true
foreach ($step in $plan) {
    if ($Only -and $Only -ne $step.Name) { continue }
    if ($step.Name -eq 'jvm' -and $SkipJvm) {
        Write-Host ""
        Write-Host "== jvm"
        Write-Host "SALTEADO por -SkipJvm (AC-061.5 NO verificado en esta corrida)."
        [void]$results.Add([pscustomobject]@{ Name = 'jvm'; Secs = 0; Rc = 'skip' })
        continue
    }
    if (-not (Invoke-Step $step.Name $step.Body)) { $ok = $false; break }
}

# El arbol, siempre (tambien tras un rojo: lo que un paso ensucio se nombra igual).
if (-not $Only -or $Only -eq 'repo') {
    $okRepo = Invoke-Step 'repo' {
        $after = Get-RepoState
        $diff = Compare-Object -ReferenceObject @($stateBefore) -DifferenceObject @($after)
        if ($diff) {
            $diff | ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
            throw "El gate cambio el arbol (git status --porcelain --ignored): '=>' es nuevo, '<=' desaparecio."
        }
        Write-Host "git status --porcelain --ignored: identico al del inicio."
    }
    if (-not $okRepo) { $ok = $false }
}

Write-Host ""
Write-Host "---- resumen ----"
foreach ($r in $results) { Write-Host ("{0,-6} {1,6:N0}s  rc={2}" -f $r.Name, $r.Secs, $r.Rc) }
if ($Only) { Write-Host "(parcial: -Only $Only)" }
if ($ok) { Write-Host "gate-windows: VERDE" ; exit 0 }
Write-Host "gate-windows: ROJO"
exit 1
