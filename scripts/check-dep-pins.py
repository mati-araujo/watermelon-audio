#!/usr/bin/env python3
# ============================================================================
# Guardrail REQ-047 S4 (AC-047.13): UN lugar por pin.
#
# Contesta una pregunta: ¿algun pin que tiene que tener UNA sola fuente aparece
# escrito en DOS lugares, o con dos valores? Es la deriva que REQ-047 encontro
# medida el 29/09: googletest escrito en nueve CMakeLists.txt, el NDK en tres
# archivos (catalogo, ci.yml x2, publish.yml), y el Xcode del CI sin escribir en
# ningun lado. Un pin duplicado se bumpea en un lugar y en el otro no, y el build
# sigue verde con dos versiones distintas.
#
# Las reglas (cada una con su clave, que es lo que el --self-test exige ver):
#
#   googletest       exactamente UNA declaracion de googletest en todo el arbol
#                    (`FetchContent_Declare(googletest` o un GIT_REPOSITORY que
#                    apunte a google/googletest), fijada por SHA completo con el tag
#                    en comentario (`GIT_TAG <40 hex>  # vX.Y.Z`), o por URL con
#                    `URL_HASH SHA256=`. Hoy vive en thirdparty/googletest.cmake.
#   ndk-cmake        ningun workflow, build de Gradle ni script declara el NDK o
#                    el CMake del SDK a mano (`ndk;N`, `cmake;N`, `ndkVersion = "…"`,
#                    ni el valor literal del NDK del catalogo). La fuente es
#                    gradle/libs.versions.toml; los workflows la leen con
#                    scripts/android-sdk-packages.py y build-logic con libs.version.
#   java-version     todos los `java-version:` de los workflows valen lo mismo, y
#                    ese major es el de `launcher_jvm` de .github/toolchain-pins.json
#                    (el JDK con el que el gate local atesta).
#   xcode            ningun workflow fija un Xcode a mano (`Xcode_N.app`,
#                    DEVELOPER_DIR, `xcode-select -s`): la fuente es la clave `xcode`
#                    de toolchain-pins.json. Y TODO job de macOS corre
#                    scripts/ci-select-xcode.sh, que la lee.
#   actions          toda action remota esta fijada por SHA completo con el tag en
#                    comentario (`owner/repo@<40 hex> # vX.Y.Z`), y una misma action
#                    no aparece con dos SHA ni con dos tags.
#   permissions      todo workflow declara `permissions:` a nivel de archivo (sin
#                    eso el GITHUB_TOKEN depende del default del repo), y ci.yml
#                    —que corre el codigo de cualquier PR— no pide escritura.
#   persist-credentials  todo `actions/checkout` de un job con permisos de
#                    escritura lleva `persist-credentials: false`: el token no se
#                    queda en .git/config para cualquier paso posterior.
#   gradle-wrapper   gradle-wrapper.properties declara `distributionSha256Sum`
#                    (64 hex): el wrapper verifica la distribucion que baja.
#   leer             "no pude medir" no es un pase: cero declaraciones de googletest,
#                    cero `uses:`, cero `java-version:`, un catalogo sin `ndk`, sin
#                    gradle-wrapper.properties, o un archivo ilegible, son rojos.
#
# Source-only (python3 y nada mas, centesimas). El --self-test arma arboles de
# juguete, uno por regla, y exige que cada violacion salga roja con SU clave y
# que el arbol limpio salga verde. Corre ANTES del lint en gate.sh y en el CI,
# por la misma razon que check-rt-safety: si el parser se rompe, el lint queda
# verde para siempre revisando nada.
#
# Usage:
#   python3 scripts/check-dep-pins.py [--root DIR]
#   python3 scripts/check-dep-pins.py --self-test
# ============================================================================
import argparse
import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import tomllib

CATALOG = "gradle/libs.versions.toml"
PINS = ".github/toolchain-pins.json"
WORKFLOWS_DIR = ".github/workflows/"
XCODE_SCRIPT = "scripts/ci-select-xcode.sh"

SKIP_DIRS = {".git", ".gradle", ".cxx", "build", "build-san", ".deps", ".idea", ".kotlin", "node_modules"}

FC_DECL = re.compile(r"FetchContent_Declare\(\s*([\w-]+)", re.I)
GTEST_REPO = re.compile(r"github\.com[/:]google/googletest", re.I)
FIND_GTEST = re.compile(r"find_package\(\s*GTest\b", re.I)
GTEST_TAG = re.compile(r"GIT_TAG\s+([^\s)]+)")
GTEST_URL = re.compile(r"\bURL\s+([^\s)]+)")
CMAKE_SET = re.compile(r"set\(\s*(\w+)\s+\"?([^\")\s]+)\"?\s*\)")
GIT_SHA = re.compile(r"^[0-9a-f]{40}$")
URL_HASH = re.compile(r"\bURL_HASH\s+SHA256=[0-9a-fA-F]{64}\b")
WRAPPER = "gradle/wrapper/gradle-wrapper.properties"
WRAPPER_SUM = re.compile(r"^distributionSha256Sum\s*=\s*([0-9a-f]{64})\s*$", re.M)
PERMISSIONS_KEY = re.compile(r"^permissions:(.*)$")
WRITE_GRANT = re.compile(r"(^|\s)[\w-]+:\s*write\s*(#.*)?$|permissions:\s*write-all")
CHECKOUT = re.compile(r"uses:\s*['\"]?actions/checkout@")
NO_PERSIST = re.compile(r"^persist-credentials:\s*['\"]?false['\"]?\s*(#.*)?$", re.I)
CI_WORKFLOW = WORKFLOWS_DIR + "ci.yml"

