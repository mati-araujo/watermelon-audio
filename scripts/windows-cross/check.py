#!/usr/bin/env python3
# ============================================================================
# Gate `windows-cross` (REQ-061, AC-061.6): el arbol tiene que COMPILAR para
# Windows x64, con -Werror, SIN ejecutar nada.
#
# Compila los OBJETOS (no linkea) de dos CMakes, cruzando con MinGW-w64:
#   - desktop/CMakeLists.txt : los objetos de produccion que lista;
#   - tests/CMakeLists.txt   : la suite de host.
# `--dll` es el otro paso: construye la DLL de desktop/CMakeLists.txt en Release
# (-O3, -Werror, -static) y verifica con check-jni-symbols.py --pe / --imports que
# exporta exactamente las JNIEXPORT y que importa solo KERNEL32 y la UCRT.
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
#   python3 scripts/windows-cross/check.py --dll              # la DLL Release (AC-061.5), aparte
#   python3 scripts/windows-cross/check.py --self-test
#
# Build dir persistente (incremental): build/windows-cross/<prefijo>/ (`/build`
# esta en .gitignore). WMA_CROSS_BUILD_DIR lo cambia.
# ============================================================================
import collections
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
DLL_BUILD_TYPE = "Release"
SYMBOLS_PY = os.path.join(REPO_ROOT, "scripts", "check-jni-symbols.py")

