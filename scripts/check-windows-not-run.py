#!/usr/bin/env python3
"""
check-windows-not-run.py — el trinquete de lo que NO corre en Windows (REQ-061 AC-061.1).

La suite de host tiene que correr entera en Windows; lo que es POSIX por naturaleza (FIFO, fd +
mmap de sub-region...) se DECLARA, no se esconde. AC-061.1: el diff entre los `--gtest_list_tests` de la
Mac y de Windows, mas los GTEST_SKIP que se ejecutan en Windows, es IGUAL a un listado declarado con la
razon de cada entrada, y el trinquete falla si una entrada sobra o falta.

EL LISTADO es `tests/windows-not-run.txt`; una entrada por linea, `tipo | nombre | razon` (la primera
forma, sin tipo, es `Suite.Test | razon`):

  Suite.Test | razon                 sale SKIPPED en Windows con `[windows-not-run]` (macro
                                      `WMA_WINDOWS_NOT_RUN("razon")`, `tests/support/WindowsNotRun.h`)
  variante | Suite.Test | razon       el cuerpo del test tiene un `#if` sobre una macro de plataforma y
                                      el test SI corre en las dos: cambia la premisa o el estimulo,
                                      no la asercion
  ambiente | <prefijo> | razon        un skip de AMBIENTE (corpus ausente, GeneralUser, Release-only...):
                                      el prefijo EXACTO del mensaje del GTEST_SKIP
  cmake | ruta | construccion | razon  UN hallazgo de CMake: un archivo de tests y la construccion
                                      normalizada (`if(WIN32) => return()`), no el archivo entero
  fuera | ruta | condicion | razon     UN `#if` de plataforma FUERA de los cuerpos de TEST (include, helper,
                                      constante, macro): `if defined(_WIN32)`; sin numeros de linea; si la
                                      misma condicion aparece N veces en el archivo, N entradas
  deshabilitado | Suite.Test | razon  un TEST con prefijo DISABLED_ (no corre en ninguna plataforma)
  solo-windows | Suite.Test o archivo.cpp | razon   test (o fuente) que existe SOLO en Windows
  posix-solo | target | Suite.Test | razon   test que se compila/registra en POSIX y NO en Windows. Lo verifica el
                                      DIFF DE REGISTRO de scripts/windows-cross/check.py (nm sobre los objetos de las dos
                                      suites): esa es la mitad de AC-061.1 que esta lista estatica no puede ver

REGLAS ESTATICAS (corren en cualquier SO)
  E1  el conjunto de tests que usan la macro == las entradas `Suite.Test | razon`.
  E2a ningun TEST/TEST_F/TEST_P/TYPED_TEST*/INSTANTIATE_* se registra dentro de un bloque #if/#ifdef/
      #ifndef/#elif (ni su #else) cuya condicion nombra una macro de plataforma (_WIN32, __APPLE__,
      __linux__, TARGET_OS_*, GTEST_OS_*...): existiria en una plataforma y en otra no. Excepcion: `solo-windows`.
  E2b un `#if` sobre una macro de plataforma DENTRO del cuerpo de un TEST exige que el cuerpo empiece
      con la macro (y entonces es un test declarado) o que el test este en `variante`. Trinquete en las
      dos direcciones: una `variante` que ya no tiene tal `#if` sobra.
  E2c un TEST con prefijo DISABLED_ es un test que no corre: exige `deshabilitado` (y sobra si ya no esta).
  E2d INVENTARIO: todo `#if/#ifdef/#ifndef/#elif` con macro de plataforma FUERA de los cuerpos de TEST (en
      toda fuente de tests, incluida tests/support) esta declarado `fuera`, en las dos direcciones. Un
      `#define SKIPW() return` bajo `#ifdef _WIN32` esconde una falla sin tocar ningun TEST.
  E3  (tambien en los `.cmake` que los CMake de tests incluyen, thirdparty/googletest.cmake incluido; las condiciones son de
      plataforma Y de compilador: WIN32, CMAKE_*_COMPILER_ID, ENV{MSYSTEM}...; un genex con PLATFORM_ID/COMPILER_ID o un
      `${WIN32}` es hallazgo donde aparezca; bajo esas condiciones tambien lo es toda definicion -D y toda propiedad;
      SKIP_REGULAR_EXPRESSION/WILL_FAIL/EXTRA_ARGS lo son siempre)
      ningun CMake de tests excluye fuentes ni targets de test (list(FILTER|REMOVE_ITEM|REMOVE_AT)), ni
      corta la configuracion (`return()`), bajo un if() sobre WIN32/UNIX/APPLE/MINGW/MSVC/CMAKE_SYSTEM_NAME...
      (el if() se parsea con parentesis balanceados, tambien multilinea), ni usa DISABLED/TEST_FILTER/
      gtest_filter/EXCLUDE_FROM_ALL sobre algo que ctest registra, salvo el hallazgo declarado `cmake`
      (archivo + construccion). Un EXCLUDE_FROM_ALL sobre un target que NINGUN CMake registra en ctest no
      cuenta. Un `cmake`/`solo-windows` que ya no se usa sobra.

REGLAS DE RESULTADOS (`--results DIR`, sobre los XML de gtest de una corrida en Windows)
  R1  los tests SALTEADOS con el marcador == las entradas `Suite.Test` (uno que corre, que no aparece o
      uno marcado que no esta declarado, falla; `--partial` tolera los ausentes).
  R2  TODO otro skip esta declarado como `ambiente` (el mensaje EMPIEZA con el prefijo). Un skip sin
      mensaje, o sin declarar, falla. Un prefijo declarado que no aparece en la corrida completa sobra
      (salvo `--partial`).

  R3  (con --build-dir) cada test que registra cada exe (`<exe> --gtest_list_tests`) tiene su XML (--partial lo tolera), ninguno
      trae `<failure>` aunque ctest haya salido 0, y un exe que el SO no deja ejecutar (Smart App Control) es ROJO.

`--self-test` prueba cada regla en las dos direcciones. Corre antes que el lint.
"""
import argparse
import collections
import os
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP = os.path.join(ROOT, 'audio', 'src', 'main', 'cpp')
LISTADO = os.path.join(CPP, 'tests', 'windows-not-run.txt')
MARCADOR = '[windows-not-run]'
MACRO = 'WMA_WINDOWS_NOT_RUN'
TIPOS = ('variante', 'ambiente', 'cmake', 'solo-windows', 'fuera', 'deshabilitado', 'posix-solo')
HALLAZGOS = ('cmake', 'fuera', 'posix-solo')   # `tipo | ruta | clave | razon`: una entrada por hallazgo
CLAVE_DE = {'cmake': 'construccion', 'fuera': 'condicion', 'posix-solo': 'Suite.Test'}

PLATFORM = re.compile(
    r'\b(_WIN32|WIN32|__WIN32__|_WIN64|__MINGW32__|__MINGW64__|__MINGW__|_MSC_VER|__CYGWIN__|WINAPI_FAMILY\w*|'
    r'__APPLE__|__MACH__|TARGET_OS_\w+|GTEST_OS_\w+|'
    r'__linux__|__linux|linux|__gnu_linux__|__unix__|__unix|unix|__ANDROID__|__ANDROID_API__|'
    r'__FreeBSD__|__NetBSD__|__OpenBSD__|__sun|__EMSCRIPTEN__|__QNX__|__has_include|'
    r'__GNUC__|__GNUG__|__clang__|__clang_major__|_POSIX_VERSION|_POSIX_C_SOURCE|_POSIX_SOURCE|_XOPEN_SOURCE)\b')
TEST_DECL = re.compile(r'^\s*(TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', re.MULTILINE)
INSTANTIATE = re.compile(
    r'^\s*(INSTANTIATE_TEST_SUITE_P|INSTANTIATE_TEST_CASE_P|INSTANTIATE_TYPED_TEST_SUITE_P)\s*\(\s*(\w+)\s*,\s*(\w+)',
    re.MULTILINE)
PP = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')
EXENTO_FUENTE = {os.path.join('tests', 'support', 'WindowsNotRun.h')}   # define la macro: no la usa


# ---------------------------------------------------------------- fuentes

