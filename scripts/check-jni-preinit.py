#!/usr/bin/env python3
"""REQ-045 AC-045.4 — una configuracion llamada antes del init NO se pierde.

La otra forma de "un fallo que no cruza la frontera" (D2 del analisis de las 25
cartas de NoisyPad) no es un resultado tirado: es un **no-op mudo**.

    JNIEXPORT void JNICALL ...nativeSetBpm(JNIEnv*, jobject, jfloat bpm) {
        wma_set_bpm(g_wmaEngine, bpm);   // g_wmaEngine es nullptr -> no paso nada
    }

La C API rechaza el handle nulo y devuelve, asi que no hay crash, no hay log y no
hay valor de retorno: el consumidor configuro el motor y **el motor nunca se
entero**. NoisyPad lo mitiga a mano re-aplicando su configuracion despues del
arranque; medido en S0 de este REQ, eso le pasaba a **50** configuraciones.

La decision 2 del REQ es que la configuracion LLEGA: `ensureEngine()` crea el
motor sin abrir el stream, asi que es la opcion "encolar" de la carta, gratis.
Este lint es lo que impide que una configuracion nueva vuelva a perderse.

## La pregunta que contesta, y las que no

  * `check-jni-symbols.py` (MINI-001) — "?el simbolo EXISTE en el .so?"
  * `check-jni-signatures.py` (REQ-025) — "?la FIRMA coincide de los dos lados?"
  * `check-jni-results.py` (REQ-045 S1) — "?el resultado que devolvio LLEGA?"
  * el arnes de `androidUnitTest` — "?alguien lo EJECUTA?"
  * **este** — "?la configuracion llega cuando todavia no hay motor?"

Son cinco preguntas distintas sobre el mismo cruce. Una entrada puede tener
nombre, firma y resultado impecables y seguir siendo un no-op mudo: el
`WmaResult` que devuelve `wma_set_modulator_type` sobre un handle nulo es un
`WMA_ERROR_NOT_INITIALIZED` perfectamente propagado — y el consumidor igual tiene
que re-aplicar todo a mano. Propagar el fallo y **hacer lo pedido** no son lo
mismo.

## Quien esta en el conjunto (la regla de S0, no una lista a mano)

Una `JNIEXPORT` entra al conjunto si (a) pasa el handle del motor
(`g_wmaEngine` / `g_jniState.engine`) y (b) alguna `wma_*` que llama tiene un
**verbo de configuracion** (`set`, `enable`, `add`, `remove`, `clear`,
`reorder`, `register`, `unregister`, `lock`, `regenerate`). Las lecturas
(`get`/`is`/`has`/...) no entran: pre-init devuelven el default que la C API
declara, y crear el motor para contestar una lectura seria un efecto que nadie
pidio. Las acciones (`start`, `trigger`, `export`, ...) tampoco: no son estado
que persista.

🔴 **La regla se deriva del arbol en cada corrida.** Una lista de nombres escrita
a mano envejece en silencio —le paso cuatro veces seguidas al bloque de conteos
de `CLAUDE.md`— y una configuracion nueva no aparece en ella sola.

## Las exclusiones, con razon obligatoria y TRINQUETE

`EXCLUIR` son las que pasan el filtro pero NO tienen que crear el motor, cada una
con su razon (las mismas de S0, revisadas una por una). El trinquete es
bidireccional: una exclusion cuyo nombre **ya no esta** en el conjunto derivado
FALLA, porque una exclusion que no se reproduce es una excepcion que nadie
revisa — la misma regla que `rt-safety-baseline.txt` y
`mechanism-callers-baseline.txt`.

## El conjunto versionado, y por que hay un archivo

`scripts/jni-preinit-config.txt` lleva el conjunto derivado, y el lint falla si
el arbol no coincide con el en cualquier direccion. Existe porque tiene **dos
consumidores**: este lint y el arnes de `androidUnitTest`
(`PreInitConfigJniTest`), que ejecuta cada una contra un `JNIEnv` real. Sin el
archivo, el arnes tendria su propia lista escrita a mano y las dos derivarian
por separado. Se redeclara con `--update`, y **su diff es la revision**.

## La guarda de completitud es lo que lo sostiene

Si el parser abarca menos `JNIEXPORT` de las que el arbol declara, **falla**: un
cuerpo con forma inesperada que quede sin revisar en silencio deja el lint verde
revisando menos, que es el modo de falla que este repo ya pago dos veces con
`rt-coverage-baseline`.

Uso:
    python3 scripts/check-jni-preinit.py              # el lint
    python3 scripts/check-jni-preinit.py --self-test  # ?puede fallar?
    python3 scripts/check-jni-preinit.py --update     # redeclara el conjunto
"""

