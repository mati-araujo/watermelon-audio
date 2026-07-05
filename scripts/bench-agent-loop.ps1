<#
.SYNOPSIS
    Benchmark the agent edit -> build -> test feedback loop for the host C++
    test suite, so different build CONFIGURATIONS can be compared over time.

.DESCRIPTION
    Measures the loop a coding agent actually pays, repeatedly:

      cold_total   wipe build dir, then configure + build + ctest   (1 sample)
      incremental  touch one test .cpp, then build + run that suite  (-Runs samples)
      test_only    just ctest                                        (-Runs samples)

    Each invocation = ONE configuration (generator + jobs + stress). Results
    are printed and appended to docs/agent-loop-bench.csv with a -Label, so you
    re-run after a change and diff rows to see which config iterates faster.

    The googletest FetchContent cache (.deps) is kept warm between runs so the
    measurement isolates generator/parallelism, not network. Uses a dedicated
    .bench-build dir so it never disturbs your normal scripts/run-cpp-tests build.

.PARAMETER Generator   Ninja (default) | "Unix Makefiles" | "Visual Studio 17 2022"
.PARAMETER Jobs        Parallel build jobs. 0 = let the generator decide (default).
.PARAMETER Stress      Include the 5s ResizableRingBuffer stress test. Default: off.
.PARAMETER Runs        Samples for incremental/test_only (median reported). Default 5.
.PARAMETER Label       Free-text tag for the CSV row (e.g. "ninja-mingw-j8").

.EXAMPLE
    pwsh scripts/bench-agent-loop.ps1 -Label ninja-j8 -Jobs 8
    pwsh scripts/bench-agent-loop.ps1 -Generator "Unix Makefiles" -Label make-j4 -Jobs 4
    pwsh scripts/bench-agent-loop.ps1 -Stress -Label ninja-with-stress
#>
[CmdletBinding()]
param(
    [ValidateSet('Ninja', 'Unix Makefiles', 'Visual Studio 17 2022')]
    [string]$Generator = 'Ninja',
    [int]$Jobs = 0,
    [switch]$Stress,
    [int]$Runs = 5,
    [string]$Label = ''
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$SrcDir   = Join-Path $RepoRoot 'audio\src\main\cpp\tests'
$BuildDir = Join-Path $SrcDir '.bench-build'
$DepsDir  = Join-Path $SrcDir '.deps'
$CsvPath  = Join-Path $RepoRoot 'docs\agent-loop-bench.csv'
# Small leaf TU edited to simulate an agent's incremental change.
$TouchFile = Join-Path $SrcDir '..\dsp\tests\test_dsp_math.cpp'

# Stress test excluded unless -Stress (it is a 5s test that dwarfs the loop).
$StressRegex = 'ResizableRingBufferTest.StressWriterReaderAndRepeatedResize'

function Find-First { param([string[]]$Candidates, [string]$OnPath)
    if ($OnPath) { $c = Get-Command $OnPath -ErrorAction SilentlyContinue; if ($c) { return $c.Source } }
    foreach ($p in $Candidates) { if ($p -and (Test-Path $p)) { return $p } }
    return $null
}

$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME }
           elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT }
           else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }

$cmakeBin = Find-First -OnPath 'cmake' -Candidates @(
    'C:\Program Files\CMake\bin\cmake.exe', (Join-Path $sdkRoot 'cmake\3.22.1\bin\cmake.exe'))
if (-not $cmakeBin) { throw 'cmake not found.' }
$ctestBin = Join-Path (Split-Path -Parent $cmakeBin) 'ctest.exe'

# Toolchain + configure args per generator.
$configureArgs = @('-G', $Generator, "-DFETCHCONTENT_BASE_DIR=$DepsDir")
$compilerDesc = ''
$multiConfig = $false

if ($Generator -eq 'Visual Studio 17 2022') {
    $multiConfig = $true
    $configureArgs += @('-DCMAKE_BUILD_TYPE=Debug')
    $compilerDesc = 'msvc'
} else {
    $gxx = Find-First -OnPath 'g++' -Candidates @('C:\msys64\mingw64\bin\g++.exe', 'C:\mingw64\bin\g++.exe')
    if (-not $gxx) { throw 'g++ (MinGW) not found.' }
    $gcc = Join-Path (Split-Path -Parent $gxx) 'gcc.exe'
    $mingwBin = Split-Path -Parent $gxx
    $compilerDesc = "gcc-$((& $gxx -dumpversion).Trim())"
    $configureArgs += @('-DCMAKE_BUILD_TYPE=Debug', "-DCMAKE_CXX_COMPILER=$gxx", "-DCMAKE_C_COMPILER=$gcc")

    if ($Generator -eq 'Ninja') {
        $ninja = Find-First -OnPath 'ninja' -Candidates @((Join-Path $sdkRoot 'cmake\3.22.1\bin\ninja.exe'))
        if (-not $ninja) { throw 'ninja not found.' }
        $configureArgs += @("-DCMAKE_MAKE_PROGRAM=$ninja")
        $env:PATH = "$mingwBin;$(Split-Path -Parent $ninja);$env:PATH"
    } else { # Unix Makefiles
        $mk = Find-First -OnPath 'mingw32-make' -Candidates @((Join-Path $mingwBin 'mingw32-make.exe'))
        if (-not $mk) { throw 'mingw32-make not found.' }
        $configureArgs += @("-DCMAKE_MAKE_PROGRAM=$mk")
        $env:PATH = "$mingwBin;$env:PATH"
    }
}