def sin_comentarios(text):
    """Blanquea comentarios y literales (conserva saltos de linea). Un `#if` o un TEST dentro de un
    comentario o de un raw string no cuenta."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == '//':
            while i < n and text[i] != '\n':
                out.append(' '); i += 1
        elif two == '/*':
            while i < n and text[i:i + 2] != '*/':
                out.append('\n' if text[i] == '\n' else ' '); i += 1
            out.append('  '); i += 2
        elif c == 'R' and text[i + 1:i + 2] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            m = re.match(r'R"([^()\\ ]{0,16})\(', text[i:])
            if not m:
                out.append(c); i += 1; continue
            end = text.find(')' + m.group(1) + '"', i)
            end = n if end < 0 else end + len(m.group(1)) + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:end])); i = end
        elif c in '"\'':
            q = c
            out.append(q); i += 1
            while i < n and text[i] != q and text[i] != '\n':
                if text[i] == '\\':
                    out.append(' '); i += 1
                out.append(' '); i += 1
            out.append(q); i += 1
        else:
            out.append(c); i += 1
    return ''.join(out)


def archivos_de_test(root, exts):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ('build', 'build-san', '.deps', 'thirdparty')]
        if not re.search(r'[/\\]tests?([/\\]|$)', dirpath + os.sep):
            continue
        for name in filenames:
            if name.endswith(exts):
                yield os.path.join(dirpath, name)


def leer(path):
    with open(path, encoding='utf-8', errors='replace') as fh:
        return fh.read()


def norm(rel):
    return rel.replace(os.sep, '/')


def usos_de_la_macro(root):
    """{ 'Suite.Test': ruta } y errores. Cada uso se atribuye al TEST que lo contiene."""
    usos, errores = {}, []
    for path in sorted(archivos_de_test(root, ('.cpp', '.h'))):
        rel = os.path.relpath(path, root)
        if rel in EXENTO_FUENTE:
            continue
        lines = sin_comentarios(leer(path)).split('\n')
        actual = None
        for no, line in enumerate(lines, 1):
            m = TEST_DECL.match(line)
            if m:
                actual = (m.group(1), m.group(2), m.group(3))
            if MACRO + '(' in line and not line.lstrip().startswith('#'):
                if actual is None:
                    errores.append('%s:%d: %s fuera de un TEST' % (rel, no, MACRO)); continue
                if actual[0] not in ('TEST', 'TEST_F'):
                    errores.append('%s:%d: %s en un %s: el nombre depende de la instanciacion; '
                                   'usa TEST o TEST_F' % (rel, no, MACRO, actual[0])); continue
                nombre = '%s.%s' % (actual[1], actual[2])
                if nombre in usos:
                    errores.append('%s:%d: %s usada dos veces en %s' % (rel, no, MACRO, nombre))
                usos[nombre] = '%s:%d' % (rel, no)
    return usos, errores


# ---------------------------------------------------------------- listado

class Listado:
    def __init__(self):
        self.no_run = {}                       # Suite.Test -> razon
        self.tipos = {t: {} for t in TIPOS if t not in HALLAZGOS}    # tipo -> { nombre: razon }
        self.hallazgos = {t: collections.Counter() for t in HALLAZGOS}   # tipo -> {(ruta, clave): cuantas}
        self.errores = []

    def __getitem__(self, tipo):
        return self.tipos[tipo]

    def cuenta(self, tipo):
        return sum(self.hallazgos[tipo].values())


def leer_listado(path):
    lst = Listado()
    base = os.path.basename(path)
    if not os.path.exists(path):
        lst.errores.append('falta el listado %s' % path)
        return lst
    for no, raw in enumerate(leer(path).splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        cabeza = line.split('|', 1)[0].strip()
        if cabeza in HALLAZGOS:
            # `tipo | ruta | clave | razon`: la clave puede traer `||` (sin espacio en medio), por eso se
            # parte en ` | `.
            campos = [c.strip() for c in re.split(r' \| ', line, maxsplit=3)]
            if len(campos) != 4 or not all(campos):
                lst.errores.append('%s:%d: se espera `%s | ruta | %s | razon` (todo con contenido): %r'
                                   % (base, no, cabeza, CLAVE_DE[cabeza], raw))
                continue
            if cabeza == 'posix-solo' and not re.fullmatch(r'\w+\.\w+', campos[2]):
                lst.errores.append('%s:%d: se espera `posix-solo | target | Suite.Test | razon`: %r' % (base, no, raw))
                continue
            lst.hallazgos[cabeza][(campos[1], campos[2])] += 1
            continue
        campos = [c.strip() for c in line.split('|', 2)]
        if campos[0] in TIPOS:
            if len(campos) != 3 or not campos[1] or not campos[2]:
                lst.errores.append('%s:%d: se espera `%s | nombre | razon` (con razon): %r' % (base, no, campos[0], raw))
                continue
            tipo, nombre, razon = campos
            if tipo in ('variante', 'deshabilitado') and not re.fullmatch(r'\w+\.\w+', nombre):
                lst.errores.append('%s:%d: `%s` lleva `Suite.Test`: %r' % (base, no, tipo, raw)); continue
            if nombre in lst.tipos[tipo]:
                lst.errores.append('%s:%d: entrada repetida: %s | %s' % (base, no, tipo, nombre))
            lst.tipos[tipo][nombre] = razon
            continue
        nombre, sep, razon = line.partition('|')
        nombre, razon = nombre.strip(), razon.strip()
        if not sep or not re.fullmatch(r'\w+\.\w+', nombre) or not razon:
            lst.errores.append('%s:%d: se espera `Suite.Test | razon` (con razon) o `tipo | nombre | razon`: %r'
                               % (base, no, raw))
            continue
        if nombre in lst.no_run:
            lst.errores.append('%s:%d: entrada repetida: %s' % (base, no, nombre))
        lst.no_run[nombre] = razon
    return lst


def comparar_hallazgos(tipo, hallados, declarados, sobra_si_falta_el_archivo=True):
    """hallados: [(ruta, clave, donde, mensaje)]; declarados: Counter {(ruta, clave): n}. Trinquete en las
    dos direcciones: un hallazgo sin declarar falla, y uno declarado que ya no existe (o que existe menos
    veces) sobra."""
    errores = []
    vistos = collections.Counter((r, c) for r, c, _, _ in hallados)
    for (r, c), n in sorted(vistos.items()):
        if n > declarados.get((r, c), 0):
            for rr, cc, donde, msg in hallados:
                if (rr, cc) == (r, c):
                    errores.append('%s: %s -- si es legitimo, declaralo: `%s | %s | %s | razon`' % (donde, msg, tipo, r, c))
    for (r, c), n in sorted(declarados.items()):
        if vistos.get((r, c), 0) < n:
            errores.append('el listado declara `%s | %s | %s` %d vez/veces y el arbol lo tiene %d (sobra la entrada)'
                           % (tipo, r, c, n, vistos.get((r, c), 0)))
    return errores


def cuerpo_de(texto, desde):
    """(inicio, fin) de offsets del `{ ... }` que sigue a `desde`, o None."""
    ini = texto.find('{', desde)
    if ini < 0:
        return None
    prof = 0
    for i in range(ini, len(texto)):
        if texto[i] == '{':
            prof += 1
        elif texto[i] == '}':
            prof -= 1
            if prof == 0:
                return ini, i
    return None


def normalizar_pp(kind, rest):
    """`#ifdef  _WIN32 ` -> `ifdef _WIN32`; `#if ! defined ( _WIN32 )` -> `if !defined(_WIN32)`."""
    cond = re.sub(r'\s+', ' ', rest).strip()
    cond = re.sub(r'\bdefined\s*\(', 'defined(', cond)
    cond = re.sub(r'\(\s+', '(', cond)
    cond = re.sub(r'\s+\)', ')', cond)
    cond = re.sub(r'!\s+', '!', cond)
    return ('%s %s' % (kind, cond)).strip()


def directivas(lines):
    """[(idx, kind, rest)] de las directivas del preprocesador; una continuacion con `\\` se une a su
    primera linea (un `#if A \\ && defined(_WIN32)` no se escapa por estar partido)."""
    out, idx = [], 0
    while idx < len(lines):
        pp = PP.match(lines[idx])
        if not pp:
            idx += 1
            continue
        kind, rest, ini = pp.group(1), pp.group(2), idx
        while rest.rstrip().endswith('\\') and idx + 1 < len(lines):
            rest = rest.rstrip()[:-1] + ' ' + lines[idx + 1]
            idx += 1
        out.append((ini, kind, rest))
        idx += 1
    return out


def analizar_fuentes(root, lst):
    """E2a / E2b / E2c / E2d. Devuelve (errores, variantes_halladas, solo_win_usados, deshabilitados, fuera)."""
    errores, variantes, solo_usados, deshab, fuera = [], set(), set(), set(), []
    solo = lst['solo-windows']
    solo_archivos = {n for n in solo if n.endswith(('.cpp', '.h'))}
    for path in sorted(archivos_de_test(root, ('.cpp', '.h'))):
        rel = os.path.relpath(path, root)
        # Un archivo `solo-windows` queda exento de E2a (sus TEST estan bajo la guarda a proposito), no del resto.
        es_solo = os.path.basename(path) in solo_archivos
        if es_solo:
            solo_usados.add(os.path.basename(path))
        texto = sin_comentarios(leer(path))
        lines = texto.split('\n')
        offsets, acc = [], 0
        for line in lines:
            offsets.append(acc)
            acc += len(line) + 1

        def linea_de(offset):
            lo, hi = 0, len(offsets) - 1
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if offsets[mid] <= offset:
                    lo = mid
                else:
                    hi = mid - 1
            return lo

        # --- los TEST con su cuerpo (por offsets) ---
        cuerpos = []   # (linea_ini, linea_fin, 'Suite.Test', empieza_con_macro)
        for m in TEST_DECL.finditer(texto):
            nombre = '%s.%s' % (m.group(2), m.group(3))
            if m.group(2).startswith('DISABLED_') or m.group(3).startswith('DISABLED_'):
                deshab.add(nombre)
                if nombre not in lst['deshabilitado']:
                    errores.append('%s:%d: %s esta DISABLED_: un test que no corre sin declarar '
                                   '(`deshabilitado | %s | razon`) (E2c)' % (norm(rel), linea_de(m.start()) + 1, nombre, nombre))
            cb = cuerpo_de(texto, m.end())
            if cb is None:
                continue
            primero = texto[cb[0] + 1:cb[1]].lstrip()
            cuerpos.append((linea_de(cb[0]), linea_de(cb[1]), nombre, primero.startswith(MACRO + '(')))

        def cuerpo_que_contiene(idx):
            for c in cuerpos:
                if c[0] < idx < c[1]:
                    return c
            return None

        dirs = directivas(lines)
        for idx, kind, rest in dirs:
            if kind not in ('if', 'ifdef', 'ifndef', 'elif') or not PLATFORM.search(rest):
                continue
            c = cuerpo_que_contiene(idx)
            if c is None:
                fuera.append((norm(rel), normalizar_pp(kind, rest), '%s:%d' % (norm(rel), idx + 1),
                              '`#%s` de plataforma fuera de los cuerpos de TEST (E2d)' % kind))
            elif not c[3]:
                variantes.add(c[2])
                if c[2] not in lst['variante']:
                    errores.append(
                        '%s:%d: `#%s` sobre una macro de plataforma dentro del cuerpo de %s, que no '
                        'empieza con %s y no esta declarado `variante` en el listado (E2b)'
                        % (norm(rel), idx + 1, kind, c[2], MACRO))
        if es_solo:
            continue

        # --- E2a: lo que se registra bajo un bloque de plataforma ---
        pila = []
        dir_por_linea = {idx: (kind, rest) for idx, kind, rest in dirs}
        for idx, line in enumerate(lines):
            if idx in dir_por_linea:
                kind, rest = dir_por_linea[idx]
                plat = bool(PLATFORM.search(rest))
                if kind in ('if', 'ifdef', 'ifndef'):
                    pila.append(plat)
                elif kind == 'elif' and pila:
                    pila[-1] = pila[-1] or plat
                elif kind == 'endif' and pila:
                    pila.pop()
                continue
            if not any(pila):
                continue
            m = TEST_DECL.match(line)
            if m:
                nombre = '%s.%s' % (m.group(2), m.group(3))
                if nombre in solo:
                    solo_usados.add(nombre)
                else:
                    errores.append('%s:%d: %s(%s, %s) dentro de un bloque condicionado a una macro de plataforma (E2a)'
                                   % (norm(rel), idx + 1, m.group(1), m.group(2), m.group(3)))
                continue
            m = INSTANTIATE.match(line)
            if m:
                errores.append('%s:%d: %s(%s, %s...) dentro de un bloque condicionado a una macro de plataforma (E2a)'
                               % (norm(rel), idx + 1, m.group(1), m.group(2), m.group(3)))
    return errores, variantes, solo_usados, deshab, fuera


# ------------------------------------------------------------------ CMake

# Una condicion de plataforma O de compilador: lo que hace que un mismo CMake configure distinto en Windows y en POSIX.
CM_PLATFORM = re.compile(r'\b(?:WIN32|UNIX|APPLE|MINGW|MSVC|MSYS|CYGWIN|ANDROID|IOS|LINUX|EMSCRIPTEN|CMAKE_(?:HOST_)?SYSTEM\w*|'
                         r'CMAKE_HOST_\w+|CMAKE_(?:\w+_)?COMPILER\w*)\b'
                         r'|\bENV\{(?:MSYSTEM\w*|MINGW\w*|OSTYPE|OS)\}')
# La misma dependencia de plataforma escrita como valor (`${WIN32}`, `$ENV{MSYSTEM}`) o como expresion generadora
# (`$<$<PLATFORM_ID:Windows>:test_x.cpp>`, `$<CXX_COMPILER_ID:GNU>`, `$<BOOL:${WIN32}>`): vale donde aparezca, no solo bajo un if().
CM_PLAT_REF = re.compile(r'\$\{(?:WIN32|UNIX|APPLE|MINGW|MSVC|MSYS|CYGWIN|ANDROID|CMAKE_(?:HOST_)?SYSTEM\w*|CMAKE_HOST_\w+|'
                         r'CMAKE_(?:\w+_)?COMPILER\w*)\}|\$ENV\{(?:MSYSTEM\w*|MINGW\w*|OSTYPE|OS)\}')
CM_GENEX = re.compile(r'\$<.*?\b(?:PLATFORM_ID|\w*COMPILER_ID|\w*COMPILER_VERSION|\w*COMPILER_FRONTEND_VARIANT)\b')
# Definiciones (-D / compile_definitions) y propiedades que cambian lo que un test hace o si se registra.
CM_DEFINE = re.compile(r'(?<![\w-])[-/]D\s*[A-Za-z_]')
CM_DEFINE_CMD = ('target_compile_definitions', 'add_compile_definitions', 'add_definitions')
CM_PROP_CMD = ('set_tests_properties', 'set_target_properties', 'set_property', 'set_source_files_properties',
               'set_directory_properties')
CM_PROP_BAJO_PLATAFORMA = re.compile(r'\bTIMEOUT\b|\bCOMPILE_DEFINITIONS\b|\bCOMPILE_FLAGS\b')
CM_SRC = re.compile(r'[\w./${}-]+\.(?:cpp|cc|c)\b')
CM_TARGET = ('add_executable', 'add_test', 'gtest_discover_tests', 'wma_gtest_discover_tests', 'add_subdirectory')
CM_LISTA = ('FILTER', 'REMOVE_ITEM', 'REMOVE_AT')
CM_FILTRO = re.compile(r'\bDISABLED(_\w*)?\b|\bTEST_FILTER\b|gtest_filter|GTEST_FILTER|\bSKIP_REGULAR_EXPRESSION\b|'
                       r'\bSKIP_RETURN_CODE\b|\bWILL_FAIL\b|\bPASS_REGULAR_EXPRESSION\b|\bEXTRA_ARGS\b', re.IGNORECASE)
CM_EXCLUDE = re.compile(r'\bEXCLUDE_FROM_ALL\b', re.IGNORECASE)
CM_CMD = re.compile(r'([A-Za-z_]\w*)\s*\(')


def cm_sin_comentarios(text):
    """Blanquea los comentarios (`# ...` y `#[[ ... ]]`) fuera de strings; conserva saltos de linea."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1]); i = j + 1
        elif c == '#':
            m = re.match(r'#\[(=*)\[', text[i:])
            if m:
                cierre = ']' + m.group(1) + ']'
                j = text.find(cierre, i)
                j = n if j < 0 else j + len(cierre)
            else:
                j = text.find('\n', i)
                j = n if j < 0 else j
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:j])); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def cm_comandos(text):
    """[(nombre_en_minusculas, args_normalizados, linea)] con los parentesis BALANCEADOS: una llamada
    partida en varias lineas (un `if(` con `NOT WIN32` en la segunda) es UNA."""
    text = cm_sin_comentarios(text)
    out, pos, n = [], 0, len(text)
    while True:
        m = CM_CMD.search(text, pos)
        if not m:
            return out
        i, prof = m.end(), 1
        while i < n and prof:
            c = text[i]
            if c == '"':
                i += 1
                while i < n and text[i] != '"':
                    i += 2 if text[i] == '\\' else 1
            elif c == '(':
                prof += 1
            elif c == ')':
                prof -= 1
            i += 1
        args = ' '.join(text[m.end():i - 1].split())
        out.append((m.group(1).lower(), args, text.count('\n', 0, m.start()) + 1))
        pos = i


