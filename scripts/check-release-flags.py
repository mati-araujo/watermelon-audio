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
verifica también, no se supone. Y que ese `.so` esté AL DÍA con ese build.ninja
se le pregunta al mismo ninja que lo construyó (`ninja -n`, en seco): si el
build.ninja se regeneró y el link falló, el `.so` viejo sigue ahí con el mismo
contenido, y sin esta pregunta el gate leería flags que nunca lo compilaron.

QUÉ AFIRMA, por ABI (las cuatro)
--------------------------------
Sobre CADA objeto que entra al `.so` (los del target `watermelon_audio` y los de
cada `.a` que linkea: dsp, effects, engines, voice, analysis, libusb, tsf), con
la semántica del driver — GANA LA ÚLTIMA flag que toca cada propiedad, así que
un `-fno-lto` o un `-fmath-errno` al final anulan lo de antes:
  - el último `-O` es `-O3` (AGP pone `-O2` antes);
  - efectivas: las cinco flags de FP seguras, `-ftree-vectorize` y `-flto=thin`;
  - SIN `-ffinite-math-only` ni nada que la implique o que reescriba el modelo
    de FP (`-ffast-math`, `-Ofast`, `-ffp-model=*`, `-funsafe-math-optimizations`,
    `-fno-honor-nans/-infinities`), en cualquier posición: borraría los guardas
    de NaN/Inf de `EffectChain::processOneEffect`;
  - debug info (`-g`, no anulado por un `-g0` posterior).
Sobre el LINK del `.so` (en el orden del comando: LANGUAGE_COMPILE_FLAGS,
ARCH_FLAGS, LINK_FLAGS): LTO efectivo y nada de lo prohibido.
Sobre el `.so` de `merged_native_libs` (la copia sin strip): que TENGA `.symtab`,
`.debug_info` y `.debug_line`, leídos con el `llvm-readelf` del mismo build. No
se infiere de la ausencia de flags de strip: un `-Wl,--gc-sections,--strip-all`
o un strip en LINK_LIBRARIES no se ven como token, y las secciones sí.

LA PROPIEDAD QUE DEFINE ESTE SCRIPT
-----------------------------------
**Nunca pasar cuando no pudo chequear.** Sin `.so`, sin la ABI, sin el
build.ninja que lo produjo, con el `.so` desactualizado respecto de él, sin
ninja o sin readelf, con cero objetos, con un objeto sin su regla de
compilación o sin `EffectChain.cpp.o` entre los recorridos: FALLO con causa.

Uso:
    python3 scripts/check-release-flags.py              # el chequeo real
    python3 scripts/check-release-flags.py --self-test  # que sabe fallar
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
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

# Cada propiedad requerida: (nombre, tokens que la PRENDEN, tokens que la APAGAN).
# Gana el último token del conjunto que aparezca, como en el driver de clang.
# `-fno-fast-math` y `-fno-unsafe-math-optimizations` restauran los defaults de
# FP, así que apagan las cinco.
_FP_OFF = ("-fno-fast-math", "-fno-unsafe-math-optimizations")
REQUIRED = (
    ("-fno-math-errno", ("-fno-math-errno",), ("-fmath-errno",) + _FP_OFF),
    ("-fno-trapping-math", ("-fno-trapping-math",), ("-ftrapping-math",) + _FP_OFF),
    ("-fno-signed-zeros", ("-fno-signed-zeros",), ("-fsigned-zeros",) + _FP_OFF),
    ("-freciprocal-math", ("-freciprocal-math",), ("-fno-reciprocal-math",) + _FP_OFF),
    ("-fassociative-math", ("-fassociative-math",), ("-fno-associative-math",) + _FP_OFF),
    ("-ftree-vectorize", ("-ftree-vectorize", "-fvectorize"),
     ("-fno-tree-vectorize", "-fno-vectorize")),
)
# -ffinite-math-only y todo lo que la implica o reescribe el modelo de FP.
# Por PRESENCIA, en cualquier posición: un `-fno-finite-math-only` después no lo
# redime, porque no hay razón legítima para que esté.
FORBIDDEN = (
    "-ffinite-math-only",
    "-ffast-math",
    "-Ofast",
    "-funsafe-math-optimizations",
    "-fno-honor-nans",
    "-fno-honor-infinities",
)
FORBIDDEN_PREFIX = ("-ffp-model=",)   # =fast implica finite-math; =precise/strict deshacen las de arriba
LTO_WANTED = "-flto=thin"