from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
JNI_DIR = REPO / "audio/src/main/cpp/jni"
CONJUNTO = REPO / "scripts/jni-preinit-config.txt"

# Toda JNIEXPORT, con o sin JNICALL (`JNI_OnLoad` no lo lleva): el denominador de
# la guarda de completitud.
EXPORT_LINE = re.compile(r"^JNIEXPORT\b", re.MULTILINE)
EXPORT_DEF = re.compile(r"JNIEXPORT\s+(?P<ret>[\w:*&<> ]+?)\s+(?:JNICALL\s+)?(?P<name>\w+)\s*\(")

HANDLE = re.compile(r"\bg_wmaEngine\b|\bg_jniState\.engine\b")
ENSURE = re.compile(r"\bensure(Engine|InputNode)\s*\(")
WMA_CALL = re.compile(r"\b(wma_\w+)\s*\(")

# 🔴 Los COMENTARIOS se sacan antes de mirar el cuerpo, y lo destapo un mutante de 2.8:
# el comentario que EXPLICA por que va `ensureEngine()` contiene el texto
# `ensureEngine()`, asi que borrar la llamada y dejar la explicacion dejaba el lint en
# VERDE. Un lint que lee la prosa como si fuera codigo es exactamente la clase de falso
# verde que este REQ existe para borrar. Misma mecanica que `check-jni-results.py`.
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)

# Los verbos. `CONFIG` es estado que persiste; `LECTURA` devuelve el default que la C
# API declara; el resto son acciones.
#
# 🔴 **La tabla FALLA CERRADO**: una `wma_*` cuyo nombre no matchea ningun verbo hace
# fallar el lint pidiendo que se la clasifique. La primera version devolvia `False` en
# silencio, o sea que un nombre nuevo quedaba VERDE por no estar en la tabla — y eso es
# el modo de falla que este repo ya pago dos veces con `rt-coverage-baseline`: el lint
# revisando menos sin cambiar de color. Medido en el review de S2: habia **20** `wma_*`
# llamadas desde `jni/` sin ningun verbo reconocido, entre ellas las tres de carga de
# SoundFont, y sacarle el `ensureEngine()` a `nativeLoadSoundFontFromPath` daba VERDE.
CONFIG = {"set", "enable", "add", "remove", "clear", "reorder", "register",
          "unregister", "lock", "regenerate", "load", "unload"}
LECTURA = {"get", "is", "has", "find", "detect", "analyze", "count",
           "size", "state", "difference", "requires", "line", "frames"}
ACCION = {"start", "stop", "pause", "resume", "trigger", "release", "update", "arm",
          "cancel", "export", "import", "capture", "prepare", "trim", "save",
          "restore", "reset", "finalize", "apply",
          "create", "destroy", "free", "abort", "note", "select"}

