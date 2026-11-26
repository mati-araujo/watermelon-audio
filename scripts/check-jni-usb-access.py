#!/usr/bin/env python3
"""MINI-042 (D8) — cada JNIEXPORT entra al LibusbBackend por el accesor que le toca.

MINI-042 sacó el puntero crudo `BackendManager::getLibusbBackend()`, con el que
`fallbackToOboe()` liberaba el backend debajo de la JNI (M1). Quedaron dos accesos
con alcance, y elegir mal uno de los dos es un defecto que NINGÚN test de host ve,
porque el arnés JNI compila un `LibusbBackend` vacío:

  * `withLibusbBackend(fn)` — `fn` corre con `mMutex`, el lock de estado que
    pregunta Main (el health-check, cada segundo). Sólo lecturas y configuraciones
    CORTAS.
  * `withLibusbBackendLifecycle(fn)` — `fn` corre con `mOpMutex` y `mMutex` LIBRE.
    Para `start()`, `stop()` y la selección de altsetting/reloj, que toman el mutex
    del backend con `lock()` y por eso esperan a un start en curso.

Un `start()` adentro de `withLibusbBackend` compila, pasa toda la suite y deja a
Main congelado lo que tarde el device en arrancar. El smoke en device tampoco lo
ve: un Main trabado unos cientos de ms no hace fallar ninguna fila. Este lint es
lo único que lo pone rojo.

## Qué afirma

  1. `getLibusbBackend` no aparece en `jni/` (fuera de comentarios).
  2. Toda función de `jni/` que nombra `LibusbBackend` o usa un accesor está en
     `CLASIFICACION`, con su accesor. Una nueva sin clasificar FALLA: es la decisión
     que alguien tiene que tomar, no un default.
  3. Cada función clasificada usa SU accesor y no el otro.
  4. Adentro de un `withLibusbBackend(...)` no hay llamadas a `start`, `stop`,
     `selectAltsetting`, `selectClockSource` ni `clearManual*` del backend.
  5. TRINQUETE: una entrada de `CLASIFICACION` que ya no está en el árbol FALLA.

Source-only: sin .so, sin NDK. `--self-test` corre ANTES (misma razón que
`check-rt-safety`: un parser roto deja el lint verde para siempre) y demuestra que
el mutante del review de MINI-042 (I2a: el start de `nativeStartUsbStreamingWithMode`
por `withLibusbBackend`) sale rojo sobre el árbol REAL.

## Qué no afirma

Que el `fn` sea corto de verdad: eso es criterio, y está en la tabla. Ni que las
lambdas hagan lo correcto con el backend: esa semántica la cubren la suite de C++
(`libusb_backend_lifetime_tests`) y el smoke en device.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
JNI_DIR = ROOT / "audio/src/main/cpp/jni"

LIFECYCLE = "withLibusbBackendLifecycle"
SHORT = "withLibusbBackend"

_P = "Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_"

# La tabla de MINI-042 (D5): 6 de ciclo de vida, 17 cortas. Las razones van en la spec.
CLASIFICACION: dict[str, str] = {
    # ---- ciclo de vida: start/stop/select toman el mutex del backend con lock()
    _P + "nativeCloseUsbDevice": LIFECYCLE,
    _P + "nativeStartUsbStreaming": LIFECYCLE,
    _P + "nativeStartUsbStreamingWithMode": LIFECYCLE,
    _P + "nativeStopUsbStreaming": LIFECYCLE,
    _P + "nativeSelectUsbAltsetting": LIFECYCLE,
    _P + "nativeSelectUsbClockSource": LIFECYCLE,
    # ---- cortas: lecturas y configuraciones que no esperan a nadie
    _P + "nativeParseUsbDescriptors": SHORT,
    _P + "nativeGetUsbTransferStats": SHORT,
    _P + "nativeUsbDeviceSupportsFullDuplex": SHORT,
    _P + "nativeUsbDeviceHasCapture": SHORT,
    _P + "nativeGetUsbDeviceUacVersion": SHORT,
    _P + "nativeGetUsbCapabilitySnapshot": SHORT,
    _P + "nativeSetUsbStreamPreference": SHORT,
    _P + "nativeSetUsbLatencyTuning": SHORT,
    _P + "nativeIsUsbDeviceDisconnected": SHORT,
    _P + "nativeGetUsbHealthStatus": SHORT,
    "rtRestoreCallbackLocked": SHORT,
    _P + "nativeUsbRoundTripStart": SHORT,
    _P + "nativeUsbRoundTripPoll": SHORT,
    _P + "nativeGetUsbRtEnv": SHORT,
    _P + "nativeGetUsbProfilingStats": SHORT,
    _P + "nativeSetUsbProfilingEnabled": SHORT,
    _P + "nativeResetUsbProfilingStats": SHORT,
}

FORBIDDEN_IN_SHORT = re.compile(
    r"->\s*(start|stop|selectAltsetting|selectClockSource|clearManual\w*)\s*\(")
CONTROL = {"if", "for", "while", "switch", "catch", "return", "sizeof", "decltype"}
HEADER = re.compile(r"\b(\w+)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->[^{};]*)?$", re.S)


def strip(src: str) -> str:
    """Comentarios y literales a espacios, conservando largo y saltos de línea."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in src[i:j])); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * (j - i - 2) + (c if j - i >= 2 else "")); i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def functions(code: str) -> list[tuple[str, int, int]]:
    """(nombre, inicio_del_cuerpo, fin) de cada función definida a nivel de namespace."""
    found = []
    stack: list[str] = []  # 'ns' | 'fn' | 'block'
    last_stmt_end = 0
    current = None
    for i, c in enumerate(code):
        if c == "{":
            head = code[last_stmt_end:i].strip()
            if all(k == "ns" for k in stack):
                m = HEADER.search(head)
                if re.search(r"\bnamespace\b|extern\s+\"\s*\"", head) or head.startswith("extern"):
                    stack.append("ns")
                elif m and m.group(1) not in CONTROL:
                    stack.append("fn")
                    current = (m.group(1), i)
                else:
                    stack.append("block")
            else:
                stack.append("block")
            last_stmt_end = i + 1
        elif c == "}":
            kind = stack.pop() if stack else "ns"
            if kind == "fn" and current:
                found.append((current[0], current[1], i))
                current = None
            last_stmt_end = i + 1
        elif c == ";":
            last_stmt_end = i + 1
    return found


