#!/usr/bin/env python3
"""MINI-036 — el `.so` que publica `assembleRelease` compila con las flags de release.

POR QUÉ ESTE CHEQUEO
--------------------
`audio/src/main/cpp/CMakeLists.txt` declaraba un bloque de optimizaciones para
release (`-O3`, FP seguras, `-ftree-vectorize`, LTO) detrás de
`if(CMAKE_BUILD_TYPE STREQUAL "Release")`. AGP construye el release nativo como
**RelWithDebInfo**, así que ese bloque no corrió NUNCA: todas las versiones
publicadas salieron a `-O2`, sin LTO y sin las flags de audio. Y nadie lo vio,
porque leer el `CMakeLists` decía que sí. `.comment` no trae flags y AGP
strippea el `.so`, así que el binario tampoco lo cuenta.

Por eso este gate NO lee el `CMakeLists`: lee las flags EFECTIVAS, del
`build.ninja` que produjo el `.so` que se empaqueta.

CÓMO SABE CUÁL ES ESE build.ninja
---------------------------------
`audio/.cxx/<tipo>/<hash>/` acumula un directorio por cada configuración que
alguna vez se construyó (otra rama, otra versión: el hash cambia), así que "el
más nuevo" no alcanza. Se ata por CONTENIDO: el `.so` de `merged_native_libs`
(la copia sin strip que AGP empaqueta) es byte-idéntico al de
`build/intermediates/cxx/<tipo>/<hash>/obj/<abi>/`, y el `build.ninja` de ese
mismo `<tipo>/<hash>/<abi>` es el que tiene la regla que lo linkea — lo que se
verifica también, no se supone.

QUÉ AFIRMA, por ABI (las cuatro)
--------------------------------
Sobre CADA objeto que entra al `.so` (los del target `watermelon_audio` y los de
cada `.a` que linkea: dsp, effects, engines, voice, analysis, libusb, tsf):
  - el ÚLTIMO `-O` es `-O3` (AGP pone `-O2` antes; el último gana);
  - las cinco flags de FP seguras, `-ftree-vectorize` y `-flto=thin`;
  - SIN `-ffinite-math-only` ni nada que la implique (`-ffast-math`, `-Ofast`,
    `-fno-honor-nans/-infinities`): borraría los guardas de NaN/Inf de
    `EffectChain::processOneEffect`;
  - debug info (`-g`, no anulado por un `-g0` posterior): es lo que sostiene la
    simbolización de la copia sin strip.
Sobre el LINK del `.so`: LTO (`-flto`) y ningún strip (`--strip-all`, `-s`...).

LA PROPIEDAD QUE DEFINE ESTE SCRIPT
-----------------------------------
**Nunca pasar cuando no pudo chequear.** Sin `.so`, sin la ABI, sin el
build.ninja que lo produjo, con cero objetos, con un objeto sin su regla de
compilación o sin `EffectChain.cpp.o` entre los recorridos: FALLO con causa.

Uso:
    python3 scripts/check-release-flags.py              # el chequeo real
    python3 scripts/check-release-flags.py --self-test  # que sabe fallar
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
AUDIO = REPO / "audio"

ABIS = ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
LIB = "libwatermelon_audio.so"

MERGED_GLOB = "build/intermediates/merged_native_libs/release/*/out/lib/{abi}/" + LIB
CXX_OBJ_GLOB = "build/intermediates/cxx/*/*/obj/{abi}/" + LIB

REQUIRED = (
    "-fno-math-errno",
    "-fno-trapping-math",
    "-fno-signed-zeros",
    "-freciprocal-math",
    "-fassociative-math",
    "-ftree-vectorize",
    "-flto=thin",
)
# -ffinite-math-only y todo lo que la implica. -Ofast implica -ffast-math.
FORBIDDEN = (
    "-ffinite-math-only",
    "-ffast-math",
    "-Ofast",
    "-fno-honor-nans",
    "-fno-honor-infinities",
)
STRIP_LINK = ("-Wl,--strip-all", "-Wl,-s", "-s", "-Wl,--strip-debug", "-Wl,-S", "-S")

# El objeto por el que existe la regla de -ffinite-math-only. Si el recorrido no
# lo ve, recorrió otra cosa.
SENTINEL = "/EffectChain.cpp.o"

COMPILE_RULE = re.compile(r"^C(XX)?_COMPILER__")
LINK_RULE = re.compile(r"^CXX_SHARED_LIBRARY_LINKER__watermelon_audio_")
STATIC_RULE = re.compile(r"^(C|CXX)_STATIC_LIBRARY_LINKER__")
OPT_RE = re.compile(r"^-O(\d|s|z|g|fast)?$")
DEBUG_RE = re.compile(r"^-g(\d|line-tables-only|dwarf.*)?$")


class CannotCheck(Exception):
    """No se pudo chequear. NO es un pase: es un fallo con causa."""


# ---------------------------------------------------------------------------
# Lectura de ninja (lo mínimo que genera CMake: sin includes ni pools)
# ---------------------------------------------------------------------------
@dataclass
class Build:
    outputs: list[str]
    rule: str
    inputs: list[str]
    vars: dict[str, str] = field(default_factory=dict)


def _split_ninja(s: str) -> list[str]:
    """Separa por espacios respetando los escapes de ninja (`$ `, `$:`, `$$`)."""
    out, cur, i = [], [], 0
    while i < len(s):
        c = s[i]
        if c == "$" and i + 1 < len(s):
            cur.append(s[i + 1])
            i += 2
            continue
        if c == " ":
            if cur:
                out.append("".join(cur))
                cur = []
        else:
            cur.append(c)
        i += 1
    if cur:
        out.append("".join(cur))
    return out


def _find_colon(s: str) -> int:
    i = 0
    while i < len(s):
        if s[i] == "$":
            i += 2
            continue
        if s[i] == ":":
            return i
        i += 1
    return -1


def parse_ninja(text: str) -> list[Build]:
    # Continuación de línea: `$` al final.
    text = re.sub(r"\$\n\s*", "", text)
    builds: list[Build] = []
    cur: Build | None = None
    for line in text.splitlines():
        if line.startswith("build "):
            body = line[len("build "):]
            k = _find_colon(body)
            if k < 0:
                raise CannotCheck(f"línea `build` sin `:`: {line[:120]}")
            outs = [o for o in _split_ninja(body[:k]) if o != "|"]
            rest = _split_ninja(body[k + 1:])
            if not rest:
                raise CannotCheck(f"línea `build` sin regla: {line[:120]}")
            rule, deps = rest[0], rest[1:]
            explicit = []
            for d in deps:
                if d in ("|", "||", "|@"):
                    break
                explicit.append(d)
            cur = Build(outs, rule, explicit)
            builds.append(cur)
        elif cur is not None and line.startswith("  ") and "=" in line:
            key, _, val = line.strip().partition("=")
            cur.vars[key.strip()] = val.strip()
        elif line.strip() == "" or not line.startswith(" "):
            cur = None
    return builds


# ---------------------------------------------------------------------------
# Qué objetos entran al .so, y qué flags tiene cada uno
# ---------------------------------------------------------------------------
@dataclass
class Result:
    objects: int
    problems: list[str]


def check_ninja(text: str, so_path: str | None = None) -> Result:
    """Revisa un build.ninja. `so_path`: la salida del link a exigir (None = la
    única regla de link de watermelon_audio)."""
    builds = parse_ninja(text)
    by_output: dict[str, Build] = {}
    for b in builds:
        for o in b.outputs:
            by_output[o] = b

    links = [b for b in builds if LINK_RULE.match(b.rule)
             and any(o.endswith("/" + LIB) or o == LIB for o in b.outputs)]
    if so_path is not None:
        links = [b for b in links if so_path in b.outputs]
    if len(links) != 1:
        raise CannotCheck(
            f"esperaba UNA regla de link de {LIB}"
            + (f" con salida {so_path}" if so_path else "")
            + f", encontré {len(links)}.")
    link = links[0]

    # Los objetos: los explícitos del link, más los de cada .a del árbol.
    objects: list[str] = [i for i in link.inputs if i.endswith(".o")]
    libs = [t for t in _split_ninja(link.vars.get("LINK_LIBRARIES", "")) if t.endswith(".a")]
    for lib in dict.fromkeys(libs):
        producer = by_output.get(lib)
        if producer is None:
            # Un .a con ruta absoluta es de afuera del árbol (prebuilt): no se
            # compila acá y no se le pueden pedir flags. Uno relativo es del
            # árbol y TIENE que tener su regla.
            if lib.startswith("/"):
                continue
            raise CannotCheck(f"`{lib}` entra al link y no encontré la regla que lo produce.")
        if not STATIC_RULE.match(producer.rule):
            raise CannotCheck(f"`{lib}` lo produce `{producer.rule}`, que no es un .a.")
        objs = [i for i in producer.inputs if i.endswith(".o")]
        if not objs:
            raise CannotCheck(f"`{lib}` no tiene objetos: el recorrido está roto.")
        objects.extend(objs)
    objects = list(dict.fromkeys(objects))

    if not objects:
        raise CannotCheck("cero objetos entran al .so: no chequeé nada.")
    if not any(o.endswith(SENTINEL) for o in objects):
        raise CannotCheck(f"`{SENTINEL.lstrip('/')}` no está entre los objetos recorridos: "
                          "el recorrido no llega a los efectos.")

    problems: list[str] = []
    for obj in objects:
        b = by_output.get(obj)
        if b is None or not COMPILE_RULE.match(b.rule):
            raise CannotCheck(f"`{obj}` entra al .so y no encontré su regla de compilación.")
        if "FLAGS" not in b.vars:
            raise CannotCheck(f"`{obj}` no tiene FLAGS en su regla.")
        problems += [f"{obj}: {p}" for p in compile_problems(_split_ninja(b.vars["FLAGS"]))]

    link_flags = _split_ninja(link.vars.get("LINK_FLAGS", "")) + \
        _split_ninja(link.vars.get("LANGUAGE_COMPILE_FLAGS", ""))
    problems += [f"link de {LIB}: {p}" for p in link_problems(link_flags)]
    return Result(len(objects), problems)


def compile_problems(flags: list[str]) -> list[str]:
    probs = []
    opts = [f for f in flags if OPT_RE.match(f)]
    if not opts:
        probs.append("sin nivel de optimización")
    elif opts[-1] != "-O3":
        probs.append(f"el último -O es {opts[-1]}, no -O3")
    for r in REQUIRED:
        if r not in flags:
            probs.append(f"falta {r}")
    for f in FORBIDDEN:
        if f in flags:
            probs.append(f"tiene {f} (borra los guardas de NaN/Inf)")
    dbg = [f for f in flags if DEBUG_RE.match(f)]
    if not dbg or dbg[-1] == "-g0":
        probs.append("sin debug info (-g): la copia sin strip no simboliza")
    return probs


def link_problems(flags: list[str]) -> list[str]:
    probs = []
    if not any(f.startswith("-flto") for f in flags):
        probs.append("el link no es LTO (falta -flto)")
    for s in STRIP_LINK:
        if s in flags:
            probs.append(f"el link strippea ({s}): la copia sin strip pierde .debug_* y .symtab")
    return probs


# ---------------------------------------------------------------------------
# De la ABI al build.ninja, atado por contenido
# ---------------------------------------------------------------------------
def _sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def resolve_abi(audio: Path, abi: str) -> tuple[Path, list[tuple[Path, Path]]]:
    """Devuelve el .so empaquetado y los (obj .so, build.ninja) que lo produjeron."""
    merged = sorted(audio.glob(MERGED_GLOB.format(abi=abi)))
    if not merged:
        raise CannotCheck(f"[{abi}] no hay {LIB} en merged_native_libs/release. "
                          "Corré primero: ./gradlew :audio:assembleRelease")
    if len(merged) > 1:
        raise CannotCheck(f"[{abi}] hay {len(merged)} {LIB} en merged_native_libs/release: "
                          + ", ".join(str(m) for m in merged))
    so = merged[0]
    digest = _sha(so)
    size = so.stat().st_size
    matches = [c for c in sorted(audio.glob(CXX_OBJ_GLOB.format(abi=abi)))
               if c.stat().st_size == size and _sha(c) == digest]
    if not matches:
        raise CannotCheck(f"[{abi}] ningún build de build/intermediates/cxx produjo el "
                          f"{LIB} empaquetado: no sé qué flags lo compilaron.")
    pairs = []
    for m in matches:
        # build/intermediates/cxx/<tipo>/<hash>/obj/<abi>/lib.so -> .cxx/<tipo>/<hash>/<abi>/
        if m.parents[1].name != "obj":
            raise CannotCheck(f"[{abi}] {m} no tiene la forma cxx/<tipo>/<hash>/obj/<abi>/.")
        btype, bhash = m.parents[3].name, m.parents[2].name
        ninja = audio / ".cxx" / btype / bhash / abi / "build.ninja"
        if not ninja.exists():
            raise CannotCheck(f"[{abi}] no existe {ninja}, el build.ninja que produjo {m}.")
        pairs.append((m, ninja))
    return so, pairs


def check_tree(audio: Path) -> tuple[list[str], list[str]]:
    """(líneas de reporte, problemas). Lanza CannotCheck si no pudo chequear."""
    report, problems = [], []
    for abi in ABIS:
        so, pairs = resolve_abi(audio, abi)
        for obj, ninja in pairs:
            res = check_ninja(ninja.read_text(encoding="utf-8"), so_path=str(obj))
            try:
                rel = ninja.relative_to(audio.parent)
            except ValueError:
                rel = ninja
            report.append(f"  {abi:<12} {res.objects:>4} objetos  {rel}")
            problems += [f"[{abi}] {p}" for p in res.problems]
    return report, problems


def run_real_check() -> int:
    print("MINI-036 — flags efectivas del release nativo (build.ninja del .so empaquetado)\n")
    report, problems = check_tree(AUDIO)
    print("\n".join(report))
    if problems:
        # Agrupado por (ABI, problema): con 91 objetos por ABI el listado plano
        # son miles de líneas iguales y esconde lo que importa.
        groups: dict[tuple[str, str], list[str]] = {}
        for p in problems:
            abi, _, rest = p.partition("] ")
            where, _, msg = rest.rpartition(": ")
            groups.setdefault((abi + "]", msg), []).append(where)
        print(f"\n\033[31mFALLA\033[0m — {len(problems)} problema(s). El .so que se publica NO "
              "compila con las flags de release:\n")
        for (abi, msg), wheres in groups.items():
            print(f"    {abi} {msg} — {len(wheres)} (ej. {wheres[0]})")
        return 1
    print("\n\033[32mok\033[0m — las cuatro ABIs compilan con -O3, FP seguras, vectorización, "
          "LTO y -g, sin -ffinite-math-only; el link no strippea.")
    return 0


# ---------------------------------------------------------------------------
# Self-test: que el chequeo SABE FALLAR, y que pasa cuando tiene que pasar.
#
# Los FLAGS de las fixtures son los REALES del build.ninja de arm64-v8a de
# EffectChain.cpp.o: HOY es master a5238f7 (RelWithDebInfo, sin el arreglo);
# FIXED es con el arreglo de MINI-036. Sólo se recortaron rutas.
# ---------------------------------------------------------------------------
FLAGS_HOY = ("-g -DANDROID -fdata-sections -ffunction-sections -funwind-tables "
             "-fstack-protector-strong -no-canonical-prefixes -D_FORTIFY_SOURCE=2 -Wformat "
             "-Werror=format-security  -std=c++20 -O2 -g -DNDEBUG -fPIC -mcpu=cortex-a53 "
             "-std=c++20")
LINK_HOY = ("-Wl,--build-id=sha1 -Wl,--no-rosegment -Wl,--no-undefined-version "
            "-Wl,--fatal-warnings -Wl,--no-undefined -Qunused-arguments  "
            "-Wl,-z,max-page-size=16384 -Wl,--gc-sections")
FLAGS_FIXED = ("-g -DANDROID -fdata-sections -ffunction-sections -funwind-tables "
               "-fstack-protector-strong -no-canonical-prefixes -D_FORTIFY_SOURCE=2 -Wformat "
               "-Werror=format-security  -std=c++20 -O2 -g -DNDEBUG -flto=thin -fPIC -O3 "
               "-DNDEBUG -fno-math-errno -fno-trapping-math -fno-signed-zeros -freciprocal-math "
               "-fassociative-math -ftree-vectorize -ffunction-sections -fdata-sections "
               "-mcpu=cortex-a53 -std=c++20")
# En el link, -flto=thin llega por LANGUAGE_COMPILE_FLAGS (CMake no lo pone en
# CMAKE_CXX_LINK_OPTIONS_IPO); la fixture lo lleva ahí, igual que el real.
LINK_FIXED = ("-Wl,--build-id=sha1 -Wl,--no-rosegment -Wl,--no-undefined-version "
              "-Wl,--fatal-warnings -Wl,--no-undefined -Qunused-arguments  "
              "-Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,--gc-sections   -fuse-ld=lld")
LINK_LANG_HOY = ("-g -DANDROID -fdata-sections -ffunction-sections -funwind-tables "
                 "-fstack-protector-strong -no-canonical-prefixes -D_FORTIFY_SOURCE=2 -Wformat "
                 "-Werror=format-security  -std=c++20 -O2 -g -DNDEBUG")
LINK_LANG_FIXED = LINK_LANG_HOY + " -flto=thin"


def fixture(flags: str = FLAGS_FIXED, link: str = LINK_FIXED,
            link_lang: str = LINK_LANG_FIXED, *,
            effects_flags: str | None = None, drop_effectchain_rule: bool = False,
            with_effectchain: bool = True, extra_test_flags: str = "-O0",
            so: str = "/abs/obj/arm64-v8a/" + LIB, libs_without_rule: bool = False) -> str:
    """Un build.ninja con la forma que genera CMake: un .o del target, un .a de
    efectos con dos objetos, un .a prebuilt absoluto y un ejecutable de test
    (EffectTest) que NO entra al .so."""
    ef = effects_flags if effects_flags is not None else flags
    eff_objs = ["effects/CMakeFiles/watermelon-effects.dir/Delay.cpp.o"]
    if with_effectchain:
        eff_objs.insert(0, "effects/CMakeFiles/watermelon-effects.dir/EffectChain.cpp.o")
    t = []
    t.append("build CMakeFiles/watermelon_audio.dir/core/AudioEngine.cpp.o: "
             "CXX_COMPILER__watermelon_audio_RelWithDebInfo /src/core/AudioEngine.cpp "
             "|| cmake_object_order_depends_target_watermelon_audio")
    t.append("  DEFINES = -DUSE_NEON=1")
    t.append(f"  FLAGS = {flags}")
    t.append("")
    for o in eff_objs:
        if drop_effectchain_rule and o.endswith(SENTINEL):
            continue
        t.append(f"build {o}: CXX_COMPILER__watermelon-effects_RelWithDebInfo /src/x.cpp")
        t.append(f"  FLAGS = {ef}")
        t.append("")
    if not libs_without_rule:
        t.append("build effects/libwatermelon-effects.a: "
                 "CXX_STATIC_LIBRARY_LINKER__watermelon-effects_RelWithDebInfo "
                 + " ".join(eff_objs))
        t.append("  LANGUAGE_COMPILE_FLAGS = " + ef)
        t.append("")
    t.append("build CMakeFiles/EffectTest.dir/t.cpp.o: CXX_COMPILER__EffectTest_RelWithDebInfo /src/t.cpp")
    t.append(f"  FLAGS = {extra_test_flags}")
    t.append("")
    t.append(f"build {so}: CXX_SHARED_LIBRARY_LINKER__watermelon_audio_RelWithDebInfo "
             "CMakeFiles/watermelon_audio.dir/core/AudioEngine.cpp.o | /other/dep.so "
             "|| effects/libwatermelon-effects.a")
    t.append(f"  LANGUAGE_COMPILE_FLAGS = {link_lang}")
    t.append(f"  LINK_FLAGS = {link}")
    t.append("  LINK_LIBRARIES = effects/libwatermelon-effects.a  "
             "/gradle/caches/oboe/liboboe.so  /abs/prebuilt/libfoo.a  -landroid  -llog")
    t.append("")
    return "\n".join(t) + "\n"


def self_test() -> int:
    fallos: list[str] = []

    def check(nombre: str, cond: bool, detalle: str = "") -> None:
        if cond:
            print(f"  \033[32mok\033[0m   {nombre}")
        else:
            print(f"  \033[31mFALLA\033[0m {nombre} {detalle}")
            fallos.append(nombre)

    def problems(text: str) -> list[str] | None:
        try:
            return check_ninja(text).problems
        except CannotCheck:
            return None

    def levanta(fn, motivo: str) -> bool:
        """Lanza CannotCheck, y por el MOTIVO esperado: un caso que muere en
        otro guarda (p. ej. el centinela) no prueba el suyo."""
        try:
            fn()
            return False
        except CannotCheck as e:
            return motivo in str(e)

    def dice(text: str, needle: str) -> bool:
        p = problems(text)
        return p is not None and any(needle in x for x in p)

    # --- las dos direcciones sobre los FLAGS reales (AC-M036.4)
    hoy = problems(fixture(FLAGS_HOY, LINK_HOY, LINK_LANG_HOY))
    check("HOY (master, RelWithDebInfo sin el arreglo) FALLA",
          hoy is not None and len(hoy) > 0, f"dio {hoy}")
    check("HOY falla por -O2 y por falta de -flto=thin, en el .o y en el .a",
          hoy is not None
          and any("AudioEngine" in p and "-O2" in p for p in hoy)
          and any("EffectChain" in p and "falta -flto=thin" in p for p in hoy)
          and any("link" in p and "-flto" in p for p in hoy), f"dio {hoy}")
    fixed = problems(fixture())
    check("FIXED (con el arreglo) PASA", fixed == [], f"dio {fixed}")

    # --- los mutantes que el gate existe para matar
    def drop(s: str, tok: str) -> str:
        toks = _split_ninja(s)
        assert tok in toks, f"la fixture no tiene {tok}"
        return " ".join(t for t in toks if t != tok)

    # Literales, NO `REQUIRED`/`FORBIDDEN`: si alguien saca una flag de esas
    # tuplas, el caso tiene que seguir existiendo y ponerse rojo, no desaparecer.
    for tok in ("-fno-math-errno", "-fno-trapping-math", "-fno-signed-zeros",
                "-freciprocal-math", "-fassociative-math", "-ftree-vectorize", "-flto=thin"):
        check(f"sin {tok} FALLA", dice(fixture(drop(FLAGS_FIXED, tok)), f"falta {tok}"))
    for tok in ("-ffinite-math-only", "-ffast-math", "-Ofast",
                "-fno-honor-nans", "-fno-honor-infinities"):
        check(f"con {tok} FALLA", dice(fixture(FLAGS_FIXED + " " + tok), f"tiene {tok}"))
    check("un -O2 DESPUÉS del -O3 FALLA (gana el último)",
          dice(fixture(FLAGS_FIXED + " -O2"), "el último -O es -O2"))
    check("sin -O3 FALLA", dice(fixture(drop(FLAGS_FIXED, "-O3")), "no -O3"))
    check("-g0 al final FALLA", dice(fixture(FLAGS_FIXED + " -g0"), "sin debug info"))
    check("sin -g FALLA", dice(fixture(" ".join(t for t in _split_ninja(FLAGS_FIXED)
                                                if t != "-g")), "sin debug info"))
    check("el link sin -flto FALLA",
          dice(fixture(link_lang=LINK_LANG_HOY), "no es LTO"))
    check("el link con -Wl,--strip-all FALLA",
          dice(fixture(link=LINK_FIXED + " -Wl,--strip-all"), "strippea"))
    check("un solo .a con flags viejas FALLA (el recorrido entra a los .a)",
          dice(fixture(effects_flags=FLAGS_HOY), "EffectChain"))
    check("EffectTest con -O0 NO cuenta (no entra al .so)",
          problems(fixture(extra_test_flags="-O0")) == [])

    # --- "no pude chequear" nunca es un pase
    check("sin regla de link NO es un pase", levanta(lambda: check_ninja(""), "esperaba UNA regla de link"))
    check("link de otra salida NO es un pase",
          levanta(lambda: check_ninja(fixture(), so_path="/otra/" + LIB), "con salida /otra/"))
    check("un .o sin su regla de compilación NO es un pase",
          levanta(lambda: check_ninja(fixture(drop_effectchain_rule=True)), "su regla de compilación"))
    check("sin EffectChain.cpp.o recorrido NO es un pase",
          levanta(lambda: check_ninja(fixture(with_effectchain=False)), "no está entre los objetos"))
    check("un .a del árbol sin su regla NO es un pase",
          levanta(lambda: check_ninja(fixture(libs_without_rule=True)), "la regla que lo produce"))

    # --- el parser de ninja: escapes y continuación
    toks = _split_ninja("a$ b c$:d  e$$f")
    check("respeta los escapes de ninja", toks == ["a b", "c:d", "e$f"], f"dio {toks}")
    cont = fixture().replace("  FLAGS = ", "  FLAGS = -Dx $\n    ", 1)
    check("sigue la continuación `$` de línea", problems(cont) == [], f"dio {problems(cont)}")

    # --- de la ABI al build.ninja, atado por contenido, sobre un árbol falso
    with tempfile.TemporaryDirectory() as d:
        audio = Path(d) / "audio"

        def make(abi: str, payload: bytes, ninja_text: str | None, bhash: str = "h1") -> Path:
            m = audio / "build/intermediates/merged_native_libs/release/mergeReleaseNativeLibs/out/lib" / abi
            m.mkdir(parents=True, exist_ok=True)
            (m / LIB).write_bytes(payload)
            o = audio / "build/intermediates/cxx/RelWithDebInfo" / bhash / "obj" / abi
            o.mkdir(parents=True, exist_ok=True)
            (o / LIB).write_bytes(payload)
            if ninja_text is not None:
                n = audio / ".cxx/RelWithDebInfo" / bhash / abi
                n.mkdir(parents=True, exist_ok=True)
                (n / "build.ninja").write_text(ninja_text.replace("@SO@", str(o / LIB)))
            return o / LIB

        def tree_ok() -> tuple[bool, list[str]]:
            try:
                _, probs = check_tree(audio)
                return True, probs
            except CannotCheck as e:
                return False, [str(e)]

        for abi in ABIS:
            make(abi, abi.encode(), fixture(so="@SO@"))
        ok, probs = tree_ok()
        check("árbol con las cuatro ABIs en regla PASA", ok and probs == [], f"dio {probs}")

        # Un build viejo en otro hash con flags de HOY y otro contenido: no se mira.
        stale = audio / "build/intermediates/cxx/RelWithDebInfo/viejo/obj/arm64-v8a"
        stale.mkdir(parents=True)
        (stale / LIB).write_bytes(b"otro binario")
        sn = audio / ".cxx/RelWithDebInfo/viejo/arm64-v8a"
        sn.mkdir(parents=True)
        (sn / "build.ninja").write_text(fixture(FLAGS_HOY, LINK_HOY, LINK_LANG_HOY, so=str(stale / LIB)))
        ok, probs = tree_ok()
        check("un build.ninja viejo de otro hash NO contamina", ok and probs == [], f"dio {probs}")

        # El empaquetado viene del build de HOY: tiene que dar rojo aunque exista el bueno.
        make("x86", b"x86-de-hoy", fixture(FLAGS_HOY, LINK_HOY, LINK_LANG_HOY, so="@SO@"), bhash="hoy")
        ok, probs = tree_ok()
        check("si lo empaquetado salió del build de HOY, FALLA",
              ok and any(p.startswith("[x86]") for p in probs)
              and not any(p.startswith("[arm64-v8a]") for p in probs), f"dio {probs}")
        make("x86", b"x86", None)  # vuelve al bueno

        # Se pierde una ABI.
        shutil.rmtree(audio / "build/intermediates/merged_native_libs/release/"
                      "mergeReleaseNativeLibs/out/lib/x86_64")
        ok, probs = tree_ok()
        check("perder una ABI NO es un pase", not ok and "x86_64" in probs[0], f"dio {probs}")
        make("x86_64", b"x86_64", None)

        # El empaquetado no coincide con ningún build.
        (audio / "build/intermediates/merged_native_libs/release/mergeReleaseNativeLibs/"
         "out/lib/armeabi-v7a" / LIB).write_bytes(b"de otro lado")
        ok, probs = tree_ok()
        check("un .so empaquetado sin build que lo produjo NO es un pase",
              not ok and "armeabi-v7a" in probs[0], f"dio {probs}")
        make("armeabi-v7a", b"armeabi-v7a", None)

        # Falta el build.ninja del build que coincide.
        (audio / ".cxx/RelWithDebInfo/h1/arm64-v8a/build.ninja").unlink()
        ok, probs = tree_ok()
        check("sin el build.ninja del build que coincide NO es un pase",
              not ok and "build.ninja" in probs[0], f"dio {probs}")

    if fallos:
        print(f"\n\033[31mself-test ROJO\033[0m — {len(fallos)} caso(s): {', '.join(fallos)}")
        return 1
    print("\n\033[32mself-test verde\033[0m — el chequeo sabe fallar, y pasa cuando tiene que pasar.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="verifica que el chequeo sabe fallar; no mira el build")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    try:
        return run_real_check()
    except CannotCheck as exc:
        print(f"\n\033[31mFALLA\033[0m — no pude chequear, así que NO doy verde:\n       {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