SDK_PKG = re.compile(r"\b(ndk|cmake);[0-9]")
NDK_VERSION_ASSIGN = re.compile(r"ndkVersion\s*(=|\()\s*\"")
XCODE_LITERAL = re.compile(r"Xcode_[0-9]|DEVELOPER_DIR|xcode-select\s+(-s|--switch)")
JAVA_VERSION = re.compile(r"(?<![\w-])java-version:\s*['\"]?([^'\"\s,}#]+)")
JAVA_VERSION_FILE = re.compile(r"(?<![\w-])java-version-file:")
USES = re.compile(r"(?:^|[\s{,])uses:\s*['\"]?([^'\"\s,}]+)['\"]?(.*)$")
USES_PINNED = re.compile(r"^([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)(/[^@\s]*)?@([0-9a-f]{40})$")
USES_COMMENT = re.compile(r"^\s*#\s*(v[0-9]+(\.[0-9]+)*)\s*$")
JOBS_KEY = re.compile(r"^jobs:\s*(#.*)?$")
RUNS_ON = re.compile(r"^\s+runs-on:\s*(.*?)\s*(#.*)?$")
NEUTRALIZE_CONTINUE = re.compile(r"^continue-on-error:\s*['\"]?true", re.I)
NEUTRALIZE_IF = re.compile(r"^if:.*\bfalse\b", re.I)
XCODE_CALL = re.compile(r"^(-\s*)?(run:\s*)?bash\s+" + re.escape("scripts/ci-select-xcode.sh") + r"\s*$")


class Unreadable(Exception):
    pass


def tracked_files(root):
    """Los archivos trackeados si `root` es un repo git; si no (el self-test), un walk."""
    try:
        out = subprocess.run(["git", "-C", root, "ls-files", "-z"], check=True,
                             capture_output=True, text=True).stdout
        if out:
            return sorted(p for p in out.split("\0") if p)
    except (OSError, subprocess.CalledProcessError):
        pass
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            found.append(os.path.relpath(os.path.join(dirpath, name), root))
    return sorted(found)


def read(root, rel):
    try:
        with open(os.path.join(root, rel), encoding="utf-8") as fh:
            return fh.read()
    except (OSError, UnicodeDecodeError) as exc:
        raise Unreadable(f"{rel}: {exc}")


def is_cmake(rel):
    return rel.endswith(".cmake") or os.path.basename(rel) == "CMakeLists.txt"


def is_workflow(rel):
    return rel.startswith(WORKFLOWS_DIR) and rel.endswith((".yml", ".yaml"))


def is_action_file(rel):
    """Workflows y composite actions locales: donde puede aparecer un `uses:`."""
    return is_workflow(rel) or (rel.startswith(".github/actions/")
                                and os.path.basename(rel) in ("action.yml", "action.yaml"))


def strip_yaml_comment_lines(text):
    return [(n, line) for n, line in enumerate(text.splitlines(), 1) if not line.lstrip().startswith("#")]


def is_build_config(rel):
    """Donde un NDK/CMake escrito a mano SI cambia lo que se instala o se usa."""
    return (is_action_file(rel) or rel.endswith((".gradle.kts", ".gradle", ".properties"))
            or (rel.startswith("build-logic/") and rel.endswith(".kt"))
            or (rel.startswith("scripts/") and rel.endswith(".sh")))


def cmake_code(text):
    """El CMake sin comentarios `#...`: un comentario que NOMBRA la declaracion no es una."""
    return re.sub(r"#[^\n]*", "", text)


def fc_blocks(code):
    """(nombre, texto) de cada FetchContent_Declare(...), cortado en su parentesis de cierre."""
    for m in FC_DECL.finditer(code):
        depth, i = 1, code.index("(", m.start()) + 1
        while i < len(code) and depth:
            depth += {"(": 1, ")": -1}.get(code[i], 0)
            i += 1
        yield m.group(1), code[m.start():i], (m.start(), i)


def check_googletest(root, files, findings):
    """Cada DECLARACION cuenta (no cada archivo): dos en el mismo .cmake son dos fuentes."""
    decls = []
    for rel in files:
        if not is_cmake(rel):
            continue
        raw = read(root, rel)
        code = cmake_code(raw)
        variables = dict(CMAKE_SET.findall(code))
        spans = []
        for name, block, span in fc_blocks(code):
            spans.append(span)
            if name.lower() != "googletest" and not GTEST_REPO.search(block):
                continue
            tag = GTEST_TAG.search(block)
            url = GTEST_URL.search(block)
            value = tag.group(1) if tag else (url.group(1) if url else None)
            if value:
                value = re.sub(r"\$\{(\w+)\}", lambda m: variables.get(m.group(1), m.group(0)), value)
            if tag:
                # Por SHA (un tag se mueve, un commit no) y con el tag en el comentario de
                # ESA linea, que es lo que se lee y lo que se reporta.
                note = re.search(r"GIT_TAG\s+" + re.escape(tag.group(1)) + r"\s*#\s*(v[0-9][\w.]*)", raw)
                fixed = bool(GIT_SHA.match(value or "")) and bool(note)
                if fixed:
                    value = note.group(1)
            else:
                fixed = bool(value) and bool(URL_HASH.search(block))
            decls.append((rel, value, fixed))
        # googletest por otro camino: un repo fuera de un FetchContent_Declare
        # (ExternalProject, un add_subdirectory de un clone) o el del sistema.
        for m in GTEST_REPO.finditer(code):
            if not any(a <= m.start() < b for a, b in spans):
                decls.append((rel, "(fuera de FetchContent_Declare)", False))
        for _m in FIND_GTEST.finditer(code):
            decls.append((rel, "find_package(GTest), el del sistema", False))
    if not decls:
        findings.append(("leer", "ninguna declaracion de googletest en el arbol: no pude medir el pin"))
        return None
    if len(decls) > 1:
        lista = ", ".join(f"{r} ({t})" for r, t, _ in decls)
        findings.append(("googletest", f"googletest declarado {len(decls)} veces, tiene que ser UNA: {lista}"))
    for rel, value, fixed in decls:
        if not fixed:
            findings.append(("googletest", f"{rel}: googletest en {value!r}: no esta fijado por SHA completo "
                                           f"con el tag en comentario (ni por URL con URL_HASH SHA256)"))
    return [(r, t) for r, t, _ in decls]


