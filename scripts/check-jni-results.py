#!/usr/bin/env python3
"""REQ-045 AC-045.9 — ninguna `JNIEXPORT` descarta un `WmaResult`.

La clase que REQ-045 vino a borrar es **un fallo que no cruza la frontera**: el
motor dice que no y el consumidor lee un exito. La forma JNI de esa clase es
exactamente una linea:

    wma_engine_start(g_wmaEngine, fadeTimeMs);   // <- el WmaResult se cae al piso

Compila, linkea, pasa `check-jni-symbols.py` (que compara NOMBRES), pasa
`check-jni-signatures.py` (que compara ANCHOS) y le devuelve
`Result.success(Unit)` a NoisyPad mientras el stream no abrio. Al 2026-09-28 eso
pasaba en **6** cruces, los seis del ciclo de vida.

## La pregunta que contesta, y las que no

  * `check-jni-symbols.py` (MINI-001) — "?el simbolo EXISTE en el .so?"
  * `check-jni-signatures.py` (REQ-025) — "?la FIRMA coincide de los dos lados?"
  * el arnes de `androidUnitTest` — "?alguien lo EJECUTA?"
  * **este** — "?el resultado que la C API devolvio LLEGA a algun lado?"

Ninguno de los cuatro cubre al otro. Una firma perfecta sobre un resultado tirado
es justo el defecto D1 del analisis de las cartas de NoisyPad.

## Que cuenta como descartado

Una llamada a una `wma_*` que el header declara devolviendo `WmaResult`, escrita
como **sentencia suelta**: la linea empieza con el nombre de la funcion y termina
en `;`. Si el valor se asigna, se compara, se devuelve, se castea o entra a otra
expresion, no es un descarte.

Excepcion unica y con razon obligatoria, en la linea de arriba o al final de la
misma:

    // RESULT-DISCARD-OK: el contrato de esta entrada es best-effort, ver ...

## La guarda de completitud es lo que lo sostiene

Si el parser abarca menos `JNIEXPORT` de las que el arbol declara, **falla**. Un
cuerpo con forma inesperada que quede sin revisar en silencio deja el lint verde
revisando menos — el modo de falla que este repo ya pago dos veces con
`rt-coverage-baseline`. La guarda es la de S0 de este REQ: cuenta contra
`grep -c JNIEXPORT`, incluidos `JNI_OnLoad`/`JNI_OnUnload`, que no llevan
`JNICALL`.

Nace en CERO y **no tiene baseline**: los 6 de hoy los paga S1.

Uso:
    python3 scripts/check-jni-results.py              # el lint
    python3 scripts/check-jni-results.py --self-test  # ?puede fallar?
"""

from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
JNI_DIR = REPO / "audio/src/main/cpp/jni"
HEADER = REPO / "audio/src/main/cpp/api/watermelon_audio.h"

ALLOW = "RESULT-DISCARD-OK"

# Las `wma_*` que devuelven WmaResult, leidas del header y no listadas a mano: una
# lista escrita a mano envejece en silencio y el lint queda verde revisando menos.
RESULT_FN = re.compile(r"WMA_API\s+WmaResult\s+(wma_\w+)\s*\(")

# Toda JNIEXPORT, con o sin JNICALL: `JNI_OnLoad` no lo lleva. El denominador de la
# guarda de completitud.
EXPORT_LINE = re.compile(r"^JNIEXPORT\b", re.MULTILINE)
EXPORT_DEF = re.compile(r"JNIEXPORT\s+(?P<ret>[\w:*&<> ]+?)\s+(?:JNICALL\s+)?(?P<name>\w+)\s*\(")

# Una sentencia suelta: la linea abre con el nombre de la wma_* (nada delante salvo
# blancos) y lo que sigue al parentesis de cierre es `;`. Si el valor se asigna, se
# compara o se devuelve, hay algo delante y esto no matchea.
STATEMENT = re.compile(r"^[ \t]*(wma_\w+)\s*\(", re.MULTILINE)


def result_functions(header_text: str) -> set[str]:
    return set(RESULT_FN.findall(header_text))