def cm_cmake_files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ('build', 'build-san', '.deps', 'thirdparty')]
        if 'CMakeLists.txt' in filenames:
            yield dirpath, os.path.join(dirpath, 'CMakeLists.txt')


def cm_archivos_de_test(root):
    """[(dirpath, path)] de lo que decide como se configura un test: los CMakeLists de los directorios de tests Y
    los `.cmake` que esos incluyen (`thirdparty/googletest.cmake` define como se registra TODO test; no se ignora
    porque viva en thirdparty), hasta el punto fijo."""
    base = [(d, p) for d, p in cm_cmake_files(root) if re.search(r'[/\\]tests?([/\\]|$)', d + os.sep)]
    por_nombre = collections.defaultdict(list)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ('build', 'build-san', '.deps')]
        for f in filenames:
            if f.endswith('.cmake'):
                por_nombre[f].append((dirpath, os.path.join(dirpath, f)))
    vistos, cola = {p for _, p in base}, list(base)
    while cola:
        _, path = cola.pop()
        for nombre, args, _ in cm_comandos(leer(path)):
            if nombre != 'include':
                continue
            for m in re.finditer(r'([\w.-]+\.cmake)\b', args):
                for dirpath, p in por_nombre.get(m.group(1), []):
                    if p not in vistos:
                        vistos.add(p)
                        base.append((dirpath, p))
                        cola.append((dirpath, p))
    return sorted(base, key=lambda t: t[1])


def cm_registrados(root):
    """(nombres de target que ctest registra, hay_registros_no_resolubles). Los de dentro de un function()/
    macro() son la definicion del helper, no un registro."""
    nombres, desconocido = set(), False
    for _, path in cm_cmake_files(root):
        prof = 0
        for nombre, args, _ in cm_comandos(leer(path)):
            if nombre in ('function', 'macro'):
                prof += 1; continue
            if nombre in ('endfunction', 'endmacro'):
                prof = max(0, prof - 1); continue
            if prof:
                continue
            toks = args.split()
            if nombre in ('gtest_discover_tests', 'wma_gtest_discover_tests'):
                cand = toks[:1]
            elif nombre == 'add_test':
                if toks[:1] == ['NAME']:
                    cand = toks[1:2] + (toks[toks.index('COMMAND') + 1:toks.index('COMMAND') + 2] if 'COMMAND' in toks else [])
                else:
                    cand = toks[:2]
            else:
                continue
            for t in cand:
                if '$' in t:
                    desconocido = True
                else:
                    nombres.add(t)
    return nombres, desconocido


def cm_objetivos_excluidos(nombre, args):
    """Los targets sobre los que un comando pone EXCLUDE_FROM_ALL, o None si no se sabe a cual."""
    toks = args.split()
    if nombre in ('add_executable', 'add_library') and toks:
        return toks[:1]
    if nombre == 'set_target_properties' and 'PROPERTIES' in toks:
        return toks[:toks.index('PROPERTIES')]
    if nombre == 'set_property' and 'TARGET' in toks:
        fin = [i for i, t in enumerate(toks) if t in ('APPEND', 'APPEND_STRING', 'PROPERTY')]
        return toks[toks.index('TARGET') + 1:(fin[0] if fin else len(toks))]
    return None


def analizar_cmake(root, lst):
    """E3. Devuelve (errores, solo_usados). Un hallazgo es (ruta, clave): la clave es la construccion
    normalizada, con el if() que la condiciona (`if(WIN32) => return()`)."""
    solo = {n for n in lst['solo-windows'] if n.endswith(('.cpp', '.h'))}
    registrados, desconocido = cm_registrados(root)
    hallados, solo_usados = [], set()
    for dirpath, path in cm_archivos_de_test(root):
        rel = norm(os.path.relpath(path, root))
        pila = []     # [es_de_plataforma, rotulo de la rama vigente, condicion del if]
        for nombre, args, no in cm_comandos(leer(path)):
            if nombre == 'if':
                pila.append([bool(CM_PLATFORM.search(args)), 'if(%s)' % args, args])
                continue
            if nombre == 'elseif' and pila:
                pila[-1][0] = pila[-1][0] or bool(CM_PLATFORM.search(args))
                pila[-1][1] = 'elseif(%s)' % args
                continue
            if nombre == 'else' and pila:
                pila[-1][1] = 'else(%s)' % pila[-1][2]
                continue
            if nombre == 'endif':
                if pila:
                    pila.pop()
                continue
            stmt = '%s(%s)' % (nombre, args)
            rotulo = ' > '.join(e[1] for e in pila if e[0])
            clave = '%s => %s' % (rotulo, stmt) if rotulo else stmt

            msgs = []
            hallazgo = msgs.append
            if CM_FILTRO.search(args):
                hallazgo('DISABLED/TEST_FILTER/gtest_filter en un CMake de tests: un test que no corre sin declarar')
            if CM_EXCLUDE.search(args):
                objetivos = cm_objetivos_excluidos(nombre, args)
                if objetivos is None or desconocido or any('$' in t or t in registrados for t in objetivos):
                    hallazgo('EXCLUDE_FROM_ALL sobre algo que ctest registra (o no se sabe cual): sus tests no se construyen')
            if CM_GENEX.search(args) or CM_PLAT_REF.search(args):
                hallazgo('expresion generadora o valor que depende de la plataforma/compilador (PLATFORM_ID, '
                         'COMPILER_ID, ${WIN32}...): lo que cambia en Windows se declara')
            if rotulo:
                _plataforma(nombre, args, hallazgo, dirpath, solo, solo_usados)
            if msgs:
                hallados.append((rel, clave, '%s:%d' % (rel, no), '; '.join(msgs)))
    return hallados, solo_usados


def _plataforma(nombre, args, hallazgo, dirpath, solo, solo_usados):
    """Lo que no puede estar bajo un if() de plataforma en un CMake de tests."""
    if nombre == 'return':
        hallazgo('return() bajo una condicion de plataforma corta la configuracion de los tests')
    if nombre in CM_DEFINE_CMD or CM_DEFINE.search(args):
        hallazgo('una definicion (-D / compile_definitions) bajo una condicion de plataforma o compilador: puede '
                 'apagar un TEST con un #if que no nombra ninguna plataforma')
    if nombre in CM_PROP_CMD and (nombre == 'set_property' or re.search(r'\bPROPERTIES\b', args)):
        hallazgo('una propiedad (%s) bajo una condicion de plataforma o compilador' % nombre)
    elif CM_PROP_BAJO_PLATAFORMA.search(args):
        hallazgo('TIMEOUT/COMPILE_DEFINITIONS bajo una condicion de plataforma o compilador')
    if nombre in CM_TARGET:
        hallazgo('un target de test (%s) bajo una condicion de plataforma' % nombre)
    if nombre == 'list' and args.split()[:1] and args.split()[0].upper() in CM_LISTA:
        hallazgo('list(%s) bajo una condicion de plataforma: excluye elementos de una lista de fuentes' % args.split()[0].upper())
    for src in CM_SRC.findall(args):
        base = os.path.basename(src)
        if base in solo:
            solo_usados.add(base)
            continue
        candidato = os.path.normpath(os.path.join(dirpath, src))
        es_de_test = (not re.search(r'[${}]', src) and os.path.exists(candidato)
                      and re.search(r'[/\\]tests?[/\\]', candidato + os.sep) is not None) \
            or re.fullmatch(r'test_\w+\.cpp', base) is not None
        if es_de_test:
            hallazgo('%s bajo una condicion de plataforma: un test no se excluye por SO, se declara en '
                     'windows-not-run.txt' % src)