# Lo que la copia sin strip tiene que conservar para simbolizar un crash.
DEBUG_SECTIONS = (".symtab", ".debug_info", ".debug_line")

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
    skipped: list[str] = field(default_factory=list)   # .a prebuilt: no se compilan acá


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
    skipped: list[str] = []
    for lib in dict.fromkeys(libs):
        producer = by_output.get(lib)
        if producer is None:
            # Un .a con ruta absoluta y SIN regla es de afuera del árbol
            # (prebuilt): no se compila acá y no se le pueden pedir flags. Uno
            # con regla se recorre aunque la ruta sea absoluta; uno relativo sin
            # regla es del árbol y el recorrido está roto.
            if lib.startswith("/"):
                skipped.append(lib)
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

    # En el orden del comando de la regla de link de CMake:
    #   ... $LANGUAGE_COMPILE_FLAGS $ARCH_FLAGS $LINK_FLAGS -shared ...
    link_flags = [t for k in ("LANGUAGE_COMPILE_FLAGS", "ARCH_FLAGS", "LINK_FLAGS")
                  for t in _split_ninja(link.vars.get(k, ""))]
    problems += [f"link de {LIB}: {p}" for p in link_problems(link_flags)]
    return Result(len(objects), problems, skipped)


def _last(flags: list[str], pred) -> str | None:
    hits = [f for f in flags if pred(f)]
    return hits[-1] if hits else None


def _is_lto(f: str) -> bool:
    return f == "-flto" or f.startswith("-flto=") or f == "-fno-lto"


def _forbidden(flags: list[str]) -> list[str]:
    return [f for f in flags if f in FORBIDDEN or f.startswith(FORBIDDEN_PREFIX)]


def compile_problems(flags: list[str]) -> list[str]:
    probs = []
    opt = _last(flags, OPT_RE.match)
    if opt is None:
        probs.append("sin nivel de optimización")
    elif opt != "-O3":
        probs.append(f"el último -O es {opt}, no -O3")
    for name, on, off in REQUIRED:
        last = _last(flags, lambda f: f in on or f in off)
        if last is None:
            probs.append(f"falta {name}")
        elif last in off:
            probs.append(f"falta {name} (la anula un {last} posterior)")
    lto = _last(flags, _is_lto)
    if lto is None:
        probs.append(f"falta {LTO_WANTED}")
    elif lto != LTO_WANTED:
        probs.append(f"falta {LTO_WANTED} (el último es {lto})")
    for f in _forbidden(flags):
        probs.append(f"tiene {f} (borra los guardas de NaN/Inf)")
    dbg = _last(flags, DEBUG_RE.match)
    if dbg is None or dbg == "-g0":
        probs.append("sin debug info (-g): la copia sin strip no simboliza")
    return probs


def link_problems(flags: list[str]) -> list[str]:
    probs = []
    lto = _last(flags, _is_lto)
    if lto is None or lto == "-fno-lto":
        probs.append("el link no es LTO (falta -flto" + (", lo anula -fno-lto)" if lto else ")"))
    for f in _forbidden(flags):
        probs.append(f"tiene {f} (el codegen de LTO pasa por el link)")
    return probs


def section_names(readelf_s: str) -> set[str]:
    """Los nombres de sección de la salida de `llvm-readelf -S -W`."""
    return set(re.findall(r"^\s*\[\s*\d+\]\s+(\S+)", readelf_s, flags=re.M))


def debug_problems(names: set[str]) -> list[str]:
    if not names:
        raise CannotCheck("readelf no listó ninguna sección: no leí el .so.")
    return [f"la copia sin strip no tiene {s}: no simboliza" for s in DEBUG_SECTIONS
            if s not in names]


# ---------------------------------------------------------------------------
# De la ABI al build.ninja, atado por contenido
# ---------------------------------------------------------------------------
def _sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cache_value(cache_text: str, key: str) -> str | None:
    m = re.search(rf"^{re.escape(key)}:[A-Z]+=(.*)$", cache_text, flags=re.M)
    return m.group(1).strip() if m and m.group(1).strip() else None


