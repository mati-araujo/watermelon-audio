#!/usr/bin/env python3
"""REQ-013 — guardrail: un mecanismo que solo llaman los tests no esta entregado.

POR QUE EXISTE
--------------
REQ-012 entrego, en tres etapas verificadas con TSan y mutacion, un mecanismo
completo —compuerta, re-preparado y costura— que NADIE llamaba en produccion.
En un telefono el DSP de entrada seguia sin seguir al rate, con la suite entera
en verde. Lo destapo un `grep` al terminar S3.

La verificacion contesta "?funciona?". Falta "?lo usa alguien?", y esa pregunta
tiene respuesta estatica.

QUE PREGUNTA, EXACTAMENTE
-------------------------
Para cada funcion de PRODUCCION: ?hay algun cuerpo de produccion que la llame,
fuera de su propia definicion? Si no lo hay, y si en cambio la llama algun
test, se reporta.

Es deliberadamente LOCAL. La formulacion obvia —"alcanzable desde las raices
`api/` + `jni/`"— se prototipo y se MIDIO el 2026-08-26 antes de escribir esto,
y no se sostiene: da 218 candidatos y deja 1673 funciones "alcanzables desde
ningun lado", porque la resolucion por nombre no aguanta alcanzabilidad
transitiva a esta escala. Salian acusadas `AudioEngine::transitionToState` (3
llamadores), `DriftResampler::updateStep` (3) y `hasBackendRole` (2). Un
guardrail que acusa a `transitionToState` no se lee: se silencia. El detalle
esta en el `plan.md` de REQ-013 (H1 y H2).

La formulacion local NO pierde la clase: un subarbol huerfano `A -> B -> C`
tiene siempre una ENTRADA `A` sin llamador de produccion, asi que se reporta
por su raiz y el hilo se sigue desde ahi.

LO QUE NO ES
------------
- No es un detector de codigo muerto. Una funcion que no llama NADIE —ni
  produccion ni tests— no se reporta: la pregunta es "solo la llaman los
  tests".
- No ve "el llamador existe pero no llega" (MINI-007: el cable salia temprano
  porque el nodo no estaba publicado). Eso es semantica de runtime y ningun
  lint estatico lo ve.
- No ve el hueco del camino JNI (REQ-016): ahi la pregunta es quien EJECUTA,
  no quien llama.

FALSO NEGATIVO DECLARADO
------------------------
Si el nombre simple de una funcion huerfana coincide con el de otra que si se
llama en produccion, la fusion conservadora la tapa. Es el mismo limite por
nombre de `check-rt-safety.py`, y se elige en esa direccion a proposito: el
error va del lado de NO ACUSAR EN FALSO, que es lo que decide si el gate se lee
o se silencia.

Y tiene TAMANO medido, que es lo que lo vuelve una limitacion y no una
advertencia: al 2026-08-26 hay **117 nombres simples** con mas de una
definicion de produccion que se llaman desde produccion Y desde tests. Todo lo
que se llame asi queda tapado. Los tres mas cargados son `reset` (96
definiciones), `read` (6) y `reportCaptureDiscontinuity` (2). Si ese numero
crece mucho, la salida es la misma que ya conoce el repo para el walker de RT:
renombrar, no redeclarar.

Uso:
    python3 scripts/check-mechanism-callers.py           # imprime los candidatos
    python3 scripts/check-mechanism-callers.py --quiet   # solo el resumen

TODAVIA NO FALLA. El baseline es de REQ-013 S2 y el cableado a `gate.sh` con su
`--self-test` es de S3. Hasta entonces esto informa y sale 0: un lint sin
baseline que fallara con 76 entradas solo ensenaria a ignorarlo.
"""

from __future__ import annotations

import argparse
import sys
import time
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cpp_callgraph import (  # noqa: E402
    CALL_RE,
    NOT_CALLS,
    REPO,
    Function,
    collect_sources,
    is_test_path,
    parse_functions,
)


def rel(path: Path) -> str:
    return str(path.relative_to(REPO)).replace("\\", "/")


def call_sites(by_qname: dict) -> tuple[dict, dict]:
    """-> (llamadores_de_produccion, llamadores_de_tests), por nombre simple.

    El valor es la lista de funciones que hacen la llamada, no un conteo: el
    reporte nombra a los tests que si la llaman, y eso es la mitad del valor
    del mensaje — dice donde mirar.
    """
    prod: dict[str, list[Function]] = defaultdict(list)
    tests: dict[str, list[Function]] = defaultdict(list)
    for group in by_qname.values():
        for fn in group:
            bucket = tests if is_test_path(fn.path) else prod
            for m in CALL_RE.finditer(fn.body):
                name = m.group("name")
                if name in NOT_CALLS:
                    continue
                bucket[name].append(fn)
    return prod, tests


def find_orphans(by_qname: dict) -> list[tuple[Function, list[Function]]]:
    prod_callers, test_callers = call_sites(by_qname)
    hits = []
    for group in by_qname.values():
        for fn in group:
            if is_test_path(fn.path):
                continue
            simple = fn.qname.split("::")[-1]
            # Un llamador que sea la funcion misma es recursion, no un cable.
            external = [c for c in prod_callers.get(simple, []) if c is not fn]
            if external:
                continue
            callers = test_callers.get(simple, [])
            if not callers:
                continue  # no la llama nadie: es otra clase, no esta
            hits.append((fn, callers))
    return sorted(hits, key=lambda h: (rel(h[0].path), h[0].line))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true", help="solo el resumen")
    args = ap.parse_args()

    t0 = time.time()
    files = collect_sources(include_tests=True)
    # Un solo parseo del universo entero: las llamadas desde tests tienen que
    # resolver contra las definiciones de produccion.
    by_qname, _, _ = parse_functions(files)
    hits = find_orphans(by_qname)
    elapsed = time.time() - t0

    prod_files = sum(1 for f in files if not is_test_path(f))
    prod_fns = sum(
        1 for g in by_qname.values() for f in g if not is_test_path(f.path)
    )

    if not args.quiet:
        for fn, callers in hits:
            suites = sorted({Path(c.path).name for c in callers})
            shown = ", ".join(suites[:3]) + (" …" if len(suites) > 3 else "")
            print(f"  {fn.qname}")
            print(f"      definida en  {rel(fn.path)}:{fn.line}")
            print(f"      la llaman    {shown}")

    print(
        f"mechanism-callers — {len(hits)} funcion(es) de produccion sin llamador "
        f"de produccion, sobre {prod_fns} en {prod_files} archivos "
        f"({elapsed:.2f} s)"
    )
    if hits:
        print(
            "  (informativo: el baseline es de REQ-013 S2 y el gate de S3; "
            "todavia no falla)"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