def function_bodies(src: str) -> list[tuple[str, str, int]]:
    """(nombre, cuerpo, offset del cuerpo) de cada `JNIEXPORT`, por balanceo de llaves.

    El offset y no la linea porque dos entradas pueden tener cuerpos IDENTICOS —pasa
    con los pares get/set— y buscar el texto del cuerpo con `str.index` le atribuiria
    a la segunda la linea de la primera.

    Se balancean llaves en vez de buscar el `}` de la columna 0 porque un cuerpo con
    un lambda o un bloque anidado rompe la version ingenua — y romperlo en silencio
    es justo lo que la guarda de completitud existe para delatar.
    """
    out: list[tuple[str, str, int]] = []
    for m in EXPORT_DEF.finditer(src):
        brace = src.find("{", m.end())
        if brace < 0:
            continue
        # Un `;` antes del `{` significa que esto era un PROTOTIPO y el `{` que se
        # encontro es el del cuerpo de la funcion SIGUIENTE. Sin esta guarda el
        # parser contaria un cuerpo que no le pertenece y la guarda de completitud
        # se anularia sola, que es el modo de falla que existe para delatar.
        if ";" in src[m.end() : brace]:
            continue
        depth = 0
        i = brace
        while i < len(src):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out.append((m.group("name"), src[brace : i + 1], brace))
    return out


def discards(path: str, src: str, result_fns: set[str]) -> list[str]:
    """Los descartes de UN archivo, con archivo:linea y el nombre de la entrada."""
    lines = src.splitlines()
    found: list[str] = []
    for name, body, body_offset in function_bodies(src):
        for m in STATEMENT.finditer(body):
            call = m.group(1)
            if call not in result_fns:
                continue
            # El `;` tiene que cerrar la sentencia: si el valor se usa, el texto entre
            # el parentesis de cierre y el `;` no esta vacio (o no hay `;` en la linea).
            close = matching_paren(body, m.end() - 1)
            if close < 0 or body[close + 1 :].lstrip()[:1] != ";":
                continue
            lineno = src.count("\n", 0, body_offset + m.start()) + 1
            if excused(lines, lineno):
                continue
            corto = name.rsplit("_", 1)[-1]
            found.append(f"{Path(path).name}:{lineno}  {corto} descarta el WmaResult de {call}()")
    return found


def matching_paren(text: str, open_idx: int) -> int:
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def excused(lines: list[str], lineno: int) -> bool:
    """`// RESULT-DISCARD-OK: razon` en la misma linea o en la de arriba, CON razon.

    Sin razon no excusa: una excepcion sin motivo escrito es la excepcion que nadie
    puede revisar, y este repo ya lo exige en `mechanism-callers-baseline.txt`.
    """
    for idx in (lineno - 1, lineno - 2):
        if 0 <= idx < len(lines) and ALLOW in lines[idx]:
            razon = lines[idx].split(ALLOW, 1)[1].lstrip(": \t")
            if razon.strip():
                return True
    return False


def read_tree() -> tuple[dict[str, str], str]:
    sources = {
        p: Path(p).read_text(encoding="utf-8", errors="replace")
        for p in sorted(glob.glob(str(JNI_DIR / "*.cpp")))
    }
    if not sources:
        sys.exit(f"FALLA — no encontre fuentes .cpp en {JNI_DIR}. Sin ellas no hay nada que revisar.")
    if not HEADER.is_file():
        sys.exit(f"FALLA — no encontre {HEADER}. Sin el header no se sabe quien devuelve WmaResult.")
    return sources, HEADER.read_text(encoding="utf-8")


def analyze(sources: dict[str, str], header_text: str) -> list[str]:
    """Todos los problemas. Lista vacia = verde. La completitud va PRIMERO."""
    problems: list[str] = []

    result_fns = result_functions(header_text)
    if not result_fns:
        problems.append(
            "el parser no encontro UNA sola `wma_*` que devuelva WmaResult en el header. "
            "Eso no es 'nada que revisar': es un parseo roto, y publicarlo como verde "
            "seria inventar la verificacion."
        )
        return problems

    declared = parsed = 0
    for src in sources.values():
        declared += len(EXPORT_LINE.findall(src))
        parsed += len(function_bodies(src))
    if parsed != declared:
        problems.append(
            f"COMPLETITUD: el parser abarco {parsed} cuerpos de JNIEXPORT y el arbol declara "
            f"{declared}.\n"
            "  Alguno tiene una forma que el parser no reconoce, y su cuerpo quedaria SIN "
            "REVISAR en silencio.\n"
            "  El arreglo es ensenarle la forma nueva al parser, NO bajar la exigencia."
        )
    if parsed == 0:
        problems.append("el parser no abarco un solo cuerpo de JNIEXPORT: el parseo se rompio.")
        return problems

    for path, src in sources.items():
        problems += discards(path, src, result_fns)
    return problems


# ============================== self-test ==============================
#
# Corre ANTES del lint en gate.sh y en el CI, por la misma razon que
# check-rt-safety y check-jni-signatures: si el parser se rompe, un lint sin
# self-test queda verde para siempre y nadie se entera.