JNI_MD_WIN32 = """\
#ifndef _JAVASOFT_JNI_MD_H_
#define _JAVASOFT_JNI_MD_H_
#define JNIEXPORT __declspec(dllexport)
#define JNIIMPORT __declspec(dllimport)
#define JNICALL
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


def build_objects(name, src_dir, build_dir, prefix, java_home, jobs, runner=run, native=False, gtest_src=None,
                  only=None):
    """Configura, y compila SOLO los objetos con `ninja -k 0`. Devuelve (todas las fuentes, fuentes que
    fallaron, log, detalle, {objeto: fuente}). `native`: el compilador del host, sin toolchain de cruce (la suite
    POSIX del diff de registro); `gtest_src`: googletest ya bajado (no se baja dos veces); `only(objeto)`: compilar
    solo esos objetos."""
    os.makedirs(build_dir, exist_ok=True)
    cfg = ["cmake", "-S", src_dir, "-B", build_dir, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={BUILD_TYPE}"]
    if not native:
        cfg.append(f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}")
    if name == "desktop":
        cfg += [f"-DWMA_JAVA_HOME={java_home}", "-DWMA_DESKTOP_WERROR=ON"]
    else:
        cfg += ["-DWMA_TESTS_WERROR=ON", f"-DFETCHCONTENT_BASE_DIR={os.path.join(build_dir, '.deps')}"]
        if gtest_src:
            cfg.append(f"-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST={gtest_src}")
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
        if only is None or only(e["output"]):
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
    return set(by_output.values()), failed, log, detail, by_output


def error_lines(text, limit=6):
    seen, out = set(), []
    for l in text.splitlines():
        if re.search(r"\berror\b", l) and l not in seen:
            seen.add(l)
            out.append(l.strip())
    return out[:limit]


def dll_gate(build_root, java_home, jobs, objdump, runner=run):
    """La DLL de produccion (desktop/CMakeLists.txt) en Release, y sus dos verificaciones. Un rojo en
    cualquiera es rojo: 0 si construye y exporta/importa lo esperado, 1 si no."""
    build_dir = os.path.join(build_root, "dll")
    os.makedirs(build_dir, exist_ok=True)
    cfg = ["cmake", "-S", os.path.join(CPP, "desktop"), "-B", build_dir, "-G", "Ninja",
           f"-DCMAKE_BUILD_TYPE={DLL_BUILD_TYPE}", f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}",
           f"-DWMA_JAVA_HOME={java_home}", "-DWMA_DESKTOP_WERROR=ON"]
    r = runner(cfg, os.path.join(build_dir, "configure.log"))
    if r.returncode != 0:
        raise Fatal("[dll] cmake no configuro:\n" + "\n".join(r.stdout.splitlines()[-25:]))
    log = os.path.join(build_dir, "ninja.log")
    r = runner(["ninja", "-C", build_dir, f"-j{jobs}"], log)
    if r.returncode != 0:
        print(f"ROJO — la DLL no construye (log: {log}):")
        for l in (error_lines(r.stdout) or r.stdout.splitlines()[-15:]):
            print(f"  {l}")
        return 1
    dlls = glob.glob(os.path.join(build_dir, "watermelon_audio.dll"))
    if len(dlls) != 1:
        raise Fatal(f"[dll] esperaba {build_dir}/watermelon_audio.dll (sin prefijo lib) y no esta")
    rc = 0
    for modo in ("--pe", "--imports"):
        r = runner([sys.executable, SYMBOLS_PY, modo, dlls[0], "--objdump", objdump])
        print(r.stdout.rstrip())
        if r.returncode != 0:
            print(f"windows-cross: ROJO — check-jni-symbols.py {modo} sobre la DLL")
            rc = 1
    if rc == 0:
        print("windows-cross: OK — la DLL Release exporta lo esperado e importa solo KERNEL32 y la UCRT")
    return rc


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
    if "--dll" in argv:
        objdump = shutil.which(prefix + "objdump")
        if not objdump:
            raise Fatal(f"no encuentro {prefix}objdump en el PATH (lo necesita check-jni-symbols.py --pe)")
        print(f"windows-cross: {gxx} (GCC {got}), {DLL_BUILD_TYPE}, -Werror, -static, DLL en "
              f"{os.path.join(build_root, 'dll')}", flush=True)
        return dll_gate(build_root, java_home, jobs, objdump)
    print(f"windows-cross: {gxx} (GCC {got}), {BUILD_TYPE}, -Werror, build en {build_root}", flush=True)
    return gate(argv, build_root, prefix, java_home, jobs,
                registro=lambda win_dir, win_by_out, fallan: registro_gate(build_root, prefix, win_dir, win_by_out, jobs,
                                                               fallan=fallan))


def gate(argv, build_root, prefix, java_home, jobs, runner=run, baseline_path=BASELINE, registro=None):
    """Compila los dos CMakes y da el veredicto contra el baseline: el exit code. `registro(win_dir, win_by_out,
    fallan)`: el diff de registro de AC-061.1 (`registro_gate`); `fallan` son las fuentes de la suite de Windows que no
    compilan (en el baseline o no): sus objetos no existen y se excluyen de los dos lados."""
    update = "--update-baseline" in argv
    known, failed, outputs = set(), set(), {}
    win_tests, fallan_tests = None, set()
    for name, src in (("desktop", os.path.join(CPP, "desktop")), ("tests", os.path.join(CPP, "tests"))):
        srcs, fl, log, detail, by_out = build_objects(name, src, os.path.join(build_root, name), prefix, java_home,
                                                      jobs, runner)
        print(f"  {name}: {len(srcs)} objetos, {len(fl)} fallan", flush=True)
        if name == "tests":
            win_tests, fallan_tests = (os.path.join(build_root, name), by_out), set(fl)
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

    rc_reg = 0
    if registro is not None:
        rc_reg = registro(*win_tests, fallan_tests)
    if not nuevas and not sobrantes and rc_reg:
        print("windows-cross: ROJO — el baseline de fuentes esta bien, pero el diff de registro (arriba) no")
        return rc_reg
    if not nuevas and not sobrantes:
        print(f"windows-cross: OK — {len(failed)} fuentes fallan, todas en el baseline"
              if baseline else "windows-cross: OK — baseline vacio, cero fallas")
        return rc_reg
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


# --- diff de registro (AC-061.1) ----------------------------------------------
#
# Que tests REGISTRA la suite en Windows contra POSIX, sin ejecutar nada: cada TEST/TEST_F/TEST_P/TYPED_TEST
# define una clase `<Suite>_<Test>_Test` con `TestBody()`; esa clase esta en el objeto o no esta, sea cual sea
# la construccion que la saque (un #if, una guarda de compilador, un genex, una definicion de CMake, un target
# que no se configura). Se compila la suite DOS veces (Windows con el cruce, POSIX con el compilador del host,
# mismo Debug y -Werror), se lee con `nm` que clases define cada target de test y se compara por target:
#
#   POSIX - Windows == lo declarado `posix-solo | target | Suite.Test  | razon`
#   Windows - POSIX == lo declarado `solo-windows | Suite.Test o archivo.cpp | razon`
#
# en las dos direcciones. Lo declarado de un target que el host POSIX no construyo (p. ej. sin libusb) es "no
# verificable en este host": se informa, no se da por bueno ni por malo. Los WMA_WINDOWS_NOT_RUN estan
# registrados en los dos lados (no entran): los cubre `check-windows-not-run.py --results`.

NM_LINE = re.compile(r"^[0-9a-fA-F]+\s+[A-Za-z]\s+(\S+)")
TESTBODY_RE = re.compile(r"(?<![\w])(\w+_Test)(?:<.*>)?::TestBody\(\)")
# TYPED_TEST_P(S, N) no define `S_N_Test`: es `namespace gtest_suite_S_ { template <T> class N }`. El nombre se arma
# juntando, NO partiendo por `_` (gtest no prohibe `_` en los nombres de verdad: con `Suite_With_Underscores` no hay
# forma de partir); `(\w+?)_::` ata el ultimo `_` antes de `::` y es inequivoco.
TESTBODY_P_RE = re.compile(r"(?<![\w])gtest_suite_(\w+?)_::(\w+)(?:<.*>)?::TestBody\(\)")
OBJ_TARGET_RE = re.compile(r"CMakeFiles/([^/]+)\.dir/")


def clase_de(nombre):
    """`Suite.Test` -> la clase que gtest genera. (`Suite_Test_Test` es ambiguo al reves: por eso se compara por
    clase y se muestra con el nombre que sale de las fuentes.)"""
    suite, _, test = nombre.partition(".")
    return f"{suite}_{test}_Test"


def simbolos_testbody(nm_out):
    """Los simbolos (mangled) DEFINIDOS que mencionan TestBody en la salida de `nm` (un indefinido no trae direccion)."""
    out = set()
    for line in nm_out.splitlines():
        if "TestBody" not in line:
            continue
        m = NM_LINE.match(line.strip())
        if m:
            out.add(m.group(1))
    return out


def clase_de_demangled(line):
    """La clase (`Suite_Test_Test`) de un simbolo demangled `...::TestBody()`, o None."""
    m = TESTBODY_RE.search(line)
    if m:
        return m.group(1)
    m = TESTBODY_P_RE.search(line)
    return f"{m.group(1)}_{m.group(2)}_Test" if m else None


def clases_de_demangled(lines):
    return {c for c in map(clase_de_demangled, lines) if c}


def targets_de_test(build_dir):
    """Los targets que registran tests con gtest_discover_tests: lo dicen los `<target>[1]_include.cmake`."""
    out = set()
    for _, _, files in os.walk(build_dir):
        for f in files:
            m = re.fullmatch(r"(.+)\[1\]_include\.cmake", f)
            if m:
                out.add(m.group(1))
    return out


def objetos_por_target(by_output, targets):
    """{target: [(salida de ninja, fuente)]}"""
    out = {t: [] for t in targets}
    for o, src in by_output.items():
        m = OBJ_TARGET_RE.search(o.replace("\\", "/"))
        if m and m.group(1) in out:
            out[m.group(1)].append((o, src))
    return out


def find_tool(names):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    raise Fatal(f"no encuentro {' ni '.join(names)} en el PATH (binutils): el diff de registro lee los objetos con nm")


def demangle(filt, symbols):
    r = subprocess.run([filt], input="\n".join(symbols) + "\n", capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    lines = r.stdout.splitlines()
    if r.returncode != 0 or len(lines) != len(symbols):
        raise Fatal(f"c++filt dio {len(lines)} lineas para {len(symbols)} simbolos (rc={r.returncode})")
    return lines


def clases_por_target(build_dir, by_output, targets, nm, filt, runner=run, demangler=demangle):
    """{target: {clase: {basename de la fuente}}} de los objetos de cada target de test."""
    trabajo = [(t, o, os.path.basename(src), o if os.path.isabs(o) else os.path.join(build_dir, o))
               for t, objs in objetos_por_target(by_output, targets).items() for o, src in objs]

    def leer_obj(item):
        r = runner([nm, item[3]])
        if r.returncode != 0:
            raise Fatal(f"`nm {item[3]}` salio {r.returncode}: {r.stdout[-300:]}")
        return simbolos_testbody(r.stdout)

    # nm por objeto es E/S (Debug, build dir montado): en paralelo
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=min(16, (os.cpu_count() or 4) * 2)) as pool:
        por_obj = {(t, o, base): syms for (t, o, base, _), syms in zip(trabajo, pool.map(leer_obj, trabajo))}
    todos = sorted(set().union(*por_obj.values())) if por_obj else []
    dem = dict(zip(todos, demangler(filt, todos))) if todos else {}
    out = {t: {} for t in targets}
    for (t, _, base), syms in por_obj.items():
        for c in clases_de_demangled(dem[s] for s in syms):
            out[t].setdefault(c, set()).add(base)
    return out


def diff_registro(posix, win, decl_posix, decl_solo, nombres=None):
    """Veredicto puro. posix/win: {target: {clase: {fuentes}}} (un target ausente en el dict = ese lado no lo
    construyo). decl_posix: Counter {(target, 'Suite.Test' o '*'): n}. decl_solo: {nombre: razon} (`Suite.Test`
    o `archivo.cpp`). Devuelve (errores, notas, resumen)."""
    nombres = nombres or {}
    legible = lambda c: nombres.get(c, c)
    errores, notas, resumen = [], [], []
    dp = collections.defaultdict(set)
    for (t, n), _ in decl_posix.items():
        dp[t].add(n)
    solo_clases = {clase_de(n) for n in decl_solo if not n.endswith((".cpp", ".h"))}
    solo_archivos = {n for n in decl_solo if n.endswith((".cpp", ".h"))}
    usados_clase, usados_archivo = set(), set()
    no_verif_win = set()

    for t in sorted(set(posix) | set(win) | set(dp)):
        P, W = posix.get(t), win.get(t)
        if P is None and W is None:
            notas.append(f"{t}: NO VERIFICABLE en este host (ni se construye en el host POSIX ni se configura en Windows): "
                         f"{len(dp[t])} entrada(s) `posix-solo` declarada(s) sin comprobar")
            continue
        if P is None:
            notas.append(f"{t}: NO VERIFICABLE en este host: Windows lo compila ({len(W)} clases) y el host POSIX no lo "
                         f"construyo; su lado `solo-windows` y su diff no se comparan aca")
            no_verif_win |= set(W)
            for c, srcs in W.items():
                usados_archivo |= srcs & solo_archivos
            resumen.append(f"{t}: posix=- windows={len(W)}")
            continue
        W = W or {}
        resumen.append(f"{t}: posix={len(P)} windows={len(W)}" + ("" if t in win else " (no se configura en Windows)"))
        # POSIX - Windows
        faltan = set(P) - set(W)
        declarado = {clase_de(n) for n in dp.get(t, set())}
        for c in sorted(faltan - declarado):
            errores.append(f"{t}: {legible(c)} se compila/registra en POSIX y NO en Windows y no esta declarado "
                           f"(`posix-solo | {t} | {legible(c)} | razon`)")
        for c in sorted(declarado - faltan):
            errores.append(f"{t}: declarado `posix-solo` {legible(c)} pero " +
                           ("no existe en POSIX (sobra la entrada)" if c not in P else "Windows tambien lo compila (sobra la entrada)"))
        # Windows - POSIX
        extra = set(W) - set(P)
        for c in sorted(extra):
            srcs = W[c]
            if c in solo_clases:
                usados_clase.add(c)
            elif srcs & solo_archivos:
                usados_archivo |= srcs & solo_archivos
            else:
                errores.append(f"{t}: {legible(c)} se compila/registra SOLO en Windows y no esta declarado "
                               f"(`solo-windows | {legible(c)} | razon`)")
    # una entrada solo-windows que no se vio en ningun diff verificable sobra
    clases_no_verif = no_verif_win
    for n in sorted(decl_solo):
        if n.endswith((".cpp", ".h")):
            if n not in usados_archivo:
                errores.append(f"el listado declara `solo-windows | {n}` y ningun test de ese archivo existe solo en Windows (sobra la entrada)")
        elif clase_de(n) not in usados_clase and clase_de(n) not in clases_no_verif:
            errores.append(f"el listado declara `solo-windows | {n}` y esa clase no existe solo en Windows (sobra la entrada)")
    return errores, notas, resumen


def nombres_de_fuentes():
    """{clase: 'Suite.Test'} de las fuentes de test, para nombrar lo que `nm` solo da como clase."""
    wnr = cargar_wnr()
    out = {}
    for path in wnr.archivos_de_test(CPP, (".cpp", ".h")):
        for m in wnr.TEST_DECL.finditer(wnr.sin_comentarios(wnr.leer(path))):
            out[f"{m.group(2)}_{m.group(3)}_Test"] = f"{m.group(2)}.{m.group(3)}"
    return out


def cargar_wnr():
    import importlib.util
    spec = importlib.util.spec_from_file_location("wnr", os.path.join(REPO_ROOT, "scripts", "check-windows-not-run.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules.setdefault("wnr", mod)
    spec.loader.exec_module(mod)
    return mod


def sin_fallados(by_output, fallan):
    """(by_output sin los objetos cuya fuente fallo, [fuentes excluidas que estaban]). Una fuente que no compila en
    Windows no deja objeto: sus clases no se cuentan en NINGUN lado (se quitan tambien del lado POSIX)."""
    if not fallan:
        return by_output, []
    return ({o: src for o, src in by_output.items() if src not in fallan},
            sorted({src for src in by_output.values() if src in fallan}))


def targets_sin_objetos(by_output, targets, fallan):
    """Los targets de test cuyos objetos fallaron TODOS: no linkean, no queda nada que comparar."""
    return sorted(t for t, objs in objetos_por_target(by_output, targets).items()
                  if objs and all(src in fallan for _, src in objs))


def registro_gate(build_root, prefix, win_dir, win_by_out, jobs, runner=run, fallan=frozenset()):
    """El diff de registro completo: compila la suite POSIX con el compilador del host, lee las dos con nm, compara.
    Devuelve el exit code (0 verde)."""
    wnr = cargar_wnr()
    lst = wnr.leer_listado(wnr.LISTADO)
    if lst.errores:
        raise Fatal("el listado declarado tiene errores:\n  " + "\n  ".join(lst.errores))
    t_win = targets_de_test(win_dir)
    if not t_win:
        raise Fatal(f"no encuentro ningun `*[1]_include.cmake` en {win_dir}: no se que targets registran tests")
    muertos = targets_sin_objetos(win_by_out, t_win, fallan)
    for t in muertos:
        print(f"  ROJO — {t}: ningun objeto de Windows compila: el target no linkea y no hay registro que comparar")
    win_by_out, excluidas = sin_fallados(win_by_out, fallan)
    if excluidas:
        print(f"  registro: {len(excluidas)} fuente(s) NO compilan para Windows (baseline): sus clases no se cuentan en "
              f"ningun lado y el diff se hace sin ellas:")
        for e in excluidas:
            print(f"    - {e}")
    posix_dir = os.path.join(build_root, "tests-posix")
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        raise Fatal("no hay compilador nativo (c++/g++/clang++) en el PATH para compilar la suite POSIX del diff de registro "
                    "(contenedor: el paquete gcc-c++ de .github/toolchain-pins.json; Mac: las Xcode command line tools)")
    ver = subprocess.run([cxx, "--version"], capture_output=True, text=True).stdout.splitlines()[:1]
    print(f"  registro: suite POSIX con {cxx} ({ver[0] if ver else '?'}), {BUILD_TYPE}, -Werror, build en {posix_dir}", flush=True)
    gtest = os.path.join(win_dir, ".deps", "googletest-src")
    os.environ.pop("CC", None); os.environ.pop("CXX", None)
    # los objetos de test de CUALQUIER target de test que el host configure; se conoce tras configurar, asi que
    # primero se configura (solo) y despues se compila lo que pertenece a un target de test.
    cfg_targets = {}

    def only(output):
        m = OBJ_TARGET_RE.search(output.replace("\\", "/"))
        return bool(m) and m.group(1) in cfg_targets

    # configurar para saber que targets de test construye el host
    os.makedirs(posix_dir, exist_ok=True)
    cfg = ["cmake", "-S", os.path.join(CPP, "tests"), "-B", posix_dir, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={BUILD_TYPE}",
           "-DWMA_TESTS_WERROR=ON", f"-DFETCHCONTENT_BASE_DIR={os.path.join(posix_dir, '.deps')}"]
    if os.path.isdir(gtest):
        cfg.append(f"-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST={gtest}")
    r = runner(cfg, os.path.join(posix_dir, "configure.log"))
    if r.returncode != 0:
        raise Fatal(f"[tests-posix] cmake no configuro (log: {posix_dir}/configure.log):\n" + "\n".join(r.stdout.splitlines()[-25:]))
    cfg_targets = {t: True for t in targets_de_test(posix_dir)}
    srcs, fl, log, detail, by_out = build_objects("tests", os.path.join(CPP, "tests"), posix_dir, prefix, "", jobs, runner,
                                                  native=True, gtest_src=gtest if os.path.isdir(gtest) else None, only=only)
    print(f"  tests-posix: {len(srcs)} objetos de {len(cfg_targets)} targets de test, {len(fl)} fallan", flush=True)
    if fl:
        for s_ in sorted(fl):
            print(f"  {s_}   [tests-posix]")
            for l in error_lines(detail.get(s_, "")):
                print(f"      {l}")
        print(f"      (log completo: {log})")
        raise Fatal("la suite POSIX no compila con el compilador del host: sin ella no hay diff de registro")

    nm_win = find_tool([prefix + "nm", prefix + "gcc-nm"])
    filt_win = shutil.which(prefix + "c++filt") or find_tool(["c++filt", "llvm-cxxfilt"])
    nm_posix = find_tool(["nm", "llvm-nm"])
    filt_posix = find_tool(["c++filt", "llvm-cxxfilt"])
    # los objetos de Windows son TODOS los de la suite: se ordena por target de test
    win = clases_por_target(win_dir, win_by_out, t_win, nm_win, filt_win, runner)
    by_out_cmp, _ = sin_fallados(by_out, fallan)
    posix = clases_por_target(posix_dir, by_out_cmp, set(cfg_targets), nm_posix, filt_posix, runner)
    posix = {t: v for t, v in posix.items() if t in cfg_targets}
    win = {t: v for t, v in win.items() if t in t_win and t not in muertos}
    posix = {t: v for t, v in posix.items() if t not in muertos}
    errores, notas, resumen = diff_registro(
        posix, win, lst.hallazgos["posix-solo"], lst["solo-windows"], nombres_de_fuentes())
    print("  diff de registro (clases `<Suite>_<Test>_Test::TestBody` por target):")
    for l in resumen:
        print(f"    {l}")
    n_decl = sum(lst.hallazgos["posix-solo"].values())
    print(f"    declarado: {n_decl} `posix-solo`, {len(lst['solo-windows'])} `solo-windows`")
    for n in notas:
        print(f"    nota: {n}")
    if muertos:
        errores.append("target(s) de test sin ningun objeto de Windows que compile: " + ", ".join(muertos))
    if errores:
        print(f"\nROJO — el diff de registro Windows/POSIX (AC-061.1) no coincide con lo declarado en "
              f"audio/src/main/cpp/tests/windows-not-run.txt:")
        for e in errores:
            print(f"  {e}")
        return 1
    print("  diff de registro: OK — lo que se registra distinto en Windows es exactamente lo declarado")
    return 0


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

        # una fuente de tests que falla Y esta en el baseline: el diff de registro corre igual (sin SALTEADO), con ella excluida
        base2 = os.path.join(tmp, "baseline2.txt")
        with open(base2, "w", encoding="utf-8") as fh:
            fh.write("a.cpp" + chr(10))
        llamadas = []

        def con_registro(rc_reg):
            def reg(win_dir, win_by_out, fallan):
                llamadas.append(set(fallan))
                return rc_reg
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = guarded(gate, [], os.path.join(tmp, "b2"), "p-", "/jh", 1,
                             fake(ninja_rc=1, ninja_out="FAILED: [code=1] a.obj" + chr(10) + "g++ -c a" + chr(10) + "a.cpp:1:1: error: boom" + chr(10)),
                             base2, reg)
            return rc, out.getvalue() + err.getvalue()
        rc, txt = con_registro(0)
        if rc != 0 or not llamadas or llamadas[-1] != {"a.cpp"} or "SALTEADO" in txt:
            bad.append(f"fuente de tests caida en el baseline: el registro tenia que correr con fallan={{a.cpp}} (rc={rc}, {llamadas}, {txt!r})")
        rc, txt = con_registro(1)
        if rc != 1:
            bad.append(f"fuente de tests caida en el baseline y registro rojo: tenia que dar 1 (dio {rc})")
    return bad


def registro_cases():
    """El diff de registro (AC-061.1) en sus dos direcciones, con nm simulado (sin Docker ni compilador)."""
    import types
    bad = []
    C = collections.Counter

    def d(name, posix, win, decl_posix=None, decl_solo=None, rojo=False, contiene=None, nota=None):
        errs, notas, _ = diff_registro(posix, win, C(decl_posix or []), decl_solo or {},
                                       {"A_B_Test": "A.B", "C_D_Test": "C.D", "W_X_Test": "W.X"})
        if bool(errs) != rojo:
            bad.append(f"registro: {name}: esperaba {'rojo' if rojo else 'verde'}, vio {errs or 'verde'}")
        if contiene and not any(contiene in e for e in errs):
            bad.append(f"registro: {name}: el rojo no nombra {contiene!r}: {errs}")
        if nota and not any(nota in n for n in notas):
            bad.append(f"registro: {name}: falta la nota {nota!r}: {notas}")

    both = {"t": {"A_B_Test": {"a.cpp"}, "C_D_Test": {"a.cpp"}}}
    d("iguales", both, both)
    d("un TEST compilado afuera en Windows, sin declarar", both, {"t": {"A_B_Test": {"a.cpp"}}}, rojo=True, contiene="C.D")
    d("... declarado", both, {"t": {"A_B_Test": {"a.cpp"}}}, decl_posix=[(("t", "C.D"))])
    d("una entrada posix-solo que sobra (Windows tambien lo compila)", both, both, decl_posix=[("t", "C.D")], rojo=True,
      contiene="C.D")
    d("una entrada posix-solo de un test que ya no existe", both, both, decl_posix=[("t", "Z.Z")], rojo=True)
    d("solo-windows sin declarar", both, {"t": {**both["t"], "W_X_Test": {"w.cpp"}}}, rojo=True, contiene="W.X")
    d("... declarado por nombre", both, {"t": {**both["t"], "W_X_Test": {"w.cpp"}}}, decl_solo={"W.X": "r"})
    d("... declarado por archivo", both, {"t": {**both["t"], "W_X_Test": {"w.cpp"}}}, decl_solo={"w.cpp": "r"})
    d("solo-windows por nombre que sobra", both, both, decl_solo={"W.X": "r"}, rojo=True, contiene="W.X")
    d("solo-windows por archivo que sobra", both, both, decl_solo={"w.cpp": "r"}, rojo=True, contiene="w.cpp")
    d("un target que Windows no configura, todo declarado", {"u": {"A_B_Test": {"a.cpp"}}}, {}, decl_posix=[("u", "A.B")])
    d("... sin declarar", {"u": {"A_B_Test": {"a.cpp"}}}, {}, rojo=True, contiene="A.B")
    d("un target de Windows que el host no construyo: no verificable, en verde", {}, {"t": {"A_B_Test": {"a.cpp"}}},
      nota="NO VERIFICABLE")
    d("... y su solo-windows por nombre no se da por sobrante", {}, {"t": {"W_X_Test": {"w.cpp"}}},
      decl_solo={"W.X": "r"}, nota="NO VERIFICABLE")
    d("lo declarado de un target que nadie construye: nota, en verde", both, both, decl_posix=[("zz", "A.B")], nota="zz")

    sym = ("0000000000000000 T _ZN8A_B_Test8TestBodyEv\n0000000000000010 W _ZN8C_D_TestIiE8TestBodyEv\n"
           "                 U _ZN8E_F_Test8TestBodyEv\n0000000000000020 T _ZN3Foo3barEv\n")
    if simbolos_testbody(sym) != {"_ZN8A_B_Test8TestBodyEv", "_ZN8C_D_TestIiE8TestBodyEv"}:
        bad.append(f"registro: nm: solo los simbolos DEFINIDOS con TestBody: {simbolos_testbody(sym)}")
    got = clases_de_demangled(["A_B_Test::TestBody()", "(anonymous namespace)::G_H_Test::TestBody()",
                               "C_D_Test<int>::TestBody()", "testing::Test::TestBody()", "Foo::bar()"])
    if got != {"A_B_Test", "G_H_Test", "C_D_Test"}:
        bad.append(f"registro: demangled: {got}")
    # TYPED_TEST_P, y nombres con `_` (no se parte por `_`: se junta)
    got = clases_de_demangled(["gtest_suite_Ty_::Name<int>::TestBody()", "gtest_suite_Ty_::Name<std::vector<int> >::TestBody()",
                               "gtest_suite_Suite_With_Underscores_::Test_Name<float>::TestBody()",
                               "Suite_With_Underscores_Test_Name_Test::TestBody()", "(anonymous namespace)::gtest_suite_Q_::Z<int>::TestBody()",
                               "gtest_suite_Ty_::Name<int>::Other()"])
    if got != {"Ty_Name_Test", "Suite_With_Underscores_Test_Name_Test", "Q_Z_Test"}:
        bad.append(f"registro: demangled TYPED_TEST_P / guiones bajos: {got}")
    if clase_de("Suite_With_Underscores.Test_Name") != "Suite_With_Underscores_Test_Name_Test":
        bad.append("registro: clase_de con guiones bajos")
    # nm simulado de punta a punta: un objeto con TYPED_TEST_P y un TEST normal
    by_out_p = {"t/CMakeFiles/t.dir/a.cpp.obj": "x/a.cpp"}
    fake_p = lambda cmd, log=None, **kw: types.SimpleNamespace(returncode=0, stdout="0 T _Z1\n0 W _Z2 TestBody\n0 T _Z3 TestBody\n")
    dem_p = {"_Z2": "gtest_suite_Ty_::Name<int>::TestBody()", "_Z3": "A_B_Test::TestBody()"}
    got = clases_por_target("/b", by_out_p, {"t"}, "nm", "filt", fake_p, lambda f, syms: [dem_p[x] for x in syms])
    if got != {"t": {"Ty_Name_Test": {"a.cpp"}, "A_B_Test": {"a.cpp"}}}:
        bad.append(f"registro: clases_por_target con TYPED_TEST_P: {got}")
    # fuentes que fallan: sus objetos salen de los dos lados; un target sin ningun objeto vivo es rojo
    by_o = {"t/CMakeFiles/t.dir/a.cpp.obj": "x/a.cpp", "t/CMakeFiles/t.dir/b.cpp.obj": "x/b.cpp",
            "u/CMakeFiles/u.dir/c.cpp.obj": "x/c.cpp"}
    resto, ex = sin_fallados(by_o, {"x/b.cpp", "x/c.cpp"})
    if sorted(resto.values()) != ["x/a.cpp"] or ex != ["x/b.cpp", "x/c.cpp"]:
        bad.append(f"registro: sin_fallados: {resto} {ex}")
    if targets_sin_objetos(by_o, {"t", "u"}, {"x/b.cpp", "x/c.cpp"}) != ["u"]:
        bad.append("registro: un target con TODOS los objetos caidos tenia que dar rojo (y t, con uno vivo, no)")
    if targets_sin_objetos(by_o, {"t", "u"}, set()) != []:
        bad.append("registro: sin fuentes caidas ningun target esta muerto")
    with tempfile.TemporaryDirectory() as tmp:
        for f in ("core_tests[1]_include.cmake", "x_tests[1]_tests.cmake"):
            open(os.path.join(tmp, f), "w").close()
        if targets_de_test(tmp) != {"core_tests"}:
            bad.append(f"registro: targets_de_test: {targets_de_test(tmp)}")
    by_out = {"t/CMakeFiles/t.dir/a.cpp.obj": "x/a.cpp", "gtest/CMakeFiles/gtest.dir/g.cc.obj": "g.cc"}
    fake = lambda cmd, log=None, **kw: types.SimpleNamespace(returncode=0, stdout="0000 T _ZN8A_B_Test8TestBodyEv\n")
    got = clases_por_target("/b", by_out, {"t"}, "nm", "filt", fake, lambda f, syms: ["A_B_Test::TestBody()"] * len(syms))
    if got != {"t": {"A_B_Test": {"a.cpp"}}}:
        bad.append(f"registro: clases_por_target: {got}")
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

    class _R:
        def __init__(self, rc, out=""):
            self.returncode, self.stdout = rc, out

    def fake(fallan=()):
        def runner(cmd, log_path=None, **kw):
            if cmd[0] == "ninja" and "ninja" in fallan:
                return _R(1, "FAILED: x.obj\nx.cpp:1:1: error: boom\n")
            if len(cmd) > 2 and cmd[2] in fallan:
                return _R(1, "rojo")
            return _R(0, "ok")
        return runner

    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "dll"))
        open(os.path.join(tmp, "dll", "watermelon_audio.dll"), "w").close()
        eq("dll: todo verde", guarded(dll_gate, tmp, "/jh", 1, "od", fake()), 0)
        eq("dll: --pe rojo", guarded(dll_gate, tmp, "/jh", 1, "od", fake(("--pe",))), 1)
        eq("dll: --imports rojo", guarded(dll_gate, tmp, "/jh", 1, "od", fake(("--imports",))), 1)
        eq("dll: no construye", guarded(dll_gate, tmp, "/jh", 1, "od", fake(("ninja",))), 1)

    bad += red_paths_cases()
    bad += registro_cases()

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