def catalog_ndk(root, findings):
    try:
        with open(os.path.join(root, CATALOG), "rb") as fh:
            versions = tomllib.load(fh).get("versions", {})
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise Unreadable(f"{CATALOG}: {exc}")
    ndk = versions.get("ndk") if isinstance(versions, dict) else None
    if not isinstance(ndk, str) or not ndk:
        findings.append(("leer", f"{CATALOG} no declara [versions].ndk: no pude medir la fuente del NDK"))
        return None
    return ndk


def check_ndk_cmake(root, files, findings):
    ndk = catalog_ndk(root, findings)
    for rel in files:
        if rel == CATALOG or not is_build_config(rel):
            continue
        text = read(root, rel)
        for n, line in enumerate(text.splitlines(), 1):
            if SDK_PKG.search(line) or NDK_VERSION_ASSIGN.search(line) or (ndk and ndk in line):
                findings.append(("ndk-cmake", f"{rel}:{n}: NDK/CMake del SDK escrito a mano "
                                              f"(la fuente es {CATALOG}): {line.strip()}"))


def check_java(root, files, findings):
    try:
        pins = json.loads(read(root, PINS))
    except json.JSONDecodeError as exc:
        raise Unreadable(f"{PINS}: {exc}")
    launcher = str(pins.get("launcher_jvm", ""))
    values = []
    for rel in files:
        if not is_action_file(rel):
            continue
        for n, line in strip_yaml_comment_lines(read(root, rel)):
            values += [(rel, v) for v in JAVA_VERSION.findall(line)]
            if JAVA_VERSION_FILE.search(line):
                findings.append(("java-version", f"{rel}:{n}: `java-version-file:` es una segunda fuente "
                                                 f"del JDK; el pin es `java-version:`"))
    if not values:
        findings.append(("leer", "ningun `java-version:` en los workflows: no pude medir el pin"))
        return
    distinct = sorted({v for _, v in values})
    if len(distinct) > 1:
        findings.append(("java-version", "java-version con valores distintos: "
                         + ", ".join(f"{r}={v}" for r, v in values)))
    major = launcher.split(".")[0]
    for rel, v in values:
        if v.split(".")[0] != major:
            findings.append(("java-version", f"{rel}: java-version {v} no es el major de "
                             f"launcher_jvm {launcher!r} de {PINS}"))


def jobs_of(text):
    """(nombre, lineas) de cada job. La indentacion de los jobs se toma de la primera clave
    bajo `jobs:` (no se asume de 2 espacios), y un encabezado puede llevar comentario."""
    lines = text.splitlines()
    start = next((i for i, l in enumerate(lines) if JOBS_KEY.match(l)), None)
    if start is None:
        return []
    body = [(i, l) for i, l in enumerate(lines[start + 1:], start + 1)
            if l.strip() and not l.lstrip().startswith("#")]
    if not body:
        return []
    indent = len(body[0][1]) - len(body[0][1].lstrip(" "))
    header = re.compile(r"^ {%d}([A-Za-z0-9_-]+):\s*(#.*)?$" % indent)
    jobs, cur = [], None
    for _i, l in body:
        lead = len(l) - len(l.lstrip(" "))
        if lead < indent:
            break  # otra clave de primer nivel despues de `jobs:`
        m = header.match(l) if lead == indent else None
        if m:
            cur = (m.group(1), [])
            jobs.append(cur)
        elif cur:
            cur[1].append(l)
    return jobs


def runs_on_of(job_lines):
    """El valor de `runs-on`, con la forma de lista en lineas siguientes incluida."""
    for k, line in enumerate(job_lines):
        m = RUNS_ON.match(line)
        if not m:
            continue
        value = m.group(1)
        lead = len(line) - len(line.lstrip(" "))
        for nxt in job_lines[k + 1:]:
            if len(nxt) - len(nxt.lstrip(" ")) <= lead:
                break
            value += " " + nxt.strip()
        return value
    return None


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def effective_call(job_lines, k):
    """Si la llamada de la linea k CORRE de verdad: ni su paso ni el job la neutralizan con
    `continue-on-error: true` (el script sale 1 y el job sigue con el Xcode default) ni con
    un `if:` que contenga `false`."""
    start = k
    while start > 0 and not job_lines[start].lstrip().startswith("- "):
        start -= 1
    s_ind = indent_of(job_lines[start])
    end = start + 1
    while end < len(job_lines):
        l = job_lines[end]
        if indent_of(l) < s_ind or (indent_of(l) == s_ind and l.lstrip().startswith("- ")):
            break
        end += 1
    step = [l.strip().lstrip("- ").strip() for l in job_lines[start:end]]
    if any(NEUTRALIZE_CONTINUE.match(l) for l in step):
        return False
    if any(NEUTRALIZE_IF.match(l) for l in step):
        return False
    job_level = [l.strip() for l in job_lines if indent_of(l) < s_ind]
    return not any(NEUTRALIZE_CONTINUE.match(l) for l in job_level)


def check_xcode(root, files, findings):
    for rel in files:
        if not is_action_file(rel):
            continue
        text = read(root, rel)
        for n, line in strip_yaml_comment_lines(text):
            if XCODE_LITERAL.search(line):
                findings.append(("xcode", f"{rel}:{n}: Xcode fijado a mano (la fuente es la clave "
                                          f"`xcode` de {PINS}): {line.strip()}"))
        if not is_workflow(rel):
            continue
        for job, job_lines in jobs_of(text):
            runs = runs_on_of(job_lines)
            if runs is None:
                continue
            if "${{" in runs:
                findings.append(("xcode", f"{rel}: el job `{job}` tiene `runs-on: {runs}`, dinamico: no puedo "
                                          f"saber si corre en macOS ni exigirle {XCODE_SCRIPT}"))
                continue
            calls = any(XCODE_CALL.match(l.strip()) and effective_call(job_lines, k)
                        for k, l in enumerate(job_lines))
            if "macos" in runs.lower() and not calls:
                findings.append(("xcode", f"{rel}: el job `{job}` corre en {runs} y no llama a "
                                          f"{XCODE_SCRIPT} en un paso que corra y pueda fallar (sin "
                                          f"`continue-on-error: true` ni `if:` con false): usaria el "
                                          f"Xcode default de la imagen"))


