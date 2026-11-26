<#
.SYNOPSIS
    Gate LOCAL de la PC Windows (MSYS2 UCRT64): pin de GCC, lint de portabilidad, suite de host,
    watermelon_audio.dll con sus exports/imports, y su carga desde una JVM.

.DESCRIPTION
    No atesta (.github/local-gate.json lo escribe gate.sh y nadie mas) y no baja ni ejecuta nada de la
    red: la suite corre con run-cpp-tests.ps1 -Offline (configure desconectado) y, si falta googletest
    en audio/src/main/cpp/tests/.deps, el paso suite falla ANTES de configurar diciendo como prepararlo.

    QUE ESCRIBE Y DONDE
      en el repo   la suite escribe en audio/src/main/cpp/tests/build y en
                   audio/src/main/cpp/tests/.deps/googletest-build (ambos ignorados por git). El chequeo
                   declara en $AllowedIgnoredWrites sólo tests/build: `.deps` está ignorado entero y
                   preexiste bajo -Offline, así que git lo colapsa en UNA entrada y lo que pase dentro de
                   él (googletest-build incluido) NO lo ve el chequeo. Cualquier otra cosa nueva es rojo.
      fuera        -WorkDir: build de la DLL (dll\), launcher JVM (jvm\), fotos del paso repo (snap\).

    El paso repo compara el arbol antes y despues:
      (a) lo versionado y lo no ignorado, POR CONTENIDO: `git status --porcelain=v1 --untracked-files=all`,
          el hash de `git diff HEAD --binary` y el hash de cada untracked no ignorado. Identico o rojo.
      (b) los paths ignorados NUEVOS (`git status --porcelain --ignored`, lineas `!!`): rojo y nombrados,
          salvo los declarados en $AllowedIgnoredWrites. Una escritura dentro de un dir ignorado que ya
          existia no se ve.
    -SelfTest prueba ese chequeo con mutantes sobre un repo descartable en -WorkDir (no toca este arbol).

    Pasos, en orden:
      pin     el GCC de MSYS2 UCRT64 es el fijado en .github/toolchain-pins.json (AC-061.6)
      lint    check-cpp-portability.sh --self-test y despues el lint
      suite   scripts/run-cpp-tests.ps1 (ctest con timeout por test + check-windows-not-run.py --results)
      dll     build Release de audio/src/main/cpp/desktop; check-jni-symbols.py --pe y --imports
      jvm     System.loadLibrary + nativeIsEngineInitialized() == false desde una JVM (AC-061.5)
      repo    el arbol al final == al principio (ver arriba)

    Antes de lanzar cualquier hijo, SetErrorMode(FAILCRITICALERRORS | NOGPFAULTERRORBOX |
    NOOPENFILEERRORBOX): se hereda a los hijos y un crash/abort() de un test vuelve solo, sin dialogo
    de WER. (ctest ademas fija su propio modo; medido en la etapa, los dos caminos vuelven en < 1 s.)

.PARAMETER Only
    Corre un solo paso (pin, lint, suite, dll, jvm, repo) para iterar. NO cuenta como corrida completa.

.PARAMETER SkipJvm
    Salta el paso jvm. La corrida es PARCIAL (como -Only): el loadLibrary de AC-061.5 no se verifico y el
    resumen y la linea final lo dicen.

.PARAMETER SelfTest
    Prueba el chequeo del paso repo con mutantes en un repo descartable bajo -WorkDir y sale.

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
    [switch]$SelfTest,
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

# Unico permiso de escritura en el repo: el build dir ignorado de la suite (relativo a la raiz, con /).
# `.deps` no va: preexiste ignorado y git lo colapsa en una entrada, un permiso para su interior nunca aplicaria.
$AllowedIgnoredWrites = @('audio/src/main/cpp/tests/build')

function Invoke-Git {
    param([Parameter(Mandatory)][string]$Root, [string[]]$GitArgs)
    $out = & git -C $Root @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') fallo (rc=$LASTEXITCODE)" }
    $out
}