# Configuracion que NO nombra el handle y entra al conjunto de todos modos, con su razon.
#
# 🔴 Viaja por `BackendManager::getInstance()`, y ahi esta el defecto: sin motor ese
# `getInstance()` devuelve un **static de fallback**, el pedido queda anotado en ESE
# manager, y `wma_engine_create` instala otro — asi que la configuracion se pierde
# exactamente igual que en D2, pero la regla del handle no la ve. Medido en el review de
# S2. La otra familia que pasa por el singleton (`getLibusbBackend()`, ~25 entradas) NO
# esta aca a proposito: son operaciones sobre un dispositivo USB que tiene que existir,
# no configuracion latcheada del manager.
SIEMPRE = {
    "nativeSetUsbStreamingMode":
        "el pedido de captura se anota en el BackendManager, no en el motor: sin "
        "ensureEngine queda en el static de fallback y se pierde al crearse el motor",
    "nativeConfigureUsbBackend":
        "idem con setSampleRate(), y encima es void: se pierde sin rastro",
    "nativeSetUsbLatencyProfile":
        "idem con setLatencyProfile(), que el manager LATCHEA para reaplicarlo en el "
        "start siguiente — y devuelve JNI_TRUE igual",
}

# Las que pasan el filtro y NO tienen que crear el motor. Cada una con su razon:
# una exclusion sin motivo escrito es la que nadie puede revisar.
EXCLUIR = {
    "nativeClearStreamError": "limpia un error: sin motor no hay error que limpiar",
    "nativeRemoveEffect": "quitar: sin motor no hay que quitar, y ya devuelve jint",
    "nativeClearAllEffects": "limpiar: sin motor no hay que limpiar, y ya devuelve jint",
    "nativeLooperClearTrack": "limpiar: sin motor no hay contenido",
    "nativeLooperClearAll": "limpiar: sin motor no hay contenido",
    "nativeSetEffectParameter": "direcciona un efecto por indice (cadena vacia pre-init); ya devuelve jint",
    "nativeSetEffectParametersBatch": "idem, y ya consulta wma_is_initialized",
    "nativeSetMultipleEffectParameters": "idem, y ya consulta wma_is_initialized",
    "nativeSetEffectBypass": "direcciona un efecto por indice; ya devuelve jint",
    "nativeReorderEffects": "direcciona efectos por indice; ya devuelve jint",
    "nativeSetTunerTarget": "ya devuelve jboolean (false pre-init)",
    "nativeSetTunerCandidates": "ya devuelve jboolean",
    "nativeLockTunerString": "ya devuelve jboolean",
    "nativeRegenerateArpPattern": "accion, no configuracion",
    "nativeSetDualTouch": "estado de toque en tiempo real, no configuracion que persiste",
    "nativeSetAutomationParameter": "valor de automatizacion en tiempo real",
    "nativeSetArpTouchActive": "estado de toque en tiempo real",
    "nativeSetArpBaseFrequency": "estado de toque en tiempo real",
    "nativeLooperSetTrackLoopRegion": "necesita contenido grabado",
}


def function_bodies(src: str) -> list[tuple[str, str]]:
    """(nombre, cuerpo) de cada `JNIEXPORT`, por balanceo de llaves.

    Misma mecanica que `check-jni-results.py`: un `;` antes del `{` es un
    PROTOTIPO y el `{` encontrado pertenece a la funcion siguiente. Sin esa
    guarda el parser cuenta un cuerpo ajeno y anula la guarda de completitud.
    """
    out: list[tuple[str, str]] = []
    for m in EXPORT_DEF.finditer(src):
        brace = src.find("{", m.end())
        if brace < 0:
            continue
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
        out.append((m.group("name"), COMMENT.sub(" ", src[brace : i + 1])))
    return out


def clasificar(call: str) -> str | None:
    """"config" / "lectura" / "accion" para una `wma_*`, o None si su verbo es desconocido.

    None NO es "no es configuracion": es "no se sabe", y el llamador lo convierte en
    FALLA. Un verbo nuevo tiene que aparecer en el diff del PR, no volverse verde solo.
    """
    for parte in call[4:].split("_"):
        if parte in CONFIG:
            return "config"
        if parte in LECTURA:
            return "lectura"
        if parte in ACCION:
            return "accion"
    return None