def check_actions(root, files, findings):
    seen = {}
    total = 0
    for rel in files:
        if not is_action_file(rel):
            continue
        text = read(root, rel)
        for n, line in strip_yaml_comment_lines(text):
            m = USES.search(line)
            if not m:
                continue
            ref, rest = m.group(1).strip("'\""), m.group(2)
            if ref.startswith("./") or ref.startswith("docker://"):
                continue
            total += 1
            pinned = USES_PINNED.match(ref)
            comment = USES_COMMENT.match(rest)
            if not pinned or not comment:
                findings.append(("actions", f"{rel}:{n}: `{ref}{rest}` no esta fijada por SHA completo "
                                            f"con el tag en comentario (`owner/repo@<40 hex> # vX.Y.Z`)"))
                continue
            key = pinned.group(1).lower()
            seen.setdefault(key, set()).add((pinned.group(3), comment.group(1)))
    if total == 0:
        findings.append(("leer", "ningun `uses:` en los workflows: no pude medir las actions"))
    for key, pins in sorted(seen.items()):
        if len(pins) > 1:
            findings.append(("actions", f"{key} aparece con {len(pins)} pins distintos: "
                             + ", ".join(f"{s[:12]} # {t}" for s, t in sorted(pins))))


def top_permissions(lines):
    """(declara, lineas del bloque) del `permissions:` de primer nivel de un workflow."""
    for i, line in enumerate(lines):
        m = PERMISSIONS_KEY.match(line)
        if not m:
            continue
        block = [line]
        for nxt in lines[i + 1:]:
            if nxt.strip() and not nxt.startswith((" ", "\t")):
                break
            block.append(nxt)
        return True, block
    return False, []


def job_permissions(job_lines):
    """Las lineas del `permissions:` propio del job (None si no declara: hereda el de arriba)."""
    keys = [l for l in job_lines if l.strip() and not l.lstrip().startswith("#")]
    if not keys:
        return None
    key_indent = min(indent_of(l) for l in keys)
    for k, line in enumerate(job_lines):
        if indent_of(line) == key_indent and line.strip().startswith("permissions:"):
            block = [line]
            for nxt in job_lines[k + 1:]:
                if nxt.strip() and indent_of(nxt) <= key_indent:
                    break
                block.append(nxt)
            return block
    return None


def grants_write(block):
    return any(WRITE_GRANT.search(l) for l in block if not l.lstrip().startswith("#"))


def step_bounds(job_lines, k):
    start = k
    while start > 0 and not job_lines[start].lstrip().startswith("- "):
        start -= 1
    s_ind = indent_of(job_lines[start])
    end = start + 1
    while end < len(job_lines):
        l = job_lines[end]
        if indent_of(l) < s_ind or (indent_of(l) == s_ind and l.lstrip().startswith("- ")):
            break
        end += 1
    return start, end


def check_permissions(root, files, findings):
    for rel in files:
        if not is_workflow(rel):
            continue
        lines = read(root, rel).splitlines()
        declared, top = top_permissions(lines)
        if not declared:
            findings.append(("permissions", f"{rel}: no declara `permissions:` a nivel de archivo; el "
                                            f"GITHUB_TOKEN queda con el default del repo"))
        jobs = jobs_of("\n".join(lines))
        if rel == CI_WORKFLOW:
            blocks = [top] + [job_permissions(jl) or [] for _j, jl in jobs]
            if any(grants_write(b) for b in blocks):
                findings.append(("permissions", f"{rel}: pide escritura, y corre el codigo de cualquier PR"))
        for job, job_lines in jobs:
            own = job_permissions(job_lines)
            if not grants_write(own if own is not None else top):
                continue
            for k, line in enumerate(job_lines):
                if line.lstrip().startswith("#") or not CHECKOUT.search(line):
                    continue
                a, b = step_bounds(job_lines, k)
                step = [l.strip().lstrip("- ").strip() for l in job_lines[a:b]]
                if not any(NO_PERSIST.match(l) for l in step):
                    findings.append(("persist-credentials", f"{rel}: el job `{job}` tiene permisos de escritura "
                                     f"y su checkout no lleva `persist-credentials: false`"))


def check_wrapper(root, files, findings):
    # Sin el archivo, read() lanza Unreadable y run_checks lo vuelve "leer": no pude medir.
    if not WRAPPER_SUM.search(read(root, WRAPPER)):
        findings.append(("gradle-wrapper", f"{WRAPPER} no declara `distributionSha256Sum` (64 hex): el "
                                           f"wrapper no verifica la distribucion que baja"))


def run_checks(root):
    findings = []
    try:
        files = tracked_files(root)
        gtest = check_googletest(root, files, findings)
        check_ndk_cmake(root, files, findings)
        check_java(root, files, findings)
        check_xcode(root, files, findings)
        check_actions(root, files, findings)
        check_permissions(root, files, findings)
        check_wrapper(root, files, findings)
    except Unreadable as exc:
        findings.append(("leer", f"ilegible: {exc}"))
        gtest = None
    return findings, gtest


def main_check(root):
    findings, gtest = run_checks(root)
    if findings:
        print(f"check-dep-pins: ROJO — {len(findings)} hallazgo(s)")
        for key, msg in findings:
            print(f"  [{key}] {msg}")
        return 1
    rel, tag = gtest[0]
    print(f"check-dep-pins: verde — googletest {tag} en {rel}; NDK/CMake sólo en {CATALOG}; "
          f"java-version, Xcode y actions con una sola fuente; permisos declarados, checkouts con "
          f"escritura sin credenciales persistidas y wrapper con checksum")
    return 0


# --- self-test ---------------------------------------------------------------

