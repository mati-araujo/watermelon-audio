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

Uso:
    python3 scripts/check-jni-symbols.py              # el chequeo real
    python3 scripts/check-jni-symbols.py --self-test  # que sabe fallar
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

BRIDGE_KT = REPO / ("audio/src/androidMain/kotlin/com/watermellonstudios/audio"
                    "/internal/bridge/AudioNativeBridge.kt")

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

    if fallos:
        print(f"\n\033[31mself-test ROJO\033[0m — {len(fallos)} caso(s): {', '.join(fallos)}")
        return 1
    print("\n\033[32mself-test verde\033[0m — el chequeo sabe fallar.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="verifica que el chequeo sabe fallar; no mira el .so")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    try:
        return run_real_check()
    except CannotCheck as exc:
        print(f"\n\033[31mFALLA\033[0m — no pude chequear, así que NO doy verde:\n       {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