def call_spans(code: str, start: int, end: int, name: str) -> list[tuple[int, int]]:
    """Los argumentos (entre paréntesis balanceados) de cada llamada a `name` en [start, end)."""
    spans = []
    for m in re.finditer(r"\b" + name + r"\s*\(", code[start:end]):
        i = start + m.end()
        depth = 1
        j = i
        while j < end and depth:
            depth += {"(": 1, ")": -1}.get(code[j], 0)
            j += 1
        spans.append((i, j))
    return spans


def check(sources: dict[str, str], table: dict[str, str]) -> list[str]:
    errors = []
    seen: set[str] = set()
    for fname, src in sources.items():
        code = strip(src)
        line = lambda pos: code.count("\n", 0, pos) + 1  # noqa: E731
        for m in re.finditer(r"\bgetLibusbBackend\b", code):
            errors.append(f"{fname}:{line(m.start())}: `getLibusbBackend` volvió: el puntero crudo "
                          "es el uso de memoria liberada de MINI-042 (M1)")
        for name, start, end in functions(code):
            body = code[start:end]
            uses_short = bool(re.search(r"\b" + SHORT + r"\s*\(", body))
            uses_life = bool(re.search(r"\b" + LIFECYCLE + r"\s*\(", body))
            touches = uses_short or uses_life or re.search(r"\bLibusbBackend\b", body)
            if not touches:
                continue
            seen.add(name)
            where = f"{fname}:{line(start)}: {name}"
            expected = table.get(name)
            if expected is None:
                errors.append(f"{where}: toca el LibusbBackend y no está clasificada. Decidí si es "
                              f"corta ({SHORT}) o de ciclo de vida ({LIFECYCLE}) y agregala a CLASIFICACION")
                continue
            if expected == LIFECYCLE and (uses_short or not uses_life):
                errors.append(f"{where}: es de ciclo de vida y no entra (sólo) por {LIFECYCLE}")
            if expected == SHORT and (uses_life or not uses_short):
                errors.append(f"{where}: es corta y no entra (sólo) por {SHORT}")
            for a, b in call_spans(code, start, end, SHORT):
                for f in FORBIDDEN_IN_SHORT.finditer(code, a, b):
                    errors.append(f"{fname}:{line(f.start())}: {name}: `{f.group(1)}` adentro de "
                                  f"{SHORT}: retiene el lock de estado (el que pregunta Main) "
                                  "durante una operación lenta")
    for name in sorted(set(table) - seen):
        errors.append(f"CLASIFICACION: `{name}` ya no toca el backend en jni/ — trinquete: sacala o "
                      "explicá por qué cambió")
    return errors


def load_tree() -> dict[str, str]:
    files = sorted(JNI_DIR.glob("*.cpp"))
    if not files:
        sys.exit(f"FALLA — no encontré fuentes .cpp en {JNI_DIR}. Sin ellas no hay nada que revisar.")
    return {f.name: f.read_text(encoding="utf-8") for f in files}


# ============================== self-test ==============================