SHA_A = "a" * 40
SHA_B = "b" * 40
GT_SHA = "063de7e9578f82b369302001269680b4b1553359"
GT_TAG_LINE = f"GIT_TAG {GT_SHA}  # v1.18.0"
SUM = "acd53f1edaf02f1a8ff99879f8a34b302661a057d9b063ae9e35b552f804d20a"

CLEAN = {
    CATALOG: '[versions]\nndk = "30.0.16248370"\ncmake = "3.22.1"\n',
    PINS: json.dumps({"xcode": "26.6 (17F113)", "launcher_jvm": "17.0.19"}),
    "audio/src/main/cpp/thirdparty/googletest.cmake":
        '# un comentario que nombra FetchContent_Declare(googletest no es una declaracion\n'
        'FetchContent_Declare(googletest\n'
        '    GIT_REPOSITORY https://github.com/google/googletest.git\n'
        f'    {GT_TAG_LINE}\n)\n',
    "audio/src/main/cpp/dsp/tests/CMakeLists.txt":
        "include(${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/googletest.cmake)\n",
    "build-logic/convention/src/main/kotlin/Plugin.kt": 'ndkVersion = libs.version("ndk")\n',
    WRAPPER: f"distributionSha256Sum={SUM}\ndistributionUrl=https\\://services.gradle.org/distributions/gradle-9.7.1-bin.zip\n",
    ".github/workflows/ci.yml":
        "name: CI\n# Xcode_26.6.app en un comentario no cuenta\npermissions:\n  contents: read\njobs:\n"
        "  build:\n    runs-on: ubuntu-latest\n    steps:\n"
        f"      - uses: actions/checkout@{SHA_A} # v7.0.1\n"
        f"      - uses: actions/setup-java@{SHA_B} # v6.0.1\n        with:\n          java-version: 17\n"
        "      - run: PKGS=\"$(python3 scripts/android-sdk-packages.py)\"\n"
        "      - uses: ./local-action\n"
        "  ios:\n    runs-on: macos-latest\n    steps:\n"
        f"      - uses: actions/checkout@{SHA_A} # v7.0.1\n"
        "      - run: bash scripts/ci-select-xcode.sh\n",
    ".github/workflows/publish.yml":
        "name: Publish\npermissions:\n  packages: write  # el publish\n  contents: read\n"
        "jobs:\n  publish:\n    runs-on: macos-latest\n    steps:\n"
        f"      - uses: actions/checkout@{SHA_A} # v7.0.1\n        with:\n          persist-credentials: false\n"
        f"      - uses: gradle/actions/setup-gradle@{SHA_A} # v6.4.0\n"
        "      - run: bash scripts/ci-select-xcode.sh\n"
        f"      - uses: actions/setup-java@{SHA_B} # v6.0.1\n        with:\n          java-version: '17'\n",
}


def ci_checkouts_without_credentials(files):
    """Los checkouts de ci.yml con persist-credentials: false, para que un caso de
    `permissions` no dispare ademas la regla de credenciales (cada caso, SU clave)."""
    p = ".github/workflows/ci.yml"
    files[p] = files[p].replace(f"      - uses: actions/checkout@{SHA_A} # v7.0.1\n",
                                f"      - uses: actions/checkout@{SHA_A} # v7.0.1\n"
                                "        with:\n          persist-credentials: false\n")


def mutate(path, old, new):
    def apply(files):
        assert old in files[path], f"el caso no encuentra su patron en {path}: {old!r}"
        files[path] = files[path].replace(old, new, 1)
    return apply


def add(path, content):
    def apply(files):
        files[path] = content
    return apply


def remove(path):
    def apply(files):
        del files[path]
    return apply