function Get-TreeSnapshot {
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Scratch)
    New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
    $status = @(Invoke-Git $Root @('status', '--porcelain=v1', '--untracked-files=all'))
    $diffFile = Join-Path $Scratch 'tree.diff'
    Invoke-Git $Root @('diff', 'HEAD', '--binary', "--output=$diffFile") | Out-Null
    $diffHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $diffFile).Hash
    Remove-Item -Force $diffFile
    $untracked = New-Object System.Collections.ArrayList
    # PS 5.1 decodifica la salida de un exe con [Console]::OutputEncoding (OEM): un `diseño.md` llegaria roto
    # y Get-FileHash no lo encontraria. git emite UTF-8 con -z (sin comillas).
    $encBefore = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
        $raw = (& git -C $Root ls-files --others --exclude-standard -z | Out-String)
        $lsRc = $LASTEXITCODE
    } finally { [Console]::OutputEncoding = $encBefore }
    if ($lsRc -ne 0) { throw "git ls-files fallo (rc=$lsRc)" }
    foreach ($f in @(($raw -split "`0") | ForEach-Object { $_.Trim("`r", "`n") } | Where-Object { $_ })) {
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Root $f)).Hash
        [void]$untracked.Add("$h  $f")
    }
    $ignored = @(Invoke-Git $Root @('status', '--porcelain', '--ignored') |
        Where-Object { $_ -like '!! *' } | ForEach-Object { $_.Substring(3).Trim('"').TrimEnd('/') })
    [pscustomobject]@{ Status = $status; DiffHash = $diffHash; Untracked = @($untracked); Ignored = $ignored }
}

# Los problemas entre dos fotos (vacio == identico salvo lo permitido).
function Compare-TreeSnapshot {
    param($Before, $After, [string[]]$Allowed)
    $problems = New-Object System.Collections.ArrayList
    foreach ($d in @(Compare-Object -ReferenceObject @($Before.Status) -DifferenceObject @($After.Status))) {
        [void]$problems.Add("git status: $($d.SideIndicator) $($d.InputObject)")
    }
    if ($Before.DiffHash -ne $After.DiffHash) {
        [void]$problems.Add("git diff HEAD --binary cambio de contenido (sha256 $($Before.DiffHash) -> $($After.DiffHash))")
    }
    foreach ($d in @(Compare-Object -ReferenceObject @($Before.Untracked) -DifferenceObject @($After.Untracked))) {
        [void]$problems.Add("untracked: $($d.SideIndicator) $($d.InputObject)")
    }
    foreach ($p in @($After.Ignored | Where-Object { @($Before.Ignored) -notcontains $_ })) {
        $ok = $false
        foreach ($a in $Allowed) { if ($p -eq $a -or $p.StartsWith("$a/")) { $ok = $true } }
        if (-not $ok) { [void]$problems.Add("ignorado NUEVO no declarado: $p") }
    }
    $problems.ToArray()
}

