#!/usr/bin/env python3
"""REQ-021 — guardrail: un conteo escrito a mano en CLAUDE.md es una afirmacion sin verificar.

POR QUE EXISTE
--------------
`CLAUDE.md` es el archivo que TODO agente lee primero, y sus conteos quedaron
stale SIETE tandas seguidas. El propio archivo escribio su diagnostico y su
receta, y la receta no se ejecuto:

    "Cuatro veces stale es la evidencia de que 'medir antes de citar' NO
     alcanza: es una regla que solo vive en prosa, y este repo ya sabe como
     terminan (WD-1.1: el callback violaba sus reglas escritas en 65 lugares).
     La salida coherente con el resto del repo es un guardrail que re-mida y
     falle contra este archivo, como rt-safety o mechanism-callers.
     NO EXISTE TODAVIA."

Esto es esa receta. Un conteo mal no rompe un build: hace que el proximo
diseñe contra un modelo mental viejo. Ya paso — un consumidor diseño tres
pedidos contra un KDoc que decia "ocho floats" cuando eran quince.

COMO FUNCIONA
-------------
Cada metrica se MIDE del arbol. `CLAUDE.md` lleva un bloque delimitado que este
script POSEE: el modo normal compara, `--update` reescribe.

    python3 scripts/check-doc-counts.py --self-test   # que sabe fallar
    python3 scripts/check-doc-counts.py               # el lint
    python3 scripts/check-doc-counts.py --update      # y SU DIFF ES LA REVISION

POR QUE UN BLOQUE Y NO ANOTACIONES EN LA PROSA
----------------------------------------------
Se evaluo marcar cada numero donde aparece. Se descarto: deja la fuente de
verdad repartida y obliga al parser a entender narrativa. Un bloque que el
script posee concentra la afirmacion en un lugar; la prosa lo refiere sin
repetirlo.

"NO PUDE MEDIR" NUNCA ES UN PASE
--------------------------------
Una metrica que no encuentra su archivo, o cuyo parseo devuelve cero, FALLA.
Nunca reporta 0. Un denominador que no se midio y se publica igual es el modo
de falla que este repo ya borro dos veces (`JniExports.fromTree`,
`fetch-corpus.sh`), y es exactamente como este guardrail podria volverse
decorativo sin cambiar de color.

LO QUE NO ES
------------
- No mide nada que requiera CONSTRUIR o CORRER (la suite de host, la cobertura
  del arnes). Esos numeros no se vigilan: se sacan de la prosa y se reemplazan
  por el comando que los imprime medidos. Ver REQ-021 S2.
- No vigila KDoc ni las specs vivas. Driftean tambien, pero son otra superficie.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOC = REPO / "CLAUDE.md"

BEGIN = "<!-- BEGIN conteos-medidos — los escribe scripts/check-doc-counts.py, NO la mano -->"
END = "<!-- END conteos-medidos -->"

RED = "\033[31m"
GREEN = "\033[32m"
OFF = "\033[0m"


class NoSePudoMedir(Exception):
    """Una metrica que no se pudo medir. NUNCA se degrada a 0."""


# ---------------------------------------------------------------------------
# Las mediciones. Cada una lee el ARBOL; ninguna construye nada.
# ---------------------------------------------------------------------------

def _leer(rel: str) -> str:
    p = REPO / rel
    if not p.is_file():
        raise NoSePudoMedir(f"no existe {rel}")
    return p.read_text(encoding="utf-8", errors="replace")


def _contar_regex(rel: str, patron: str) -> int:
    n = len(re.findall(patron, _leer(rel), re.MULTILINE))
    if n == 0:
        raise NoSePudoMedir(
            f"el patron /{patron}/ no encontro NADA en {rel}. Eso no es 'cero': "
            f"es un parseo roto, y publicarlo como 0 seria inventar la medicion"
        )
    return n


def _contar_archivos(rel: str, sufijo: str) -> int:
    d = REPO / rel
    if not d.is_dir():
        raise NoSePudoMedir(f"no existe el directorio {rel}")
    n = sum(1 for p in d.rglob(f"*{sufijo}") if p.is_file())
    if n == 0:
        raise NoSePudoMedir(f"cero archivos {sufijo} bajo {rel}: el arbol se movio")
    return n


def _fuentes_planas(rel: str) -> int:
    """Fuentes .h/.cpp del NIVEL DE ARRIBA de un directorio — sin `tests/` y sin CMakeLists.

    🔴 El alcance esta cableado a proposito y no es un detalle. La primera version
    de esta metrica contaba recursivo y devolvia 49 donde la prosa afirma 17: media
    ALGO, pero no lo que el texto dice. Una metrica que mide otra cosa que la
    afirmacion que vigila es peor que no tenerla — pasa en verde y da confianza
    falsa, que es exactamente la clase que este guardrail existe para borrar.
    """
    d = REPO / rel
    if not d.is_dir():
        raise NoSePudoMedir(f"no existe el directorio {rel}")
    n = sum(1 for p in d.iterdir() if p.is_file() and p.suffix in (".h", ".cpp"))
    if n == 0:
        raise NoSePudoMedir(f"cero fuentes .h/.cpp en el nivel de arriba de {rel}")
    return n


def _lineas(rel: str) -> int:
    n = len(_leer(rel).splitlines())
    if n == 0:
        raise NoSePudoMedir(f"{rel} esta vacio")
    return n


def _version_toml(clave: str) -> str:
    txt = _leer("gradle/libs.versions.toml")
    m = re.search(rf'^{re.escape(clave)}\s*=\s*"([^"]+)"', txt, re.MULTILINE)
    if not m:
        raise NoSePudoMedir(f"no encontre la clave '{clave}' en gradle/libs.versions.toml")
    return m.group(1)


def _jniexport_total() -> int:
    d = REPO / "audio/src/main/cpp/jni"
    if not d.is_dir():
        raise NoSePudoMedir("no existe audio/src/main/cpp/jni")
    total = sum(
        len(re.findall(r"^JNIEXPORT\b", p.read_text(encoding="utf-8", errors="replace"), re.MULTILINE))
        for p in sorted(d.glob("*.cpp"))
    )
    if total == 0:
        raise NoSePudoMedir("cero JNIEXPORT en jni/*.cpp: el parseo se rompio")
    return total


def _baseline_reparto() -> str:
    """El reparto del baseline de mechanism-callers, DERIVADO — el propio archivo
    documenta que no se cuenta a mano."""
    txt = _leer("scripts/mechanism-callers-baseline.txt")
    cats: dict[str, int] = {}
    for linea in txt.splitlines():
        if not linea.strip() or linea.startswith("#"):
            continue
        cat = linea.split(" | ")[0].split("::")[-1].strip()
        if cat:
            cats[cat] = cats.get(cat, 0) + 1
    if not cats:
        raise NoSePudoMedir("el baseline de mechanism-callers no tiene una sola entrada parseable")
    return " / ".join(f"{cats[k]} {k}" for k in sorted(cats))


BRIDGE = "audio/src/androidMain/kotlin/com/watermellonstudios/audio/internal/bridge/AudioNativeBridge.kt"

# nombre -> (descripcion para el humano, medicion)
METRICAS: dict[str, tuple[str, callable]] = {
    "kt-commonMain":   ("archivos .kt en commonMain",            lambda: _contar_archivos("audio/src/commonMain", ".kt")),
    "kt-androidMain":  ("archivos .kt en androidMain",           lambda: _contar_archivos("audio/src/androidMain", ".kt")),
    "kt-iosMain":      ("archivos .kt en iosMain",               lambda: _contar_archivos("audio/src/iosMain", ".kt")),
    "bridge-loc":      ("LOC de AudioNativeBridge.kt",           lambda: _lineas(BRIDGE)),
    "bridge-external": ("`external fun` en AudioNativeBridge",   lambda: _contar_regex(BRIDGE, r"external fun")),
    "jniexport-bridge":("JNIEXPORT en jni_audio_bridge.cpp",     lambda: _contar_regex("audio/src/main/cpp/jni/jni_audio_bridge.cpp", r"^JNIEXPORT\b")),
    "jniexport-total": ("JNIEXPORT en todo jni/*.cpp",           _jniexport_total),
    "wma-api":         ("declaraciones WMA_API en la C API",     lambda: _contar_regex("audio/src/main/cpp/api/watermelon_audio.h", r"WMA_API")),
    "analysis-files":  ("fuentes .h/.cpp en cpp/analysis/ (sin tests/)", lambda: _fuentes_planas("audio/src/main/cpp/analysis")),
    "callers-baseline":("reparto del baseline de llamadores",    _baseline_reparto),
    "ver-kotlin":      ("version de Kotlin",                     lambda: _version_toml("kotlin")),
    "ver-agp":         ("version de AGP",                        lambda: _version_toml("agp")),
}


def medir_todo() -> dict[str, str]:
    """Mide TODAS las metricas. Una que no se pueda medir aborta: no hay 0 silencioso."""
    out: dict[str, str] = {}
    rotas: list[str] = []
    for nombre, (_desc, fn) in METRICAS.items():
        try:
            out[nombre] = str(fn())
        except NoSePudoMedir as e:
            rotas.append(f"{nombre}: {e}")
    if rotas:
        raise NoSePudoMedir(
            "no se pudieron medir " + str(len(rotas)) + " metrica(s):\n  - " + "\n  - ".join(rotas)
        )
    return out


# ---------------------------------------------------------------------------
# El bloque: leerlo, renderizarlo, escribirlo.
# ---------------------------------------------------------------------------

def render(valores: dict[str, str]) -> str:
    filas = "\n".join(
        f"| `{n}` | {valores[n]} | {METRICAS[n][0]} |" for n in METRICAS if n in valores
    )
    return (
        f"{BEGIN}\n"
        "| métrica | valor | qué mide |\n"
        "|---|---|---|\n"
        f"{filas}\n"
        f"{END}"
    )


def leer_bloque(texto: str) -> dict[str, str]:
    """Parsea el bloque. Ausente o corrupto => NoSePudoMedir (nunca un dict vacio)."""
    i, j = texto.find(BEGIN), texto.find(END)
    if i < 0 or j < 0 or j < i:
        raise NoSePudoMedir(
            "CLAUDE.md no tiene el bloque de conteos medidos (o esta partido). "
            "Generalo con: python3 scripts/check-doc-counts.py --update"
        )
    filas = dict(re.findall(r"^\|\s*`([^`]+)`\s*\|\s*([^|]+?)\s*\|", texto[i:j], re.MULTILINE))
    if not filas:
        raise NoSePudoMedir(
            "el bloque de CLAUDE.md existe pero no tiene una sola fila parseable: "
            "esta corrupto, y leerlo como 'sin metricas' lo dejaria verde para siempre"
        )
    return filas


def escribir_bloque(texto: str, valores: dict[str, str]) -> str:
    i, j = texto.find(BEGIN), texto.find(END)
    nuevo = render(valores)
    if i < 0 or j < 0:
        raise NoSePudoMedir("no hay bloque que actualizar: insertalo a mano UNA vez y despues --update lo mantiene")
    return texto[:i] + nuevo + texto[j + len(END):]


def comparar(escrito: dict[str, str], real: dict[str, str]) -> list[str]:
    problemas = []
    for n in METRICAS:
        if n not in escrito:
            problemas.append(f"{n}: falta en el bloque (real: {real[n]})")
        elif escrito[n] != real[n]:
            problemas.append(f"{n}: el bloque dice '{escrito[n]}' y el arbol dice '{real[n]}'  ({METRICAS[n][0]})")
    for n in escrito:
        if n not in METRICAS:
            problemas.append(f"{n}: el bloque declara una metrica que el script ya no mide")
    return problemas


# ---------------------------------------------------------------------------
# El self-test. Corre ANTES del lint, por la misma razon que check-rt-safety:
# si el parser se rompe, el lint queda verde para siempre.
# ---------------------------------------------------------------------------

def self_test() -> int:
    fallos = []

    # 🔴 `medir_todo()` va adentro de un try, y lo destapo una MUTACION: al romper el
    # regex de una metrica, el self-test explotaba con un traceback en vez de decir
    # "self-test ROJO — no pude medir X". El exit code era 1 igual, asi que el gate
    # fallaba — pero un traceback no NOMBRA la causa, y en este repo el diagnostico
    # es parte del entregable, no un lujo (misma regla que `JniHarness.loadDiagnosis`,
    # que existe justamente para no tragarse el mensaje).
    try:
        real = medir_todo()
    except NoSePudoMedir as e:
        print(f"\n{RED}self-test ROJO{OFF} — el guardrail no puede medir el arbol:\n  {e}")
        return 1
    if not real:
        fallos.append("medir_todo devolvio vacio")

    # (a) un valor alterado tiene que detectarse, y nombrar la metrica
    victima = "wma-api"
    alterado = dict(real)
    alterado[victima] = str(int(real[victima]) + 1)
    probs = comparar(alterado, real)
    if not any(victima in p for p in probs):
        fallos.append("un valor alterado NO se detecto")

    # (b) una metrica que falta en el bloque tiene que acusarse
    faltante = {k: v for k, v in real.items() if k != victima}
    if not any(victima in p and "falta" in p for p in comparar(faltante, real)):
        fallos.append("una metrica ausente del bloque NO se acuso")

    # (c) el bloque ausente NO puede leerse como "sin metricas"
    try:
        leer_bloque("no hay bloque aca")
        fallos.append("un bloque AUSENTE se leyo sin error")
    except NoSePudoMedir:
        pass

    # (d) el bloque corrupto tampoco
    try:
        leer_bloque(f"{BEGIN}\nbasura sin filas\n{END}")
        fallos.append("un bloque CORRUPTO se leyo sin error")
    except NoSePudoMedir:
        pass

    # (e) 🔴 el corazon de AC-021.2: una medicion rota FALLA, no devuelve 0
    try:
        _contar_regex("CLAUDE.md", r"^ESTE_PATRON_NO_EXISTE_EN_NINGUN_LADO$")
        fallos.append("un patron que no matchea devolvio un valor en vez de fallar")
    except NoSePudoMedir:
        pass
    try:
        _leer("no/existe/este/archivo.txt")
        fallos.append("un archivo ausente no fallo")
    except NoSePudoMedir:
        pass

    # (f) el archivo real tiene que estar completo y coincidir consigo mismo
    try:
        escrito = leer_bloque(DOC.read_text(encoding="utf-8"))
        if comparar(escrito, real) == [] and escrito != {k: real[k] for k in escrito}:
            fallos.append("comparar() dijo OK sobre valores distintos")
    except NoSePudoMedir:
        pass  # que el bloque no exista todavia es legitimo antes del primer --update

    if fallos:
        print(f"\n{RED}self-test ROJO{OFF} — {len(fallos)} caso(s):")
        for f in fallos:
            print(f"  - {f}")
        return 1
    print(f"{GREEN}self-test verde{OFF} — el guardrail sabe fallar "
          f"({len(METRICAS)} metricas, {len(real)} medidas del arbol).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="REQ-021 — los conteos de CLAUDE.md, re-medidos del arbol.")
    ap.add_argument("--self-test", action="store_true",
                    help="verifica que el guardrail SABE fallar. Corre ANTES del lint.")
    ap.add_argument("--update", action="store_true",
                    help="reescribe el bloque con lo medido. SU DIFF ES LA REVISION.")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    try:
        real = medir_todo()
    except NoSePudoMedir as e:
        print(f"{RED}FALLA{OFF} — {e}")
        print("\n  Una metrica que no se pudo medir NO se degrada a 0: eso publicaria una")
        print("  medicion inventada, que es exactamente lo que este guardrail existe para evitar.")
        return 1

    texto = DOC.read_text(encoding="utf-8")

    if args.update:
        try:
            DOC.write_text(escribir_bloque(texto, real), encoding="utf-8")
        except NoSePudoMedir as e:
            print(f"{RED}FALLA{OFF} — {e}")
            return 1
        print(f"{GREEN}bloque actualizado{OFF} — {len(real)} metricas. Revisa el diff: ES la revision.")
        return 0

    try:
        escrito = leer_bloque(texto)
    except NoSePudoMedir as e:
        print(f"{RED}FALLA{OFF} — {e}")
        return 1

    problemas = comparar(escrito, real)
    if problemas:
        print(f"{RED}FALLA{OFF} — {len(problemas)} conteo(s) de CLAUDE.md no coinciden con el arbol:\n")
        for p in problemas:
            print(f"  - {p}")
        print("\n  Un numero escrito a mano envejece en silencio: nadie lo lee como 'esto puede")
        print("  estar viejo', se lee como un hecho. Re-medilo y revisa el diff:")
        print("      python3 scripts/check-doc-counts.py --update")
        return 1

    print(f"doc-counts — {len(real)} metricas medidas del arbol, todas coinciden con CLAUDE.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