CASES = [
    # (nombre, mutacion, clave esperada)
    ("googletest declarado otra vez en una suite hija", add(
        "audio/src/main/cpp/core/tests/CMakeLists.txt",
        "FetchContent_Declare(googletest\n  GIT_REPOSITORY https://github.com/google/googletest.git\n"
        f"  GIT_TAG {SHA_B}  # v1.15.2\n)\n"), "googletest"),
    ("googletest declarado con otro nombre pero el mismo repo", add(
        "audio/src/main/cpp/voice/tests/CMakeLists.txt",
        "FetchContent_Declare(gtest\n  GIT_REPOSITORY https://github.com/google/googletest.git\n"
        f"  GIT_TAG {GT_SHA}  # v1.18.0\n)\n"), "googletest"),
    ("googletest en una rama y no en un tag", mutate(
        "audio/src/main/cpp/thirdparty/googletest.cmake", GT_TAG_LINE,
        "GIT_TAG main"), "googletest"),
    ("googletest sin declarar en ningun lado", remove(
        "audio/src/main/cpp/thirdparty/googletest.cmake"), "leer"),
    ("NDK como paquete literal en un workflow", mutate(
        ".github/workflows/ci.yml", 'PKGS="$(python3 scripts/android-sdk-packages.py)"',
        'sdkmanager --install "ndk;30.0.16248370"'), "ndk-cmake"),
    ("CMake del SDK como paquete literal en un workflow", mutate(
        ".github/workflows/publish.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      - run: bash scripts/ci-select-xcode.sh\n      - run: sdkmanager 'cmake;3.22.1'\n"),
     "ndk-cmake"),
    ("ndkVersion literal en build-logic", mutate(
        "build-logic/convention/src/main/kotlin/Plugin.kt", 'libs.version("ndk")', '"28.2.13676358"'),
     "ndk-cmake"),
    ("el valor del NDK del catalogo repetido en un script", add(
        "scripts/instalar.sh", "echo 30.0.16248370\n"), "ndk-cmake"),
    ("catalogo sin ndk", mutate(CATALOG, 'ndk = "30.0.16248370"\n', ""), "leer"),
    ("java-version con dos valores", mutate(
        ".github/workflows/publish.yml", "java-version: '17'", "java-version: 21"), "java-version"),
    ("java-version que no es el launcher del gate local", lambda f: [
        mutate(".github/workflows/publish.yml", "java-version: '17'", "java-version: 21")(f),
        mutate(".github/workflows/ci.yml", "java-version: 17", "java-version: 21")(f)], "java-version"),
    ("ningun java-version", lambda f: [
        mutate(".github/workflows/publish.yml", "          java-version: '17'\n", "")(f),
        mutate(".github/workflows/ci.yml", "          java-version: 17\n", "")(f)], "leer"),
    ("Xcode_N.app escrito en un workflow", mutate(
        ".github/workflows/ci.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      - run: bash scripts/ci-select-xcode.sh\n      - run: sudo xcode-select -s /Applications/Xcode_26.5.app\n"),
     "xcode"),
    ("DEVELOPER_DIR en un workflow", mutate(
        ".github/workflows/publish.yml", "    steps:\n",
        "    env:\n      DEVELOPER_DIR: /Applications/Xcode.app\n    steps:\n"), "xcode"),
    ("un job de macOS sin el script de Xcode", mutate(
        ".github/workflows/publish.yml", "      - run: bash scripts/ci-select-xcode.sh\n", ""), "xcode"),
    # Las DOS apariciones de checkout, para que la regla de "dos pins" no lo tape.
    ("action por tag", lambda f: [f.__setitem__(p, f[p].replace(f"actions/checkout@{SHA_A} # v7.0.1",
                                                              "actions/checkout@v7 # v7.0.1"))
                                  for p in (".github/workflows/ci.yml", ".github/workflows/publish.yml")],
     "actions"),
    ("action por SHA corto", mutate(
        ".github/workflows/publish.yml", f"setup-gradle@{SHA_A}", f"setup-gradle@{SHA_A[:12]}"), "actions"),
    ("action por SHA sin el tag en comentario", mutate(
        ".github/workflows/publish.yml", f"setup-gradle@{SHA_A} # v6.4.0", f"setup-gradle@{SHA_A}"),
     "actions"),
    ("la misma action con dos SHA", mutate(
        ".github/workflows/publish.yml", f"actions/setup-java@{SHA_B} # v6.0.1",
        f"actions/setup-java@{SHA_A} # v6.0.1"), "actions"),
    ("la misma action con dos tags", mutate(
        ".github/workflows/publish.yml", f"actions/setup-java@{SHA_B} # v6.0.1",
        f"actions/setup-java@{SHA_B} # v5.0.0"), "actions"),
    ("toolchain-pins ilegible", add(PINS, "{no es json"), "leer"),
    # --- de la review de S4
    ("dos declaraciones de googletest en el MISMO archivo", mutate(
        "audio/src/main/cpp/thirdparty/googletest.cmake", f"    {GT_TAG_LINE}\n)\n",
        f"    {GT_TAG_LINE}\n)\nif(OLD)\nFetchContent_Declare(googletest\n"
        "    GIT_REPOSITORY https://github.com/google/googletest.git\n"
        f"    GIT_TAG {SHA_B}  # v1.15.2\n)\nendif()\n"),
     "googletest"),
    ("el GIT_TAG de OTRA dependencia no tapa un googletest en rama", lambda f: [
        mutate("audio/src/main/cpp/thirdparty/googletest.cmake", "\nFetchContent_Declare(googletest\n",
               "\nFetchContent_Declare(benchmark\n  GIT_REPOSITORY https://github.com/google/benchmark.git\n"
               f"  GIT_TAG {SHA_B}  # v1.9.1\n)\nFetchContent_Declare(googletest\n")(f),
        mutate("audio/src/main/cpp/thirdparty/googletest.cmake", GT_TAG_LINE,
               "GIT_TAG main")(f)], "googletest"),
    ("googletest por URL de tarball en otra suite", add(
        "audio/src/main/cpp/voice/tests/CMakeLists.txt",
        "FetchContent_Declare(gt\n  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip\n)\n"),
     "googletest"),
    ("googletest del sistema", add(
        "audio/src/main/cpp/looper/tests/CMakeLists.txt", "find_package(GTest REQUIRED)\n"), "googletest"),
    ("el NDK literal en un .properties", add("gradle.properties", "ndk.version=30.0.16248370\n"), "ndk-cmake"),
    ("java-version: mismo major, otro valor", mutate(
        ".github/workflows/publish.yml", "java-version: '17'", "java-version: '17.0.2'"), "java-version"),
    ("java-version-file como segunda fuente", mutate(
        ".github/workflows/publish.yml", "          java-version: '17'\n",
        "          java-version: '17'\n          java-version-file: .java-version\n"), "java-version"),
    ("java-version en un mapa en linea", mutate(
        ".github/workflows/publish.yml", "        with:\n          java-version: '17'\n",
        "        with: { distribution: temurin, java-version: '21' }\n"), "java-version"),
    ("xcode-select sin literal", mutate(
        ".github/workflows/ci.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      - run: bash scripts/ci-select-xcode.sh\n      - run: sudo xcode-select -s \"$XCODE\"\n"),
     "xcode"),
    ("el script de Xcode nombrado en el name de un paso, sin llamarlo", mutate(
        ".github/workflows/publish.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      - name: Fijar Xcode (ver scripts/ci-select-xcode.sh)\n        run: echo nada\n"), "xcode"),
    ("el script de Xcode nombrado sólo en un comentario", mutate(
        ".github/workflows/publish.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      # ver scripts/ci-select-xcode.sh\n"), "xcode"),
    ("dos jobs de macOS en un workflow, uno sin el script", mutate(
        ".github/workflows/ci.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      - run: bash scripts/ci-select-xcode.sh\n  ios-otro:\n    runs-on: macos-latest\n"
        "    steps:\n      - run: echo hola\n"), "xcode"),
    ("runs-on macOS en mayusculas", mutate(
        ".github/workflows/ci.yml", "  build:\n    runs-on: ubuntu-latest\n",
        "  build:\n    runs-on: macOS-14\n"), "xcode"),
    ("runs-on dinamico", mutate(
        ".github/workflows/ci.yml", "  build:\n    runs-on: ubuntu-latest\n",
        "  build:\n    runs-on: ${{ matrix.os }}\n"), "xcode"),
    ("runs-on en forma de lista", mutate(
        ".github/workflows/ci.yml", "  build:\n    runs-on: ubuntu-latest\n",
        "  build:\n    runs-on:\n      - self-hosted\n      - macos\n"), "xcode"),
    ("un encabezado de job con comentario no se funde con el anterior", mutate(
        ".github/workflows/ci.yml", "      - uses: ./local-action\n",
        "      - uses: ./local-action\n  mac2:  # un comentario\n    runs-on: macos-latest\n"
        "    steps:\n      - run: echo hola\n"), "xcode"),
    ("jobs indentados con 4 espacios", add(
        ".github/workflows/otro.yml",
        "name: Otro\npermissions:\n  contents: read\njobs:\n    mac:\n        runs-on: macos-latest\n        steps:\n"
        f"            - uses: actions/checkout@{SHA_A} # v7.0.1\n"), "xcode"),
    ("ningun uses", lambda f: [f.__setitem__(p, re.sub(r"(?m)^.*uses:.*\n", "", f[p]))
                               for p in (".github/workflows/ci.yml", ".github/workflows/publish.yml")],
     "leer"),
    ("uses en un mapa en linea, por tag", mutate(
        ".github/workflows/ci.yml", "      - uses: ./local-action\n",
        "      - uses: ./local-action\n      - { uses: actions/cache@v4 }\n"), "actions"),
    ("el paso de Xcode con continue-on-error", mutate(
        ".github/workflows/publish.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      - name: Fijar Xcode\n        continue-on-error: true\n        run: bash scripts/ci-select-xcode.sh\n"),
     "xcode"),
    ("el paso de Xcode con if: false", mutate(
        ".github/workflows/publish.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      - if: false\n        run: bash scripts/ci-select-xcode.sh\n"), "xcode"),
    ("el job de macOS entero con continue-on-error", mutate(
        ".github/workflows/publish.yml", "    runs-on: macos-latest\n",
        "    runs-on: macos-latest\n    continue-on-error: true\n"), "xcode"),
    ("java-version en una composite action", add(
        ".github/actions/java/action.yml", "runs:\n  using: composite\n  steps:\n"
        f"    - uses: actions/setup-java@{SHA_B} # v6.0.1\n      with:\n        java-version: 21\n"), "java-version"),
    ("Xcode_N.app en una composite action", add(
        ".github/actions/xc/action.yml", "runs:\n  using: composite\n  steps:\n"
        "    - run: sudo xcode-select -s /Applications/Xcode_27.0.app\n      shell: bash\n"), "xcode"),
    ("el NDK como paquete en una composite action", add(
        ".github/actions/ndk/action.yml", "runs:\n  using: composite\n  steps:\n"
        "    - run: sdkmanager 'ndk;28.0.1'\n      shell: bash\n"), "ndk-cmake"),
    ("googletest unico por URL de una rama", mutate(
        "audio/src/main/cpp/thirdparty/googletest.cmake",
        f"    GIT_REPOSITORY https://github.com/google/googletest.git\n    {GT_TAG_LINE}\n",
        "    URL https://github.com/google/googletest/archive/refs/heads/main.zip\n"), "googletest"),
    ("googletest por ExternalProject en otra suite", add(
        "audio/src/main/cpp/engines/tests/CMakeLists.txt",
        "ExternalProject_Add(gt\n  GIT_REPOSITORY https://github.com/google/googletest.git\n  GIT_TAG v1.18.0\n)\n"),
     "googletest"),
    # --- 4.13 (security-auditor, decision del humano)
    ("googletest por tag y no por SHA", mutate(
        "audio/src/main/cpp/thirdparty/googletest.cmake", GT_TAG_LINE, "GIT_TAG v1.18.0"), "googletest"),
    ("googletest por SHA corto", mutate(
        "audio/src/main/cpp/thirdparty/googletest.cmake", GT_TAG_LINE, f"GIT_TAG {GT_SHA[:12]}  # v1.18.0"),
     "googletest"),
    ("googletest por SHA sin el tag en comentario", mutate(
        "audio/src/main/cpp/thirdparty/googletest.cmake", GT_TAG_LINE, f"GIT_TAG {GT_SHA}"), "googletest"),
    ("googletest por URL con version pero sin URL_HASH", mutate(
        "audio/src/main/cpp/thirdparty/googletest.cmake",
        f"    GIT_REPOSITORY https://github.com/google/googletest.git\n    {GT_TAG_LINE}\n",
        "    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip\n"), "googletest"),
    ("un workflow sin permissions a nivel de archivo", mutate(
        ".github/workflows/ci.yml", "permissions:\n  contents: read\n", ""), "permissions"),
    ("permissions sólo dentro de un job no cuenta como de archivo", mutate(
        ".github/workflows/ci.yml", "permissions:\n  contents: read\njobs:\n  build:\n",
        "jobs:\n  build:\n    permissions:\n      contents: read\n"), "permissions"),
    ("ci.yml pide escritura", lambda f: [ci_checkouts_without_credentials(f), mutate(
        ".github/workflows/ci.yml", "permissions:\n  contents: read\n", "permissions:\n  contents: write\n")(f)],
     "permissions"),
    ("ci.yml pide escritura en un job", lambda f: [ci_checkouts_without_credentials(f), mutate(
        ".github/workflows/ci.yml", "  build:\n    runs-on: ubuntu-latest\n",
        "  build:\n    runs-on: ubuntu-latest\n    permissions:\n      pull-requests: write\n")(f)],
     "permissions"),
    ("checkout con credenciales en un job con escritura", mutate(
        ".github/workflows/publish.yml", "        with:\n          persist-credentials: false\n", ""),
     "persist-credentials"),
    ("persist-credentials: true en un job con escritura", mutate(
        ".github/workflows/publish.yml", "persist-credentials: false", "persist-credentials: true"),
     "persist-credentials"),
    ("persist-credentials: false en OTRO paso no cubre el checkout", mutate(
        ".github/workflows/publish.yml",
        f"      - uses: actions/checkout@{SHA_A} # v7.0.1\n        with:\n          persist-credentials: false\n"
        f"      - uses: gradle/actions/setup-gradle@{SHA_A} # v6.4.0\n",
        f"      - uses: actions/checkout@{SHA_A} # v7.0.1\n"
        f"      - uses: gradle/actions/setup-gradle@{SHA_A} # v6.4.0\n        with:\n          persist-credentials: false\n"),
     "persist-credentials"),
    ("escritura declarada en el job, checkout con credenciales", lambda f: [
        mutate(".github/workflows/ci.yml", "  ios:\n    runs-on: macos-latest\n",
               "  ios:\n    runs-on: macos-latest\n    permissions:\n      contents: read\n")(f),
        mutate(".github/workflows/publish.yml", "permissions:\n  packages: write  # el publish\n  contents: read\n",
               "permissions:\n  contents: read\n")(f),
        mutate(".github/workflows/publish.yml", "    runs-on: macos-latest\n",
               "    runs-on: macos-latest\n    permissions:\n      packages: write\n")(f),
        mutate(".github/workflows/publish.yml", "        with:\n          persist-credentials: false\n", "")(f)],
     "persist-credentials"),
    ("permissions: write-all con checkout con credenciales", lambda f: [
        mutate(".github/workflows/publish.yml", "permissions:\n  packages: write  # el publish\n  contents: read\n",
               "permissions: write-all\n")(f),
        mutate(".github/workflows/publish.yml", "        with:\n          persist-credentials: false\n", "")(f)],
     "persist-credentials"),
    ("wrapper sin distributionSha256Sum", mutate(WRAPPER, f"distributionSha256Sum={SUM}\n", ""), "gradle-wrapper"),
    ("wrapper con un checksum que no es sha256", mutate(WRAPPER, f"distributionSha256Sum={SUM}",
                                                         f"distributionSha256Sum={SUM[:40]}"), "gradle-wrapper"),
    ("sin gradle-wrapper.properties", remove(WRAPPER), "leer"),
    ("uses por tag en una composite action local", add(
        ".github/actions/setup/action.yml",
        "runs:\n  using: composite\n  steps:\n    - uses: actions/setup-java@v5\n"), "actions"),
]


