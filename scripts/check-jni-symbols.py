#!/usr/bin/env python3
"""MINI-001 — toda `external fun` del bridge tiene su símbolo JNI en el `.so`.

POR QUÉ ESTE CHEQUEO
--------------------
Un nombre que no coincide entre el `external fun nativeFoo(...)` de
`AudioNativeBridge.kt` y el `Java_..._AudioNativeBridge_nativeFoo` del C++
**compila limpio de los dos lados**. Kotlin no verifica que el símbolo exista y
el C++ no sabe quién lo declara. La falla aparece recién en runtime, en el
device, como `UnsatisfiedLinkError`, y con suerte en la función que nadie probó.

`scripts/c-api-gap.py` NO cubre esto: compara la C-API contra la superficie JNI
por conjuntos de tokens, o sea "¿existe un wma_* que haga esto?". La pregunta de
acá es otra y es literal: "¿el símbolo que Kotlin va a buscar está exportado?".

LA PROPIEDAD QUE DEFINE ESTE SCRIPT
-----------------------------------
**Nunca pasar cuando no pudo chequear.** Un gate que no encuentra el `.so`, que
no encuentra `llvm-nm`, o cuyo regex dejó de matchear y lee cero declaraciones,
se ve desde afuera exactamente igual que uno que chequeó y salió limpio: `ok 1s`.
Esa es la forma en que mueren los gates, así que las tres degradaciones son
FALLO explícito, no un pase silencioso ni un skip.

LA DLL DE WINDOWS (REQ-061 AC-061.5)
-----------------------------------
`watermelon_audio.dll` (MinGW UCRT64, `-static`) tiene dos chequeos propios, los dos
sobre `objdump -p` y los dos con la misma regla de arriba (no pudo leer = FALLO):

  * `--pe DLL`: los exports de la tabla de nombres son EXACTAMENTE las `external fun`
    del bridge menos lo que `WMA_JNI_USB=0` recorta (las regiones `#if WMA_JNI_USB`
    de `jni_audio_bridge.cpp` y los archivos `jni_usb.cpp` / `jni_benchmark.cpp`, que
    desktop no compila), mas `JNI_OnLoad`/`JNI_OnUnload`. Falla en las DOS
    direcciones: falta uno, o sobra uno (un `wma_*` de la C API, un simbolo de
    libstdc++). Un export decorado (`_nombre`, `nombre@N`) es rojo y se nombra: no se
    normaliza en silencio. Se mide la tabla que escribio el linker, asi que un
    `--export-all-symbols` o un `.def` no se aceptan: se ve lo que exportaron de mas.
  * `--imports DLL`: toda `DLL Name:` es `KERNEL32.dll` o `api-ms-win-crt-*.dll`
    (case-insensitive) y nada mas. `libwinpthread-1.dll`, `libstdc++-6.dll`,
    `libgcc_s_seh-1.dll`, `ucrtbase.dll`, `ADVAPI32.dll`... son rojo y se nombran.
    Cero imports es rojo.

Ambos exigen que el archivo sea un PE por su magic (`MZ` ... `PE\\0\\0`), no por la
extension. `--objdump X` (o la variable `OBJDUMP`; default `objdump`) elige el
binutils: el de UCRT64 en Windows, `x86_64-w64-mingw32ucrt-objdump` en el contenedor.

Uso:
    python3 scripts/check-jni-symbols.py              # el chequeo real (.so de Android)
    python3 scripts/check-jni-symbols.py --self-test  # que sabe fallar
    python3 scripts/check-jni-symbols.py --pe DLL [--objdump X]
    python3 scripts/check-jni-symbols.py --imports DLL [--objdump X]
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jni_bridge_source import BRIDGE_KT_REL  # noqa: E402 — el source set, en un solo lugar

BRIDGE_KT = REPO / BRIDGE_KT_REL

# Una sola ABI a propósito: las cuatro salen del mismo CMake con las mismas
# fuentes, así que leer las cuatro cuadruplica el costo sin agregar una pregunta.
SO_GLOB = ("audio/build/intermediates/stripped_native_libs/*/*/out/lib/"
           "arm64-v8a/libwatermelon_audio.so")

JNI_PREFIX = "Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_"

DECL_RE = re.compile(r"\bexternal\s+fun\s+(\w+)\s*\(")
SYM_RE = re.compile(re.escape(JNI_PREFIX) + r"(\w+)")


class CannotCheck(Exception):
    """No se pudo chequear. NO es un pase: es un fallo con causa."""


def find_so() -> Path:
    matches = sorted(REPO.glob(SO_GLOB))
    if not matches:
        raise CannotCheck(
            f"no encontré el .so en {SO_GLOB}.\n"
            "       Corré primero: ./gradlew :audio:assembleRelease"
        )
    # El más nuevo, por si quedaron variantes viejas de builds anteriores.
    return max(matches, key=lambda p: p.stat().st_mtime)


def find_nm() -> str:
    """`llvm-nm` del NDK, sin la versión escrita en ningún lado (AC-4)."""
    sdk = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if not sdk:
        # local.properties es la fuente que usa el propio build de este repo.
        props = REPO / "local.properties"
        if props.exists():
            for line in props.read_text(encoding="utf-8").splitlines():
                if line.startswith("sdk.dir="):
                    sdk = line.split("=", 1)[1].strip()
                    break
    if sdk:
        # Cualquier versión de NDK y cualquier host: se ordena y se toma la última.
        candidates = sorted(Path(sdk).glob("ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-nm"))
        if candidates:
            return str(candidates[-1])

    # Un llvm-nm del sistema sirve igual: lo único que se le pide es -D.
    for fallback in ("llvm-nm", "nm"):
        try:
            subprocess.run([fallback, "--version"], capture_output=True, check=True)
            return fallback
        except (OSError, subprocess.CalledProcessError):
            continue

    raise CannotCheck(
        "no encontré llvm-nm. Buscado en el NDK (ANDROID_HOME / sdk.dir de\n"
        "       local.properties) y en el PATH."
    )


def declared_functions(text: str) -> list[str]:
    return DECL_RE.findall(text)


def exported_symbols(nm: str, so: Path) -> set[str]:
    try:
        out = subprocess.run([nm, "-D", str(so)], capture_output=True, text=True,
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise CannotCheck(f"{nm} -D falló sobre {so}: {exc}") from exc
    return set(SYM_RE.findall(out))


def compare(declared: list[str], exported: set[str]) -> list[str]:
    """Las declaradas sin símbolo. Lanza si no hay nada que comparar (AC-3)."""
    if not declared:
        raise CannotCheck(
            f"leí CERO `external fun` en {BRIDGE_KT.name}.\n"
            "       O el archivo se movió, o el regex dejó de matchear. En\n"
            "       cualquier caso esto no chequeó nada y no puede dar verde."
        )
    if not exported:
        raise CannotCheck(
            "leí CERO símbolos JNI del .so.\n"
            "       O el prefijo de la clase cambió, o el .so no es el que creo."
        )
    return [d for d in declared if d not in exported]


# ---------------------------------------------------------------------------
# Windows: la DLL (PE). Todo sale de `objdump -p`.
# ---------------------------------------------------------------------------
JNI_DIR = REPO / "audio/src/main/cpp/jni"
# Archivos que desktop/CMakeLists.txt NO compila: sus JNIEXPORT no estan en la DLL.
NOT_IN_DESKTOP = ("jni_usb.cpp", "jni_benchmark.cpp")
# Los exporta jni_engine.cpp, sin el prefijo del bridge.
LIFECYCLE_EXPORTS = ("JNI_OnLoad", "JNI_OnUnload")

EXPORT_HEADER_RE = re.compile(r"\[Ordinal/Name Pointer\] Table")
# `\t[   0] +base[   1]  0000 Java_...`
EXPORT_ROW_RE = re.compile(r"^\s*\[\s*\d+\]\s+\+base\[\s*\d+\]\s+[0-9a-fA-F]+\s+(\S+)\s*$")
DLL_NAME_RE = re.compile(r"^\s*DLL Name:\s*(\S+)\s*$")
DECORATION_RE = re.compile(r"^_|@\d+$")
ALLOWED_IMPORTS = ("kernel32.dll", "api-ms-win-crt-*.dll")


def is_pe(path: Path) -> bool:
    """PE por magic (`MZ`, e_lfanew en 0x3C, `PE\\0\\0`), no por la extension."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x40)
            if len(head) < 0x40 or head[:2] != b"MZ":
                return False
            f.seek(int.from_bytes(head[0x3C:0x40], "little"))
            return f.read(4) == b"PE\0\0"
    except OSError as exc:
        raise CannotCheck(f"no pude leer {path}: {exc}") from exc