$buildArgs = @('--build', $BuildDir)
if ($multiConfig) { $buildArgs += @('--config', 'Debug') }
if ($Jobs -gt 0)  { $buildArgs += @('-j', "$Jobs") }

$ctestBase = @('--test-dir', $BuildDir, '--output-on-failure')
if ($multiConfig) { $ctestBase += @('-C', 'Debug') }
if (-not $Stress) { $ctestBase += @('-E', $StressRegex) }

# --- timing helpers ------------------------------------------------------
$script:ec = 0
function Time-Once { param([scriptblock]$Block)
    $t = Measure-Command { & $Block }
    return $t.TotalSeconds
}
function Median { param([double[]]$xs)
    $s = $xs | Sort-Object; $n = $s.Count
    if ($n -eq 0) { return 0 }
    if ($n % 2) { return $s[[int](($n-1)/2)] }
    return ($s[$n/2 - 1] + $s[$n/2]) / 2.0
}

# Native exes (cmake/ctest) write progress to stderr; under -ErrorActionPreference
# Stop that surfaces as a terminating NativeCommandError. Run them with Continue
# and rely on $LASTEXITCODE instead.
# NB: parameter must NOT be named $Args — that shadows the automatic $args and
# the @-splat silently expands to empty, invoking the exe with no arguments.
function Run-Native { param([string]$Exe, [string[]]$CmdArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Exe @CmdArgs 2>&1 | Out-Null; $script:ec = $LASTEXITCODE }
    finally { $ErrorActionPreference = $prev }
}
function Run-Cmake { param([string[]]$CmdArgs) Run-Native $cmakeBin $CmdArgs }
function Run-Ctest { param([string[]]$CmdArgs) Run-Native $ctestBin $CmdArgs }

Write-Host "Generator : $Generator"
Write-Host "Compiler  : $compilerDesc"
Write-Host "Jobs      : $(if ($Jobs -gt 0) {$Jobs} else {'auto'})"
Write-Host "Stress    : $([bool]$Stress)"
Write-Host "Runs      : $Runs (cold = 1)"
Write-Host ""

# --- cold_total (wipe, configure, build, test) ---------------------------
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
$cold = Time-Once {
    Run-Cmake (@('-S', $SrcDir, '-B', $BuildDir) + $configureArgs)
    if ($script:ec) { throw "configure failed ($script:ec)" }
    Run-Cmake $buildArgs
    if ($script:ec) { throw "build failed ($script:ec)" }
    Run-Ctest $ctestBase
}
Write-Host ("cold_total   : {0,7:N2} s" -f $cold)

# --- incremental (touch one TU, incremental build + run its suite) -------
$incTimes = @()
$dspCtest = $ctestBase + @('-R', 'Dsp|BiquadFilter|ParameterSmoother|SoftClipper|DelayLine|Lfo|DcBlocker|LockFreeRingBuffer')
for ($i = 0; $i -lt $Runs; $i++) {
    (Get-Item $TouchFile).LastWriteTime = Get-Date
    $incTimes += Time-Once {
        Run-Cmake $buildArgs
        if ($script:ec) { throw "incremental build failed ($script:ec)" }
        Run-Ctest $dspCtest
    }
}
$incMed = Median $incTimes
Write-Host ("incremental  : {0,7:N2} s  (min {1:N2} / max {2:N2})" -f $incMed, ($incTimes | Measure-Object -Minimum).Minimum, ($incTimes | Measure-Object -Maximum).Maximum)

# --- test_only (ctest, no rebuild) ---------------------------------------
$testTimes = @()
for ($i = 0; $i -lt $Runs; $i++) { $testTimes += Time-Once { Run-Ctest $ctestBase } }
$testMed = Median $testTimes
Write-Host ("test_only    : {0,7:N2} s  (min {1:N2} / max {2:N2})" -f $testMed, ($testTimes | Measure-Object -Minimum).Minimum, ($testTimes | Measure-Object -Maximum).Maximum)

# --- append CSV ----------------------------------------------------------
$gitRev = (git -C $RepoRoot rev-parse --short HEAD 2>$null); if (-not $gitRev) { $gitRev = 'nogit' }
$ts = (Get-Date).ToString('s')
$jobsVal = if ($Jobs -gt 0) { $Jobs } else { 0 }
if (-not (Test-Path $CsvPath)) {
    'timestamp,git_rev,label,generator,compiler,jobs,stress,scenario,runs,median_s,min_s,max_s' | Out-File -FilePath $CsvPath -Encoding utf8
}
# Invariant decimal point so the comma-separated CSV is never corrupted by a
# locale that uses ',' as the decimal separator (e.g. es-AR).
function Fmt { param([double]$x) $x.ToString('0.000', [System.Globalization.CultureInfo]::InvariantCulture) }
function Add-Row { param([string]$Scenario, [double[]]$Times, [int]$N)
    $mn = ($Times | Measure-Object -Minimum).Minimum
    $mx = ($Times | Measure-Object -Maximum).Maximum
    $md = Median $Times
    "$ts,$gitRev,$Label,$Generator,$compilerDesc,$jobsVal,$([int][bool]$Stress),$Scenario,$N,$(Fmt $md),$(Fmt $mn),$(Fmt $mx)" |
        Out-File -FilePath $CsvPath -Append -Encoding utf8
}
Add-Row 'cold_total'  @($cold)    1
Add-Row 'incremental' $incTimes   $Runs
Add-Row 'test_only'   $testTimes  $Runs

Write-Host ""
Write-Host "Appended 3 rows to $CsvPath"