GREEN_CASES = [
    ("un job de lectura en un workflow con escritura puede no llevar persist-credentials", lambda f: [
        mutate(".github/workflows/publish.yml", "    steps:\n",
               "    permissions:\n      contents: read\n    steps:\n")(f),
        mutate(".github/workflows/publish.yml", "        with:\n          persist-credentials: false\n", "")(f)]),
    ("un job de lectura en ci.yml puede no llevar persist-credentials", lambda f: None),
    ("googletest unico por URL de tarball con version", mutate(
        "audio/src/main/cpp/thirdparty/googletest.cmake",
        f"    GIT_REPOSITORY https://github.com/google/googletest.git\n    {GT_TAG_LINE}\n",
        "    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip\n"
        f"    URL_HASH SHA256={SUM}\n")),
    ("el paso de Xcode con el if de la atestacion", mutate(
        ".github/workflows/publish.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      - name: Fijar Xcode\n        if: steps.attest.outputs.valid != 'true'\n"
        "        run: bash scripts/ci-select-xcode.sh\n")),
    ("el script de Xcode llamado dentro de un run de varias lineas", mutate(
        ".github/workflows/publish.yml", "      - run: bash scripts/ci-select-xcode.sh\n",
        "      - run: |\n          bash scripts/ci-select-xcode.sh\n")),
]


