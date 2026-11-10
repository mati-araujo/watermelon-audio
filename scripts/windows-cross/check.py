#!/usr/bin/env python3
# ============================================================================
# Gate `windows-cross` (REQ-061, AC-061.6): el arbol tiene que COMPILAR para
# Windows x64, con -Werror, SIN ejecutar nada.
#
# Compila los OBJETOS (no linkea) de dos CMakes, cruzando con MinGW-w64:
#   - desktop/CMakeLists.txt : los objetos de produccion que lista (la DLL y su
#     link son de una etapa posterior);
#   - tests/CMakeLists.txt   : la suite de host.
#
# Mismo build type que la suite de host en la Mac (run-cpp-tests.sh) y en la
# nativa de Windows (run-cpp-tests.ps1): Debug. El veredicto del cruce tiene que
# ser el del nativo; -O3 agrega warnings (-Wmaybe-uninitialized) que ninguno de
# los dos ve, y un rojo que el nativo no ve no es un rojo de portabilidad.
#
# El baseline (scripts/windows-cross/baseline.txt) es un TRINQUETE sobre FUENTES:
# la etapa falla si falla un objeto cuya fuente no esta listada Y si una fuente
# listada ya compila (sobra). Baseline vacio = cero fallas.
#
# Compilador: ${WMA_CROSS_PREFIX}g++ (default x86_64-w64-mingw32-, el mingw-w64
# de Homebrew). Su -dumpfullversion tiene que ser el que .github/toolchain-pins.json
# fija para ESE prefijo.
#
#   python3 scripts/windows-cross/check.py                    # el gate
#   python3 scripts/windows-cross/check.py --update-baseline  # reescribe el baseline
#   python3 scripts/windows-cross/check.py --self-test
#
# Build dir persistente (incremental): build/windows-cross/<prefijo>/ (`/build`
# esta en .gitignore). WMA_CROSS_BUILD_DIR lo cambia.
# ============================================================================
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CPP = os.path.join(REPO_ROOT, "audio", "src", "main", "cpp")
PINS = os.path.join(REPO_ROOT, ".github", "toolchain-pins.json")
BASELINE = os.path.join(HERE, "baseline.txt")
TOOLCHAIN = os.path.join(HERE, "toolchain.cmake")
DEFAULT_PREFIX = "x86_64-w64-mingw32-"
BUILD_TYPE = "Debug"

JNI_MD_WIN32 = """\
#ifndef _JAVASOFT_JNI_MD_H_
#define _JAVASOFT_JNI_MD_H_
#define JNIEXPORT __declspec(dllexport)
#define JNIIMPORT __declspec(dllimport)
#define JNICALL __stdcall
typedef long jint;
typedef long long jlong;
typedef signed char jbyte;
#endif
"""

SOURCE_EXT = (".cpp", ".cc", ".cxx", ".c")
FAILED_RE = re.compile(r"^FAILED:(?:\s+\[[^\]]*\])?\s+(.*\S)\s*$")


class Fatal(Exception):
    pass


# --- puro: parseo y veredicto -------------------------------------------------

def parse_failed(ninja_output):
    """Las salidas que ninja declara fallidas. Soporta `FAILED: [code=1] x` (ninja
    reciente), `FAILED: x` y varias salidas en una linea (separadas por espacio;
    un espacio dentro de una ruta viene como `\\ `)."""
    out = []
    for line in ninja_output.splitlines():
        m = FAILED_RE.match(line)
        if not m:
            continue
        for tok in re.split(r"(?<!\\)\s+", m.group(1)):
            tok = tok.replace("\\ ", " ").strip()
            if tok:
                out.append(tok)
    return out


def failed_blocks(ninja_output):
    """salida fallida -> el texto que ninja imprimio para ella (comando y diagnosticos),
    desde su `FAILED:` hasta la siguiente linea de progreso `[n/m]` o `FAILED:`."""
    blocks, cur, key = {}, [], None
    for line in ninja_output.splitlines():
        if FAILED_RE.match(line) or re.match(r"^\[\d+/\d+\]", line):
            if key is not None:
                blocks[key] = "\n".join(cur)
            key, cur = None, []
            if FAILED_RE.match(line):
                key = parse_failed(line)[0]
        elif key is not None:
            cur.append(line)
    if key is not None:
        blocks[key] = "\n".join(cur)
    return blocks


