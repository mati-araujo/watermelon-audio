#!/usr/bin/env python3
"""Parseo del arbol C++ compartido por los guardrails que razonan sobre el
grafo de llamadas.

POR QUE EXISTE
--------------
`check-rt-safety.py` (WD-1.1) construyo un parser de definiciones de funcion
por matcheo de llaves, y funciona. REQ-013 necesita EL MISMO parseo para otra
pregunta —"?quien llama a esto?" en vez de "?que se alcanza desde el
callback?"—, asi que lo que era propiedad de un script pasa a ser un modulo.

Lo que vive aca es el PARSEO y nada mas: que es una definicion de funcion, que
es una llamada, y que archivos entran. Las decisiones de cada guardrail —sus
raices, sus prohibiciones, sus baselines— se quedan en su script.

EL LIMITE, HEREDADO Y DECLARADO
-------------------------------
La resolucion es por NOMBRE, no por tipo: no hay frontend de C++ aca. Cada
consumidor decide como convive con eso. `check-rt-safety.py` sigue solo lo que
resuelve a UNA definicion y lo compensa con un trinquete de cobertura;
`check-mechanism-callers.py` fusiona los homonimos de forma conservadora para
no acusar en falso. Las dos elecciones estan documentadas en su script.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CPP_ROOT = REPO / "audio/src/main/cpp"

EXCLUDED_PARTS = ("/thirdparty/", "/ios/build/", "/.deps/")
TEST_PART = "/tests/"
SOURCE_SUFFIXES = (".cpp", ".h", ".hpp", ".mm", ".inc")

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


def is_test_path(path: Path) -> bool:
    return TEST_PART in "/" + str(path.relative_to(REPO)).replace("\\", "/")


def collect_sources(include_tests: bool = False) -> list[Path]:
    """Los archivos C++ del motor. `include_tests=False` deja afuera `/tests/`,
    que es lo que quiere un guardrail que razona sobre PRODUCCION."""
    files = []
    for p in sorted(CPP_ROOT.rglob("*")):
        if p.suffix not in SOURCE_SUFFIXES or not p.is_file():
            continue
        rel = "/" + str(p.relative_to(REPO)).replace("\\", "/")
        if any(part in rel for part in EXCLUDED_PARTS):
            continue
        if not include_tests and TEST_PART in rel:
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