def self_test() -> int:
    print("self-test de check-jni-usb-access:")
    failures = []

    def expect(label: str, sources: dict[str, str], table: dict[str, str], needle: str | None):
        errs = check(sources, table)
        ok = (not errs) if needle is None else any(needle in e for e in errs)
        print(f"  {'ok ' if ok else 'MAL'} {label}")
        if not ok:
            failures.append(f"{label}: {errs}")

    good = """
extern "C" {
JNIEXPORT jint JNICALL Java_x_start(JNIEnv*, jobject) {
    // getLibusbBackend en un comentario no cuenta; "withLibusbBackend(" en un string tampoco
    return mgr.withLibusbBackendLifecycle([&](LibusbBackend* b) -> jint { b->start(); return 0; });
}
JNIEXPORT jboolean JNICALL Java_x_read(JNIEnv*, jobject) {
    return mgr.withLibusbBackend([](LibusbBackend* b) { if (b) { return b->isRunning(); } return false; });
}
JNIEXPORT void JNICALL Java_x_other(JNIEnv*, jobject) { int a = 0; (void)a; }
}
namespace {
void helper() { mgr.withLibusbBackend([](LibusbBackend* b) { b->swapCallback(nullptr); }); }
}
"""
    table = {"Java_x_start": LIFECYCLE, "Java_x_read": SHORT, "helper": SHORT}
    expect("un árbol correcto da verde", {"a.cpp": good}, table, None)
    expect("start adentro de withLibusbBackend es rojo",
           {"a.cpp": good.replace("withLibusbBackendLifecycle(", "withLibusbBackend(")},
           table, "adentro de withLibusbBackend")
    expect("una de ciclo de vida por el accesor corto es rojo",
           {"a.cpp": good.replace("withLibusbBackendLifecycle(", "withLibusbBackend(")},
           table, "es de ciclo de vida")
    expect("una corta por el accesor de ciclo de vida es rojo",
           {"a.cpp": good.replace("return mgr.withLibusbBackend([]", "return mgr.withLibusbBackendLifecycle([]")},
           table, "es corta")
    expect("stop/select/clearManual adentro de withLibusbBackend son rojos",
           {"a.cpp": good.replace("b->swapCallback(nullptr)", "b->clearManualClockSourceSelection()")},
           table, "`clearManualClockSourceSelection` adentro")
    expect("getLibusbBackend que vuelve es rojo",
           {"a.cpp": good + "\nvoid h2() { auto* b = mgr.getLibusbBackend(); }\n"},
           {**table, "h2": SHORT}, "volvió")
    expect("una función nueva que toca el backend sin clasificar es rojo",
           {"a.cpp": good.replace("int a = 0; (void)a;", "mgr.withLibusbBackend([](LibusbBackend*) {});")},
           table, "no está clasificada")
    expect("una entrada de la tabla que ya no existe es rojo (trinquete)",
           {"a.cpp": good}, {**table, "Java_x_gone": SHORT}, "ya no toca el backend")

    # El mutante del review (I2a) sobre el árbol REAL.
    tree = load_tree()
    expect("el árbol real da verde", tree, CLASIFICACION, None)
    src = tree["jni_audio_bridge.cpp"]
    target = "nativeStartUsbStreamingWithMode"
    i = src.index(target + "(")
    j = src.index(LIFECYCLE + "(", i)
    mutated = src[:j] + SHORT + src[j + len(LIFECYCLE):]
    expect("I2a: nativeStartUsbStreamingWithMode por withLibusbBackend es rojo en el árbol real",
           {**tree, "jni_audio_bridge.cpp": mutated}, CLASIFICACION, "adentro de withLibusbBackend")
    n_life = sum(1 for v in CLASIFICACION.values() if v == LIFECYCLE)
    n_short = sum(1 for v in CLASIFICACION.values() if v == SHORT)
    shape = (n_life, n_short) == (6, 17)
    print(f"  {'ok ' if shape else 'MAL'} la tabla tiene 6 de ciclo de vida y 17 cortas ({n_life}/{n_short})")
    if not shape:
        failures.append("forma de la tabla")

    if failures:
        print("\n\033[31mself-test FALLA\033[0m — el lint no puede fallar como debe:")
        for f in failures:
            print("  - " + f)
        return 1
    print("\n\033[32mself-test ok\033[0m — el lint puede fallar.")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    tree = load_tree()
    errors = check(tree, CLASIFICACION)
    if errors:
        print("jni-usb-access — FALLA:")
        for e in errors:
            print("  " + e)
        return 1
    print(f"jni-usb-access — {len(CLASIFICACION)} funciones de jni/ entran al LibusbBackend por su "
          f"accesor (6 de ciclo de vida, 17 cortas); ningún getLibusbBackend; nada lento bajo {SHORT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
