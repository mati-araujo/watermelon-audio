#!/usr/bin/env python3
"""REQ-025 — la FIRMA de cada cruce JNI coincide de los dos lados.

`CLAUDE.md` declara este defecto desde MINI-001 y hasta hoy nada lo verificaba:

    un `Int` declarado donde el C++ espera `jlong` compila de los dos lados,
    linkea, pasa ese gate y corrompe memoria en el device.

Los otros dos guardrails del cruce contestan OTRA pregunta:

  * `check-jni-symbols.py` (MINI-001) — "¿el símbolo EXISTE en el .so?". Compara
    sólo NOMBRES: da verde con las 309 firmas mal declaradas.
  * el arnés de `androidUnitTest` (REQ-016+) — "¿alguien lo EJECUTA?". Sí ve la
    firma, pero sólo de las que ejerce, que son una fracción.

🔴 **REQ-024 intentó comprar esta garantía DE A UNA FUNCIÓN** —un `--add-opens`
permanente más reflexión sobre un campo privado del JDK, por `loadSoundFontFromFd`—
y MINI-015 lo revirtió. Este script la compra de una sola vez, para todas.

## Es source-only, y eso es parte del diseño

No necesita `.so`, ni NDK, ni `llvm-nm`: lee el Kotlin y el C++ del árbol. Corre en
centésimas en cualquier máquina, así que puede ir en `gate.sh` sin costo. La
verificación contra el binario ya la hace `check-jni-symbols.py`, que es otra
pregunta y sí necesita build.

## Lo que NO verifica, a propósito

La **semántica** de lo que cruza —que el pinneo de un array se libere, que el largo
sea el correcto, que un `null` se maneje, que no quede una excepción pendiente— no
se ve en la firma. Eso lo compra el arnés EJECUTANDO, y sigue siendo su razón de
existir: este gate no lo reemplaza, lo libera de tener que probar firmas.

Uso:
    python3 scripts/check-jni-signatures.py            # el lint
    python3 scripts/check-jni-signatures.py --self-test  # ¿puede fallar?
"""

from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
JNI_DIR = REPO / "audio/src/main/cpp/jni"
BRIDGE_KT = REPO / (
    "audio/src/androidMain/kotlin/com/watermellonstudios/audio"
    "/internal/bridge/AudioNativeBridge.kt"
)
CLASS_PREFIX = "Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_"

# El equivalente JNI de cada tipo de Kotlin. Cualquier otro tipo —un listener, una
# interfaz— cruza como `jobject`, y eso TAMBIÉN se verifica: declarar un objeto donde
# el C++ espera un `jstring` es un desajuste real.
KT_TO_JNI = {
    "Int": "jint",
    "Long": "jlong",
    "Float": "jfloat",
    "Double": "jdouble",
    "Boolean": "jboolean",
    "Byte": "jbyte",
    "Short": "jshort",
    "String": "jstring",
    "IntArray": "jintArray",
    "LongArray": "jlongArray",
    "FloatArray": "jfloatArray",
    "DoubleArray": "jdoubleArray",
    "ByteArray": "jbyteArray",
    "ShortArray": "jshortArray",
    "BooleanArray": "jbooleanArray",
    "Array<String>": "jobjectArray",
    "Unit": "void",
}

PROTOTYPE = re.compile(
    r"JNIEXPORT\s+(?P<ret>\w+)\s+JNICALL\s*\n?\s*"
    + re.escape(CLASS_PREFIX)
    + r"(?P<name>\w+)\s*\((?P<params>[^)]*)\)"
)
# Sólo cuenta las que ABREN un bloque de definición, para que la guarda de
# completitud no acuse a un prototipo declarado aparte.
EXPORT_LINE = re.compile(r"^JNIEXPORT\b", re.MULTILINE)
EXTERNAL_FUN = re.compile(
    r"external fun (?P<name>native\w+)"
    r"\((?P<params>(?:[^()]|\([^()]*\))*)\)"
    r"\s*(?::\s*(?P<ret>[\w<>?]+))?"
)


def jni_type(kotlin_type: str) -> str:
    """El tipo JNI de un tipo de Kotlin. Nullable (`String?`) no cambia el ancho."""
    return KT_TO_JNI.get(kotlin_type.strip().rstrip("?"), "jobject")


def split_top_level(text: str) -> list[str]:
    """Parte por comas de primer nivel: `Array<String>` no se puede cortar al medio."""
    out: list[str] = []
    depth = 0
    current = ""
    for ch in text:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(current)
            current = ""
        else:
            current += ch
    if current.strip():
        out.append(current)
    return out


