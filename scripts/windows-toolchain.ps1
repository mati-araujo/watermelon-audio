# Toolchain de host de la PC Windows: cmake, ninja y el GCC de MSYS2 UCRT64 FIJADO.
# Se carga con dot-source desde scripts/run-cpp-tests.ps1 y scripts/gate-windows.ps1 (una sola
# logica; el pin vive solo en .github/toolchain-pins.json, scripts/check-dep-pins.py lo vigila).
# No hay Write-Host ni efectos: devuelve un objeto y deja que el llamador arme el PATH.

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

function Get-WmaHostToolchain {
    param([Parameter(Mandatory)][string]$RepoRoot)

    # ---- cmake / ninja ----------------------------------------------------
    $sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME }
               elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT }
               else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }

    $cmakeBin = Find-First -OnPath 'cmake' -Candidates @(
        'C:\Program Files\CMake\bin\cmake.exe',
        (Join-Path $sdkRoot 'cmake\3.22.1\bin\cmake.exe')
    )
    if (-not $cmakeBin) { throw "cmake not found. Install CMake or the Android SDK 'cmake;3.22.1' package." }

    $ninjaBin = Find-First -OnPath 'ninja' -Candidates @(
        (Join-Path $sdkRoot 'cmake\3.22.1\bin\ninja.exe'),
        'C:\msys64\ucrt64\bin\ninja.exe'
    )
    if (-not $ninjaBin) { throw "ninja not found. It ships with the Android SDK 'cmake;3.22.1' package." }

    # ---- g++ / gcc: the pinned MSYS2 UCRT64 -------------------------------
    $pinsFile = Join-Path $RepoRoot '.github\toolchain-pins.json'
    if (-not (Test-Path $pinsFile)) { throw "$pinsFile not found: cannot read the pinned MSYS2 UCRT64 toolchain." }
    $msys2 = (Get-Content -Raw -Encoding UTF8 $pinsFile | ConvertFrom-Json)._windows_cross.msys2_ucrt64
    if (-not $msys2 -or -not $msys2.root -or -not $msys2.gcc) {
        throw "$pinsFile has no _windows_cross.msys2_ucrt64 {root, gcc}."
    }
    $gxx = Join-Path $msys2.root 'bin\g++.exe'
    if (-not (Test-Path $gxx)) {
        throw "MSYS2 UCRT64 g++ not found at $gxx. Install it from an MSYS2 shell: 'pacman -S mingw-w64-ucrt-x86_64-gcc' (package $($msys2.package) $($msys2.package_version)). mingw64 is NOT an accepted fallback: it is a different C runtime."
    }
    $gccVersion = (& $gxx -dumpfullversion).Trim()
    if ($gccVersion -ne $msys2.gcc) {
        throw "$gxx is GCC '$gccVersion' but .github/toolchain-pins.json pins '$($msys2.gcc)' (package $($msys2.package) $($msys2.package_version)). Install that version, or change the pin deliberately."
    }
    $mingwBin = Split-Path -Parent $gxx

    [pscustomobject]@{
        Cmake      = $cmakeBin
        Ctest      = Join-Path (Split-Path -Parent $cmakeBin) 'ctest.exe'
        Ninja      = $ninjaBin
        Gxx        = $gxx
        Gcc        = Join-Path $mingwBin 'gcc.exe'
        Objdump    = Join-Path $mingwBin 'objdump.exe'
        MingwBin   = $mingwBin
        GccVersion = $gccVersion
        PinsFile   = $pinsFile
    }
}
