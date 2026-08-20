#!/usr/bin/env python3
"""
REQ-006.4 — que un subsistema no vuelva a prepararse con un sample rate LITERAL.

POR QUE ESTE LINT EXISTE
------------------------
WD-3.4 acusaba a `SynthEngineDispatcher`, que prepara dieciseis engines con
`prepare(48000, 4096)` literal, de dejar la cuerda de Karplus 1,4 semitonos baja
en un device a 44,1 kHz. Medido: **el sintoma no reproducia**. El rate SI le
llegaba, por `AudioEngine::start()`.

Lo que faltaba no era la constante: era el CAMINO. `onStreamConfigChanged` no
re-preparaba a nadie mas que a tres subsistemas, y el camino de coercion de
`start()` re-preparaba con el thread de audio adentro (una carrera real, 4
warnings de TSan). Eso lo pagaron REQ-006.1 y REQ-006.2.

Entonces este lint NO persigue el numero 48000. De las 250 ocurrencias fuera de
tests, 232 son comentarios, constantes de diseño e inicializaciones de miembro en
constructores — y un constructor NO TIENE el rate, asi que ahi el literal es la
unica opcion honesta. Contarlas seria ruido que nadie mira.

Lo que persigue son las LLAMADAS a preparar un subsistema con un rate literal,
que es el sitio donde el rate real casi siempre esta disponible y no se uso.

TRINQUETE BIDIRECCIONAL
-----------------------
Igual que `rt-safety-baseline.txt` y `reset-baseline.txt`: falla si aparece una
llamada que no esta declarada, Y TAMBIEN si una declarada ya no se reproduce. Sin
la segunda mitad el baseline envejece y termina describiendo un arbol que ya no
existe — que es la forma en que un guardrail se vuelve decorativo.
"""

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CPP_ROOT = REPO / "audio/src/main/cpp"
BASELINE = REPO / "scripts/literal-rate-baseline.txt"

# Las funciones que PREPARAN un subsistema. No es "cualquier funcion con un
# 48000": es el acto de configurar algo con un rate.
PREPARERS = r"(?:prepare|setSampleRate|setOutputSampleRate|setCaptureSampleRate)"
# Un rate literal: 4 o 5 digitos, con o sin sufijo de float. Cubre 8000..192000.
LITERAL = r"[0-9]{4,6}(?:\.[0-9]+)?f?"
CALL = re.compile(rf"\.{PREPARERS}\s*\(\s*{LITERAL}|->{PREPARERS}\s*\(\s*{LITERAL}")

SUFFIXES = {".cpp", ".h", ".mm", ".inc"}


def is_comment(line: str) -> bool:
    s = line.strip()
    return s.startswith("//") or s.startswith("*") or s.startswith("/*")


def scan(root: Path, base: Path = None):
    """Devuelve {"ruta::linea": texto} de cada llamada con rate literal.

    `base` es contra quien se relativizan las rutas; por default el repo. El
    self-test pasa su propio tmpdir, que no cuelga del repo.
    """
    base = base or REPO
    found = {}
    for path in sorted(root.rglob("*")):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        # Los tests preparan con literales A PROPOSITO: un test que pide 44100
        # exacto no puede leerlo de un device que no existe.
        if "/tests/" in str(path).replace("\\", "/"):
            continue
        rel = path.relative_to(base).as_posix()
        for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if is_comment(line):
                continue
            if CALL.search(line):
                found[f"{rel}::{n}"] = line.strip()
    return found


def load_baseline(path: Path):
    if not path.exists():
        return set()
    out = set()
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.add(line.split(" | ")[0].strip())
        continue
    return out


def report(found, declared):
    keys = set(found)
    new = sorted(keys - declared)
    stale = sorted(declared - keys)
    for k in new:
        print(f"  NUEVA   {k}\n            {found[k]}")
    for k in stale:
        print(f"  MUERTA  {k}  (declarada en el baseline y ya no se reproduce)")
    return new, stale


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true",
                    help="verifica que el lint PUEDE fallar, en las dos direcciones")
    ap.add_argument("--update-baseline", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    found = scan(CPP_ROOT)

    if args.update_baseline:
        lines = [
            "# Llamadas que preparan un subsistema con un sample rate LITERAL.",
            "#",
            "# TRINQUETE BIDIRECCIONAL: `check-literal-rate.py` falla si aparece una que no",
            "# esta aca, y TAMBIEN si una de aca ya no se reproduce.",
            "#",
            "# Estar declarada NO significa 'esta bien': significa 'se miro y se decidio'.",
            "# La clasificacion va en la columna de la derecha.",
            "#",
            "# Formato:  <archivo>::<linea> | <clasificacion>",
            "",
        ]
        for k in sorted(found):
            lines.append(f"{k} | sin clasificar")
        BASELINE.write_text("\n".join(lines) + "\n")
        print(f"baseline reescrito con {len(found)} entradas — CLASIFICALAS a mano")
        return 0

    declared = load_baseline(BASELINE)
    new, stale = report(found, declared)
    if new or stale:
        print(f"\nrate literal — {len(new)} nueva(s), {len(stale)} muerta(s)")
        if new:
            print("  Una llamada nueva con rate literal: usa el rate real, o declarala\n"
                  "  en scripts/literal-rate-baseline.txt con su clasificacion y su razon.")
        if stale:
            print("  Una entrada declarada ya no se reproduce: sacala del baseline.\n"
                  "  El trinquete falla en las DOS direcciones a proposito.")
        return 1

    print(f"rate literal — {len(found)} llamadas declaradas, ninguna nueva ni muerta")
    return 0


def self_test() -> int:
    """El lint tiene que poder fallar. Si el parser se rompe, queda verde para siempre."""
    ok = True

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "fake.cpp").write_text(
            "void f() {\n"
            "    thing->prepare(48000, 4096);\n"
            "    // other->prepare(44100, 512);   <- comentario, no cuenta\n"
            "    other.setSampleRate(96000);\n"
            "    good->prepare(sampleRate, maxBlock);\n"
            "}\n"
        )
        found = scan(root, root)
        hits = {k.rsplit("::", 1)[1] for k in found} if found else set()
        # Se esperan las lineas 2 y 4; NI la 3 (comentario) NI la 5 (variable).
        if len(found) != 2:
            print(f"self-test FALLO: se esperaban 2 llamadas, se detectaron {len(found)}: {found}")
            ok = False
        if any("sampleRate, maxBlock" in v for v in found.values()):
            print("self-test FALLO: conto una llamada que usa el rate REAL")
            ok = False
        if any(v.strip().startswith("//") for v in found.values()):
            print("self-test FALLO: conto un comentario")
            ok = False

        # Direccion 1: una llamada no declarada tiene que dar NUEVA.
        new, stale = report(found, set())
        if len(new) != 2 or stale:
            print("self-test FALLO: no marco como NUEVAS las llamadas sin declarar")
            ok = False

        # Direccion 2: una entrada declarada que no existe tiene que dar MUERTA.
        new, stale = report(found, set(found) | {"inventado.cpp::1"})
        if new or stale != ["inventado.cpp::1"]:
            print("self-test FALLO: no marco como MUERTA una entrada que ya no se reproduce")
            ok = False

    print("self-test OK — detecta llamadas nuevas Y entradas muertas; "
          "ignora comentarios y llamadas que usan el rate real" if ok else "self-test FALLO")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