def es_configuracion(body: str) -> tuple[bool, list[str]]:
    """(¿configura?, las `wma_*` del cuerpo cuyo verbo no se pudo clasificar)."""
    configura = False
    sin_clasificar: list[str] = []
    for call in WMA_CALL.findall(body):
        k = clasificar(call)
        if k is None:
            sin_clasificar.append(call)
        elif k == "config":
            configura = True
    return configura, sin_clasificar


def corto(name: str) -> str:
    return name.rsplit("_", 1)[-1]


def scan(sources: dict[str, str], siempre: dict[str, str] | None = None
         ) -> tuple[dict[str, bool], int, int, set[str], set[str]]:
    """(conjunto -> tiene ensureEngine, parseadas, declaradas, verbos sin clasificar, nombres vistos)."""
    siempre = SIEMPRE if siempre is None else siempre
    bucket: dict[str, bool] = {}
    desconocidos: set[str] = set()
    vistos: set[str] = set()
    parsed = declared = 0
    for src in sources.values():
        declared += len(EXPORT_LINE.findall(src))
        for name, body in function_bodies(src):
            parsed += 1
            n = corto(name)
            vistos.add(n)
            configura, sin_clasificar = es_configuracion(body)
            desconocidos |= set(sin_clasificar)
            # `SIEMPRE` entra aunque no pase el handle: hay configuracion que viaja por el
            # singleton del BackendManager y nunca nombra `g_wmaEngine`.
            if (HANDLE.search(body) and configura) or n in siempre:
                bucket[n] = bool(ENSURE.search(body))
    return bucket, parsed, declared, desconocidos, vistos


def declarado() -> list[str] | None:
    if not CONJUNTO.is_file():
        return None
    return [
        l.strip()
        for l in CONJUNTO.read_text(encoding="utf-8").splitlines()
        if l.strip() and not l.startswith("#")
    ]


def analyze(sources: dict[str, str], esperado: list[str] | None,
            excluir: dict[str, str] | None = None,
            siempre: dict[str, str] | None = None) -> list[str]:
    """Todos los problemas. Lista vacia = verde. La completitud va PRIMERO."""
    excluir = EXCLUIR if excluir is None else excluir
    siempre = SIEMPRE if siempre is None else siempre
    problems: list[str] = []

    bucket, parsed, declared, desconocidos, vistos = scan(sources, siempre)
    if desconocidos:
        problems.append(
            "VERBO SIN CLASIFICAR: " + ", ".join(sorted(desconocidos)) + "\n"
            "  El lint no sabe si eso es configuracion, lectura o accion, y por eso FALLA en\n"
            "  vez de suponer que no lo es: un verbo nuevo que devuelve 'no es configuracion'\n"
            "  en silencio deja el lint verde revisando menos.\n"
            "  Clasificalo en CONFIG / LECTURA / ACCION de este script."
        )
    huerfanas_siempre = sorted(set(siempre) - vistos)
    if huerfanas_siempre:
        problems.append(
            "SIEMPRE HUERFANAS: " + ", ".join(huerfanas_siempre) + "\n"
            "  Ya no existen como JNIEXPORT, asi que su razon no se puede revisar contra nada."
        )
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
    if not bucket:
        problems.append(
            "el parser no encontro UNA sola entrada de configuracion que pase el handle. "
            "Eso no es 'nada que revisar': es un parseo roto, y publicarlo como verde seria "
            "inventar la verificacion."
        )
        return problems

    # El trinquete de las exclusiones: una que ya no se reproduce es una excepcion
    # que nadie revisa.
    huerfanas = sorted(set(excluir) - set(bucket))
    if huerfanas:
        problems.append(
            "EXCLUSIONES HUERFANAS: " + ", ".join(huerfanas) + "\n"
            "  Ya no estan en el conjunto derivado del arbol, asi que su razon no se puede\n"
            "  revisar contra nada. Borralas de EXCLUIR."
        )

    requerido = sorted(n for n in bucket if n not in excluir)

    faltan = [n for n in requerido if not bucket[n]]
    for n in faltan:
        problems.append(
            f"{n} configura el motor y NO llama a ensureEngine(): llamada antes del init, "
            "la configuracion se pierde sin rastro (D2)."
        )

    if esperado is None:
        problems.append(
            f"no encontre {CONJUNTO.name}. El conjunto es el denominador del arnes: sin el, "
            "no hay contra que comparar. Redeclaralo con --update."
        )
    elif esperado != requerido:
        nuevas = sorted(set(requerido) - set(esperado))
        idas = sorted(set(esperado) - set(requerido))
        detalle = ""
        if nuevas:
            detalle += "\n  NUEVAS en el arbol: " + ", ".join(nuevas)
        if idas:
            detalle += "\n  YA NO estan en el arbol: " + ", ".join(idas)
        problems.append(
            f"CONJUNTO: el arbol deriva {len(requerido)} configuraciones y "
            f"{CONJUNTO.name} declara {len(esperado)}." + detalle + "\n"
            "  El trinquete es bidireccional a proposito: sumar o sacar una configuracion\n"
            "  tiene que aparecer en el diff del PR, porque el arnes ejerce exactamente\n"
            "  esta lista. Redeclaralo con --update."
        )
    return problems