def chequeo_estatico(root, listado):
    lst = leer_listado(listado)
    errores = list(lst.errores)
    usos, e1 = usos_de_la_macro(root)
    errores += e1
    for nombre in sorted(set(usos) - set(lst.no_run)):
        errores.append('%s usa %s pero no esta en el listado (falta la entrada): %s' % (usos[nombre], MACRO, nombre))
    for nombre in sorted(set(lst.no_run) - set(usos)):
        errores.append('el listado declara %s pero ningun TEST usa %s (sobra la entrada)' % (nombre, MACRO))

    e2, variantes, solo_fuentes, deshab, fuera = analizar_fuentes(root, lst)
    errores += e2
    for nombre in sorted(set(lst['variante']) - variantes):
        errores.append('el listado declara `variante | %s` pero su cuerpo ya no tiene un #if de plataforma, o empieza '
                       'con %s, o no existe (sobra la entrada)' % (nombre, MACRO))
    for nombre in sorted(set(lst['deshabilitado']) - deshab):
        errores.append('el listado declara `deshabilitado | %s` pero ningun TEST tiene ese nombre con prefijo '
                       'DISABLED_ (sobra la entrada)' % nombre)
    errores += comparar_hallazgos('fuera', fuera, lst.hallazgos['fuera'])
    hallados_cm, solo_cmake = analizar_cmake(root, lst)
    errores += comparar_hallazgos('cmake', hallados_cm, lst.hallazgos['cmake'])
    for nombre in sorted(lst['solo-windows']):
        if nombre.endswith(('.cpp', '.h')):
            if nombre not in solo_fuentes:
                errores.append('el listado declara `solo-windows | %s` pero no hay tal archivo de test (sobra la entrada)' % nombre)
        elif nombre not in solo_fuentes:
            errores.append('el listado declara `solo-windows | %s` pero ningun TEST con ese nombre esta bajo un #if '
                           'de plataforma (sobra la entrada)' % nombre)
    return errores


# ------------------------------------------------------------- resultados

def saltos_de_xml(directorio):
    """{ 'Suite.Test': mensaje } de todo skip en los XML de gtest; ademas, el conjunto de los vistos."""
    saltos, vistos = {}, set()
    xmls = []
    for dirpath, _, filenames in os.walk(directorio):
        xmls += [os.path.join(dirpath, f) for f in filenames if f.endswith('.xml')]
    for path in sorted(xmls):
        tree = ET.parse(path)
        for tc in tree.iter('testcase'):
            if tc.get('status') is None:
                continue   # el XML de `--gtest_list_tests` (discovery de ctest): lista, no corrio nada
            nombre = '%s.%s' % (tc.get('classname'), tc.get('name'))
            vistos.add(nombre)
            sk = tc.find('skipped')
            if sk is None and tc.get('status') == 'notrun':
                saltos[nombre] = 'DISABLED'
            elif sk is not None:
                msg = (sk.get('message') or sk.text or '')
                # gtest antepone `archivo:linea`; lo que importa es lo que el test dijo.
                cuerpo = [l for l in msg.replace('&#x0A;', '\n').splitlines()
                          if l.strip() and not re.fullmatch(r'.*:\d+\s*', l.strip())]
                saltos[nombre] = ' '.join(cuerpo)
    return saltos, vistos, len(xmls)


def resumen_por_ejecutable(directorio):
    """{ exe: [pass, fail, skip] }. El XML de cada test cuelga de `<directorio>/<ejecutable>/`."""
    filas = {}
    for dirpath, _, filenames in os.walk(directorio):
        for f in sorted(filenames):
            if not f.endswith('.xml'):
                continue
            partes = os.path.relpath(dirpath, directorio).split(os.sep)
            exe = partes[0] if partes[0] != '.' else re.sub(r'_\d+$', '', f[:-4])
            fila = filas.setdefault(exe, [0, 0, 0])
            for tc in ET.parse(os.path.join(dirpath, f)).iter('testcase'):
                if tc.get('status') is None:
                    continue   # listado de discovery, no una corrida
                if tc.find('failure') is not None or tc.find('error') is not None:
                    fila[1] += 1
                elif tc.find('skipped') is not None or tc.get('status') == 'notrun':
                    fila[2] += 1
                else:
                    fila[0] += 1
    return filas


def fallos_de_xml(directorio):
    """{ 'Suite.Test': 'failure'|'error' } de todo testcase con un `<failure>` o `<error>`, sin mirar el exit code
    de ctest: SKIP_REGULAR_EXPRESSION o WILL_FAIL lo dejan en 0 con la falla escrita en el XML."""
    out = {}
    for dirpath, _, filenames in os.walk(directorio):
        for f in sorted(filenames):
            if not f.endswith('.xml'):
                continue
            for tc in ET.parse(os.path.join(dirpath, f)).iter('testcase'):
                if tc.get('status') is None:
                    continue
                for tag in ('failure', 'error'):
                    if tc.find(tag) is not None:
                        out['%s.%s' % (tc.get('classname'), tc.get('name'))] = tag
    return out


LISTA_SUITE = re.compile(r'^([\w/]+)\.(?:\s+#.*)?$')
LISTA_TEST = re.compile(r'^\s+(\S+)(?:\s+#.*)?$')


def parsear_lista(salida):
    """Los nombres completos `Suite.Test` de un `--gtest_list_tests`: suites parametrizadas (`Inst/Suite.` y
    `Test/3  # GetParam() = ...`) y typed (`Suite/0.  # TypeParam = int`) incluidas."""
    nombres, suite = set(), None
    for line in salida.splitlines():
        if not line.strip():
            continue
        m = LISTA_SUITE.match(line.rstrip())
        if m and not line.startswith(' '):
            suite = m.group(1)
            continue
        m = LISTA_TEST.match(line)
        if m and suite is not None and line.startswith(' '):
            nombres.add('%s.%s' % (suite, m.group(1)))
    return nombres


def descubrir_ejecutables(build_dir):
    """[(target, exe, cwd, filtro, args)] de los `<target>[1]_include.cmake` que escribe gtest_discover_tests: lo
    que ctest va a ejecutar de verdad."""
    out = []
    for dirpath, _, filenames in os.walk(build_dir):
        for f in sorted(filenames):
            if not f.endswith('_include.cmake'):
                continue
            texto = leer(os.path.join(dirpath, f))

            def campo(nombre):
                m = re.search(r'\b%s\s+\[=*\[(.*?)\]=*\]' % nombre, texto, re.DOTALL)
                return m.group(1) if m else ''
            exe = campo('TEST_EXECUTABLE')
            if exe:
                out.append((os.path.basename(exe).rsplit('.exe', 1)[0], exe, campo('TEST_WORKING_DIR') or dirpath,
                            campo('TEST_FILTER'), campo('TEST_EXTRA_ARGS')))
    return sorted(out)


def ejecutar_lista(exe, cwd):
    """(stdout, error): `<exe> --gtest_list_tests`. Un exe que el SO no deja ejecutar (Smart App Control, una
    directiva de aplicaciones) es un ERROR, nunca un verde: no se sabe que registra."""
    import subprocess
    try:
        r = subprocess.run([exe, '--gtest_list_tests'], cwd=cwd, capture_output=True, text=True, timeout=300,
                           encoding='utf-8', errors='replace')
    except OSError as exc:
        return '', ('no pude ejecutar `%s --gtest_list_tests` (%s): si es una directiva del SO (Smart App Control, '
                    'AppLocker) el exe no se puede listar y no se puede decir que tests registra' % (exe, exc))
    except subprocess.TimeoutExpired:
        return '', '`%s --gtest_list_tests` no termino en 300 s' % exe
    if r.returncode != 0:
        return '', '`%s --gtest_list_tests` salio %d: %s' % (exe, r.returncode, (r.stderr or r.stdout).strip()[:300])
    return r.stdout, None


def registrados_de(build_dir, ejecutar=ejecutar_lista):
    """({target: set(Suite.Test)}, errores): lo que cada exe de la suite registra segun `--gtest_list_tests`."""
    regs, errores = {}, []
    exes = descubrir_ejecutables(build_dir)
    if not exes:
        return regs, ['no hay ningun `*_include.cmake` de gtest_discover_tests en %s: no se sabe que exes tiene la suite' % build_dir]
    for target, exe, cwd, filtro, args in exes:
        if filtro.strip():
            errores.append('%s: gtest_discover_tests con TEST_FILTER=%r: registra menos de lo que el exe tiene' % (target, filtro))
        if args.strip():
            errores.append('%s: gtest_discover_tests con EXTRA_ARGS=%r: cambia lo que cada test corre' % (target, args))
        salida, err = ejecutar(exe, cwd)
        if err:
            errores.append('%s: %s' % (target, err))
            continue
        nombres = parsear_lista(salida)
        if not nombres:
            errores.append('%s: `--gtest_list_tests` no listo ningun test' % target)
        regs[target] = nombres
    return regs, errores


def chequeo_resultados(directorio, listado, parcial, registrados=None):
    """registrados: {target: set(Suite.Test)} de `registrados_de` (None = no se verifica el registro; solo los
    tests unitarios del skip lo omiten). Cada test que un exe registra tiene su XML, salvo --partial."""
    lst = leer_listado(listado)
    errores, notas = list(lst.errores), []
    saltos, vistos, n_xml = saltos_de_xml(directorio)
    if n_xml == 0:
        return ['no hay XML de gtest en %s: la corrida no los genero' % directorio], notas
    for nombre, tag in sorted(fallos_de_xml(directorio).items()):
        errores.append('%s: <%s> en el XML (un test que falla no se acepta aunque ctest haya salido 0)' % (nombre, tag))
    if registrados is not None:
        deshab = set(lst['deshabilitado'])
        for target in sorted(registrados):
            if parcial:
                continue
            sin = sorted(n for n in registrados[target] - vistos
                         if not any(n == d or n.startswith(d + '/') for d in deshab))
            for n in sin[:25]:
                errores.append('%s: %s esta registrado (--gtest_list_tests) y no tiene XML: ctest no lo corrio '
                               '(TEST_FILTER, DISABLED, -E, un filtro...)' % (target, n))
            if len(sin) > 25:
                errores.append('%s: ... y %d registrados mas sin XML' % (target, len(sin) - 25))
    entradas = lst.no_run
    marcados = {n for n, m in saltos.items() if MARCADOR in m}
    for nombre in sorted(marcados - set(entradas)):
        errores.append('%s salio SKIPPED con %s pero no esta en el listado' % (nombre, MARCADOR))
    for nombre in sorted(set(entradas) - marcados):
        if nombre in vistos:
            errores.append('%s esta en el listado pero NO salio saltado con %s (corre o se salteo por otra razon)'
                           % (nombre, MARCADOR))
        elif not parcial:
            errores.append('%s esta en el listado pero no aparece en los resultados' % nombre)
    prefijos = lst['ambiente']
    usados = set()
    for nombre in sorted(set(saltos) - marcados):
        msg = saltos[nombre]
        if not msg:
            errores.append('%s: skip SIN mensaje' % nombre)
        elif msg == 'DISABLED':
            errores.append('%s: salio DISABLED (no corrio)' % nombre)
        else:
            hit = [p for p in prefijos if msg.startswith(p)]
            if not hit:
                errores.append('%s: skip no declarado (`ambiente | <prefijo> | razon`): %s' % (nombre, msg))
            else:
                usados.update(hit)
                notas.append('skip: %s -- %s' % (nombre, msg))
    if not parcial:
        for p in sorted(set(prefijos) - usados):
            errores.append('el listado declara `ambiente | %s` pero ningun skip de la corrida empieza asi (sobra)' % p)
    return errores, notas


# -------------------------------------------------------------- self-test