def parse_baseline(text):
    return sorted({l.strip() for l in text.splitlines()
                   if l.strip() and not l.lstrip().startswith("#")})


def verdict(failed_sources, baseline, known_sources):
    """(nuevas, sobrantes): fuentes que fallan sin estar listadas, y listadas que
    no fallan (compilan, o ya ni existen entre las fuentes de los dos CMakes)."""
    failed, base = set(failed_sources), set(baseline)
    return sorted(failed - base), sorted(base - failed)


# --- entorno -----------------------------------------------------------------

def load_pins():
    try:
        with open(PINS, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        raise Fatal(f"no pude leer {PINS}: {exc}")


def expected_gcc(pins, prefix):
    try:
        cross = pins["_windows_cross"]
        brew, cont = cross["homebrew"], cross["container"]
        if prefix == brew["prefix"]:
            return pins[brew["gcc_key"]], "Homebrew mingw-w64"
        if prefix == cont["prefix"]:
            return cont["gcc"], "el contenedor (wa-windows-cross)"
    except KeyError as exc:
        raise Fatal(f"{PINS} no tiene la clave {exc} del cruce a Windows")
    raise Fatal(f"el prefijo {prefix!r} no tiene pin en {PINS} (_windows_cross): "
                f"{brew['prefix']!r} (Homebrew) o {cont['prefix']!r} (contenedor)")


def find_compiler(prefix):
    gxx = shutil.which(prefix + "g++")
    if not gxx:
        raise Fatal(
            f"no encuentro {prefix}g++ en el PATH.\n"
            f"  macOS : brew install mingw-w64  (instala x86_64-w64-mingw32-g++)\n"
            f"  otro  : WMA_CROSS_PREFIX=<prefijo> con ese compilador en el PATH, o la imagen\n"
            f"          del contenedor: bash scripts/windows-cross/build-image.sh")
    r = subprocess.run([gxx, "-dumpfullversion"], capture_output=True, text=True)
    return gxx, r.stdout.strip()


def find_host_jni_h():
    cands = []
    for var in ("WMA_JAVA_HOME", "JAVA_HOME"):
        if os.environ.get(var):
            cands.append(os.path.join(os.environ[var], "include", "jni.h"))
    jh = shutil.which("/usr/libexec/java_home") or ("/usr/libexec/java_home"
                                                    if os.path.exists("/usr/libexec/java_home") else None)
    if jh:
        r = subprocess.run([jh], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            cands.append(os.path.join(r.stdout.strip(), "include", "jni.h"))
    cands += sorted(glob.glob("/usr/lib/jvm/*/include/jni.h"))
    for c in cands:
        if os.path.isfile(c):
            return c
    raise Fatal("no encuentro el jni.h de un JDK del host (necesita el CMake de desktop).\n"
                "  Exporta JAVA_HOME (o WMA_JAVA_HOME) a un JDK; el jni_md.h de win32 lo genera este script.")


def write_if_changed(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.isfile(path):
        with open(path, "rb") as fh:
            if fh.read() == data:
                return
    with open(path, "wb") as fh:
        fh.write(data)


def make_java_home(build_root):
    """Un 'JAVA_HOME' minimo: el jni.h del host + el jni_md.h de win32 generado."""
    home = os.path.join(build_root, "jni-win32")
    with open(find_host_jni_h(), "rb") as fh:
        write_if_changed(os.path.join(home, "include", "jni.h"), fh.read())
    write_if_changed(os.path.join(home, "include", "win32", "jni_md.h"), JNI_MD_WIN32.encode())
    return home


def run(cmd, log_path=None, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", **kw)
    if log_path:
        with open(log_path, "w", encoding="utf-8") as fh:
            fh.write(r.stdout)
    return r


def rel_source(path, directory):
    p = path if os.path.isabs(path) else os.path.join(directory, path)
    p = os.path.normpath(p)
    try:
        if os.path.commonpath([p, REPO_ROOT]) == REPO_ROOT:
            return os.path.relpath(p, REPO_ROOT).replace(os.sep, "/")
    except ValueError:
        pass
    return p.replace(os.sep, "/")


def build_objects(name, src_dir, build_dir, prefix, java_home, jobs, runner=run):
    """Configura, y compila SOLO los objetos con `ninja -k 0`. Devuelve
    (todas las fuentes, fuentes que fallaron, log)."""
    os.makedirs(build_dir, exist_ok=True)
    cfg = ["cmake", "-S", src_dir, "-B", build_dir, "-G", "Ninja",
           f"-DCMAKE_BUILD_TYPE={BUILD_TYPE}",
           f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}"]
    if name == "desktop":
        cfg += [f"-DWMA_JAVA_HOME={java_home}", "-DWMA_DESKTOP_WERROR=ON"]
    else:
        cfg += ["-DWMA_TESTS_WERROR=ON", f"-DFETCHCONTENT_BASE_DIR={os.path.join(build_dir, '.deps')}"]
    cfg_log = os.path.join(build_dir, "configure.log")
    r = runner(cfg, cfg_log)
    if r.returncode != 0:
        raise Fatal(f"[{name}] cmake no configuro (log: {cfg_log}):\n" + "\n".join(r.stdout.splitlines()[-25:]))

    r = runner(["ninja", "-C", build_dir, "-t", "compdb"])
    if r.returncode != 0:
        raise Fatal(f"[{name}] ninja -t compdb fallo:\n{r.stdout[-2000:]}")
    db = json.loads(r.stdout)
    by_output = {}
    for e in db:
        # compdb tambien lista los custom commands (edit_cache, rebuild_cache...): sólo objetos.
        if not str(e.get("file", "")).endswith(SOURCE_EXT):
            continue
        if not e.get("output"):
            raise Fatal(f"[{name}] compdb sin 'output' para {e.get('file')}: ninja demasiado viejo")
        by_output[e["output"]] = rel_source(e["file"], e["directory"])
    if not by_output:
        raise Fatal(f"[{name}] el CMake no declara ningun objeto: no pude medir nada")

    log = os.path.join(build_dir, "ninja.log")
    r = runner(["ninja", "-C", build_dir, "-k", "0", f"-j{jobs}"] + sorted(by_output), log)
    failed_outputs = parse_failed(r.stdout)
    if r.returncode != 0 and not failed_outputs:
        raise Fatal(f"[{name}] ninja salio {r.returncode} sin ningun `FAILED:` (no es un error de "
                    f"compilacion; log: {log}):\n" + "\n".join(r.stdout.splitlines()[-25:]))
    failed, detail = set(), {}
    blocks = failed_blocks(r.stdout)
    for o in failed_outputs:
        src = by_output.get(o) or by_output.get(os.path.normpath(o))
        if src is None:
            raise Fatal(f"[{name}] ninja fallo en {o!r}, que no es un objeto de la compdb (log: {log})")
        failed.add(src)
        detail[src] = blocks.get(o, "")
    return set(by_output.values()), failed, log, detail


def error_lines(text, limit=6):
    seen, out = set(), []
    for l in text.splitlines():
        if re.search(r"\berror\b", l) and l not in seen:
            seen.add(l)
            out.append(l.strip())
    return out[:limit]


def main(argv):
    update = "--update-baseline" in argv
    prefix = os.environ.get("WMA_CROSS_PREFIX") or DEFAULT_PREFIX
    pins = load_pins()
    want, quien = expected_gcc(pins, prefix)
    gxx, got = find_compiler(prefix)
    if got != want:
        raise Fatal(f"{prefix}g++ ({gxx}) es GCC {got!r}; .github/toolchain-pins.json fija {want!r} "
                    f"para {quien}.\n  Si la version nueva es la correcta, se cambia el pin (eso fuerza "
                    f"una corrida completa en CI), no se acepta en silencio.")
    os.environ["WMA_CROSS_PREFIX"] = prefix  # lo lee toolchain.cmake
    for tool in ("cmake", "ninja"):
        if not shutil.which(tool):
            raise Fatal(f"falta {tool} en el PATH")

    build_root = os.environ.get("WMA_CROSS_BUILD_DIR") or os.path.join(
        REPO_ROOT, "build", "windows-cross", prefix.rstrip("-"))
    java_home = make_java_home(build_root)
    jobs = os.cpu_count() or 4
    print(f"windows-cross: {gxx} (GCC {got}), {BUILD_TYPE}, -Werror, build en {build_root}", flush=True)
    return gate(argv, build_root, prefix, java_home, jobs)


def gate(argv, build_root, prefix, java_home, jobs, runner=run, baseline_path=BASELINE):
    """Compila los dos CMakes y da el veredicto contra el baseline: el exit code."""
    update = "--update-baseline" in argv
    known, failed, outputs = set(), set(), {}
    for name, src in (("desktop", os.path.join(CPP, "desktop")), ("tests", os.path.join(CPP, "tests"))):
        srcs, fl, log, detail = build_objects(name, src, os.path.join(build_root, name), prefix, java_home, jobs, runner)
        print(f"  {name}: {len(srcs)} objetos, {len(fl)} fallan", flush=True)
        known |= srcs
        failed |= fl
        for src_file, text in detail.items():
            outputs.setdefault(src_file, (name, log, text))

    if update:
        with open(baseline_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(baseline_header())
            fh.write("".join(f"{s}\n" for s in sorted(failed)))
        print(f"baseline reescrito: {len(failed)} fuentes ({baseline_path})")
        return 0

    try:
        with open(baseline_path, encoding="utf-8") as fh:
            baseline = parse_baseline(fh.read())
    except OSError as exc:
        raise Fatal(f"no pude leer el baseline: {exc}")
    nuevas, sobrantes = verdict(failed, baseline, known)

    if not nuevas and not sobrantes:
        print(f"windows-cross: OK — {len(failed)} fuentes fallan, todas en el baseline"
              if baseline else "windows-cross: OK — baseline vacio, cero fallas")
        return 0
    if nuevas:
        print(f"\nROJO — {len(nuevas)} fuente(s) NO compilan para Windows y no estan en el baseline:")
        for s in nuevas:
            name, log, text = outputs[s]
            print(f"  {s}   [{name}]")
            for l in error_lines(text):
                print(f"      {l}")
            print(f"      (log completo: {log})")
    if sobrantes:
        print(f"\nROJO — {len(sobrantes)} entrada(s) del baseline SOBRAN (compilan, o ya no existen): "
              f"sacalas de scripts/windows-cross/baseline.txt")
        for s in sobrantes:
            print(f"  {s}" + ("" if s in known else "   (no es fuente de ningun CMake)"))
    return 1


def guarded(fn, *args, **kw):
    """El exit code de fn: un Fatal es ROJO (1), nunca una traza ni un verde."""
    try:
        return fn(*args, **kw)
    except Fatal as exc:
        print(f"windows-cross: ROJO — {exc}", file=sys.stderr)
        return 1


def baseline_header():
    return ("# Fuentes (ruta relativa a la raiz) cuyo objeto hoy NO compila para Windows.\n"
            "# trinquete; REQ-061 S2 lo vacia. Falla un objeto no listado y falla un listado que compila.\n"
            "# Regenerar (su diff es la revision): python3 scripts/windows-cross/check.py --update-baseline\n")


# --- self-test ---------------------------------------------------------------

def red_paths_cases():
    """Los caminos de rojo de build_objects/gate con un runner falso (sin cmake,
    ninja ni Docker). Devuelve la lista de fallas."""
    import contextlib
    import io
    import types

    compdb = json.dumps([{"directory": REPO_ROOT, "file": "a.cpp", "output": "a.obj", "command": "x"}])

    def fake(ninja_rc=0, ninja_out="", cmake_rc=0):
        def runner(cmd, log_path=None, **kw):
            if cmd[0] == "cmake":
                return types.SimpleNamespace(returncode=cmake_rc, stdout="cmake boom\n")
            if "compdb" in cmd:
                return types.SimpleNamespace(returncode=0, stdout=compdb)
            return types.SimpleNamespace(returncode=ninja_rc, stdout=ninja_out)
        return runner

    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        base = os.path.join(tmp, "baseline.txt")
        with open(base, "w", encoding="utf-8") as fh:
            fh.write("# vacio\n")

        def code(runner, argv=()):
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = guarded(gate, list(argv), os.path.join(tmp, "b"), "p-", "/jh", 1, runner, base)
            return rc, out.getvalue() + err.getvalue()

        rc, _ = code(fake())
        if rc != 0:
            bad.append(f"verde (ninja 0, baseline vacio) tenia que dar 0, dio {rc}")
        rc, txt = code(fake(cmake_rc=1))
        if rc != 1 or "cmake no configuro" not in txt:
            bad.append(f"cmake que no configura tenia que dar 1 (dio {rc})")
        rc, txt = code(fake(ninja_rc=1, ninja_out="ninja: error: loading 'build.ninja'\n"))
        if rc != 1 or "sin ningun `FAILED:`" not in txt:
            bad.append(f"ninja != 0 sin FAILED tenia que dar 1 (dio {rc})")
        rc, txt = code(fake(ninja_rc=1, ninja_out="FAILED: [code=1] zzz.obj\ng++ -c z\n"))
        if rc != 1 or "no es un objeto de la compdb" not in txt:
            bad.append(f"FAILED fuera de la compdb tenia que dar 1 (dio {rc})")
        rc, txt = code(fake(ninja_rc=1, ninja_out="FAILED: [code=1] a.obj\ng++ -c a\na.cpp:1:1: error: boom\n"))
        if rc != 1 or "ROJO" not in txt:
            bad.append(f"objeto que falla fuera del baseline tenia que dar 1 (dio {rc})")
    return bad


def self_test():
    bad = []

    def eq(name, got, want):
        if got != want:
            bad.append(f"{name}: esperaba {want!r}, vio {got!r}")

    eq("ninja reciente", parse_failed("[1/3] x\nFAILED: [code=1] a/b.cpp.obj \ng++ ...\n"), ["a/b.cpp.obj"])
    eq("ninja viejo", parse_failed("FAILED: a/b.cpp.obj\n"), ["a/b.cpp.obj"])
    eq("varias salidas", parse_failed("FAILED: [code=2] x.obj y.obj\n"), ["x.obj", "y.obj"])
    eq("ruta con espacio", parse_failed("FAILED: [code=1] d/a\\ b.obj\n"), ["d/a b.obj"])
    eq("una linea que NO es FAILED", parse_failed("  FAILED: x\nnote: FAILED: y\nerror: FAILED: z\n"), [])
    eq("senal", parse_failed("FAILED: [signal 9 (Killed)] x.obj\n"), ["x.obj"])
    eq("baseline con comentarios", parse_baseline("# c\n\n b.cpp \na.cpp\na.cpp\n  # d\n"), ["a.cpp", "b.cpp"])
    eq("igual", verdict({"a"}, ["a"], {"a", "b"}), ([], []))
    eq("nueva", verdict({"a", "n"}, ["a"], {"a", "n"}), (["n"], []))
    eq("sobra", verdict({"a"}, ["a", "s"], {"a", "s"}), ([], ["s"]))
    eq("sobra inexistente", verdict(set(), ["fantasma.cpp"], {"a"}), ([], ["fantasma.cpp"]))
    eq("baseline vacio y cero fallas", verdict(set(), [], {"a"}), ([], []))
    eq("baseline vacio exige cero", verdict({"a"}, [], {"a"}), (["a"], []))
    sample = ("[1/3] cc a\nFAILED: [code=1] a.obj \ng++ -c a.cpp\na.cpp:1:1: error: boom\n"
              "[2/3] cc b\nFAILED: [code=1] b.obj \ng++ -c b.cpp\nb.cpp:2:2: error: bam\n")
    blk = failed_blocks(sample)
    eq("bloques por salida", sorted(blk), ["a.obj", "b.obj"])
    eq("el bloque de a no trae el error de b", "bam" in blk["a.obj"], False)
    eq("el bloque de a trae el suyo", error_lines(blk["a.obj"]), ["a.cpp:1:1: error: boom"])
    eq("rel_source dentro del repo", rel_source(os.path.join(REPO_ROOT, "x", "..", "y.cpp"), "/"), "y.cpp")

    pins = {"mingw_w64_gcc": "1.0.0", "_windows_cross": {
        "homebrew": {"prefix": "p-", "gcc_key": "mingw_w64_gcc"}, "container": {"prefix": "q-", "gcc": "2.0.0"}}}
    eq("pin homebrew", expected_gcc(pins, "p-")[0], "1.0.0")
    eq("pin contenedor", expected_gcc(pins, "q-")[0], "2.0.0")
    try:
        expected_gcc(pins, "z-")
        bad.append("un prefijo sin pin tenia que fallar")
    except Fatal:
        pass

    with tempfile.TemporaryDirectory() as tmp:
        old = os.environ.get("PATH", "")
        os.environ["PATH"] = tmp
        try:
            find_compiler("no-existe-")
            bad.append("un compilador inexistente tenia que fallar")
        except Fatal as exc:
            if "brew install mingw-w64" not in str(exc):
                bad.append("el mensaje no dice como instalarlo")
        finally:
            os.environ["PATH"] = old

    bad += red_paths_cases()

    if bad:
        print("windows-cross --self-test: FALLA")
        for b in bad:
            print("  " + b)
        return 1
    print("windows-cross --self-test: ok")
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(self_test())
    sys.exit(guarded(main, sys.argv[1:]))