def read_tree() -> dict[str, str]:
    sources = {
        p: Path(p).read_text(encoding="utf-8", errors="replace")
        for p in sorted(glob.glob(str(JNI_DIR / "*.cpp")))
    }
    if not sources:
        sys.exit(f"FALLA — no encontre fuentes .cpp en {JNI_DIR}. Sin ellas no hay nada que revisar.")
    return sources


# ============================== self-test ==============================
#
# Corre ANTES del lint en gate.sh y en el CI, por la misma razon que
# check-rt-safety, check-jni-signatures y check-jni-results: si el parser se
# rompe, un lint sin self-test queda verde para siempre y nadie se entera.

PERDIDA = """
JNIEXPORT void JNICALL
Java_x_nativeSetAlgo(JNIEnv* env, jobject thiz, jfloat v) {
    wma_set_algo(g_wmaEngine, v);
}
"""
SANA = """
JNIEXPORT void JNICALL
Java_x_nativeSetAlgo(JNIEnv* env, jobject thiz, jfloat v) {
    if (!ensureEngine()) return;
    wma_set_algo(g_wmaEngine, v);
}
"""


def self_test() -> int:
    fallos: list[str] = []

    def check(nombre: str, ok: bool, detalle: str = "") -> None:
        print(f"  {'ok  ' if ok else 'FALLA'}  {nombre}{'' if ok else ' — ' + detalle}")
        if not ok:
            fallos.append(nombre)

    print("self-test de check-jni-preinit:")

    real = analyze(read_tree(), declarado())
    check("el arbol real sale verde", not real, str(real[:3]))

    # El mutante que importa: una configuracion nueva SIN ensureEngine.
    check(
        "mata un setter nuevo que se pierde pre-init (AC-045.4)",
        any("NO llama a ensureEngine" in p
            for p in analyze({"v.cpp": PERDIDA}, ["nativeSetAlgo"], {}, {})),
    )
    check(
        "NO acusa al mismo setter cuando llama ensureEngine",
        not analyze({"v.cpp": SANA}, ["nativeSetAlgo"], {}, {}),
        str(analyze({"v.cpp": SANA}, ["nativeSetAlgo"], {}, {})[:1]),
    )

    # Las lecturas y las acciones no entran al conjunto: crear el motor para
    # contestar una lectura seria un efecto que nadie pidio.
    for forma, cuerpo in (
        ("una lectura", PERDIDA.replace("nativeSetAlgo", "nativeGetAlgo")
                               .replace("wma_set_algo", "wma_get_algo")),
        ("una accion", PERDIDA.replace("nativeSetAlgo", "nativeTriggerAlgo")
                              .replace("wma_set_algo", "wma_trigger_algo")),
    ):
        problemas = analyze({"v.cpp": cuerpo}, [], {}, {})
        check(f"NO mete {forma} en el conjunto",
              not any("ensureEngine" in p for p in problemas), str(problemas[:1]))

    # Una entrada que no pasa el handle tampoco: no hay motor que perder.
    sin_handle = PERDIDA.replace("g_wmaEngine, ", "")
    check("NO mete una entrada que no pasa el handle",
          not any("ensureEngine" in p for p in analyze({"v.cpp": sin_handle}, [], {}, {})))

    # 🔴 El COMENTARIO que menciona `ensureEngine()` NO cuenta como la llamada. Lo
    # destapo un mutante de 2.8: borrar la llamada y dejar su explicacion al lado dejaba
    # el lint en VERDE, porque el regex encontraba el texto en la prosa.
    SOLO_COMENTADO = PERDIDA.replace(
        "    wma_set_algo(g_wmaEngine, v);",
        "    // ensureEngine() va primero, ver el review de S2\n    wma_set_algo(g_wmaEngine, v);")
    check(
        "un comentario que nombra ensureEngine() NO excusa (M19)",
        any("NO llama a ensureEngine" in p
            for p in analyze({"v.cpp": SOLO_COMENTADO}, ["nativeSetAlgo"], {}, {})),
    )

    # 🔴 El VERBO DESCONOCIDO falla, no se supone. Son los dos casos del review de S2: un
    # nombre nuevo cuyo verbo no esta en la tabla quedaba VERDE en silencio, y con el
    # `ensureEngine` sacado tambien.
    DESCONOCIDO = """
JNIEXPORT void JNICALL
Java_x_nativeConfigureAgc(JNIEnv* env, jobject thiz, jfloat v) {
    wma_input_configure_agc(g_wmaEngine, v);
}
"""
    check(
        "mata un verbo que no esta clasificado (M12)",
        any("VERBO SIN CLASIFICAR" in p for p in analyze({"v.cpp": DESCONOCIDO}, [], {}, {})),
    )
    check(
        "lo mata TAMBIEN cuando el setter si llama ensureEngine",
        any("VERBO SIN CLASIFICAR" in p
            for p in analyze({"v.cpp": DESCONOCIDO.replace(
                "    wma_input_configure_agc", "    if (!ensureInputNode()) return;\n    wma_input_configure_agc")},
                ["nativeConfigureAgc"], {}, {})),
    )
    # Un verbo de CARGA es configuracion: `load` estaba en el limbo y por eso sacarle el
    # ensureEngine a nativeLoadSoundFontFromPath daba verde (M13).
    CARGA = PERDIDA.replace("nativeSetAlgo", "nativeLoadAlgoFromPath").replace(
        "wma_set_algo(g_wmaEngine, v)", "wma_algo_load_path(g_wmaEngine, \"x\")")
    check(
        "mata una CARGA sin ensureEngine (M13)",
        any("NO llama a ensureEngine" in p
            for p in analyze({"v.cpp": CARGA}, ["nativeLoadAlgoFromPath"], {}, {})),
    )

    # `SIEMPRE`: entra al conjunto aunque el cuerpo no nombre el handle...
    SINGLETON = """
JNIEXPORT void JNICALL
Java_x_nativeConfigurarPorElSingleton(JNIEnv* env, jobject thiz, jint rate) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    manager.setSampleRate(rate);
}
"""
    check(
        "mata una configuracion por el singleton que esta en SIEMPRE",
        any("NO llama a ensureEngine" in p
            for p in analyze({"v.cpp": SINGLETON}, ["nativeConfigurarPorElSingleton"], {},
                             {"nativeConfigurarPorElSingleton": "por el singleton"})),
    )
    check(
        "NO la mete en el conjunto si no esta en SIEMPRE",
        not any("NO llama a ensureEngine" in p
                for p in analyze({"v.cpp": SINGLETON}, [], {}, {})),
    )
    check(
        "mata una entrada de SIEMPRE que ya no existe",
        any("SIEMPRE HUERFANAS" in p
            for p in analyze({"v.cpp": SANA}, ["nativeSetAlgo"], {}, {"nativeFantasma": "vieja"})),
    )

    # La exclusion con razon calla el fallo...
    check("una exclusion con razon excusa",
          not analyze({"v.cpp": PERDIDA}, [], {"nativeSetAlgo": "porque si"}, {}))
    # ...y una exclusion que ya no se reproduce FALLA (trinquete).
    check(
        "mata una exclusion huerfana",
        any("HUERFANAS" in p
            for p in analyze({"v.cpp": SANA}, ["nativeSetAlgo"], {"nativeYaNoExiste": "vieja"}, {})),
    )

    # El trinquete del conjunto, en las DOS direcciones.
    check(
        "mata un conjunto que declara de menos",
        any("CONJUNTO" in p for p in analyze({"v.cpp": SANA}, [], {}, {})),
    )
    check(
        "mata un conjunto que declara de mas",
        any("CONJUNTO" in p
            for p in analyze({"v.cpp": SANA}, ["nativeSetAlgo", "nativeFantasma"], {}, {})),
    )

    # La guarda de completitud: un prototipo suelto la baja.
    roto = "JNIEXPORT void JNICALL Java_x_nativeDeclaradaAparte(JNIEnv*, jobject);\n" + SANA
    check(
        "mata un parser que abarco de menos (completitud)",
        any("COMPLETITUD" in p for p in analyze({"v.cpp": roto}, ["nativeSetAlgo"], {}, {})),
    )

    # Un arbol sin configuraciones es FALLA, nunca "nada que revisar".
    check(
        "un arbol sin configuraciones es falla, no un pase",
        any("parseo roto" in p
            for p in analyze({"v.cpp": PERDIDA.replace("wma_set_algo", "wma_get_algo")
                                             .replace("nativeSetAlgo", "nativeGetAlgo")}, [], {}, {})),
    )

    if fallos:
        print(f"\n\033[31mSELF-TEST ROJO\033[0m — {len(fallos)}: {', '.join(fallos)}")
        return 1
    print("\n\033[32mself-test ok\033[0m — el lint puede fallar.")
    return 0


