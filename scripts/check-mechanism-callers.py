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

EL TRINQUETE
------------
`scripts/mechanism-callers-baseline.txt` declara lo que hay hoy, con la
CATEGORIA y la RAZON de cada entrada. El lint falla en las DOS direcciones: si
aparece algo que no esta declarado, y si algo declarado ya no se reproduce. El
segundo corte es el que evita que el archivo se vuelva un cementerio de razones
caducas. Y una entrada sin razon tambien falla: una excepcion tiene que costar
escribir por que.

Uso:
    python3 scripts/check-mechanism-callers.py           # falla con exit 1 si el
                                                         # conjunto cambio
    python3 scripts/check-mechanism-callers.py --list    # imprime lo detectado
    python3 scripts/check-mechanism-callers.py --quiet   # solo el resumen
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


BASELINE_PATH = REPO / "scripts/mechanism-callers-baseline.txt"

CATEGORIES = {"entrada", "callback-externo", "sonda-de-tests", "deuda"}


def rel(path: Path) -> str:
    return str(path.relative_to(REPO)).replace("\\", "/")


def load_baseline() -> tuple[dict[str, tuple[str, str]], list[str]]:
    """-> ({clave: (categoria, razon)}, errores_de_formato)

    La clave es `<archivo>::<qname>`. Se parsea DESDE LA DERECHA porque el qname
    lleva `::` adentro: primero se corta la razon por ` | `, despues la categoria
    por el ultimo `::`.
    """
    entries: dict[str, tuple[str, str]] = {}
    errors: list[str] = []
    if not BASELINE_PATH.exists():
        return entries, [f"no existe {rel(BASELINE_PATH)} — el lint no puede chequear nada"]
    for n, raw_line in enumerate(BASELINE_PATH.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if " | " not in line:
            errors.append(f"{rel(BASELINE_PATH)}:{n} sin razon (falta ' | <razon>')")
            continue
        left, why = line.split(" | ", 1)
        if not why.strip():
            errors.append(f"{rel(BASELINE_PATH)}:{n} razon vacia")
            continue
        if "::" not in left:
            errors.append(f"{rel(BASELINE_PATH)}:{n} formato invalido: {line[:60]}")
            continue
        rest, cat = left.rsplit("::", 1)
        if cat not in CATEGORIES:
            errors.append(
                f"{rel(BASELINE_PATH)}:{n} categoria desconocida '{cat}' "
                f"(validas: {', '.join(sorted(CATEGORIES))})"
            )
            continue
        entries[rest] = (cat, why.strip())
    return entries, errors


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
    ap.add_argument("--list", action="store_true", help="imprime lo detectado")
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

    if args.list:
        for fn, callers in hits:
            suites = sorted({Path(c.path).name for c in callers})
            shown = ", ".join(suites[:3]) + (" …" if len(suites) > 3 else "")
            print(f"  {fn.qname}")
            print(f"      definida en  {rel(fn.path)}:{fn.line}")
            print(f"      la llaman    {shown}")

    declared, errors = load_baseline()
    found = {f"{rel(fn.path)}::{fn.qname}": (fn, callers) for fn, callers in hits}

    nuevas = sorted(k for k in found if k not in declared)
    muertas = sorted(k for k in declared if k not in found)

    if not args.quiet:
        for key in nuevas:
            fn, callers = found[key]
            suites = sorted({Path(c.path).name for c in callers})
            print(
                f"SIN DECLARAR  {fn.qname}\n"
                f"    {rel(fn.path)}:{fn.line}\n"
                f"    la llaman solo tests: {', '.join(suites[:4])}\n"
                f"    si es un mecanismo de produccion, le falta un llamador; si no,\n"
                f"    declarala en {rel(BASELINE_PATH)} con su categoria y su razon."
            )
        for key in muertas:
            cat, _ = declared[key]
            print(
                f"YA NO SE REPRODUCE  {key}  ({cat})\n"
                f"    esta declarada en el baseline y el lint ya no la detecta.\n"
                f"    Borrala: una entrada que no se reproduce miente sobre la deuda."
            )
        for e in errors:
            print(f"BASELINE INVALIDO  {e}")

    print(
        f"mechanism-callers — {len(hits)} detectadas / {len(declared)} declaradas, "
        f"sobre {prod_fns} funciones en {prod_files} archivos ({elapsed:.2f} s)"
    )
    if nuevas or muertas or errors:
        print(
            f"  ROJO — {len(nuevas)} sin declarar, {len(muertas)} que ya no se "
            f"reproducen, {len(errors)} error(es) de formato"
        )
        return 1
    print("  sin deuda nueva — el baseline es exactamente lo detectado")
    return 0


if __name__ == "__main__":
    sys.exit(main())