def find_ninja() -> str | None:
    """Sólo para el self-test: el chequeo real usa el ninja del CMakeCache."""
    w = shutil.which("ninja")
    if w:
        return w
    sdk = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    props = REPO / "local.properties"
    if not sdk and props.exists():
        for line in props.read_text(encoding="utf-8").splitlines():
            if line.startswith("sdk.dir="):
                sdk = line.split("=", 1)[1].strip()
    if sdk:
        c = sorted(Path(sdk).glob("cmake/*/bin/ninja"))
        if c:
            return str(c[-1])
    return None


def ninja_up_to_date(dry_run_output: str) -> bool:
    """`ninja -n` al día dice exactamente eso; cualquier paso planeado es que no."""
    lines = [ln for ln in dry_run_output.splitlines()
             if ln.strip() and not ln.startswith("ninja: Entering directory")]
    return lines == ["ninja: no work to do."]


class RealTools:
    """ninja y llvm-readelf del MISMO build (su CMakeCache.txt), no del PATH."""

    def _tool(self, build_dir: Path, key: str) -> str:
        cache = build_dir / "CMakeCache.txt"
        if not cache.exists():
            raise CannotCheck(f"no existe {cache}: no sé con qué herramientas se construyó.")
        v = cache_value(cache.read_text(encoding="utf-8", errors="replace"), key)
        if v is None or not Path(v).exists():
            raise CannotCheck(f"{key} no está en {cache} o no existe ({v}).")
        return v

    def up_to_date(self, build_dir: Path, target: str) -> tuple[bool, str]:
        ninja = self._tool(build_dir, "CMAKE_MAKE_PROGRAM")
        try:
            r = subprocess.run([ninja, "-C", str(build_dir), "-n", target],
                               capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.SubprocessError) as exc:
            raise CannotCheck(f"`{ninja} -n` falló en {build_dir}: {exc}") from exc
        out = (r.stdout + r.stderr).strip()
        if r.returncode != 0:
            raise CannotCheck(f"`{ninja} -n {target}` salió {r.returncode}:\n{out[-800:]}")
        return ninja_up_to_date(out), out

    def sections(self, build_dir: Path, so: Path) -> set[str]:
        readelf = self._tool(build_dir, "CMAKE_READELF")
        try:
            out = subprocess.run([readelf, "-S", "-W", str(so)], capture_output=True,
                                 text=True, check=True, timeout=120).stdout
        except (OSError, subprocess.SubprocessError) as exc:
            raise CannotCheck(f"{readelf} -S falló sobre {so}: {exc}") from exc
        return section_names(out)


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
        # El glob fija la forma build/intermediates/cxx/<tipo>/<hash>/obj/<abi>/lib.so,
        # y el build.ninja vive en .cxx/<tipo>/<hash>/<abi>/.
        btype, bhash = m.parents[3].name, m.parents[2].name
        ninja = audio / ".cxx" / btype / bhash / abi / "build.ninja"
        if not ninja.exists():
            raise CannotCheck(f"[{abi}] no existe {ninja}, el build.ninja que produjo {m}.")
        pairs.append((m, ninja))
    return so, pairs