def update() -> int:
    bucket, _, _, _, _ = scan(read_tree())
    requerido = sorted(n for n in bucket if n not in EXCLUIR)
    CONJUNTO.write_text(
        "# REQ-045 AC-045.4 — las configuraciones que TIENEN que llegar al motor aunque\n"
        "# se llamen antes de que exista. Lo escribe `check-jni-preinit.py --update`,\n"
        "# derivado del arbol; editarlo a mano fabrica la prueba de una medicion que no\n"
        "# se hizo. Dos consumidores: ese lint y PreInitConfigJniTest (el arnes).\n"
        + "".join(f"{n}\n" for n in requerido),
        encoding="utf-8",
    )
    print(f"{CONJUNTO.name}: {len(requerido)} configuraciones. Su diff es la revision.")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    if "--update" in sys.argv:
        return update()

    sources = read_tree()
    problems = analyze(sources, declarado())

    if problems:
        print(f"\033[31mFALLA\033[0m — {len(problems)} problema(s) en la configuracion pre-init.\n")
        for p in problems:
            print(f"  {p}")
        print(
            "\nUna configuracion que se llama antes de que exista el motor y no crea el motor\n"
            "es un NO-OP MUDO: no hay crash, no hay log y no hay retorno, y el consumidor\n"
            "queda re-aplicando su estado a mano (D2 de las cartas de NoisyPad). El arreglo\n"
            "es `if (!ensureEngine()) return;` — el motor se crea SIN abrir stream. Si de\n"
            "verdad no corresponde, va a EXCLUIR con su razon escrita."
        )
        return 1

    bucket, _, _, _, _ = scan(sources)
    print(
        f"\033[32mok\033[0m — las {len(bucket) - len(EXCLUIR)} configuraciones del conjunto "
        f"aseguran el motor ({len(EXCLUIR)} exclusiones y {len(SIEMPRE)} inclusiones "
        "explicitas, todas con razon)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