def kotlin_param_types(params: str) -> list[str]:
    """Los tipos de `(a: Int, b: FloatArray)`, sin los nombres."""
    flat = re.sub(r"\s+", " ", params).strip()
    if not flat:
        return []
    return [p.split(":", 1)[1].strip() for p in split_top_level(flat) if ":" in p]


def cpp_param_types(params: str) -> list[str]:
    """Los tipos REALES de un prototipo JNI, sin `JNIEnv*` ni el `thiz`.

    Los dos primeros se descartan **por posición**, no por nombre: filtrar por
    `"jobject thiz"` se rompe en silencio el día que alguien lo llame distinto, y un
    parámetro de más se leería como desajuste de aridad en una función sana.
    """
    raw = [re.sub(r"\s+", " ", p).strip() for p in params.split(",")]
    if len(raw) < 2:
        return raw
    return [re.sub(r"\s+\w+$", "", p) for p in raw[2:]]


def parse_cpp(sources: dict[str, str]) -> tuple[dict[str, tuple[str, list[str]]], int]:
    """Prototipos por nombre, y cuántas `JNIEXPORT` de esta clase hay en el texto.

    El segundo número alimenta la guarda de completitud (AC-025.4).
    """
    protos: dict[str, tuple[str, list[str]]] = {}
    declared = 0
    for text in sources.values():
        for line in EXPORT_LINE.finditer(text):
            tail = text[line.start() : line.start() + 400]
            if CLASS_PREFIX in tail:
                declared += 1
        for m in PROTOTYPE.finditer(text):
            protos[m.group("name")] = (m.group("ret"), cpp_param_types(m.group("params")))
    return protos, declared


def parse_kotlin(text: str) -> dict[str, tuple[str, list[str]]]:
    return {
        m.group("name"): (m.group("ret") or "Unit", kotlin_param_types(m.group("params")))
        for m in EXTERNAL_FUN.finditer(text)
    }


def compare(
    protos: dict[str, tuple[str, list[str]]],
    declared_exports: int,
    decls: dict[str, tuple[str, list[str]]],
) -> list[str]:
    """Todos los desajustes. Lista vacía = verde."""
    problems: list[str] = []

    # AC-025.4 — la guarda de completitud va PRIMERO. Un prototipo con forma
    # inesperada que quede sin revisar en silencio deja el lint verde revisando
    # menos, que es el modo de falla ya pagado dos veces acá (rt-coverage-baseline).
    if len(protos) != declared_exports:
        problems.append(
            f"COMPLETITUD: el parser abarcó {len(protos)} prototipos pero el árbol declara "
            f"{declared_exports} JNIEXPORT de {CLASS_PREFIX[:-1]}.\n"
            "  Alguno tiene una forma que el regex no reconoce, y quedaría SIN REVISAR en "
            "silencio.\n"
            "  El arreglo es enseñarle la forma nueva al parser, NO bajar la exigencia."
        )

    if not protos or not decls:
        problems.append(
            "el parser leyó CERO de un lado. Eso no es 'nada que revisar': es un parseo roto, "
            "y publicarlo como verde sería inventar la verificación."
        )
        return problems

    for name in sorted(decls):
        if name not in protos:
            problems.append(f"{name}: declarada en Kotlin y sin prototipo en {JNI_DIR.name}/")
            continue
        kt_ret, kt_params = decls[name]
        cpp_ret, cpp_params = protos[name]

        if len(kt_params) != len(cpp_params):
            problems.append(
                f"{name}: ARIDAD — Kotlin declara {len(kt_params)} parámetro(s) y el C++ "
                f"toma {len(cpp_params)}.\n"
                f"  kotlin: {kt_params}\n  c++   : {cpp_params}"
            )
            continue

        for i, (kt_t, cpp_t) in enumerate(zip(kt_params, cpp_params)):
            if jni_type(kt_t) != cpp_t:
                problems.append(
                    f"{name}: parámetro {i} — Kotlin `{kt_t}` cruza como `{jni_type(kt_t)}` "
                    f"y el C++ espera `{cpp_t}`."
                )

        if jni_type(kt_ret) != cpp_ret:
            problems.append(
                f"{name}: RETORNO — Kotlin `{kt_ret}` cruza como `{jni_type(kt_ret)}` "
                f"y el C++ devuelve `{cpp_ret}`."
            )

    return problems