def build_tree(base, files):
    for rel, content in files.items():
        path = os.path.join(base, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(content)


def self_test():
    failures = []

    def run(files):
        with tempfile.TemporaryDirectory(prefix="dep-pins-") as tmp:
            build_tree(tmp, files)
            return run_checks(tmp)

    findings, gtest = run(dict(CLEAN))
    if findings:
        failures.append(f"el arbol limpio salio ROJO: {findings}")
    elif not gtest or gtest[0][1] != "v1.18.0":
        failures.append(f"el arbol limpio no resolvio el tag de googletest: {gtest}")

    for name, apply in GREEN_CASES:
        files = dict(CLEAN)
        apply(files)
        found, _ = run(files)
        if found:
            failures.append(f"MAL verde: {name}: esperaba verde, vio {found}")

    for name, apply, want in CASES:
        files = dict(CLEAN)
        apply(files)
        found, _ = run(files)
        keys = {k for k, _ in found}
        if keys != {want}:
            failures.append(f"MAL {name}: esperaba exactamente [{want}], vio {sorted(keys)} {found}")

    # main() tiene que devolver 1 ante un rojo y 0 ante un verde (el camino que usa el gate).
    with tempfile.TemporaryDirectory(prefix="dep-pins-") as tmp:
        build_tree(tmp, CLEAN)
        with contextlib.redirect_stdout(io.StringIO()):
            rc_clean = main_check(tmp)
        if rc_clean != 0:
            failures.append("main_check sobre el arbol limpio no devolvio 0")
        with open(os.path.join(tmp, ".github/workflows/ci.yml"), "a", encoding="utf-8") as fh:
            fh.write("      - uses: actions/cache@v6\n")
        with contextlib.redirect_stdout(io.StringIO()):
            rc_red = main_check(tmp)
        if rc_red != 1:
            failures.append("main_check sobre un arbol rojo no devolvio 1")

    if failures:
        print("check-dep-pins --self-test: FALLA")
        for f in failures:
            print("  " + f)
        return 1
    print(f"check-dep-pins --self-test: ok ({len(CASES)} casos rojos, cada uno con su clave; "
          f"{len(GREEN_CASES)} verdes y el limpio)")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Un lugar por pin (REQ-047 S4).")
    parser.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return main_check(os.path.abspath(args.root))


if __name__ == "__main__":
    sys.exit(main())
