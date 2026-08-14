#!/usr/bin/env python3
"""WD-1.1 — guardrail: nada que viole las reglas de tiempo real puede vivir en
el call-graph del callback de audio.

POR QUE EXISTE
--------------
`IAudioBackend.h` lista las prohibiciones correctas —sin allocations, sin locks
que bloqueen, sin syscalls, sin logging, sin `std::shared_ptr`— y el callback
las violaba en cuatro lugares, dos de los cuales SOBREVIVIAN a `NDEBUG` porque
salteaban los macros `LOGI/LOGW` y llamaban a `wma::logMessage` directo.

Una regla que solo vive en prosa la rompe el proximo que pase por ahi de buena
fe: el codigo que la violaba parecia sancionado justamente porque ya estaba.
Esto la vuelve mecanica.

COMO FUNCIONA
-------------
1. Parsea las definiciones de funcion de todo el arbol C++ (sin tests, sin
   thirdparty) por matcheo de llaves, salteando strings y comentarios.
2. Arranca de las RAICES RT declaradas abajo y camina el grafo de llamadas.
3. En cada cuerpo alcanzable busca los patrones prohibidos.

LIMITE CONOCIDO, Y COMO SE COMPENSA
-----------------------------------
La resolucion es por NOMBRE, no por tipo: no hay frontend de C++ aca. Eso deja
dos opciones malas y una razonable.

Seguir TODAS las definiciones de un nombre ambiguo sobre-aproxima tanto que el
lint se vuelve ruido: `prepare(` tiene veinte definiciones en el arbol y
arrastra `SpectrumAnalyzer::prepare` a un grafo donde no esta. Un guardrail con
150 falsos positivos no se lee, se silencia.

Asi que se siguen solo las llamadas que resuelven a UNA definicion, mas un mapa
explicito de los puntos de despacho virtual que SI estan en el path RT
(`VIRTUAL_EXPANSIONS`). Ese mapa es donde vive el juicio, y por eso esta a la
vista y se revisa: agregar un `Effect` nuevo no lo toca, pero agregar una
jerarquia virtual nueva al callback si.

El error queda del lado de la SUB-aproximacion, y se compensa con dos cosas:
las raices se declaran generosamente (`EffectChain::process` y `OutputStage`
son raices propias aunque el motor ya las llame), y `--graph` imprime lo
alcanzado para que un hueco se pueda ver.

ESCAPE HATCH
------------
Una linea marcada con `// RT-SAFE-ALLOW: <razon>` se exceptua. La razon es
obligatoria y el script falla si falta: el objetivo es que una excepcion cueste
escribir por que, no que sea gratis.

Uso:
    python3 scripts/check-rt-safety.py            # falla con exit 1 si hay violaciones
    python3 scripts/check-rt-safety.py --graph    # imprime el call-graph alcanzado
    python3 scripts/check-rt-safety.py --self-test  # se verifica a si mismo (ver abajo)

El --self-test inyecta una violacion sintetica en un cuerpo alcanzable y afirma
que el script la encuentra. Un lint que nunca se vio fallar no es un lint.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CPP_ROOT = REPO / "audio/src/main/cpp"

EXCLUDED_PARTS = ("/tests/", "/thirdparty/", "/ios/build/", "/.deps/")
SOURCE_SUFFIXES = (".cpp", ".h", ".hpp", ".mm", ".inc")

# ---------------------------------------------------------------------------
# Las raices RT: todo lo que un thread de audio ejecuta.
#
# Se declaran por nombre porque son puntos de entrada de una interfaz
# (IAudioCallback) o callbacks de un backend. Agregar un backend nuevo con su
# propio callback obliga a agregarlo aca — que es exactamente la friccion que
# se quiere: un backend cuyo callback no esta en esta lista no esta cubierto.
# ---------------------------------------------------------------------------
RT_ROOTS = {
    # El punto de convergencia de TODOS los backends (IAudioCallback).
    "AudioEngine::onAudioReady",
    "AudioEngine::processAudioBlock",
    # Thread de captura de Android — es un segundo thread RT, con su propio DSP.
    "InputNode::processInputBlock",
    # Callbacks de backend: cada uno corre en el thread RT de su plataforma.
    "OboeBackend::onAudioReady",
    "SplitBackend::InputCallback::onAudioReady",
    "SplitBackend::OutputCallback::onAudioReady",
    "RoundTripMeasurer::onAudioReady",
    # La cadena de efectos y la etapa de salida: llamadas desde el callback.
    "EffectChain::process",
    "OutputStage::processOutput",
    "OutputStage::processOutputLightweight",
}

# ---------------------------------------------------------------------------
# Lo prohibido. Cada patron lleva la razon en el mensaje: el que lo vea en rojo
# tiene que entender por que sin abrir este archivo.
# ---------------------------------------------------------------------------
FORBIDDEN = [
    # --- logging: formatea y hace syscall ---
    (r"\bwma::logMessage\b", "logging (formatea + syscall)"),
    (r"\blogMessage\s*\(", "logging (formatea + syscall)"),
    (r"\b[A-Z][A-Z0-9_]*_LOG[A-Z]*\s*\(", "logging via macro con prefijo"),
    (r"\bLOG[IWEDV]\s*\(", "logging via macro"),
    (r"\bAUDIO_DIAG\s*\(", "logging via macro"),
    (r"\bLOGI_CALLBACK\s*\(", "logging via macro"),
    (r"\b(?:sn|s|f|v)?printf\s*\(", "formateo de string"),
    (r"\b__android_log", "logging de plataforma"),
    (r"\bos_log", "logging de plataforma"),
    (r"\bNSLog\b", "logging de plataforma"),
    (r"\bstd::c(?:out|err)\b", "stream de salida"),
    # --- allocation ---
    (r"\bnew\s+[A-Za-z_]", "allocation (new)"),
    (r"\b(?:m|c|re)alloc\s*\(", "allocation (malloc family)"),
    (r"\.resize\s*\(", "allocation (resize)"),
    (r"\.reserve\s*\(", "allocation (reserve)"),
    (r"\.push_back\s*\(", "allocation (push_back)"),
    (r"\.emplace_back\s*\(", "allocation (emplace_back)"),
    (r"\bmake_unique\b", "allocation (make_unique)"),
    (r"\bmake_shared\b", "allocation (make_shared)"),
    # --- lifetime: liberar en el thread de audio ---
    (r"\bshared_ptr\b", "shared_ptr (refcount + posible free en el thread RT)"),
    (r"\bdelete\s+[A-Za-z_(]", "deallocation (delete)"),
    (r"(?<![A-Za-z_.>])\bfree\s*\(", "deallocation (free)"),
    # --- bloqueo ---
    (r"\block_guard\b", "lock que bloquea"),
    (r"\bunique_lock\b", "lock que bloquea"),
    (r"(?<!try_)\.lock\s*\(", "lock que bloquea (usar try_lock)"),
    (r"\bsleep_for\b", "sleep"),
    (r"\bcondition_variable\b", "condition variable"),
    (r"\.join\s*\(\s*\)", "join de thread"),
    # --- tipos que alocan ---
    (r"\bstd::string\b", "std::string (aloca)"),
    (r"\bstd::function\b", "std::function (puede alocar al asignarse)"),
    # --- excepciones ---
    (r"\bthrow\b", "excepcion"),
]
FORBIDDEN = [(re.compile(p), why) for p, why in FORBIDDEN]

BASELINE_PATH = REPO / "scripts/rt-safety-baseline.txt"

ALLOW_RE = re.compile(r"//\s*RT-SAFE-ALLOW:\s*(?P<reason>\S.*)$")
ALLOW_BARE_RE = re.compile(r"//\s*RT-SAFE-ALLOW\b")

# Definicion de funcion: tipo de retorno + [Clase::]nombre(args) [const] {
DEF_RE = re.compile(
    r"^[ \t]*"
    r"(?:(?:template\s*<[^>]*>|inline|static|virtual|constexpr|explicit|friend)\s+)*"
    r"(?:[A-Za-z_][\w:<>,\s*&~]*?\s+)?"                       # tipo de retorno (opcional: ctors)
    r"(?P<qname>[A-Za-z_]\w*(?:::[A-Za-z_~]\w*)*)"            # [Clase::]nombre
    r"\s*\([^;{]*?\)"                                          # argumentos
    r"(?:\s*(?:const|noexcept|override|final|mutable))*"
    r"\s*(?:->[\w:<>,\s*&]+)?"                                 # trailing return
    r"\s*\{",
    re.MULTILINE,
)

CALL_RE = re.compile(r"(?:(?P<qual>[A-Za-z_]\w*)::)?(?P<name>[A-Za-z_]\w*)\s*\(")

# ---------------------------------------------------------------------------
# Despacho virtual que SI ocurre en el path RT. Para estos nombres se siguen
# TODAS las definiciones, porque el tipo concreto no se puede resolver aca y
# todas las implementaciones estan genuinamente en el callback:
#
#   process  -> los 26 Effect, los AudioNode (Mixer/Oscillator/EffectChain/Input),
#               AudioLooper, los moduladores
#   render   -> los SynthEngine (FM, KS, Supersaw, Wavetable, Granular, SoundFont)
#   tick     -> Transport
#
# Agregar una jerarquia virtual nueva al callback obliga a tocar este mapa. Esa
# friccion es deliberada: un despacho virtual que no esta aca no esta cubierto.
# ---------------------------------------------------------------------------
# El valor acota a que subarbol se expande. `None` = todas las definiciones.
# El acotamiento importa: `reset` es un nombre generico —hay decenas de
# `reset()` de ciclo de vida en el arbol— y expandirlo entero arrastraba
# `VoiceManager::reset` y `ResizableRingBuffer::reset`, que corren en el thread
# de control. El unico `reset` que SI es RT es el de los efectos, que
# `EffectChain::reset()` invoca desde el callback (ver Effect.h).
VIRTUAL_EXPANSIONS = {
    "process": None,
    "render": None,
    "renderBlock": None,
    "processBlock": None,
    "tick": None,
    "reset": "effects/",
    "processOneEffect": None,
    "processWithMode": None,
}

# Palabras que parecen llamadas pero son sintaxis o construccion de valores.
NOT_CALLS = {
    "if", "for", "while", "switch", "return", "sizeof", "catch", "throw",
    "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast",
    "int", "float", "double", "bool", "char", "void", "size_t", "auto",
    "int32_t", "int64_t", "uint32_t", "uint64_t", "uint8_t", "size_type",
    "min", "max", "abs", "fabs", "fabsf", "sqrt", "sqrtf", "sin", "cos", "tan",
    "exp", "expf", "log", "logf", "pow", "powf", "floor", "ceil", "round",
    "isfinite", "isnan", "isinf", "clamp", "memcpy", "memset", "memmove",
    "fill", "copy", "swap", "move", "forward", "load", "store", "fetch_add",
    "fetch_sub", "compare_exchange_strong", "compare_exchange_weak", "exchange",
    "data", "size", "empty", "begin", "end", "assert", "static_assert",
}


def strip_noise(text: str) -> str:
    """Reemplaza comentarios y literales por espacios, preservando offsets.

    Preservar offsets importa: los numeros de linea que se reportan salen de
    contar '\\n' sobre este mismo buffer.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if text[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            quote, j = c, i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                if text[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def body_span(text: str, brace_pos: int) -> int:
    """Devuelve el offset justo despues de la llave de cierre."""
    depth, i, n = 0, brace_pos, len(text)
    while i < n:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


class Function:
    """`line` es la linea de la firma (para reportar donde vive la funcion).
    `body_line` es la linea de la llave de apertura, y es la unica base valida
    para convertir un offset dentro de `body` en un numero de linea: una firma
    puede ocupar varias lineas y usar `line` corre todo el reporte."""

    __slots__ = ("qname", "path", "line", "body_line", "body")

    def __init__(self, qname, path, line, body_line, body):
        self.qname = qname
        self.path = path
        self.line = line
        self.body_line = body_line
        self.body = body


def collect_sources() -> list[Path]:
    files = []
    for p in sorted(CPP_ROOT.rglob("*")):
        if p.suffix not in SOURCE_SUFFIXES or not p.is_file():
            continue
        rel = "/" + str(p.relative_to(REPO)).replace("\\", "/")
        if any(part in rel for part in EXCLUDED_PARTS):
            continue
        files.append(p)
    return files


def parse_functions(files: list[Path]) -> tuple[dict, dict, dict]:
    """-> (por_qname, por_nombre_simple, texto_crudo_por_path)"""
    by_qname: dict[str, list[Function]] = {}
    by_simple: dict[str, list[Function]] = {}
    raw: dict[Path, str] = {}

    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        raw[path] = text
        clean = strip_noise(text)

        # Contexto de clase para metodos definidos inline dentro de `class X {`.
        class_at = []
        for m in re.finditer(r"\b(?:class|struct)\s+([A-Za-z_]\w*)[^;{]*\{", clean):
            class_at.append((m.end(), m.group(1), body_span(clean, m.end() - 1)))

        for m in DEF_RE.finditer(clean):
            qname = m.group("qname")
            simple = qname.split("::")[-1]
            if simple in NOT_CALLS:
                continue
            brace = clean.index("{", m.end() - 1)
            end = body_span(clean, brace)
            body = clean[brace:end]
            if "::" not in qname:
                enclosing = [c for (s, c, e) in class_at if s <= brace < e]
                if enclosing:
                    qname = f"{enclosing[-1]}::{qname}"
            fn = Function(
                qname,
                path,
                clean.count("\n", 0, m.start()) + 1,
                clean.count("\n", 0, brace) + 1,
                body,
            )
            by_qname.setdefault(qname, []).append(fn)
            by_simple.setdefault(simple, []).append(fn)
    return by_qname, by_simple, raw


def reachable(by_qname, by_simple) -> tuple[list[Function], list[str]]:
    """BFS desde las raices RT. Devuelve (funciones, raices_no_encontradas)."""
    missing = [r for r in RT_ROOTS if r not in by_qname and r.split("::")[-1] not in by_simple]
    seen_ids, order, queue = set(), [], []

    for root in sorted(RT_ROOTS):
        for fn in by_qname.get(root, []) or by_simple.get(root.split("::")[-1], []):
            if id(fn) not in seen_ids:
                seen_ids.add(id(fn))
                queue.append(fn)

    while queue:
        fn = queue.pop(0)
        order.append(fn)
        for m in CALL_RE.finditer(fn.body):
            name = m.group("name")
            if name in NOT_CALLS:
                continue
            qual = m.group("qual")
            cands = []
            if qual:
                cands = by_qname.get(f"{qual}::{name}", [])
            if not cands:
                pool = by_simple.get(name, [])
                # Solo se sigue lo que resuelve sin ambiguedad, salvo los
                # despachos virtuales declarados. Ver la nota del docstring.
                if len(pool) == 1:
                    cands = pool
                elif name in VIRTUAL_EXPANSIONS:
                    scope = VIRTUAL_EXPANSIONS[name]
                    cands = [
                        f for f in pool
                        if scope is None or scope in str(f.path).replace("\\", "/")
                    ]
            for c in cands:
                if id(c) not in seen_ids:
                    seen_ids.add(id(c))
                    queue.append(c)
    return order, missing


def scan(functions: list[Function], raw: dict) -> tuple[list, list]:
    violations, bad_allows = [], []
    for fn in functions:
        src_lines = raw[fn.path].splitlines()
        for m_pat, why in FORBIDDEN:
            for m in m_pat.finditer(fn.body):
                line_no = fn.body_line + fn.body[: m.start()].count("\n")
                # `fn.line` es la linea de la firma; el offset del cuerpo puede
                # estar unas lineas mas abajo si la firma ocupa varias.
                raw_line = src_lines[line_no - 1] if 0 < line_no <= len(src_lines) else ""
                # El ALLOW vale en la misma linea o en el bloque de comentario
                # inmediatamente anterior — que es donde un C++ lo escribiria.
                context = [raw_line]
                k = line_no - 2
                while k >= 0 and src_lines[k].lstrip().startswith("//"):
                    context.append(src_lines[k])
                    k -= 1
                if any(ALLOW_RE.search(c) for c in context):
                    continue
                if any(ALLOW_BARE_RE.search(c) for c in context):
                    bad_allows.append((fn.path, line_no, raw_line.strip()))
                    continue
                violations.append((fn.path, line_no, fn.qname, why, raw_line.strip()))
    # Deduplicar: un mismo (archivo, linea) puede caer por varios patrones.
    seen, deduped = set(), []
    for v in violations:
        key = (v[0], v[1])
        if key in seen:
            continue
        seen.add(key)
        deduped.append(v)
    deduped.sort(key=lambda v: (str(v[0]), v[1]))
    return deduped, bad_allows


def load_baseline() -> dict[str, str]:
    """Violaciones aceptadas por ahora, cada una con el WD que la saca.

    Es un TRINQUETE, no una lista de excepciones: el lint falla si aparece una
    violacion que no esta aca, Y TAMBIEN si una entrada de aca ya no se
    reproduce. Lo segundo es lo que impide que el archivo se pudra — una linea
    obsoleta es un error, no un residuo inofensivo.

    La clave es archivo::funcion::motivo, no archivo:linea: los numeros de
    linea se corren con cualquier edit y volverian el baseline inutil al primer
    commit.
    """
    if not BASELINE_PATH.exists():
        return {}
    entries = {}
    for raw in BASELINE_PATH.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, _, owner = line.partition(" | ")
        entries[key.strip()] = owner.strip() or "(sin dueno declarado)"
    return entries


def main() -> int:
    ap = argparse.ArgumentParser(description="WD-1.1 — guardrail de RT-safety")
    ap.add_argument("--graph", action="store_true", help="imprimir el call-graph alcanzado")
    ap.add_argument("--self-test", action="store_true", help="verificar que el lint detecta")
    args = ap.parse_args()

    files = collect_sources()
    by_qname, by_simple, raw = parse_functions(files)
    functions, missing = reachable(by_qname, by_simple)

    if missing:
        print("ERROR: raices RT declaradas que no se encontraron en el arbol:", file=sys.stderr)
        for r in missing:
            print(f"  - {r}", file=sys.stderr)
        print("\nO se renombraron, o se borraron. Actualizar RT_ROOTS.", file=sys.stderr)
        return 2

    if args.graph:
        print(f"call-graph RT: {len(functions)} funciones alcanzables desde {len(RT_ROOTS)} raices\n")
        for fn in functions:
            print(f"  {fn.qname:<55} {fn.path.relative_to(REPO)}:{fn.line}")
        return 0

    if args.self_test:
        # Inyecta una violacion sintetica en un cuerpo alcanzable y afirma que
        # el escaneo la ve. Sin esto, un cambio que rompa el parser dejaria el
        # lint en verde permanente — el peor estado posible para un guardrail.
        target = functions[0]
        target.body = target.body[:1] + '\n    LOGE("inyectado por --self-test");\n' + target.body[1:]
        found, _ = scan([target], raw)
        if not found:
            print("SELF-TEST FALLO: el lint no detecto una violacion inyectada.", file=sys.stderr)
            return 1
        print(f"self-test OK — violacion inyectada detectada en {target.qname}")
        return 0

    violations, bad_allows = scan(functions, raw)

    baseline = load_baseline()
    def key_of(v):
        return f"{v[0].relative_to(REPO)}::{v[2]}::{v[3]}"

    seen_keys = {key_of(v) for v in violations}
    baselined = [v for v in violations if key_of(v) in baseline]
    violations = [v for v in violations if key_of(v) not in baseline]
    stale = sorted(set(baseline) - seen_keys)

    print(f"RT-safety — {len(functions)} funciones alcanzables desde {len(RT_ROOTS)} raices RT")
    if baselined:
        print(f"  {len(baselined)} violaciones en el baseline (deuda con dueno declarado):")
        for v in baselined:
            print(f"    {v[0].relative_to(REPO)}:{v[1]} — {baseline[key_of(v)]}")

    if stale:
        print(
            "\nEntradas del baseline que ya no se reproducen. Borralas de "
            f"{BASELINE_PATH.relative_to(REPO)}:",
            file=sys.stderr,
        )
        for k in stale:
            print(f"  {k}", file=sys.stderr)
        print(
            "\nUn baseline con entradas muertas deja de decir la verdad sobre la deuda.",
            file=sys.stderr,
        )

    if bad_allows:
        print("\nRT-SAFE-ALLOW sin razon (la razon es obligatoria):", file=sys.stderr)
        for path, line, text in bad_allows:
            print(f"  {path.relative_to(REPO)}:{line}: {text}", file=sys.stderr)

    if violations:
        print(f"\n{len(violations)} violaciones en el call-graph del thread de audio:\n", file=sys.stderr)
        for path, line, qname, why, text in violations:
            print(f"  {path.relative_to(REPO)}:{line}", file=sys.stderr)
            print(f"      en {qname}() — {why}", file=sys.stderr)
            print(f"      {text}", file=sys.stderr)
        print(
            "\nSi alguna es un falso positivo, marcala con `// RT-SAFE-ALLOW: <razon>`.\n"
            "La razon es obligatoria: una excepcion tiene que costar escribir por que.",
            file=sys.stderr,
        )
        return 1

    if bad_allows or stale:
        return 1

    print("sin violaciones nuevas")
    return 0


if __name__ == "__main__":
    sys.exit(main())