function Invoke-RepoSelfTest {
    $root = Join-Path $WorkDir 'selftest-repo'
    $scratch = Join-Path $WorkDir 'selftest-scratch'
    if (Test-Path $root) { Remove-Item -Recurse -Force $root }
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    foreach ($g in @(@('init', '-q'), @('config', 'user.email', 'selftest@example.invalid'),
                     @('config', 'user.name', 'selftest'), @('config', 'core.autocrlf', 'false'))) {
        Invoke-Git $root $g | Out-Null
    }
    Set-Content -Path (Join-Path $root '.gitignore') -Value "build/`nout/" -Encoding ASCII
    Set-Content -Path (Join-Path $root 'tracked.txt') -Value 'a' -Encoding ASCII
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'sub') | Out-Null
    Invoke-Git $root @('add', '-A') | Out-Null
    Invoke-Git $root @('commit', '-q', '-m', 'base') | Out-Null
    # Estado inicial "sucio": un versionado ya modificado y un untracked.
    # [char]0xF1 y no el literal: PS 5.1 lee este archivo (sin BOM) como ANSI.
    $nonAscii = "dise$([char]0xF1)o.md"
    $dirty = {
        Invoke-Git $root @('checkout', '-q', '--', 'tracked.txt') | Out-Null
        Add-Content -Path (Join-Path $root 'tracked.txt') -Value 'dirty'
        Set-Content -Path (Join-Path $root 'untracked.txt') -Value 'u' -Encoding ASCII
        Set-Content -Path (Join-Path $root $nonAscii) -Value 'd' -Encoding ASCII
        foreach ($rm in 'build', 'out', 'sub\nuevo.txt') {
            $pth = Join-Path $root $rm
            if (Test-Path $pth) { Remove-Item -Recurse -Force $pth }
        }
    }
    & $dirty
    $allowed = @('build')
    $fail = New-Object System.Collections.ArrayList
    $cases = @(
        @{ Name = 'sin cambios'; Want = $false; Do = { } },
        @{ Name = 'build dir declarado, nuevo'; Want = $false; Do = {
            New-Item -ItemType Directory -Force -Path (Join-Path $root 'build') | Out-Null
            Set-Content -Path (Join-Path $root 'build\x.o') -Value 'x' } },
        @{ Name = 'versionado ya modificado, mas contenido'; Want = $true; Do = {
            Add-Content -Path (Join-Path $root 'tracked.txt') -Value 'mas' } },
        @{ Name = 'untracked con otro contenido'; Want = $true; Do = {
            Set-Content -Path (Join-Path $root 'untracked.txt') -Value 'otro' -Encoding ASCII } },
        @{ Name = 'untracked no ASCII, sin cambios'; Want = $false; Do = { } },
        @{ Name = 'untracked no ASCII, otro contenido'; Want = $true; Do = {
            Set-Content -Path (Join-Path $root $nonAscii) -Value 'otro' -Encoding ASCII } },
        @{ Name = 'untracked nuevo'; Want = $true; Do = {
            Set-Content -Path (Join-Path $root 'sub\nuevo.txt') -Value 'n' -Encoding ASCII } },
        @{ Name = 'ignorado nuevo no declarado'; Want = $true; Do = {
            New-Item -ItemType Directory -Force -Path (Join-Path $root 'out') | Out-Null
            Set-Content -Path (Join-Path $root 'out\y') -Value 'y' } }
    )
    foreach ($c in $cases) {
        $before = Get-TreeSnapshot $root $scratch
        & $c.Do
        $after = Get-TreeSnapshot $root $scratch
        $probs = @(Compare-TreeSnapshot $before $after $allowed)
        $red = $probs.Count -gt 0
        $mark = 'ok   '
        if ($red -ne $c.Want) { $mark = 'FALLA'; [void]$fail.Add($c.Name) }
        Write-Host ("  {0} {1,-42} rojo={2}  {3}" -f $mark, $c.Name, $red, ($probs -join ' | '))
        & $dirty
    }
    Remove-Item -Recurse -Force $root, $scratch -ErrorAction SilentlyContinue
    if ($fail.Count) { throw "self-test del chequeo del repo: fallaron $($fail -join ', ')" }
    Write-Host "self-test del chequeo del repo: ok"
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
        (Join-Path $PSScriptRoot 'run-cpp-tests.ps1'), '-Offline', '-TimeoutSec', $TestTimeoutSec)
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
        '-DWMA_DESKTOP_WERROR=ON',
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
if ($SelfTest) { Invoke-RepoSelfTest; exit 0 }
if ($Only) { Write-Host "-Only $Only : PARCIAL, no cuenta como corrida completa." }
if ($SkipJvm) { Write-Host "-SkipJvm : PARCIAL, no cuenta como corrida completa; el loadLibrary (AC-061.5) NO se verifica." }
$partial = [bool]$Only -or $SkipJvm

$snapDir = Join-Path $WorkDir 'snap'
$stateBefore = Get-TreeSnapshot $RepoRoot $snapDir
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
        $after = Get-TreeSnapshot $RepoRoot $snapDir
        $problems = @(Compare-TreeSnapshot $stateBefore $after $AllowedIgnoredWrites)
        if ($problems.Count) {
            $problems | ForEach-Object { Write-Host "  $_" }
            throw "El gate cambio el arbol ('=>' nuevo, '<=' desaparecio); solo se permite escribir en: $($AllowedIgnoredWrites -join ', ')."
        }
        Write-Host "arbol identico por contenido al del inicio (permitido escribir: $($AllowedIgnoredWrites -join ', '))."
    }
    if (-not $okRepo) { $ok = $false }
}

Write-Host ""
Write-Host "---- resumen ----"
foreach ($r in $results) { Write-Host ("{0,-6} {1,6:N0}s  rc={2}" -f $r.Name, $r.Secs, $r.Rc) }
if ($Only) { Write-Host "(parcial: -Only $Only)" }
if ($SkipJvm) { Write-Host "(parcial: -SkipJvm, el loadLibrary de AC-061.5 NO se verifico)" }
if ($ok -and $partial) { Write-Host "gate-windows: VERDE PARCIAL (no cuenta como corrida completa)" ; exit 0 }
if ($ok) { Write-Host "gate-windows: VERDE" ; exit 0 }
Write-Host "gate-windows: ROJO"
exit 1