def require_pe(path: Path) -> None:
    if not path.is_file():
        raise CannotCheck(f"no existe {path}")
    if not is_pe(path):
        raise CannotCheck(f"{path} no es un PE (no tiene el magic MZ ... PE\\0\\0): "
                          "no entra por la rama de Windows.")


def run_objdump(objdump: str, dll: Path) -> str:
    try:
        res = subprocess.run([objdump, "-p", str(dll)], capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        err = getattr(exc, "stderr", None) or b""
        raise CannotCheck(f"`{objdump} -p {dll}` fallo: {exc} {err.decode(errors='replace').strip()}") from exc
    return res.stdout.decode("utf-8", errors="replace")


def parse_pe_exports(text: str) -> list[str]:
    """Nombres de la `[Ordinal/Name Pointer] Table`. Sin tabla o vacia es FALLO."""
    lines = text.splitlines()  # tolera CRLF
    start = next((i for i, l in enumerate(lines) if EXPORT_HEADER_RE.search(l)), None)
    if start is None:
        raise CannotCheck("la salida de objdump -p no tiene `[Ordinal/Name Pointer] Table`:\n"
                          "       la DLL no exporta nada, o objdump no es el que creo.")
    names: list[str] = []
    for line in lines[start + 1:]:
        if not line.strip():
            if names:
                break
            continue
        if line.strip().startswith("Ordinal") and line.strip().endswith("Name"):
            continue
        m = EXPORT_ROW_RE.match(line)
        if not m:
            raise CannotCheck(f"fila de exports que no entiendo (export sin nombre?): {line.strip()!r}")
        names.append(m.group(1))
    if not names:
        raise CannotCheck("la tabla de exports esta vacia: no chequeo nada y no doy verde.")
    return names


def usb_region_text(cpp: str) -> str:
    """El texto de las regiones `#if WMA_JNI_USB ... #endif` (con #if anidados)."""
    out: list[str] = []
    depth = 0  # 0 = fuera de la region; >0 = profundidad de #if dentro de ella
    for line in cpp.splitlines():
        stripped = line.lstrip()
        if depth == 0:
            if re.match(r"#\s*if\s+WMA_JNI_USB\b", stripped):
                depth = 1
            continue
        if re.match(r"#\s*if", stripped):
            depth += 1
        elif re.match(r"#\s*endif", stripped):
            depth -= 1
            continue
        out.append(line)
    return "\n".join(out)


def expected_pe_exports(kt: str, bridge_cpp: str, outside: list[str]) -> set[str]:
    """external fun del bridge - lo que desktop no compila (+ JNI_OnLoad/OnUnload)."""
    declared = set(declared_functions(kt))
    cut = set(SYM_RE.findall(usb_region_text(bridge_cpp)))
    for text in outside:
        cut |= set(SYM_RE.findall(text))
    if not declared:
        raise CannotCheck(f"leí CERO `external fun` en {BRIDGE_KT.name}.")
    if not cut:
        raise CannotCheck("no encontre ningun simbolo USB que recortar en las fuentes: el regex "
                          "o los archivos cambiaron, y sin eso el conjunto esperado miente.")
    return {JNI_PREFIX + n for n in declared - cut} | set(LIFECYCLE_EXPORTS)


def check_pe_exports(names: list[str], expected: set[str]) -> list[str]:
    """Lineas de error (vacio = OK)."""
    seen: set[str] = set()
    dup = {n for n in names if n in seen or seen.add(n)}
    got = set(names)
    errs = [f"falta el export {n}" for n in sorted(expected - got)]
    for n in sorted((got - expected) | dup):
        if n in dup:
            errs.append(f"export repetido {n}")
        elif DECORATION_RE.search(n):
            errs.append(f"sobra el export DECORADO {n} (x64 no decora: _prefijo / @N)")
        else:
            errs.append(f"sobra el export {n}")
    return errs


def parse_pe_imports(text: str) -> list[str]:
    dlls = [m.group(1) for l in text.splitlines() if (m := DLL_NAME_RE.match(l))]
    if not dlls:
        raise CannotCheck("la salida de objdump -p no tiene ninguna `DLL Name:`: una DLL sin imports "
                          "ni de KERNEL32 no es la que creo.")
    return dlls


def disallowed_imports(dlls: list[str]) -> list[str]:
    return sorted({d for d in dlls
                   if not any(fnmatch.fnmatchcase(d.lower(), pat) for pat in ALLOWED_IMPORTS)},
                  key=str.lower)


def _report(titulo: str, errs: list[str], ok_msg: str, why: str) -> int:
    if errs:
        print(f"\n\033[31mFALLA\033[0m — {titulo}: {len(errs)} problema(s). {why}\n")
        for e in errs:
            print(f"          - {e}")
        return 1
    print(f"\n\033[32mok\033[0m — {ok_msg}")
    return 0


def run_pe_check(dll: Path, objdump: str) -> int:
    print("REQ-061 — exports de la DLL vs external fun del bridge\n")
    require_pe(dll)
    if not BRIDGE_KT.exists():
        raise CannotCheck(f"no existe {BRIDGE_KT}")
    rd = lambda n: (JNI_DIR / n).read_text(encoding="utf-8")  # noqa: E731
    expected = expected_pe_exports(BRIDGE_KT.read_text(encoding="utf-8"), rd("jni_audio_bridge.cpp"),
                                   [rd(n) for n in NOT_IN_DESKTOP])
    actual = parse_pe_exports(run_objdump(objdump, dll))
    print(f"  dll        {dll}")
    print(f"  esperados  {len(expected)}")
    print(f"  exportados {len(actual)}")
    return _report("exports de la DLL", check_pe_exports(actual, expected),
                   "los exports son exactamente los esperados.",
                   "Falta = UnsatisfiedLinkError en runtime; sobra = superficie que nadie decidio.")


def run_imports_check(dll: Path, objdump: str) -> int:
    print("REQ-061 — imports de la DLL (solo KERNEL32 y api-ms-win-crt-*)\n")
    require_pe(dll)
    dlls = parse_pe_imports(run_objdump(objdump, dll))
    print(f"  dll       {dll}")
    print(f"  imports   {', '.join(sorted(set(dlls), key=str.lower))}")
    return _report("imports de la DLL", [f"importa {d}" for d in disallowed_imports(dlls)],
                   "la DLL solo depende de KERNEL32 y de la UCRT.",
                   "Tiene que correr en un Windows limpio, sin el runtime de MinGW al lado.")


def run_real_check() -> int:
    print("MINI-001 — símbolos JNI declarados vs exportados\n")
    if not BRIDGE_KT.exists():
        raise CannotCheck(f"no existe {BRIDGE_KT}")

    so = find_so()
    nm = find_nm()
    declared = declared_functions(BRIDGE_KT.read_text(encoding="utf-8"))
    exported = exported_symbols(nm, so)
    missing = compare(declared, exported)

    print(f"  .so         {so.relative_to(REPO)}")
    print(f"  declaradas  {len(declared)}")
    print(f"  exportadas  {len(exported)}")

    if missing:
        print(f"\n\033[31mFALLA\033[0m — {len(missing)} `external fun` sin símbolo JNI.")
        print("        Compila de los dos lados y revienta en runtime, en el device,")
        print("        con UnsatisfiedLinkError. Falta el Java_..._<nombre> en C++:\n")
        for m in missing:
            print(f"          - {m}")
        return 1

    print("\n\033[32mok\033[0m — cada `external fun` tiene su símbolo exportado.")
    return 0


# ---------------------------------------------------------------------------
# Self-test: que el chequeo SABE FALLAR.
#
# Sin esto, "pasa" no significa nada: un comparador que devuelve siempre lista
# vacía da verde para siempre y se ve idéntico a uno que funciona. Cubre las
# tres degradaciones del criterio de muerte, no sólo el caso feliz.
# ---------------------------------------------------------------------------
def _fake_objdump(exports: list[str] | None, imports: list[str] | None, crlf: bool = False) -> str:
    """`objdump -p` sintetico con el formato real de binutils (tabs incluidos)."""
    out = ["", "watermelon_audio.dll:     file format pei-x86-64", ""]
    if exports is not None:
        out += ["The Export Tables (interpreted .edata section contents)", "",
                "[Ordinal/Name Pointer] Table -- Ordinal Base 1", "\t          Ordinal   Hint Name"]
        out += [f"\t[{i:4}] +base[{i + 1:4}]  {i:04x} {n}" for i, n in enumerate(exports)]
        out += [""]
    if imports is not None:
        out += ["The Import Tables (interpreted .idata section contents)", ""]
        for d in imports:
            out += [f"\tDLL Name: {d}", "\tvma:     Ordinal  Hint  Member-Name  Bound-To",
                    "\t00034340  <none>  0097  CloseHandle", ""]
    return ("\r\n" if crlf else "\n").join(out)


def _build_min_dll(tmp: Path, static: bool) -> tuple[Path, str] | str:
    """DLL minima de 2 JNIEXPORT con un toolchain MinGW. (dll, objdump) o el motivo de omision."""
    src = tmp / "min.cpp"
    src.write_text('#include <string>\n#define JNIEXPORT extern "C" __declspec(dllexport)\n'
                   'JNIEXPORT int Java_a_b_nativeOne(void*, void*) { return (int)std::string("x").size(); }\n'
                   'JNIEXPORT void Java_a_b_nativeTwo(void*, void*) {}\n'
                   'void helper() {}\n', encoding="utf-8")
    ucrt = Path("C:/msys64/ucrt64/bin")
    candidates = [("x86_64-w64-mingw32ucrt-g++", "x86_64-w64-mingw32ucrt-objdump"),
                  (str(ucrt / "g++.exe"), str(ucrt / "objdump.exe")),
                  ("g++", "objdump")]
    for cxx, objdump in candidates:
        if not (shutil.which(cxx) and shutil.which(objdump)):
            continue
        dll = tmp / f"min_{'static' if static else 'dyn'}.dll"
        cmd = [cxx, "-shared", *(["-static"] if static else []), "-o", str(dll), str(src)]
        # cc1plus/as del toolchain de MSYS2 buscan sus DLL en el PATH.
        env = {**os.environ, "PATH": str(Path(shutil.which(cxx)).parent) + os.pathsep + os.environ.get("PATH", "")}
        if subprocess.run(cmd, capture_output=True, env=env).returncode == 0 and dll.is_file() and is_pe(dll):
            return dll, objdump
    return "ningun g++ que produzca PE (MinGW UCRT) en PATH ni en C:/msys64/ucrt64/bin"


def pe_self_test(check, levanta) -> None:  # noqa: ANN001
    """Casos de la rama PE (exports e imports). Fixtures de texto + una DLL real si hay MinGW."""
    exp = {JNI_PREFIX + "nativeAlpha", JNI_PREFIX + "nativeBeta", "JNI_OnLoad", "JNI_OnUnload"}
    base = sorted(exp)

    def exports(names: list[str], crlf: bool = False) -> list[str]:
        return check_pe_exports(parse_pe_exports(_fake_objdump(names, [], crlf)), exp)

    print("\n  -- PE: exports")
    check("PE exports: iguales -> OK", exports(base) == [])
    check("PE exports: tolera CRLF de objdump", exports(base, crlf=True) == [])
    check("PE exports: falta uno -> rojo y lo nombra",
          exports(base[1:]) == [f"falta el export {base[0]}"], str(exports(base[1:])))
    e = exports(base + ["wma_engine_create"])
    check("PE exports: sobra un wma_* de la C API -> rojo y lo nombra",
          e == ["sobra el export wma_engine_create"], str(e))
    e = exports(base + ["_ZNSt6vectorIiSaIiEE9push_backERKi"])
    check("PE exports: sobra un simbolo de libstdc++ -> rojo",
          len(e) == 1 and "_ZNSt6vectorIiSaIiEE9push_backERKi" in e[0], str(e))
    e = exports([n for n in base if n != "JNI_OnLoad"] + ["_JNI_OnLoad"])
    check("PE exports: decorado (_Nombre) -> rojo, NO normalizado en silencio",
          any("falta el export JNI_OnLoad" in x for x in e) and any("DECORADO _JNI_OnLoad" in x for x in e), str(e))
    e = exports([n for n in base if n != "JNI_OnLoad"] + ["JNI_OnLoad@12"])
    check("PE exports: decorado (Nombre@N) -> rojo", any("DECORADO JNI_OnLoad@12" in x for x in e), str(e))
    check("PE exports: nombre repetido -> rojo", any("repetido" in x for x in exports(base + [base[0]])))
    check("PE exports: sin tabla de exports NO es un pase",
          levanta(lambda: parse_pe_exports(_fake_objdump(None, ["KERNEL32.dll"]))))
    check("PE exports: tabla vacia NO es un pase",
          levanta(lambda: parse_pe_exports(_fake_objdump([], ["KERNEL32.dll"]))))
    check("PE exports: fila sin nombre (solo ordinal) NO es un pase",
          levanta(lambda: parse_pe_exports(
              "[Ordinal/Name Pointer] Table -- Ordinal Base 1\n\t[   0] +base[   1]  0000\n")))

    print("\n  -- PE: fuentes del conjunto esperado")
    cpp = ("#if WMA_JNI_USB\n#if X\n" + JNI_PREFIX + "nativeUsbA(\n#endif\n" + JNI_PREFIX + "nativeUsbB(\n"
           "#endif  // WMA_JNI_USB\n" + JNI_PREFIX + "nativeAlpha(\n")
    check("PE fuentes: la region USB incluye lo anidado y no lo de afuera",
          set(SYM_RE.findall(usb_region_text(cpp))) == {"nativeUsbA", "nativeUsbB"})
    kt = "external fun nativeAlpha()\nexternal fun nativeUsbA()\nexternal fun nativeUsbB()\nexternal fun nativeBench()\n"
    got = expected_pe_exports(kt, cpp, [JNI_PREFIX + "nativeBench("])
    check("PE fuentes: esperado = external fun - USB - archivos fuera de desktop + OnLoad/OnUnload",
          got == {JNI_PREFIX + "nativeAlpha", "JNI_OnLoad", "JNI_OnUnload"}, str(got))
    check("PE fuentes: nada que recortar NO es un pase (regex muerto)",
          levanta(lambda: expected_pe_exports(kt, "int x;", [])))
    real = expected_pe_exports(BRIDGE_KT.read_text(encoding="utf-8"),
                               (JNI_DIR / "jni_audio_bridge.cpp").read_text(encoding="utf-8"),
                               [(JNI_DIR / n).read_text(encoding="utf-8") for n in NOT_IN_DESKTOP])
    check("PE fuentes: el repo real recorta USB y deja el resto",
          JNI_PREFIX + "nativeInitializeUsbDevice" not in real and JNI_PREFIX + "nativeLooperIsTrackSendToFx" in real
          and "JNI_OnLoad" in real and "JNI_OnUnload" in real)

    print("\n  -- PE: magic")
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "x.so").write_bytes(b"\x7fELF" + bytes(100))
        (tmp / "fake.dll").write_bytes(b"MZ" + bytes(0x3A) + (0x40).to_bytes(4, "little") + b"PE\0\0")
        (tmp / "trunc.dll").write_bytes(b"MZ")
        check("PE magic: un ELF no es PE y no entra por la rama PE",
              not is_pe(tmp / "x.so") and levanta(lambda: require_pe(tmp / "x.so")))
        check("PE magic: MZ + PE\\0\\0 es PE aunque la extension sea .dll o no", is_pe(tmp / "fake.dll"))
        check("PE magic: truncado no es PE", not is_pe(tmp / "trunc.dll"))
        check("PE magic: el script mismo (texto) no es PE", not is_pe(Path(__file__)))
        check("PE magic: archivo inexistente es FALLO",
              levanta(lambda: require_pe(tmp / "no-existe.dll")))
        check("objdump que no existe es FALLO, no un pase",
              levanta(lambda: run_objdump("objdump-que-no-existe-xyz", tmp / "fake.dll")))

    print("\n  -- PE: imports")
    ok_imps = ["KERNEL32.dll", "api-ms-win-crt-runtime-l1-1-0.dll", "api-ms-win-crt-stdio-l1-1-0.dll"]

    def bad(imps: list[str]) -> list[str]:
        return disallowed_imports(parse_pe_imports(_fake_objdump(["x"], imps)))

    check("imports: solo permitidas -> OK", bad(ok_imps) == [])
    check("imports: case-insensitive (kernel32.DLL, API-MS-WIN-CRT-x.dll)",
          bad(["kernel32.DLL", "API-MS-WIN-CRT-x-l1-1-0.DLL"]) == [])
    check("imports: libwinpthread-1.dll -> rojo y la nombra",
          bad(ok_imps + ["libwinpthread-1.dll"]) == ["libwinpthread-1.dll"])
    check("imports: libstdc++-6.dll y libgcc_s_seh-1.dll -> rojo",
          bad(ok_imps + ["libstdc++-6.dll", "libgcc_s_seh-1.dll"]) == ["libgcc_s_seh-1.dll", "libstdc++-6.dll"])
    check("imports: ADVAPI32.dll -> rojo", bad(ok_imps + ["ADVAPI32.dll"]) == ["ADVAPI32.dll"])
    check("imports: ucrtbase.dll -> rojo", bad(ok_imps + ["ucrtbase.dll"]) == ["ucrtbase.dll"])
    check("imports: api-ms-win-core-* (no crt) -> rojo",
          bad(ok_imps + ["api-ms-win-core-synch-l1-2-0.dll"]) == ["api-ms-win-core-synch-l1-2-0.dll"])
    check("imports: prefijo parecido (api-ms-win-crtx.dll, kernel32x.dll) -> rojo",
          bad(["api-ms-win-crtx.dll", "kernel32x.dll"]) == ["api-ms-win-crtx.dll", "kernel32x.dll"])
    check("imports: sin ninguna DLL Name NO es un pase", levanta(lambda: parse_pe_imports(_fake_objdump(["x"], []))))
    check("imports: salida vacia NO es un pase", levanta(lambda: parse_pe_imports("")))
    check("imports: tolera CRLF",
          disallowed_imports(parse_pe_imports(_fake_objdump(["x"], ok_imps, crlf=True))) == [])

    print("\n  -- PE: DLL real (MinGW)")
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        st = _build_min_dll(tmp, static=True)
        if isinstance(st, str):
            print(f"  omitido: {st}")
            return
        dll, objdump = st
        two = {"Java_a_b_nativeOne", "Java_a_b_nativeTwo"}
        names = parse_pe_exports(run_objdump(objdump, dll))
        check("DLL real: 2 JNIEXPORT contra lista de 2 -> OK (el helper no exportado no aparece)",
              check_pe_exports(names, two) == [], str(names))
        check("DLL real: contra lista de 3 -> falta",
              check_pe_exports(names, two | {"Java_a_b_nativeThree"}) == ["falta el export Java_a_b_nativeThree"])
        check("DLL real: contra lista de 1 -> sobra",
              check_pe_exports(names, {"Java_a_b_nativeOne"}) == ["sobra el export Java_a_b_nativeTwo"])
        imps = parse_pe_imports(run_objdump(objdump, dll))
        check("DLL real -static: solo KERNEL32 + api-ms-win-crt-*", disallowed_imports(imps) == [],
              f"importa {sorted(set(imps))}")
        dyn = _build_min_dll(tmp, static=False)
        if isinstance(dyn, str):
            print(f"  omitido (sin -static): {dyn}")
            return
        dimps = parse_pe_imports(run_objdump(dyn[1], dyn[0]))
        check("DLL real SIN -static: el runtime de MinGW aparece y es rojo", disallowed_imports(dimps) != [],
              f"importa {sorted(set(dimps))}")