def _w(base, rel, text):
    p = os.path.join(base, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8') as fh:
        fh.write(text)
    return p


def _xml(base, nombre, casos):
    cuerpo = ''.join(
        '<testcase name="%s" status="run" classname="%s">%s</testcase>' % (
            c[1], c[0], ('<skipped message="%s"/>' % c[2]) if len(c) > 2 and c[2] is not None else '')
        for c in casos)
    return _w(base, nombre, '<testsuites><testsuite name="x">%s</testsuite></testsuites>' % cuerpo)


def self_test():
    fallos = []

    def caso(descripcion, esperado_rojo, errores):
        ok = bool(errores) == esperado_rojo
        print('  %-76s %s' % (descripcion, 'OK' if ok else 'ROTO'))
        if not ok:
            fallos.append(descripcion)
            for e in errores:
                print('      ' + e)

    TEST_OK = '#include "tests/support/WindowsNotRun.h"\nTEST(S, A) {\n    WMA_WINDOWS_NOT_RUN("r");\n}\n'
    with tempfile.TemporaryDirectory() as tmp:
        def arbol(nombre, fuente=TEST_OK, listado='S.A | r\n', cmake=None, extra=None):
            base = os.path.join(tmp, nombre)
            _w(base, 'x/tests/test_a.cpp', fuente)
            lst = _w(base, 'tests/windows-not-run.txt', listado)
            if cmake is not None:
                _w(base, 'x/tests/CMakeLists.txt', cmake)
            for rel, text in (extra or {}).items():
                _w(base, rel, text)
            return base, lst

        def estatico(nombre, **kw):
            b, l = arbol(nombre, **kw)
            return chequeo_estatico(b, l)

        # E1
        caso('E1 macro y listado coinciden -> verde', False, estatico('e1_ok'))
        caso('E1 entrada de mas en el listado -> rojo', True, estatico('e1_sobra', listado='S.A | r\nS.B | otra\n'))
        caso('E1 test con la macro sin entrada -> rojo', True, estatico('e1_falta', listado=''))
        caso('E1 entrada sin razon -> rojo', True, estatico('e1_sinrazon', listado='S.A |\n'))
        caso('E1 la macro dentro de un comentario no cuenta -> verde', False,
             estatico('e1_comentario', fuente='// WMA_WINDOWS_NOT_RUN("x")\nTEST(S, A) {}\n', listado=''))
        caso('E1 macro en TEST_P (nombre ambiguo) -> rojo', True,
             estatico('e1_tp', fuente='TEST_P(S, A) {\n WMA_WINDOWS_NOT_RUN("r");\n}\n'))
        caso('E1 un tipo desconocido en el listado -> rojo', True,
             estatico('e1_tipo', listado='S.A | r\nfoo | bar | baz\n'))

        # E2a — el registro
        caso('E2a TEST dentro de #if defined(_WIN32) -> rojo', True,
             estatico('e2a_win', fuente=TEST_OK + '#if defined(_WIN32)\nTEST(S, B) {}\n#endif\n'))
        caso('E2a TEST dentro de #if !defined(_WIN32) -> rojo', True,
             estatico('e2a_notwin', fuente=TEST_OK + '#if !defined(_WIN32)\nTEST(S, B) {}\n#endif\n'))
        caso('E2a TEST en el #else de un #ifdef _WIN32 -> rojo', True,
             estatico('e2a_else', fuente=TEST_OK + '#ifdef _WIN32\nint a;\n#else\nTEST_F(S, B) {}\n#endif\n'))
        caso('E2a TEST bajo __APPLE__ -> rojo', True,
             estatico('e2a_apple', fuente=TEST_OK + '#if defined(__APPLE__)\nTEST(S, B) {}\n#endif\n'))
        caso('E2a TEST bajo TARGET_OS_IPHONE / __linux__ / __ANDROID__ -> rojo', True,
             estatico('e2a_ios', fuente=TEST_OK + '#if TARGET_OS_IPHONE || defined(__linux__)\nTEST(S, B) {}\n#endif\n'))
        caso('E2a TEST_P / TYPED_TEST bajo una macro de plataforma -> rojo', True,
             estatico('e2a_tp', fuente=TEST_OK + '#ifdef __unix__\nTYPED_TEST(S, B) {}\n#endif\n'))
        caso('E2a INSTANTIATE_TEST_SUITE_P bajo una macro de plataforma -> rojo', True,
             estatico('e2a_inst', fuente=TEST_OK + '#ifdef _MSC_VER\nINSTANTIATE_TEST_SUITE_P(I, S, ::testing::Values(1));\n#endif\n'))
        caso('E2a TEST bajo una condicion que NO es de plataforma -> verde', False,
             estatico('e2a_otro', fuente=TEST_OK + '#if defined(WMA_TEST_HOOKS)\nTEST(S, B) {}\n#endif\n'))
        caso('E2a un #if de plataforma que NO envuelve ningun TEST, DECLARADO `fuera` -> verde', False,
             estatico('e2a_helper', fuente='#if defined(_WIN32)\nint helper();\n#endif\n' + TEST_OK,
                      listado='S.A | r\nfuera | x/tests/test_a.cpp | if defined(_WIN32) | helper\n'))
        caso('E2c TEST con DISABLED_ sin declarar -> rojo', True,
             estatico('e2c_disabled', fuente=TEST_OK + 'TEST(S, DISABLED_B) {}\n'))
        caso('E2c TEST_F con el DISABLED_ en la suite sin declarar -> rojo', True,
             estatico('e2c_disabled_suite', fuente=TEST_OK + 'TEST_F(DISABLED_Suite, B) {}\n'))
        caso('E2c ... declarado `deshabilitado` con razon -> verde', False,
             estatico('e2c_decl', fuente=TEST_OK + 'TEST(S, DISABLED_B) {}\n',
                      listado='S.A | r\ndeshabilitado | S.DISABLED_B | no corre en ninguna plataforma\n'))
        caso('E2c ... y si el DISABLED_ ya no esta -> rojo (sobra)', True,
             estatico('e2c_sobra', listado='S.A | r\ndeshabilitado | S.DISABLED_B | x\n'))
        caso('E2c ... sin razon -> rojo', True,
             estatico('e2c_sinrazon', fuente=TEST_OK + 'TEST(S, DISABLED_B) {}\n',
                      listado='S.A | r\ndeshabilitado | S.DISABLED_B |\n'))
        caso('E2c un DISABLED_ solo dentro de un comentario -> verde', False,
             estatico('e2c_comentario', fuente=TEST_OK + '// TEST(S, DISABLED_B) {}\n'))
        FW = 'fuera | x/tests/test_wav_dump_windows.cpp | if defined(_WIN32) | envuelve el test solo-windows\n'
        b, l = arbol('e2a_solo', listado='S.A | r\nsolo-windows | test_wav_dump_windows.cpp | solo existe en Windows\n' + FW)
        _w(b, 'x/tests/test_wav_dump_windows.cpp', '#if defined(_WIN32)\nTEST(WavWin, A) {}\n#endif\n')
        caso('E2a el archivo sólo-Windows declarado -> verde', False, chequeo_estatico(b, l))
        b, l = arbol('e2a_solo_sobra', listado='S.A | r\nsolo-windows | test_wav_dump_windows.cpp | x\n')
        caso('E2a ... y si el archivo declarado no existe -> rojo (sobra)', True, chequeo_estatico(b, l))
        b, l = arbol('e2a_solo_disabled', listado='S.A | r\nsolo-windows | test_wav_dump_windows.cpp | x\n' + FW)
        _w(b, 'x/tests/test_wav_dump_windows.cpp', '#if defined(_WIN32)\nTEST(WavWin, DISABLED_A) {}\n#endif\n')
        caso('E2c el archivo solo-windows NO exime de DISABLED_ -> rojo', True, chequeo_estatico(b, l))
        b, l = arbol('e2a_solo_fuera', listado='S.A | r\nsolo-windows | test_wav_dump_windows.cpp | x\n')
        _w(b, 'x/tests/test_wav_dump_windows.cpp', '#ifdef _WIN32\n#define SKIPW() return\n#else\n#define SKIPW()\n#endif\n')
        caso('E2d el archivo solo-windows NO exime del inventario `fuera` -> rojo', True, chequeo_estatico(b, l))
        caso('E2a un test sólo-Windows suelto y declarado -> verde', False,
             estatico('e2a_solot', fuente=TEST_OK + '#if defined(_WIN32)\nTEST_F(U, W) {}\n#endif\n',
                      listado='S.A | r\nsolo-windows | U.W | solo Windows\nfuera | x/tests/test_a.cpp | if defined(_WIN32) | envuelve U.W\n'))
        caso('E2a ... y sin la declaracion -> rojo', True,
             estatico('e2a_solot_no', fuente=TEST_OK + '#if defined(_WIN32)\nTEST_F(U, W) {}\n#endif\n',
                      listado='S.A | r\nfuera | x/tests/test_a.cpp | if defined(_WIN32) | x\n'))
        caso('E2a ... y si ya no esta bajo #if -> rojo (sobra)', True,
             estatico('e2a_solot_sobra', fuente=TEST_OK + 'TEST_F(U, W) {}\n',
                      listado='S.A | r\nsolo-windows | U.W | solo Windows\n'))

        # E2b — el cuerpo
        caso('E2b #if _WIN32 dentro del cuerpo de un test DECLARADO (empieza con la macro) -> verde', False,
             estatico('e2b_decl', fuente=TEST_OK.replace('    WMA_WINDOWS_NOT_RUN("r");\n',
                      '    WMA_WINDOWS_NOT_RUN("r");\n#if !defined(_WIN32)\n    int x = 0;\n#endif\n')))
        caso('E2b #ifdef _WIN32 GTEST_SKIP() << "pendiente" en el cuerpo -> rojo', True,
             estatico('e2b_skip', fuente=TEST_OK + 'TEST(S, B) {\n#ifdef _WIN32\n    GTEST_SKIP() << "pendiente";\n#endif\n    EXPECT_TRUE(true);\n}\n'))
        caso('E2b cuerpo vacio bajo #if !defined(_WIN32) sin la macro -> rojo', True,
             estatico('e2b_vacio', fuente=TEST_OK + 'TEST(S, B) {\n#if !defined(_WIN32)\n    EXPECT_TRUE(true);\n#endif\n}\n'))
        caso('E2b #elif de plataforma en el cuerpo -> rojo', True,
             estatico('e2b_elif', fuente=TEST_OK + 'TEST(S, B) {\n#if FOO\n    int a;\n#elif defined(__APPLE__)\n    int b;\n#endif\n}\n'))
        caso('E2b #if de plataforma en el cuerpo, DECLARADO como variante -> verde', False,
             estatico('e2b_var', fuente=TEST_OK + 'TEST(S, B) {\n#if defined(_WIN32)\n    int n = 16;\n#else\n    int n = 1;\n#endif\n}\n',
                      listado='S.A | r\nvariante | S.B | en Windows el estimulo usa 16 bloques\n'))
        caso('E2b ... la variante sin razon -> rojo', True,
             estatico('e2b_var_sinrazon', fuente=TEST_OK + 'TEST(S, B) {\n#if defined(_WIN32)\n    int n = 16;\n#endif\n}\n',
                      listado='S.A | r\nvariante | S.B |\n'))
        caso('E2b una variante que ya no tiene #if de plataforma -> rojo (sobra)', True,
             estatico('e2b_var_sobra', fuente=TEST_OK + 'TEST(S, B) {\n    int n = 1;\n}\n',
                      listado='S.A | r\nvariante | S.B | x\n'))
        caso('E2b una variante que empieza con la macro -> rojo (sobra)', True,
             estatico('e2b_var_macro', listado='S.A | r\nvariante | S.A | x\n',
                      fuente=TEST_OK.replace('    WMA_WINDOWS_NOT_RUN("r");\n', '    WMA_WINDOWS_NOT_RUN("r");\n#if defined(_WIN32)\n#endif\n')))
        caso('E2b #if que NO es de plataforma en el cuerpo -> verde', False,
             estatico('e2b_otro', fuente=TEST_OK + 'TEST(S, B) {\n#if defined(NDEBUG)\n    int a;\n#endif\n}\n'))
        # E2d — el inventario de lo que esta FUERA de los cuerpos
        SKIPW = ('#ifdef _WIN32\n#define SKIPW() return\n#else\n#define SKIPW()\n#endif\n'
                 'TEST(Zz, Y) { SKIPW(); EXPECT_TRUE(false); }\n')
        caso('E2d #define SKIPW() bajo #ifdef _WIN32 fuera de los cuerpos, sin declarar -> rojo', True,
             estatico('e2d_skipw', fuente=TEST_OK + SKIPW))
        caso('E2d ... el #ifdef declarado `fuera` -> verde', False,
             estatico('e2d_skipw_ok', fuente=TEST_OK + SKIPW,
                      listado='S.A | r\nfuera | x/tests/test_a.cpp | ifdef _WIN32 | el macro cambia en Windows\n'))
        caso('E2d ... y un #ifdef declarado que ya no esta -> rojo (sobra)', True,
             estatico('e2d_sobra', listado='S.A | r\nfuera | x/tests/test_a.cpp | ifdef _WIN32 | x\n'))
        caso('E2d ... declarado SIN razon -> rojo', True,
             estatico('e2d_sinrazon', fuente=TEST_OK + SKIPW, listado='S.A | r\nfuera | x/tests/test_a.cpp | ifdef _WIN32 |\n'))
        DOS = '#if defined(_WIN32)\nint a;\n#endif\n#if defined(_WIN32)\nint b;\n#endif\n'
        caso('E2d la misma condicion DOS veces con UNA sola entrada -> rojo', True,
             estatico('e2d_dos', fuente=TEST_OK + DOS,
                      listado='S.A | r\nfuera | x/tests/test_a.cpp | if defined(_WIN32) | a\n'))
        caso('E2d ... y con DOS entradas -> verde', False,
             estatico('e2d_dos_ok', fuente=TEST_OK + DOS,
                      listado='S.A | r\nfuera | x/tests/test_a.cpp | if defined(_WIN32) | a\nfuera | x/tests/test_a.cpp | if defined(_WIN32) | b\n'))
        caso('E2d los espacios en la condicion no cambian la clave -> verde', False,
             estatico('e2d_espacios', fuente=TEST_OK + '#  if  ! defined ( _WIN32 )\nint a;\n#endif\n',
                      listado='S.A | r\nfuera | x/tests/test_a.cpp | if !defined(_WIN32) | a\n'))
        caso('E2d una condicion con `||` en la clave se declara y calza -> verde', False,
             estatico('e2d_or', fuente=TEST_OK + '#if defined(_WIN32) || defined(__APPLE__)\nint a;\n#endif\n',
                      listado='S.A | r\nfuera | x/tests/test_a.cpp | if defined(_WIN32) || defined(__APPLE__) | a\n'))
        caso('E2d un #if partido con barra invertida que nombra _WIN32 en la 2da linea -> rojo', True,
             estatico('e2d_continuacion', fuente=TEST_OK + '#if FOO && \\\n    defined(_WIN32)\nint a;\n#endif\n'))
        caso('E2d en un archivo de tests/support -> rojo', True,
             estatico('e2d_support', extra={'x/tests/support/H.h': '#ifdef _WIN32\nint h;\n#endif\n'}))
        caso('E2d un #if que NO es de plataforma fuera de los cuerpos -> verde', False,
             estatico('e2d_otro', fuente=TEST_OK + '#if defined(NDEBUG)\nint a;\n#endif\n'))
        caso('E2d un #if de plataforma en un comentario -> verde', False,
             estatico('e2d_comentario', fuente=TEST_OK + '// #ifdef _WIN32\n/* #if defined(__APPLE__) */\n'))
        # macros de plataforma que la regex no veia
        for macro in ('GTEST_OS_WINDOWS', '_WIN64', '__CYGWIN__', '__FreeBSD__', '__MACH__', 'GTEST_OS_LINUX'):
            caso('E2a #ifndef %s envolviendo un TEST -> rojo' % macro, True,
                 estatico('e2a_' + macro, fuente=TEST_OK + '#ifndef %s\nTEST(Zz, Y) {}\n#endif\n' % macro))
        caso('E2b #ifdef GTEST_OS_WINDOWS en el cuerpo de un test sin declarar -> rojo', True,
             estatico('e2b_gtest_os', fuente=TEST_OK + 'TEST(S, B) {\n#ifdef GTEST_OS_WINDOWS\n    int a;\n#endif\n}\n'))

        # E3 — CMake
        CMK = 'x/tests/CMakeLists.txt'

        def cm(clave, razon='r'):
            return 'S.A | r\ncmake | %s | %s | %s\n' % (CMK, clave, razon)

        caso('E3 el sólo-Windows declarado bajo if(WIN32) -> verde', False,
             estatico('e3_ok', cmake='add_executable(t test_a.cpp)\nif(WIN32)\n  target_sources(t PRIVATE test_wav_dump_windows.cpp)\nendif()\n',
                      listado='S.A | r\nsolo-windows | test_wav_dump_windows.cpp | solo Windows\n'
                              'fuera | x/tests/test_wav_dump_windows.cpp | if defined(_WIN32) | envuelve\n',
                      extra={'x/tests/test_wav_dump_windows.cpp': '#if defined(_WIN32)\nTEST(W, A) {}\n#endif\n'}))
        caso('E3 test_a.cpp bajo if(NOT WIN32) -> rojo', True,
             estatico('e3_notwin', cmake='if(NOT WIN32)\n  target_sources(t PRIVATE test_a.cpp)\nendif()\n'))
        caso('E3 test_a.cpp bajo if(UNIX) -> rojo', True,
             estatico('e3_unix', cmake='if(UNIX)\n  target_sources(t PRIVATE test_a.cpp)\nendif()\n'))
        caso('E3 test_a.cpp bajo if(APPLE) / if(CMAKE_SYSTEM_NAME ...) -> rojo', True,
             estatico('e3_apple', cmake='if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")\n  list(APPEND S test_a.cpp)\nendif()\n'))
        caso('E3 test_a.cpp en el else() de if(WIN32) -> rojo', True,
             estatico('e3_else', cmake='if(WIN32)\n  set(x 1)\nelse()\n  list(APPEND S test_a.cpp)\nendif()\n'))
        caso('E3 un add_executable bajo if(MSVC) -> rojo', True,
             estatico('e3_target', cmake='if(MSVC)\n  add_executable(t2 foo.cpp)\nendif()\n'))
        caso('E3 un gtest_discover_tests bajo if(UNIX) -> rojo', True,
             estatico('e3_disc', cmake='if(UNIX)\n  gtest_discover_tests(t)\nendif()\n'))
        caso('E3 return() bajo if(WIN32) no declarado -> rojo', True,
             estatico('e3_return', cmake='if(WIN32)\n  return()\nendif()\n'))
        caso('E3 return() bajo if(WIN32) DECLARADO por hallazgo -> verde', False,
             estatico('e3_return_ok', cmake='if(WIN32)\n  return()\nendif()\n', listado=cm('if(WIN32) => return()')))
        caso('E3 ... y una declaracion de otra construccion -> rojo (falta y sobra)', True,
             estatico('e3_return_otro', cmake='if(WIN32)\n  return()\nendif()\n', listado=cm('if(UNIX) => return()')))
        caso('E3 ... y una declaracion sin condicion -> rojo (sobra)', True,
             estatico('e3_cmake_sobra', cmake='add_executable(t test_a.cpp)\n', listado=cm('if(WIN32) => return()')))
        caso('E3 cmake de tres campos (el archivo entero) -> rojo', True,
             estatico('e3_tres', cmake='if(WIN32)\n  return()\nendif()\n',
                      listado='S.A | r\ncmake | %s | exime todo el archivo\n' % CMK))
        caso('E3 el archivo declarado NO exime otra construccion: return() declarado + REMOVE_ITEM nuevo -> rojo', True,
             estatico('e3_no_exime', cmake='if(WIN32)\n  return()\nendif()\nif(WIN32)\n  list(REMOVE_ITEM S test_a.cpp)\nendif()\n',
                      listado=cm('if(WIN32) => return()')))
        caso('E3 DISABLED en un CMake de tests -> rojo', True,
             estatico('e3_disabled', cmake='set_tests_properties(S.A PROPERTIES DISABLED TRUE)\n'))
        caso('E3 TEST_FILTER en un CMake de tests -> rojo', True,
             estatico('e3_filter', cmake='gtest_discover_tests(t TEST_FILTER -S.*)\n'))
        caso('E3 TEST_FILTER nuevo en un target con otro hallazgo declarado -> rojo', True,
             estatico('e3_filter_nuevo', cmake='if(WIN32)\n  return()\nendif()\ngtest_discover_tests(t TEST_FILTER -S.*)\n',
                      listado=cm('if(WIN32) => return()')))
        caso('E3 gtest_filter -> rojo', True,
             estatico('e3_gfilter', cmake='add_test(NAME x COMMAND t --gtest_filter=-S.*)\n'))
        caso('E3 DISABLED declarado por hallazgo -> verde', False,
             estatico('e3_disabled_ok', cmake='set_tests_properties(S.A PROPERTIES DISABLED TRUE)\n',
                      listado=cm('set_tests_properties(S.A PROPERTIES DISABLED TRUE)')))
        caso('E3 una fuente de produccion bajo if(WIN32) -> verde', False,
             estatico('e3_prod', cmake='if(WIN32)\n  set(P ../PlatformWindows.cpp)\nendif()\nadd_executable(t test_a.cpp ${P})\n'))
        caso('E3 un set() bajo if(WIN32) -> verde', False,
             estatico('e3_defs', cmake='if(WIN32)\n  set(X 1)\nendif()\n'))
        caso('E3 un comentario con DISABLED -> verde', False,
             estatico('e3_comentario', cmake='# no DISABLED aca\nadd_executable(t test_a.cpp)\n'))
        caso('E3 un bloque de comentario #[[ ]] con return() bajo if(WIN32) -> verde', False,
             estatico('e3_bloque', cmake='if(WIN32)\n#[[ return() ]]\nendif()\n'))
        # E3 — los huecos que el reviewer midio
        caso('E3 if(WIN32) list(REMOVE_ITEM ... test_x.cpp) -> rojo', True,
             estatico('e3_remove', cmake='set(S test_a.cpp)\nif(WIN32)\n  list(REMOVE_ITEM S test_c_api_song.cpp)\nendif()\n'))
        caso('E3 if(WIN32) list(REMOVE_ITEM S ${X}) (sin nombre de test) -> rojo', True,
             estatico('e3_remove_var', cmake='if(WIN32)\n  list(REMOVE_ITEM S ${X})\nendif()\n'))
        caso('E3 if(WIN32) list(FILTER S EXCLUDE REGEX "x") -> rojo', True,
             estatico('e3_lfilter', cmake='if(WIN32)\n  list(FILTER S EXCLUDE REGEX "test_a")\nendif()\n'))
        caso('E3 if(WIN32) list(REMOVE_AT S 0) -> rojo', True,
             estatico('e3_remove_at', cmake='if(WIN32)\n  list(REMOVE_AT S 0)\nendif()\n'))
        caso('E3 if(WIN32) list(APPEND S foo.cpp) (no es exclusion) -> verde', False,
             estatico('e3_append', cmake='if(WIN32)\n  list(APPEND S foo.cpp)\nendif()\n'))
        caso('E3 if( partido en dos lineas con NOT WIN32 en la 2da -> rojo', True,
             estatico('e3_multilinea', cmake='if(FOO AND\n   NOT WIN32)\n  list(REMOVE_ITEM S x.cpp)\nendif()\n'))
        caso('E3 ... y el endif() de un if multilinea desapila el bloque CORRECTO -> verde despues', False,
             estatico('e3_multilinea_endif', cmake='if(FOO AND\n   NOT WIN32)\n  set(a 1)\nendif()\nlist(REMOVE_ITEM S x.cpp)\n'))
        caso('E3 if(WIN32) anidado dentro de un if() comun -> rojo', True,
             estatico('e3_anidado', cmake='if(FOO)\n  if(WIN32)\n    return()\n  endif()\nendif()\n'))
        caso('E3 elseif(WIN32) con return() -> rojo', True,
             estatico('e3_elseif', cmake='if(FOO)\n  set(a 1)\nelseif(WIN32)\n  return()\nendif()\n'))
        caso('E3 EXCLUDE_FROM_ALL sobre un target que ctest registra -> rojo', True,
             estatico('e3_efa', cmake='add_executable(t EXCLUDE_FROM_ALL test_a.cpp)\nwma_gtest_discover_tests(t)\n'))
        caso('E3 EXCLUDE_FROM_ALL por set_target_properties sobre uno registrado -> rojo', True,
             estatico('e3_efa_props', cmake='add_executable(t test_a.cpp)\nset_target_properties(t PROPERTIES EXCLUDE_FROM_ALL ON)\n'
                                            'gtest_discover_tests(t)\n'))
        caso('E3 EXCLUDE_FROM_ALL sobre un target que NINGUN CMake registra (una herramienta) -> verde', False,
             estatico('e3_efa_tool', cmake='add_executable(t test_a.cpp)\nwma_gtest_discover_tests(t)\n'
                                           'add_executable(tool EXCLUDE_FROM_ALL tool.cpp)\n'))
        caso('E3 EXCLUDE_FROM_ALL de una herramienta y un registro por variable -> rojo (no se sabe)', True,
             estatico('e3_efa_var', cmake='add_executable(tool EXCLUDE_FROM_ALL tool.cpp)\nwma_gtest_discover_tests(${T})\n'))
        caso('E3 EXCLUDE_FROM_ALL sobre un add_subdirectory -> rojo', True,
             estatico('e3_efa_sub', cmake='add_subdirectory(sub EXCLUDE_FROM_ALL)\n'))
        caso('E3 el target registrado en OTRO CMake del arbol cuenta -> rojo', True,
             estatico('e3_efa_otro', cmake='add_executable(tool EXCLUDE_FROM_ALL tool.cpp)\n',
                      extra={'y/CMakeLists.txt': 'add_test(NAME tool COMMAND tool)\n'}))

        # E3 — plataforma Y compilador, genex, definiciones, propiedades, y los .cmake que los tests incluyen
        GTC = 'thirdparty/googletest.cmake'
        INC = 'include(${CMAKE_CURRENT_LIST_DIR}/../thirdparty/googletest.cmake)\n'
        caso('E3 genex $<$<NOT:$<PLATFORM_ID:Windows>>:test_x.cpp> -> rojo', True,
             estatico('e3_genex', cmake='add_executable(t test_a.cpp $<$<NOT:$<PLATFORM_ID:Windows>>:test_x.cpp>)\n'))
        caso('E3 genex $<$<CXX_COMPILER_ID:GNU>:test_x.cpp> -> rojo', True,
             estatico('e3_genex_cc', cmake='target_sources(t PRIVATE $<$<CXX_COMPILER_ID:GNU>:test_x.cpp>)\n'))
        caso('E3 genex $<BOOL:${WIN32}> -> rojo', True,
             estatico('e3_genex_bool', cmake='target_sources(t PRIVATE $<$<BOOL:${WIN32}>:test_x.cpp>)\n'))
        caso('E3 un valor ${MINGW} usado como variable -> rojo', True,
             estatico('e3_ref', cmake='set(SKIP ${MINGW})\n'))
        caso('E3 un genex que NO es de plataforma (TARGET_FILE) -> verde', False,
             estatico('e3_genex_ok', cmake='add_custom_command(TARGET t COMMAND $<TARGET_FILE:t>)\n'))
        caso('E3 if(WIN32) target_compile_definitions(WMA_SKIP_SONG=1) -> rojo', True,
             estatico('e3_def', cmake='if(WIN32)\n  target_compile_definitions(core_tests PRIVATE WMA_SKIP_SONG=1)\nendif()\n'))
        caso('E3 ... declarado -> verde', False,
             estatico('e3_def_ok', cmake='if(WIN32)\n  target_compile_definitions(core_tests PRIVATE WMA_SKIP_SONG=1)\nendif()\n',
                      listado=cm('if(WIN32) => target_compile_definitions(core_tests PRIVATE WMA_SKIP_SONG=1)')))
        caso('E3 if(WIN32) add_definitions(-DWMA_SKIP=1) / add_compile_options(-DX) -> rojo', True,
             estatico('e3_def2', cmake='if(WIN32)\n  add_compile_options(-DWMA_SKIP=1)\nendif()\n'))
        caso('E3 add_compile_options(-Wall) bajo if(WIN32) (no es una definicion) -> verde', False,
             estatico('e3_opt', cmake='if(WIN32)\n  add_compile_options(-Wall)\nendif()\n'))
        caso('E3 una definicion fuera de toda condicion -> verde', False,
             estatico('e3_def_plana', cmake='target_compile_definitions(t PRIVATE WMA_TEST_HOOKS=1)\n'))
        caso('E3 definicion bajo if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU") -> rojo', True,
             estatico('e3_def_cc', cmake='if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")\n  add_compile_definitions(X=1)\nendif()\n'))
        caso('E3 definicion bajo if(CMAKE_C_COMPILER_ID ...) / CMAKE_HOST_WIN32 / ENV{MSYSTEM} -> rojo', True,
             estatico('e3_def_c', cmake='if(CMAKE_C_COMPILER_ID MATCHES "Clang")\n  add_definitions(-DX)\nendif()\n'))
        caso('E3 ... CMAKE_HOST_WIN32 -> rojo', True,
             estatico('e3_def_host', cmake='if(CMAKE_HOST_WIN32)\n  add_compile_definitions(X=1)\nendif()\n'))
        caso('E3 ... DEFINED ENV{MSYSTEM} -> rojo', True,
             estatico('e3_def_env', cmake='if(DEFINED ENV{MSYSTEM})\n  add_compile_definitions(X=1)\nendif()\n'))
        caso('E3 set_tests_properties SKIP_REGULAR_EXPRESSION bajo if(WIN32) -> rojo', True,
             estatico('e3_skiprx', cmake='if(WIN32)\n  set_tests_properties(S.A PROPERTIES SKIP_REGULAR_EXPRESSION ".*")\nendif()\n'))
        caso('E3 SKIP_REGULAR_EXPRESSION fuera de toda condicion -> rojo igual', True,
             estatico('e3_skiprx_plano', cmake='set_tests_properties(S.A PROPERTIES SKIP_REGULAR_EXPRESSION ".*")\n'))
        caso('E3 WILL_FAIL -> rojo', True, estatico('e3_willfail', cmake='set_tests_properties(S.A PROPERTIES WILL_FAIL TRUE)\n'))
        caso('E3 TIMEOUT bajo if(WIN32) -> rojo; sin condicion -> verde', True,
             estatico('e3_timeout', cmake='if(WIN32)\n  set_tests_properties(S.A PROPERTIES TIMEOUT 1)\nendif()\n'))
        caso('E3 ... TIMEOUT sin condicion -> verde', False,
             estatico('e3_timeout_plano', cmake='set_tests_properties(S.A PROPERTIES TIMEOUT 1)\n'))
        caso('E3 EXTRA_ARGS / TEST_FILTER en un gtest_discover_tests -> rojo', True,
             estatico('e3_extra', cmake='gtest_discover_tests(t EXTRA_ARGS --gtest_filter=-S.*)\n'))
        caso('E3 un .cmake incluido por el CMake de tests: if(WIN32) TEST_FILTER -> rojo', True,
             estatico('e3_gt_filter', cmake=INC,
                      extra={GTC: 'function(w t)\n  if(WIN32)\n    gtest_discover_tests(${t} TEST_FILTER -S.*)\n  endif()\nendfunction()\n'}))
        caso('E3 un .cmake incluido: add_compile_definitions bajo if(UNIX) -> rojo', True,
             estatico('e3_gt_def', cmake=INC, extra={GTC: 'if(UNIX)\n  add_compile_definitions(WMA_SKIP=1)\nendif()\n'}))
        caso('E3 un .cmake incluido sin nada de plataforma -> verde', False,
             estatico('e3_gt_ok', cmake=INC, extra={GTC: 'function(w t)\n  gtest_discover_tests(${t} XML_OUTPUT_DIR x)\nendfunction()\n'}))
        caso('E3 un .cmake que NINGUN test incluye (produccion) con if(WIN32) -> verde', False,
             estatico('e3_prod_cmake', extra={'thirdparty/otra.cmake': 'if(WIN32)\n  add_compile_definitions(X=1)\nendif()\n'}))
        caso('E3 el .cmake incluido por otro .cmake incluido cuenta -> rojo', True,
             estatico('e3_gt_anidado', cmake=INC,
                      extra={GTC: 'include(${CMAKE_CURRENT_LIST_DIR}/hijo.cmake)\n',
                             'thirdparty/hijo.cmake': 'if(MSVC)\n  add_definitions(-DX)\nendif()\n'}))

        # fuentes: guardas de compilador / POSIX
        for macro, cond in (('__GNUC__', '#ifdef __GNUC__'), ('__clang__', '#if defined(__clang__)'),
                            ('_POSIX_VERSION', '#if defined(_POSIX_VERSION)'), ('_POSIX_C_SOURCE', '#if _POSIX_C_SOURCE >= 200809L'),
                            ('__unix', '#ifdef __unix')):
            caso('E2a TEST bajo %s -> rojo' % macro, True,
                 estatico('e2a_cc_' + macro, fuente=TEST_OK + cond + '\nTEST(Zz, Y) {}\n#endif\n'))
            caso('E2d %s fuera de un cuerpo, sin declarar -> rojo' % macro, True,
                 estatico('e2d_cc_' + macro, fuente=TEST_OK + cond + '\nint a;\n#endif\n'))
        caso('E2b #ifdef __GNUC__ en el cuerpo de un test sin declarar -> rojo', True,
             estatico('e2b_gnuc', fuente=TEST_OK + 'TEST(S, B) {\n#ifdef __GNUC__\n    GTEST_SKIP();\n#endif\n}\n'))

        # R — lo que cada exe registra contra los XML, y los <failure>
        def reg_xml(nombre, casos, registrados, listado='S.A | r\n', parcial=False):
            base = os.path.join(tmp, nombre)
            cuerpo = ''.join('<testcase name="%s" status="run" classname="%s">%s</testcase>' % (
                c[1], c[0], '<failure message="boom"/>' if len(c) > 2 and c[2] == 'failure' else
                ('<skipped message="%s"/>' % c[2] if len(c) > 2 else '')) for c in casos)
            _w(base, 'x.xml', '<testsuites><testsuite name="x">%s</testsuite></testsuites>' % cuerpo)
            return chequeo_resultados(base, _w(base, 'l.txt', listado), parcial, registrados)[0]

        MARC = MARCADOR + ' razon'
        caso('R3 todo lo registrado tiene XML -> verde', False,
             reg_xml('r3_ok', [('S', 'A', MARC), ('S', 'B')], {'t': {'S.A', 'S.B'}}))
        caso('R3 un registrado SIN XML (TEST_FILTER, DISABLED, -E) -> rojo', True,
             reg_xml('r3_sinxml', [('S', 'A', MARC)], {'t': {'S.A', 'S.B'}}))
        caso('R3 ... con --partial explicito se tolera -> verde', False,
             reg_xml('r3_parcial', [('S', 'A', MARC)], {'t': {'S.A', 'S.B'}}, parcial=True))
        caso('R3 un registrado declarado `deshabilitado` sin XML -> verde', False,
             reg_xml('r3_deshab', [('S', 'A', MARC)], {'t': {'S.A', 'S.DISABLED_B'}},
                     listado='S.A | r\ndeshabilitado | S.DISABLED_B | no corre\n'))
        caso('R3 un <failure> en el XML (ctest pudo salir 0) -> rojo', True,
             reg_xml('r3_failure', [('S', 'A', MARC), ('S', 'B', 'failure')], {'t': {'S.A', 'S.B'}}))
        caso('R3 ... el <failure> sigue rojo con --partial', True,
             reg_xml('r3_failure_p', [('S', 'A', MARC), ('S', 'B', 'failure')], {'t': {'S.A', 'S.B'}}, parcial=True))
        caso('R3 un test parametrizado y uno typed con su XML -> verde', False,
             reg_xml('r3_param', [('S', 'A', MARC), ('Inst/P', 'T/0'), ('Ty/1', 'U')],
                     {'t': {'S.A', 'Inst/P.T/0', 'Ty/1.U'}}))
        caso('R3 ... y el typed sin XML -> rojo', True,
             reg_xml('r3_param_no', [('S', 'A', MARC), ('Inst/P', 'T/0')],
                     {'t': {'S.A', 'Inst/P.T/0', 'Ty/1.U'}}))
        eq_lista = parsear_lista(
            'Running main() from gtest_main.cc\nS.\n  A\n  B\nInst/P.\n  T/0  # GetParam() = 1\n  T/1  # GetParam() = 2\n'
            'Ty/0.  # TypeParam = int\n  U\nTy/1.  # TypeParam = float\n  U\n')
        caso('R3 el parser de --gtest_list_tests: suites, parametrizadas y typed', False,
             [] if eq_lista == {'S.A', 'S.B', 'Inst/P.T/0', 'Inst/P.T/1', 'Ty/0.U', 'Ty/1.U'} else [repr(sorted(eq_lista))])

        def lista_con(nombre, salidas, include_extra=''):
            base = os.path.join(tmp, nombre)
            for t, out in salidas.items():
                exe = os.path.join(base, t, t + '.exe')
                _w(base, '%s/%s[1]_include.cmake' % (t, t),
                   'gtest_discover_tests_impl(\n  TEST_EXECUTABLE [==[%s]==]\n  TEST_WORKING_DIR [==[%s]==]\n'
                   '  TEST_EXTRA_ARGS [==[]==]\n  TEST_FILTER [==[%s]==]\n)\n' % (exe.replace(os.sep, '/'), os.path.dirname(exe).replace(os.sep, '/'), include_extra))
            return base

        def ejecutor(salidas):
            def f(exe, cwd):
                s = salidas[os.path.basename(exe)[:-4]]
                return (None, s[1]) if isinstance(s, tuple) else (s, None)
            return f
        b = lista_con('lista_ok', {'t': ''})
        regs, errs = registrados_de(b, ejecutor({'t': 'S.\n  A\n'}))
        caso('R3 registrados_de: un exe que lista -> verde', False, errs + ([] if regs == {'t': {'S.A'}} else ['no listo S.A']))
        regs, errs = registrados_de(b, ejecutor({'t': ('', 'no pude ejecutar t.exe (WinError 4551): Smart App Control')}))
        caso('R3 un exe que el SO no deja listar (Smart App Control) -> rojo con el mensaje', True,
             errs if errs and 'Smart App Control' in errs[0] else [])
        b = lista_con('lista_filtro', {'t': ''}, include_extra='-S.*')
        regs, errs = registrados_de(b, ejecutor({'t': 'S.\n  A\n'}))
        caso('R3 gtest_discover_tests con TEST_FILTER no vacio -> rojo', True, errs)
        base = os.path.join(tmp, 'lista_vacia'); os.makedirs(base)
        caso('R3 un build sin ningun *_include.cmake -> rojo', True, registrados_de(base, ejecutor({}))[1])

        # R1 / R2
        def resultados(nombre, casos, listado='S.A | r\n', parcial=False):
            base = os.path.join(tmp, nombre)
            _xml(base, 'x.xml', casos)
            lst = _w(base, 'l.txt', listado)
            return chequeo_resultados(base, lst, parcial)[0]

        marcado = MARCADOR + ' razon'
        amb = 'S.A | r\nambiente | sin corpus grabado | no se bajo el corpus\n'
        caso('R1 el saltado con marcador == listado -> verde', False, resultados('r1_ok', [('S', 'A', marcado)]))
        caso('R1 uno del listado que CORRIO -> rojo', True, resultados('r1_corre', [('S', 'A')]))
        caso('R1 uno marcado que no esta en el listado -> rojo', True,
             resultados('r1_extra', [('S', 'A', marcado), ('S', 'B', marcado)]))
        caso('R1 uno del listado que no aparece -> rojo', True, resultados('r1_falta', [('S', 'Z')]))
        caso('R1 ... pero con --partial se tolera -> verde', False,
             resultados('r1_parcial', [('S', 'Z')], listado='S.A | r\n', parcial=True))
        caso('R2 skip de ambiente DECLARADO (prefijo) -> verde', False,
             resultados('r2_ok', [('S', 'A', marcado), ('S', 'C', 'sin corpus grabado (ausente)')], listado=amb))
        caso('R2 skip SIN declarar -> rojo', True,
             resultados('r2_nodecl', [('S', 'A', marcado), ('S', 'C', 'pendiente')], listado=amb))
        caso('R2 skip cuyo mensaje solo CONTIENE el prefijo (no empieza) -> rojo', True,
             resultados('r2_contiene', [('S', 'A', marcado), ('S', 'C', 'ojo: sin corpus grabado')], listado=amb))
        caso('R2 skip SIN mensaje -> rojo', True,
             resultados('r2_mudo', [('S', 'A', marcado), ('S', 'C', '')], listado=amb))
        caso('R2 prefijo declarado que no aparece en la corrida completa -> rojo (sobra)', True,
             resultados('r2_sobra', [('S', 'A', marcado)], listado=amb))
        caso('R2 ... pero con --partial se tolera -> verde', False,
             resultados('r2_sobra_p', [('S', 'A', marcado)], listado=amb, parcial=True))
        caso('R2 un skip no declarado sigue rojo con --partial', True,
             resultados('r2_nodecl_p', [('S', 'A', marcado), ('S', 'C', 'pendiente')], listado=amb, parcial=True))
        base = os.path.join(tmp, 'r_lista')
        _w(base, 'listado.xml', '<testsuites><testsuite name="S"><testcase name="A"/></testsuite></testsuites>')
        caso('R el XML de discovery (sin status) no cuenta como corrida -> rojo si es lo unico',
             True, chequeo_resultados(base, _w(base, 'l.txt', 'S.A | r\n'), False)[0])
        base = os.path.join(tmp, 'r_vacio'); os.makedirs(base)
        caso('R sin XML -> rojo', True, chequeo_resultados(base, _w(base, 'l.txt', ''), False)[0])
    return 0 if not fallos else 3


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--results', metavar='DIR', help='directorio con los XML de gtest de una corrida en Windows')
    ap.add_argument('--build-dir', metavar='DIR', help='el build de la suite (con los *_include.cmake de gtest): de ahi sale '
                    'que exes hay y que tests registra cada uno; obligatorio con --results')
    ap.add_argument('--partial', action='store_true', help='la corrida fue filtrada a proposito (-Filter/-Fast): tolera '
                    'los del listado ausentes y los registrados sin XML')
    args = ap.parse_args()

    if args.self_test:
        print('== self-test de check-windows-not-run ==')
        rc = self_test()
        print('self-test OK.' if rc == 0 else 'self-test ROTO.')
        return rc

    errores = chequeo_estatico(CPP, LISTADO)
    notas = []
    if args.results:
        if not args.build_dir:
            print('check-windows-not-run: --results necesita --build-dir (sin el no se sabe que tests registra cada exe)',
                  file=sys.stderr)
            return 2
        regs, e_reg = registrados_de(args.build_dir)
        errores += e_reg
        e, notas = chequeo_resultados(args.results, LISTADO, args.partial, regs)
        errores += [x for x in e if x not in errores]
        notas.append('registrados: ' + ', '.join('%s=%d' % (t, len(regs[t])) for t in sorted(regs)))
        filas = resumen_por_ejecutable(args.results)
        print('  %-28s %7s %6s %6s' % ('ejecutable', 'pass', 'fail', 'skip'))
        for exe in sorted(filas):
            print('  %-28s %7d %6d %6d' % ((exe,) + tuple(filas[exe])))
        print('  %-28s %7d %6d %6d' % (('TOTAL',) + tuple(sum(c) for c in zip(*filas.values()))
                                         if filas else ('TOTAL', 0, 0, 0)))
    for n in notas:
        print('  ' + n)
    if errores:
        print('check-windows-not-run: FALLA\n', file=sys.stderr)
        for e in errores:
            print('  ' + e, file=sys.stderr)
        return 1
    lst = leer_listado(LISTADO)
    print('check-windows-not-run: OK (%d declarados, %d variantes, %d de ambiente, %d cmake, %d fuera, %d deshabilitados, '
          '%d solo-windows%s)' % (
        len(lst.no_run), len(lst['variante']), len(lst['ambiente']), lst.cuenta('cmake'), lst.cuenta('fuera'),
        len(lst['deshabilitado']), len(lst['solo-windows']),
        ', resultados verificados' if args.results else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