def check_tree(audio: Path, tools=None) -> tuple[list[str], list[str]]:
    """(líneas de reporte, problemas). Lanza CannotCheck si no pudo chequear."""
    tools = tools or RealTools()
    report, problems = [], []
    for abi in ABIS:
        so, pairs = resolve_abi(audio, abi)
        # Si hay varios builds con el mismo contenido, se revisan TODOS: no hay
        # forma de saber cuál de ellos es "el" que se empaquetó.
        for obj, ninja in pairs:
            fresh, out = tools.up_to_date(ninja.parent, str(obj))
            if not fresh:
                raise CannotCheck(
                    f"[{abi}] {obj.name} NO está al día con {ninja}: ninja lo reconstruiría. "
                    "Las flags de ese build.ninja no son las que lo compilaron. Corré "
                    "./gradlew :audio:assembleRelease.\n       " + out[:400].replace("\n", "\n       "))
            res = check_ninja(ninja.read_text(encoding="utf-8"), so_path=str(obj))
            try:
                rel = ninja.relative_to(audio.parent)
            except ValueError:
                rel = ninja
            report.append(f"  {abi:<12} {res.objects:>4} objetos  {rel}")
            for lib in res.skipped:
                report.append(f"  {'':<12}      prebuilt salteado: {lib}")
            problems += [f"[{abi}] {p}" for p in res.problems]
            problems += [f"[{abi}] {so.name} (merged): {p}"
                         for p in debug_problems(tools.sections(ninja.parent, so))]
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
          "LTO y -g, sin -ffinite-math-only; la copia sin strip conserva "
          + ", ".join(DEBUG_SECTIONS) + ".")
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
            with_effectchain: bool = True, extra_test_flags: str = FLAGS_FIXED,
            so: str = "/abs/obj/arm64-v8a/" + LIB, libs_without_rule: bool = False,
            effects_lib: str = "effects/libwatermelon-effects.a") -> str:
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
        t.append(f"build {effects_lib}: "
                 "CXX_STATIC_LIBRARY_LINKER__watermelon-effects_RelWithDebInfo "
                 + " ".join(eff_objs))
        t.append("  LANGUAGE_COMPILE_FLAGS = " + ef)
        t.append("")
    t.append("build CMakeFiles/EffectTest.dir/t.cpp.o: CXX_COMPILER__EffectTest_RelWithDebInfo /src/t.cpp")
    t.append(f"  FLAGS = {extra_test_flags}")
    t.append("")
    t.append(f"build {so}: CXX_SHARED_LIBRARY_LINKER__watermelon_audio_RelWithDebInfo "
             "CMakeFiles/watermelon_audio.dir/core/AudioEngine.cpp.o | /other/dep.so "
             f"|| {effects_lib}")
    t.append(f"  LANGUAGE_COMPILE_FLAGS = {link_lang}")
    t.append(f"  LINK_FLAGS = {link}")
    t.append(f"  LINK_LIBRARIES = {effects_lib}  "
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
    check("un solo .a con flags viejas FALLA (el recorrido entra a los .a)",
          dice(fixture(effects_flags=FLAGS_HOY), "EffectChain"))
    check("EffectTest con -O0 NO cuenta (no entra al .so)",
          problems(fixture(extra_test_flags="-O0")) == [])

    # --- lo que implica -ffinite-math-only o reescribe el modelo de FP (hallazgo 2)
    for tok in ("-ffp-model=fast", "-ffp-model=precise", "-funsafe-math-optimizations"):
        check(f"con {tok} FALLA", dice(fixture(FLAGS_FIXED + " " + tok), f"tiene {tok}"))
    check("-ffast-math en el LINK FALLA (el codegen de LTO pasa por ahí)",
          dice(fixture(link_lang=LINK_LANG_FIXED + " -ffast-math"), "link de"))

    # --- gana la ÚLTIMA: una negación posterior anula (hallazgo 3)
    for tok, needle in (("-fno-lto", "el último es -fno-lto"),
                        ("-flto=full", "el último es -flto=full"),
                        ("-fno-tree-vectorize", "la anula un -fno-tree-vectorize"),
                        ("-fno-vectorize", "la anula un -fno-vectorize"),
                        ("-fmath-errno", "la anula un -fmath-errno"),
                        ("-ftrapping-math", "la anula un -ftrapping-math"),
                        ("-fsigned-zeros", "la anula un -fsigned-zeros"),
                        ("-fno-reciprocal-math", "la anula un -fno-reciprocal-math"),
                        ("-fno-associative-math", "la anula un -fno-associative-math"),
                        ("-fno-fast-math", "falta -fassociative-math (la anula un -fno-fast-math"),
                        ("-fno-unsafe-math-optimizations",
                         "falta -freciprocal-math (la anula un -fno-unsafe-math-optimizations")):
        check(f"{tok} DESPUÉS FALLA", dice(fixture(FLAGS_FIXED + " " + tok), needle))
    check("un -fno-lto al final del LINK FALLA",
          dice(fixture(link=LINK_FIXED + " -fno-lto"), "lo anula -fno-lto"))
    check("-fvectorize vale como -ftree-vectorize (no es un falso rojo)",
          problems(fixture(drop(FLAGS_FIXED, "-ftree-vectorize") + " -fvectorize")) == [])

    # --- un .a con regla se recorre aunque la ruta sea absoluta (hallazgo 6)
    res = check_ninja(fixture(effects_flags=FLAGS_HOY, effects_lib="/abs/tree/libwatermelon-effects.a"))
    check("un .a del árbol con ruta ABSOLUTA se recorre igual",
          any("EffectChain" in p for p in res.problems), f"dio {res.problems}")
    check("el .a prebuilt sin regla se saltea, y se DICE",
          check_ninja(fixture()).skipped == ["/abs/prebuilt/libfoo.a"],
          f"dio {check_ninja(fixture()).skipped}")

    # --- las secciones de la copia sin strip (hallazgo 5)
    readelf = ("There are 3 section headers:\n"
               "  [Nr] Name              Type            Address          Off    Size\n"
               "  [ 0]                   NULL            0000000000000000 000000 000000\n"
               "  [14] .text             PROGBITS        00000000000778e0 0778e0 0d4f68\n"
               "  [30] .debug_info       PROGBITS        0000000000000000 12f000 5e2c11\n"
               "  [33] .debug_line       PROGBITS        0000000000000000 81a000 1a0000\n"
               "  [36] .symtab           SYMTAB          0000000000000000 a00000 020000\n")
    names = section_names(readelf)
    check("lee los nombres de sección de llvm-readelf -S -W",
          {".text", ".debug_info", ".debug_line", ".symtab"} <= names, f"leyó {names}")
    check("con .symtab, .debug_info y .debug_line no hay problema", debug_problems(names) == [])
    for sec in (".symtab", ".debug_info", ".debug_line"):
        check(f"sin {sec} FALLA", any(sec in p for p in debug_problems(names - {sec})))
    check("cero secciones NO es un pase", levanta(lambda: debug_problems(set()), "ninguna sección"))

    # --- la respuesta de `ninja -n` (salidas reales de ninja 1.10.2)
    check("`ninja -n` sin trabajo = al día",
          ninja_up_to_date("ninja: Entering directory `.'\nninja: no work to do."))
    check("`ninja -n` con un link planeado = desactualizado",
          not ninja_up_to_date("ninja: Entering directory `.'\n"
                               "[1/1] Linking CXX shared library /abs/obj/arm64-v8a/" + LIB))
    check("`ninja -n` mudo NO es al día", not ninja_up_to_date(""))

    # --- de dónde saca ninja y readelf: el CMakeCache del mismo build
    cache = ("CMAKE_MAKE_PROGRAM:UNINITIALIZED=/sdk/cmake/3.22.1/bin/ninja\n"
             "CMAKE_READELF:FILEPATH=/ndk/bin/llvm-readelf\n"
             "CMAKE_READELF-ADVANCED:INTERNAL=1\n")
    check("lee CMAKE_MAKE_PROGRAM y CMAKE_READELF del CMakeCache",
          cache_value(cache, "CMAKE_MAKE_PROGRAM") == "/sdk/cmake/3.22.1/bin/ninja"
          and cache_value(cache, "CMAKE_READELF") == "/ndk/bin/llvm-readelf"
          and cache_value(cache, "CMAKE_NM") is None)
    # --- RealTools.up_to_date de punta a punta, con un ninja REAL sobre un
    # build.ninja de juguete: nunca construido, construido, y con la entrada
    # tocada después. Sin ninja el self-test FALLA: no lo saltea.
    ninja_bin = find_ninja()
    check("hay un ninja para probar `ninja -n` de verdad", ninja_bin is not None,
          "(ni en el PATH ni en <sdk>/cmake/*/bin)")
    if ninja_bin is not None:
        with tempfile.TemporaryDirectory() as d:
            bd = Path(d)
            (bd / "CMakeCache.txt").write_text(f"CMAKE_MAKE_PROGRAM:FILEPATH={ninja_bin}\n")
            (bd / "build.ninja").write_text("rule cp\n  command = cp $in $out\n"
                                            "build out.so: cp in.txt\n")
            (bd / "in.txt").write_text("x")
            rt = RealTools()
            nunca, _ = rt.up_to_date(bd, "out.so")
            subprocess.run([ninja_bin, "-C", d, "out.so"], capture_output=True, check=True)
            aldia, _ = rt.up_to_date(bd, "out.so")
            t = (bd / "out.so").stat().st_mtime + 10
            os.utime(bd / "in.txt", (t, t))
            tocado, _ = rt.up_to_date(bd, "out.so")
            check("`ninja -n` real: sin construir, construido, entrada tocada",
                  (nunca, aldia, tocado) == (False, True, False), f"dio {(nunca, aldia, tocado)}")

    with tempfile.TemporaryDirectory() as d:
        check("sin CMakeCache.txt NO es un pase",
              levanta(lambda: RealTools().sections(Path(d), Path(d) / LIB), "CMakeCache.txt"))

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

        class FakeTools:
            def __init__(self) -> None:
                self.stale: set[str] = set()
                self.secs: dict[str, set[str]] = {}

            def up_to_date(self, build_dir: Path, target: str) -> tuple[bool, str]:
                if target in self.stale:
                    return False, "[1/1] Linking CXX shared library " + target
                return True, "ninja: no work to do."

            def sections(self, build_dir: Path, so: Path) -> set[str]:
                return self.secs.get(str(so), {".text", *DEBUG_SECTIONS})

        tools = FakeTools()

        def tree_ok() -> tuple[bool, list[str]]:
            try:
                _, probs = check_tree(audio, tools)
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

        # Dos builds con el MISMO contenido; el segundo, con flags viejas: se
        # revisan los dos, no el primero que aparezca (hallazgo 6).
        make("arm64-v8a", b"arm64-v8a", fixture(FLAGS_HOY, LINK_HOY, LINK_LANG_HOY, so="@SO@"),
             bhash="h2")
        ok, probs = tree_ok()
        check("con dos builds idénticos se revisan TODOS",
              ok and any(p.startswith("[arm64-v8a]") and "-O2" in p for p in probs), f"dio {probs}")
        shutil.rmtree(audio / "build/intermediates/cxx/RelWithDebInfo/h2")

        # El .so no está al día con su build.ninja (se regeneró y el link falló).
        so_arm = str(audio / "build/intermediates/cxx/RelWithDebInfo/h1/obj/arm64-v8a" / LIB)
        tools.stale.add(so_arm)
        ok, probs = tree_ok()
        check("un .so desactualizado respecto de su build.ninja NO es un pase",
              not ok and "NO está al día" in probs[0], f"dio {probs}")
        tools.stale.clear()

        # La copia sin strip perdió la debug info.
        merged_x86 = audio / ("build/intermediates/merged_native_libs/release/"
                              "mergeReleaseNativeLibs/out/lib/x86") / LIB
        tools.secs[str(merged_x86)] = {".text", ".symtab"}
        ok, probs = tree_ok()
        check("un merged sin .debug_info FALLA",
              ok and any(p.startswith("[x86]") and ".debug_info" in p for p in probs), f"dio {probs}")
        tools.secs.clear()

        # Dos merged para la misma ABI.
        otro = audio / "build/intermediates/merged_native_libs/release/otraTask/out/lib/x86"
        otro.mkdir(parents=True)
        (otro / LIB).write_bytes(b"x86")
        ok, probs = tree_ok()
        check("dos .so empaquetados para una ABI NO es un pase",
              not ok and "hay 2" in probs[0], f"dio {probs}")
        shutil.rmtree(audio / "build/intermediates/merged_native_libs/release/otraTask")

        ok, probs = tree_ok()
        check("de vuelta al árbol en regla, PASA", ok and probs == [], f"dio {probs}")

        # Se pierde una ABI.
        shutil.rmtree(audio / "build/intermediates/merged_native_libs/release/"
                      "mergeReleaseNativeLibs/out/lib/x86_64")
        ok, probs = tree_ok()
        check("perder una ABI NO es un pase",
              not ok and probs[0].startswith("[x86_64] no hay " + LIB), f"dio {probs}")
        make("x86_64", b"x86_64", None)

        # El empaquetado no coincide con ningún build.
        (audio / "build/intermediates/merged_native_libs/release/mergeReleaseNativeLibs/"
         "out/lib/armeabi-v7a" / LIB).write_bytes(b"de otro lado")
        ok, probs = tree_ok()
        check("un .so empaquetado sin build que lo produjo NO es un pase",
              not ok and probs[0].startswith("[armeabi-v7a] ningún build"), f"dio {probs}")
        make("armeabi-v7a", b"armeabi-v7a", None)

        # Falta el build.ninja del build que coincide.
        (audio / ".cxx/RelWithDebInfo/h1/arm64-v8a/build.ninja").unlink()
        ok, probs = tree_ok()
        check("sin el build.ninja del build que coincide NO es un pase",
              not ok and "no existe" in probs[0] and "build.ninja" in probs[0], f"dio {probs}")

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
