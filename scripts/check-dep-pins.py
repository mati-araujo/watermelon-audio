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
#                    apunte a google/googletest), y su tag tiene forma de version.
#                    Hoy vive en audio/src/main/cpp/thirdparty/googletest.cmake.
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
#   leer             "no pude medir" no es un pase: cero declaraciones de googletest,
#                    cero `uses:`, cero `java-version:`, un catalogo sin `ndk`, o un
#                    archivo ilegible, son rojos.
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

GTEST_DECL = re.compile(r"FetchContent_Declare\(\s*googletest\b|GIT_REPOSITORY\s+\S*github\.com[/:]google/googletest")
GTEST_TAG = re.compile(r"GIT_TAG\s+(\S+)")
CMAKE_SET = re.compile(r"set\(\s*(\w+)\s+\"?([^\")\s]+)\"?\s*\)")
TAG_VERSION = re.compile(r"^v?[0-9]+\.[0-9]+(\.[0-9]+)?$")

SDK_PKG = re.compile(r"\b(ndk|cmake);[0-9]")
NDK_VERSION_ASSIGN = re.compile(r"ndkVersion\s*(=|\()\s*\"")
XCODE_LITERAL = re.compile(r"Xcode_[0-9]|DEVELOPER_DIR|xcode-select\s+(-s|--switch)")
JAVA_VERSION = re.compile(r"^\s*java-version:\s*['\"]?([^'\"\s#]+)", re.M)
USES = re.compile(r"^\s*-?\s*uses:\s*(\S+)(.*)$", re.M)
USES_PINNED = re.compile(r"^([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)(/[^@\s]*)?@([0-9a-f]{40})$")
USES_COMMENT = re.compile(r"^\s*#\s*(v[0-9]+(\.[0-9]+)*)\s*$")
JOB_HEADER = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$", re.M)
RUNS_ON = re.compile(r"^\s+runs-on:\s*(\S+)", re.M)


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


def is_build_config(rel):
    """Donde un NDK/CMake escrito a mano SI cambia lo que se instala o se usa."""
    return (is_workflow(rel) or rel.endswith((".gradle.kts", ".gradle", ".properties"))
            or (rel.startswith("build-logic/") and rel.endswith(".kt"))
            or (rel.startswith("scripts/") and rel.endswith(".sh")))


def check_googletest(root, files, findings):
    decls = []
    for rel in files:
        if not is_cmake(rel):
            continue
        text = read(root, rel)
        if not GTEST_DECL.search(text):
            continue
        variables = dict(CMAKE_SET.findall(text))
        tags = GTEST_TAG.findall(text)
        tag = tags[0] if tags else None
        if tag:
            tag = re.sub(r"\$\{(\w+)\}", lambda m: variables.get(m.group(1), m.group(0)), tag)
        decls.append((rel, tag))
    if not decls:
        findings.append(("leer", "ninguna declaracion de googletest en el arbol: no pude medir el pin"))
        return None
    if len(decls) > 1:
        lista = ", ".join(f"{r} ({t})" for r, t in decls)
        findings.append(("googletest", f"googletest declarado en {len(decls)} lugares, tiene que ser UNO: {lista}"))
    for rel, tag in decls:
        if not tag or not TAG_VERSION.match(tag):
            findings.append(("googletest", f"{rel}: el tag de googletest es {tag!r}, no una version fija"))
    return decls


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
        if is_workflow(rel):
            values += [(rel, v) for v in JAVA_VERSION.findall(read(root, rel))]
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
    """(nombre, cuerpo) de cada job de un workflow, cortando por el encabezado de 2 espacios."""
    start = text.find("\njobs:")
    if start < 0:
        return []
    body = text[start:]
    heads = list(JOB_HEADER.finditer(body))
    return [(m.group(1), body[m.end():heads[i + 1].start() if i + 1 < len(heads) else len(body)])
            for i, m in enumerate(heads)]