def self_test() -> int:
    sources, header = read_tree()
    fallos: list[str] = []

    def check(nombre: str, ok: bool, detalle: str = "") -> None:
        print(f"  {'ok  ' if ok else 'FALLA'}  {nombre}{'' if ok else ' — ' + detalle}")
        if not ok:
            fallos.append(nombre)

    print("self-test de check-jni-results:")

    real = analyze(sources, header)
    check("el arbol real sale verde", not real, str(real[:3]))

    # Un archivo de juguete es el sujeto de los mutantes: asi el self-test no depende
    # de que tal funcion siga existiendo en el arbol.
    VICTIMA = """
JNIEXPORT void JNICALL
Java_x_nativeArranca(JNIEnv* env, jobject thiz, jint fade) {
    wma_engine_start(g_wmaEngine, fade);
}
"""
    SANA = """
JNIEXPORT jint JNICALL
Java_x_nativeArranca(JNIEnv* env, jobject thiz, jint fade) {
    return static_cast<jint>(wma_engine_start(g_wmaEngine, fade));
}
"""
    hdr = "WMA_API WmaResult wma_engine_start(WmaEngine* engine, int fade_time_ms);\n"

    check(
        "mata una sentencia suelta que descarta el WmaResult (AC-045.9)",
        any("descarta el WmaResult" in p for p in analyze({"v.cpp": VICTIMA}, hdr)),
    )
    check(
        "NO acusa a la misma llamada cuando el valor se devuelve",
        not analyze({"v.cpp": SANA}, hdr),
        str(analyze({"v.cpp": SANA}, hdr)),
    )
    for forma, texto in (
        ("asignado", "    WmaResult r = wma_engine_start(g_wmaEngine, fade);\n    (void)r;"),
        ("comparado", "    if (wma_engine_start(g_wmaEngine, fade) != WMA_OK) { return; }"),
        ("casteado", "    return wma_engine_start(g_wmaEngine, fade) == WMA_OK ? 1 : 0;"),
    ):
        cuerpo = VICTIMA.replace("    wma_engine_start(g_wmaEngine, fade);", texto)
        check(f"NO acusa un resultado {forma}", not analyze({"v.cpp": cuerpo}, hdr))

    # La excepcion, y su exigencia de razon.
    con_razon = VICTIMA.replace(
        "    wma_engine_start",
        "    // RESULT-DISCARD-OK: esta entrada es best-effort por contrato\n    wma_engine_start",
    )
    sin_razon = VICTIMA.replace(
        "    wma_engine_start", "    // RESULT-DISCARD-OK:\n    wma_engine_start"
    )
    check("la excepcion CON razon excusa", not analyze({"v.cpp": con_razon}, hdr))
    check(
        "la excepcion SIN razon NO excusa",
        any("descarta el WmaResult" in p for p in analyze({"v.cpp": sin_razon}, hdr)),
    )

    # Una wma_* que NO devuelve WmaResult no es un descarte.
    check(
        "NO acusa a una wma_* que devuelve void",
        not analyze({"v.cpp": VICTIMA.replace("wma_engine_start", "wma_set_master_volume")}, hdr),
    )

    # La guarda de completitud: un cuerpo sin `{` la baja.
    roto = "JNIEXPORT void JNICALL Java_x_nativeDeclaradaAparte(JNIEnv*, jobject);\n" + VICTIMA
    check(
        "mata un parser que abarco de menos (completitud)",
        any("COMPLETITUD" in p for p in analyze({"v.cpp": roto}, hdr)),
    )

    # Un header sin WmaResult es FALLA, nunca "nada que revisar".
    check(
        "un header sin WmaResult es falla, no un pase",
        bool(analyze({"v.cpp": VICTIMA}, "// nada\n")),
    )

    if fallos:
        print(f"\n\033[31mSELF-TEST ROJO\033[0m — {len(fallos)}: {', '.join(fallos)}")
        return 1
    print("\n\033[32mself-test ok\033[0m — el lint puede fallar.")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()

    sources, header = read_tree()
    problems = analyze(sources, header)

    if problems:
        print(f"\033[31mFALLA\033[0m — {len(problems)} resultado(s) descartado(s) en el cruce JNI.\n")
        for p in problems:
            print(f"  {p}")
        print(
            "\nUn WmaResult tirado al piso le devuelve `Result.success` al consumidor con el "
            "motor\ndiciendo que no (D1 de las cartas de NoisyPad). El arreglo es PROPAGARLO: "
            "la entrada\ndevuelve `jint` y Kotlin lo mapea a `Result.failure`. Si de verdad es "
            "best-effort,\n`// RESULT-DISCARD-OK: razon` — con la razon escrita."
        )
        return 1

    n = len(result_functions(header))
    print(
        f"\033[32mok\033[0m — ninguna JNIEXPORT descarta el WmaResult de las {n} `wma_*` "
        "que lo devuelven."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
