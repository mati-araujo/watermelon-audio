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
  E3  ningun CMake de tests excluye fuentes ni targets de test (list(FILTER|REMOVE_ITEM|REMOVE_AT)), ni
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
TIPOS = ('variante', 'ambiente', 'cmake', 'solo-windows', 'fuera', 'deshabilitado')
HALLAZGOS = ('cmake', 'fuera')   # `tipo | ruta | clave | razon`: una entrada por hallazgo

PLATFORM = re.compile(
    r'\b(_WIN32|WIN32|__WIN32__|_WIN64|__MINGW32__|__MINGW64__|__MINGW__|_MSC_VER|__CYGWIN__|WINAPI_FAMILY\w*|'
    r'__APPLE__|__MACH__|TARGET_OS_\w+|GTEST_OS_\w+|'
    r'__linux__|__linux|linux|__gnu_linux__|__unix__|__unix|unix|__ANDROID__|__ANDROID_API__|'
    r'__FreeBSD__|__NetBSD__|__OpenBSD__|__sun|__EMSCRIPTEN__|__QNX__|__has_include)\b')
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
                                   % (base, no, cabeza, 'construccion' if cabeza == 'cmake' else 'condicion', raw))
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

CM_PLATFORM = re.compile(r'\b(WIN32|UNIX|APPLE|MINGW|MSVC|CYGWIN|ANDROID|IOS|LINUX|EMSCRIPTEN|CMAKE_(HOST_)?SYSTEM\w*|'
                         r'CMAKE_HOST_WIN32|CMAKE_HOST_UNIX|CMAKE_HOST_APPLE)\b')
CM_SRC = re.compile(r'[\w./${}-]+\.(?:cpp|cc|c)\b')
CM_TARGET = ('add_executable', 'add_test', 'gtest_discover_tests', 'wma_gtest_discover_tests', 'add_subdirectory')
CM_LISTA = ('FILTER', 'REMOVE_ITEM', 'REMOVE_AT')
CM_FILTRO = re.compile(r'\bDISABLED(_\w*)?\b|\bTEST_FILTER\b|gtest_filter|GTEST_FILTER', re.IGNORECASE)
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
    for dirpath, path in cm_cmake_files(root):
        if not re.search(r'[/\\]tests?([/\\]|$)', dirpath + os.sep):
            continue
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
            if rotulo:
                _plataforma(nombre, args, hallazgo, dirpath, solo, solo_usados)
            if msgs:
                hallados.append((rel, clave, '%s:%d' % (rel, no), '; '.join(msgs)))
    return hallados, solo_usados


def _plataforma(nombre, args, hallazgo, dirpath, solo, solo_usados):
    """Lo que no puede estar bajo un if() de plataforma en un CMake de tests."""
    if nombre == 'return':
        hallazgo('return() bajo una condicion de plataforma corta la configuracion de los tests')
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


def chequeo_resultados(directorio, listado, parcial):
    lst = leer_listado(listado)
    errores, notas = list(lst.errores), []
    saltos, vistos, n_xml = saltos_de_xml(directorio)
    if n_xml == 0:
        return ['no hay XML de gtest en %s: la corrida no los genero' % directorio], notas
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
        caso('E3 un set() y un target_compile_definitions bajo if(WIN32) -> verde', False,
             estatico('e3_defs', cmake='if(WIN32)\n  set(X 1)\n  target_compile_definitions(t PRIVATE A=1)\nendif()\n'))
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
    ap.add_argument('--partial', action='store_true', help='la corrida fue filtrada: tolera los del listado ausentes')
    args = ap.parse_args()

    if args.self_test:
        print('== self-test de check-windows-not-run ==')
        rc = self_test()
        print('self-test OK.' if rc == 0 else 'self-test ROTO.')
        return rc

    errores = chequeo_estatico(CPP, LISTADO)
    notas = []
    if args.results:
        e, notas = chequeo_resultados(args.results, LISTADO, args.partial)
        errores += [x for x in e if x not in errores]
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