def read_tree() -> tuple[dict[str, str], str]:
    sources = {
        p: Path(p).read_text(encoding="utf-8", errors="replace")
        for p in sorted(glob.glob(str(JNI_DIR / "*.cpp")))
    }
    if not sources:
        sys.exit(f"FALLA — no encontré fuentes .cpp en {JNI_DIR}. Sin ellas no hay nada que comparar.")
    if not BRIDGE_KT.is_file():
        sys.exit(f"FALLA — no encontré {BRIDGE_KT}.")
    return sources, BRIDGE_KT.read_text(encoding="utf-8")


# ============================== self-test ==============================
#
# Corre ANTES del lint en gate.sh y en el CI, por la misma razón que
# check-rt-safety: si el parser se rompe, un lint sin self-test queda verde
# para siempre y nadie se entera.


def self_test() -> int:
    sources, kt = read_tree()
    protos, declared = parse_cpp(sources)
    decls = parse_kotlin(kt)
    fallos = []

    def check(nombre, ok, detalle=""):
        print(f"  {'ok  ' if ok else 'FALLA'}  {nombre}{'' if ok else ' — ' + detalle}")
        if not ok:
            fallos.append(nombre)

    print("self-test de check-jni-signatures:")

    check("el árbol real sale verde",
          not compare(protos, declared, decls),
          str(compare(protos, declared, decls)[:2]))
    check("parseó algo de los dos lados", bool(protos) and bool(decls),
          f"protos={len(protos)} decls={len(decls)}")

    # AC-025.1 — ancho: el defecto exacto que nombra CLAUDE.md.
    victima = next(n for n, (_, ps) in decls.items() if "Long" in ps)
    mutado = dict(decls)
    r, ps = mutado[victima]
    mutado[victima] = (r, ["Int" if p == "Long" else p for p in ps])
    check("mata un `Int` donde el C++ espera `jlong` (AC-025.1)",
          any("parámetro" in p for p in compare(protos, declared, mutado)))

    # AC-025.2 — aridad.
    victima = next(n for n, (_, ps) in decls.items() if len(ps) >= 2)
    mutado = dict(decls)
    r, ps = mutado[victima]
    mutado[victima] = (r, ps[:-1])
    check("mata un parámetro de menos (AC-025.2)",
          any("ARIDAD" in p for p in compare(protos, declared, mutado)))

    # AC-025.3 — retorno.
    victima = next(n for n, (ret, _) in decls.items() if ret != "Unit")
    mutado = dict(decls)
    _, ps = mutado[victima]
    mutado[victima] = ("Unit", ps)
    check("mata un retorno cambiado (AC-025.3)",
          any("RETORNO" in p for p in compare(protos, declared, mutado)))

    # AC-025.1 (variante array) — el tipo de array equivocado.
    victima = next((n for n, (_, ps) in decls.items() if "IntArray" in ps), None)
    if victima:
        mutado = dict(decls)
        r, ps = mutado[victima]
        mutado[victima] = (r, ["FloatArray" if p == "IntArray" else p for p in ps])
        check("mata un `FloatArray` donde el C++ espera `jintArray`",
              any("parámetro" in p for p in compare(protos, declared, mutado)))

    # AC-025.4 — la guarda de completitud.
    recortado = dict(list(protos.items())[:-1])
    check("mata un parser que abarcó de menos (AC-025.4)",
          any("COMPLETITUD" in p for p in compare(recortado, declared, decls)))

    # Un parseo vacío es FALLA, nunca "nada que revisar".
    check("un parseo vacío es falla, no un pase",
          bool(compare({}, 0, {})))

    if fallos:
        print(f"\n\033[31mSELF-TEST ROJO\033[0m — {len(fallos)}: {', '.join(fallos)}")
        return 1
    print("\n\033[32mself-test ok\033[0m — el lint puede fallar.")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()

    sources, kt = read_tree()
    protos, declared = parse_cpp(sources)
    decls = parse_kotlin(kt)
    problems = compare(protos, declared, decls)

    if problems:
        print(f"\033[31mFALLA\033[0m — {len(problems)} desajuste(s) de firma en el cruce JNI.\n")
        for p in problems:
            print(f"  {p}")
        print(
            "\nUna firma mal declarada compila de los dos lados, linkea y PASA "
            "check-jni-symbols.py\n(que compara sólo nombres). El síntoma aparece en el "
            "device, como memoria corrompida."
        )
        return 1

    print(
        f"\033[32mok\033[0m — {len(decls)} `external fun` con firma idéntica a su prototipo "
        f"C++ (aridad, anchos y retorno)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