def check_xcode(root, files, findings):
    for rel in files:
        if not is_workflow(rel):
            continue
        text = read(root, rel)
        for n, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            if XCODE_LITERAL.search(line):
                findings.append(("xcode", f"{rel}:{n}: Xcode fijado a mano (la fuente es la clave "
                                          f"`xcode` de {PINS}): {line.strip()}"))
        for job, body in jobs_of(text):
            runs = RUNS_ON.search(body)
            if runs and "macos" in runs.group(1) and XCODE_SCRIPT not in body:
                findings.append(("xcode", f"{rel}: el job `{job}` corre en {runs.group(1)} y no "
                                          f"llama a {XCODE_SCRIPT}: usaria el Xcode default de la imagen"))


def check_actions(root, files, findings):
    seen = {}
    total = 0
    for rel in files:
        if not is_workflow(rel):
            continue
        text = read(root, rel)
        for n, line in enumerate(text.splitlines(), 1):
            m = USES.match(line)
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


def run_checks(root):
    findings = []
    try:
        files = tracked_files(root)
        gtest = check_googletest(root, files, findings)
        check_ndk_cmake(root, files, findings)
        check_java(root, files, findings)
        check_xcode(root, files, findings)
        check_actions(root, files, findings)
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
          f"java-version, Xcode y actions con una sola fuente")
    return 0


# --- self-test ---------------------------------------------------------------

SHA_A = "a" * 40
SHA_B = "b" * 40

CLEAN = {
    CATALOG: '[versions]\nndk = "30.0.16248370"\ncmake = "3.22.1"\n',
    PINS: json.dumps({"xcode": "26.6 (17F113)", "launcher_jvm": "17.0.19"}),
    "audio/src/main/cpp/thirdparty/googletest.cmake":
        'set(WMA_GOOGLETEST_VERSION "1.18.0")\nFetchContent_Declare(googletest\n'
        '    GIT_REPOSITORY https://github.com/google/googletest.git\n'
        '    GIT_TAG v${WMA_GOOGLETEST_VERSION}\n)\n',
    "audio/src/main/cpp/dsp/tests/CMakeLists.txt":
        "include(${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/googletest.cmake)\n",
    "build-logic/convention/src/main/kotlin/Plugin.kt": 'ndkVersion = libs.version("ndk")\n',
    ".github/workflows/ci.yml":
        "name: CI\n# Xcode_26.6.app en un comentario no cuenta\njobs:\n"
        "  build:\n    runs-on: ubuntu-latest\n    steps:\n"
        f"      - uses: actions/checkout@{SHA_A} # v7.0.1\n"
        f"      - uses: actions/setup-java@{SHA_B} # v6.0.1\n        with:\n          java-version: 17\n"
        "      - run: PKGS=\"$(python3 scripts/android-sdk-packages.py)\"\n"
        "      - uses: ./local-action\n"
        "  ios:\n    runs-on: macos-latest\n    steps:\n"
        f"      - uses: actions/checkout@{SHA_A} # v7.0.1\n"
        "      - run: bash scripts/ci-select-xcode.sh\n",
    ".github/workflows/publish.yml":
        "name: Publish\njobs:\n  publish:\n    runs-on: macos-latest\n    steps:\n"
        f"      - uses: gradle/actions/setup-gradle@{SHA_A} # v6.4.0\n"
        "      - run: bash scripts/ci-select-xcode.sh\n"
        f"      - uses: actions/setup-java@{SHA_B} # v6.0.1\n        with:\n          java-version: '17'\n",
}


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
        "  GIT_TAG v1.15.2\n)\n"), "googletest"),
    ("googletest declarado con otro nombre pero el mismo repo", add(
        "audio/src/main/cpp/voice/tests/CMakeLists.txt",
        "FetchContent_Declare(gtest\n  GIT_REPOSITORY https://github.com/google/googletest.git\n"
        "  GIT_TAG v1.18.0\n)\n"), "googletest"),
    ("googletest en una rama y no en un tag", mutate(
        "audio/src/main/cpp/thirdparty/googletest.cmake", "GIT_TAG v${WMA_GOOGLETEST_VERSION}",
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
    ("action por tag", mutate(
        ".github/workflows/ci.yml", f"actions/checkout@{SHA_A} # v7.0.1\n      - uses: actions/setup-java",
        "actions/checkout@v7\n      - uses: actions/setup-java"), "actions"),
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
    print(f"check-dep-pins --self-test: ok ({len(CASES)} casos rojos, cada uno con su clave, y el limpio verde)")
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