def self_test() -> int:
    fallos: list[str] = []

    def check(nombre: str, cond: bool, detalle: str = "") -> None:
        if cond:
            print(f"  \033[32mok\033[0m   {nombre}")
        else:
            print(f"  \033[31mFALLA\033[0m {nombre} {detalle}")
            fallos.append(nombre)

    # --- que sepa LEER lo que tiene que leer
    kt = """
        private external fun nativeAlpha(a: Int)
        private external fun nativeBeta(): Boolean
        external fun nativeGamma(x: Float, y: Float)
        fun noEsExternal(z: Int) {}
    """
    decl = declared_functions(kt)
    check("lee las external fun y sólo esas", decl == ["nativeAlpha", "nativeBeta", "nativeGamma"],
          f"leyó {decl}")

    # --- que sepa FALLAR: es el punto del self-test
    check("detecta la que falta",
          compare(["nativeAlpha", "nativeBeta"], {"nativeAlpha"}) == ["nativeBeta"])
    check("no inventa faltantes cuando están todas",
          compare(["nativeAlpha"], {"nativeAlpha", "nativeDeMas"}) == [])

    # --- las TRES degradaciones del criterio de muerte, todas como FALLO
    def levanta(fn) -> bool:
        try:
            fn()
            return False
        except CannotCheck:
            return True

    check("cero declaraciones NO es un pase", levanta(lambda: compare([], {"nativeAlpha"})))
    check("cero símbolos NO es un pase", levanta(lambda: compare(["nativeAlpha"], set())))
    check("las dos en cero NO son un pase", levanta(lambda: compare([], set())))

    # --- el regex del símbolo, contra una línea real de nm
    linea = ("0000000000093740 T " + JNI_PREFIX + "nativeLooperIsTrackSendToFx\n"
             "0000000000012340 T Java_de_otra_clase_nativeAjena\n")
    syms = set(SYM_RE.findall(linea))
    check("extrae el nombre del símbolo y no toma los de otra clase",
          syms == {"nativeLooperIsTrackSendToFx"}, f"extrajo {syms}")

    pe_self_test(check, levanta)

    if fallos:
        print(f"\n\033[31mself-test ROJO\033[0m — {len(fallos)} caso(s): {', '.join(fallos)}")
        return 1
    print("\n\033[32mself-test verde\033[0m — el chequeo sabe fallar.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="verifica que el chequeo sabe fallar; no mira el .so ni la DLL")
    ap.add_argument("--pe", metavar="DLL", help="exports de la DLL == los esperados (objdump -p)")
    ap.add_argument("--imports", metavar="DLL", help="imports de la DLL: solo KERNEL32 y api-ms-win-crt-*")
    ap.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"),
                    help="objdump a usar con --pe/--imports (default: $OBJDUMP o `objdump`)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    try:
        if args.pe or args.imports:
            rc = 0
            if args.pe:
                rc |= run_pe_check(Path(args.pe), args.objdump)
            if args.imports:
                rc |= run_imports_check(Path(args.imports), args.objdump)
            return rc
        return run_real_check()
    except CannotCheck as exc:
        print(f"\n\033[31mFALLA\033[0m — no pude chequear, así que NO doy verde:\n       {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
