#!/usr/bin/env python3
"""REQ-047 S2 — el artefacto que se publica, diffeado contra una version del registro.

POR QUE EXISTE
--------------
Un bump de build (AGP, NDK, Oboe, compileSdk) puede cambiar lo que recibe el
consumidor SIN tocar una linea de codigo y con todos los gates en verde: una
entrada de mas en el AAR, un `.so` alineado a 4 KB en vez de 16 KB, un
`minCompileSdk` que sube, una dependencia transitiva que cambia de version en el
POM, un `libc++_shared.so` que pasa a pedir otra biblioteca. Nada de eso lo ve
un test: lo ve comparar el artefacto.

Este script compara lo que `publishToMavenLocal` dejo en `~/.m2` contra una
version ya publicada en GitHub Packages, HECHO POR HECHO, y exige que cada
diferencia este DECLARADA CON SU VALOR. Lo usan S2, S3 y el cierre de REQ-047.

    python3 scripts/diff-published-artifact.py --self-test
    ./gradlew :audio:publishToMavenLocal -Pversion=2.21.0-local
    GPR_KEY=... python3 scripts/diff-published-artifact.py \\
        --against 2.21.0 --local-version 2.21.0-local \\
        --expect-file scripts/published-artifact-vs-2.21.0.txt
    # para (re)declarar: imprime lo observado en la sintaxis del archivo
    python3 scripts/diff-published-artifact.py --against 2.21.0 ... --print-declarations

QUE COMPARA
-----------
Las CUATRO publicaciones (`audio`, `audio-android`, `audio-iosarm64`,
`audio-iossimulatorarm64`) son obligatorias; si falta una, no hay veredicto.
  - POM: packaging y cada dependencia —de `<dependencies>` y de
    `<dependencyManagement>`—, indexada por grupo:artefacto[:classifier], con
    version, scope, type, optional y sus exclusiones. Una repetida es ilegible.
  - `.module` (Gradle Module Metadata): `formatVersion`, la version de Gradle de
    `createdBy`, el `component` (url, grupo, modulo, atributos) y, por variante,
    sus atributos, capacidades, dependencias (spec de version, excludes,
    atributos y capacidades pedidas), constraints, `available-at` y el nombre y
    la url de cada archivo. Una dependencia o un constraint repetido por
    grupo:modulo en la misma variante es ilegible (no se pisan en silencio), y un
    `.module` sin variantes tambien. No compara tamaños ni hashes de archivos:
    eso lo hace el AAR, hecho por hecho.
Y del AAR de `audio-android`:
  - el conjunto de entradas del zip;
  - `aar-metadata.properties`, clave por clave;
  - el hash del contenido de cada archivo de texto (manifest, R.txt, proguard...)
    y de cada binario que no es `.so` ni `.jar`;
  - de cada `.jar` interno: el conjunto de clases, el hash de cada entrada que no
    es clase (el `.kotlin_module`, que describe la superficie Kotlin) y el
    conjunto de "major versions" del bytecode (el JDK minimo que exige);
  - de cada `.so`: la alineacion minima de sus PT_LOAD, la seccion `.comment`
    (el compilador), las `DT_NEEDED` (de que bibliotecas depende), el `DT_SONAME`
    y —salvo el nuestro, ver abajo— el hash de sus BYTES. El ELF se lee aca, en
    Python: no depende de llvm-readelf ni del NDK.
El TAMAÑO se imprime siempre, entrada por entrada, en bytes. El del AAR entero
es una diferencia (`aar/size`) si se mueve mas del 5 % (el no funcional de
REQ-047), y lo que entra al trinquete es QUE se cruzo el umbral y en que
sentido, no los bytes: `aar/size = referencia -> crece-mas-del-5%` (o
`achica-mas-del-5%`). Los bytes no son un hecho reproducible, por la misma razon
que los del `.so` de abajo: publicar el MISMO arbol con otra version movio el
AAR 48 bytes (REQ-047 S3), asi que una linea en bytes quedaba atada a un publish.

La VERSION se normaliza en todo lo que es texto (nombres de archivo, POM,
`.module`, referencias entre nuestras publicaciones).

LOS BYTES DE `libwatermelon_audio.so` SE IMPRIMEN, NO SE DECLARAN
-----------------------------------------------------------------
Su hash no es un hecho reproducible: el `.so` embebe la version publicada como
string (otro `-Pversion`, otros bytes) y su `.note.gnu.build-id` hashea el
binario antes del strip, con la info de debug que lleva la RUTA del arbol donde
se compilo (medido en REQ-047 S2: la misma fuente en dos worktrees difiere en
esos 20 bytes). Declararlo ataria el trinquete a un publish y a un directorio.
Por eso sale del trinquete: se imprime su hash crudo y el hash con el build-id
en cero. La igualdad de bytes se afirma solo en un diff PAREADO —las dos
construcciones publicadas con la MISMA version— y la afirma QUIEN LEE ese
segundo hash: el instrumento lo imprime, no lo compara. Para parear, se copian
los archivos de una construccion a un directorio plano y se lo usa de base:
`--against V --cache <dir> --local-version V` (`--against-local V` con la misma
V leeria el mismo directorio de los dos lados). Su alineacion, `.comment`,
`DT_NEEDED` y `DT_SONAME` siguen en el trinquete, y el hash de `liboboe.so` y
`libc++_shared.so` tambien.

LO QUE NO ABRE
--------------
Los klibs de iOS (`.klib`) y el `.jar` de metadata comun NO se abren: de esas
publicaciones se comparan solo el POM y el `.module`. Una diferencia que viva
adentro de un klib este instrumento no la ve.

DECLARAR NO ES SILENCIAR: ES UN TRINQUETE SOBRE EL VALOR
--------------------------------------------------------
Cada diferencia se declara con sus DOS valores, una por linea de
`--expect-file` (o con `--expect`):

    CLAVE = VALOR_BASE -> VALOR_HEAD   # razon

`(ausente)` es el valor de un hecho que no existe de ese lado y `(vacio)` el de
un hecho que existe con valor vacio. La razon es obligatoria: una linea sin ella
o con el `RAZON` que deja `--print-declarations` se rechaza, para que declarar
no sea copiar lo observado sin revisarlo. Un valor que contiene `#` o `->`, o
que empieza o termina con espacio, no se puede escribir sin ambiguedad:
`--print-declarations` lo rechaza con codigo 2 y la diferencia queda roja.
Una clave declarada dos veces tambien se rechaza. El veredicto es:
  - diferencia NO declarada                   -> ROJO
  - declarada con OTROS valores               -> ROJO (declarar 36 -> 37 no
                                                 acepta 36 -> 99, ni que la
                                                 clave desaparezca)
  - declaracion que NO se reproduce           -> ROJO (declarar de mas tapa un
                                                 cambio futuro; es el mismo
                                                 trinquete que rt-safety-baseline)
Asi una diferencia nueva aparece en el diff del PR que la agrega, no despues.

"NO PUDE BAJAR" / "NO PUDE LEER" NUNCA ES UN PASE
-------------------------------------------------
Una publicacion que falta, un `.so` que no parsea como ELF, un `.class` que no
es bytecode, un AAR sin `aar-metadata.properties` o sin ningun `.so`, un
`.module` sin variantes, una dependencia repetida, o CUALQUIER excepcion al
leer: sale con codigo 2, nunca con 0 ni con 1.

LA CREDENCIAL
-------------
Se lee de `GPR_KEY` (y `GPR_USER`, opcional) en el entorno de ESTE proceso.
Nunca se imprime, nunca se escribe a disco, nunca va en la URL. Lo bajado se
cachea en `build/published-artifacts/<version>/` (ignorado por git).
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import hashlib
import io
import json
import os
import re
import struct
import sys
import tempfile
import traceback
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GROUP = "com.watermellonstudios"
GROUP_PATH = GROUP.replace(".", "/")
REGISTRY = "https://maven.pkg.github.com/mati-araujo/watermelon-audio"
PUBLICATIONS = ["audio", "audio-android", "audio-iosarm64", "audio-iossimulatorarm64"]
AAR_PUBLICATION = "audio-android"
SIZE_TOLERANCE = 0.05
TEXT_SUFFIXES = (".xml", ".txt", ".pro", ".properties", ".json", ".MF")
VERSION_TOKEN = "<VERSION>"
ABSENT = "(ausente)"
EMPTY = "(vacio)"
NO_VERSION = "(sin-version)"
PLACEHOLDER_REASON = "RAZON"
SIZE_KEY = "aar/size"
# Los VALORES de `aar/size` (REQ-047 S4, 4.11): el cruce del umbral y su sentido,
# no los bytes. El porcentaje sale de SIZE_TOLERANCE: si el umbral cambia, una
# declaracion hecha con el viejo deja de coincidir y queda roja.
SIZE_BASE = "referencia"
SIZE_UP = f"crece-mas-del-{round(SIZE_TOLERANCE * 100)}%"
SIZE_DOWN = f"achica-mas-del-{round(SIZE_TOLERANCE * 100)}%"
# El .so cuyos BYTES no son un hecho reproducible (ver el docstring): su hash se
# imprime, no entra al trinquete.
UNPINNED_SO = "libwatermelon_audio.so"


class ReadError(Exception):
    """Algo que no se pudo leer o bajar. Nunca se convierte en un pase."""


def sha(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()[:16]


# =============================================================================
# ELF: PT_LOAD, .comment, DT_NEEDED y DT_SONAME, de 32 y 64 bits, little endian.
# =============================================================================

PT_LOAD = 1
PT_DYNAMIC = 2
PT_GNU_STACK = 0x6474E551
SHT_DYNAMIC = 6
DT_NULL, DT_NEEDED, DT_SONAME = 0, 1, 14


def _cstr(blob: bytes, off: int) -> str:
    end = blob.find(b"\0", off)
    if end < 0:
        raise ReadError(f"string sin terminar en offset {off}")
    return blob[off:end].decode("utf-8", "replace")


def elf_facts(data: bytes, where: str) -> dict[str, str]:
    try:
        return _elf_facts(data, where)
    except ReadError:
        raise
    except (struct.error, IndexError, ValueError) as e:
        raise ReadError(f"{where}: ELF ilegible ({e})")


def _elf_facts(data: bytes, where: str) -> dict[str, str]:
    if data[:4] != b"\x7fELF":
        raise ReadError(f"{where}: no es un ELF")
    ei_class, ei_data = data[4], data[5]
    if ei_data != 1:
        raise ReadError(f"{where}: ELF big endian, no soportado")
    if ei_class == 2:
        (e_phoff, e_shoff) = struct.unpack_from("<QQ", data, 0x20)
        (e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHHHH", data, 0x36)
        ph_fmt, sh_fmt, dyn_fmt = "<IIQQQQQQ", "<IIQQQQIIQQ", "<qQ"
    elif ei_class == 1:
        (e_phoff, e_shoff) = struct.unpack_from("<II", data, 0x1C)
        (e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHHHH", data, 0x2A)
        ph_fmt, sh_fmt, dyn_fmt = "<IIIIIIII", "<IIIIIIIIII", "<iI"
    else:
        raise ReadError(f"{where}: EI_CLASS {ei_class} desconocido")

    # p_type es el campo 0 y p_align el 7 en las dos clases.
    aligns = []
    for i in range(e_phnum):
        ph = struct.unpack_from(ph_fmt, data, e_phoff + i * e_phentsize)
        if ph[0] == PT_LOAD:
            aligns.append(ph[7])
    if not aligns:
        raise ReadError(f"{where}: sin PT_LOAD")

    # sh_name, sh_type, sh_offset, sh_size y sh_link son los campos 0, 1, 4, 5 y 6.
    sections = []
    for i in range(e_shnum):
        sh = struct.unpack_from(sh_fmt, data, e_shoff + i * e_shentsize)
        sections.append((sh[0], sh[1], sh[4], sh[5], sh[6]))
    if not sections or e_shstrndx >= len(sections):
        raise ReadError(f"{where}: sin tabla de secciones")
    _n, _t, str_off, str_size, _l = sections[e_shstrndx]
    shstrtab = data[str_off:str_off + str_size]

    comment = ABSENT
    needed: list[str] = []
    soname = ABSENT
    dynamic_seen = False
    no_build_id = bytearray(data)
    for name_off, s_type, off, size, link in sections:
        sname = _cstr(shstrtab, name_off)
        if sname == ".note.gnu.build-id":
            if off + size > len(data):
                raise ReadError(f"{where}: .note.gnu.build-id fuera del archivo")
            no_build_id[off:off + size] = bytes(size)
        if sname == ".comment":
            parts = [p.decode("utf-8", "replace") for p in data[off:off + size].split(b"\0") if p]
            comment = " | ".join(sorted(set(parts)))
        if s_type == SHT_DYNAMIC:
            dynamic_seen = True
            _dn, _dt, dstr_off, dstr_size, _dl = sections[link]
            dynstr = data[dstr_off:dstr_off + dstr_size]
            step = struct.calcsize(dyn_fmt)
            for j in range(size // step):
                tag, val = struct.unpack_from(dyn_fmt, data, off + j * step)
                if tag == DT_NULL:
                    break
                if tag == DT_NEEDED:
                    needed.append(_cstr(dynstr, val))
                elif tag == DT_SONAME:
                    soname = _cstr(dynstr, val)
    if not dynamic_seen:
        raise ReadError(f"{where}: sin seccion dinamica — no es una biblioteca compartida")
    return {
        "pt_load_align_min": str(min(aligns)),
        "comment": comment,
        "needed": ",".join(sorted(needed)) or "(ninguna)",
        "soname": soname,
        "sha": sha(data),
        "sha-sin-build-id": sha(bytes(no_build_id)),
    }


# =============================================================================
# Extraccion de hechos. Cada hecho es CLAVE -> valor (str). Un conjunto se
# expande a una clave por miembro con valor "presente".
# =============================================================================

def normalize(text: str, version: str) -> str:
    return text.replace(version, VERSION_TOKEN)


POM_NS = {"m": "http://maven.apache.org/POM/4.0.0"}


def pom_facts(raw: bytes, artifact: str, version: str) -> dict[str, str]:
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as e:
        raise ReadError(f"POM de {artifact}: {e}")
    ns = POM_NS
    facts: dict[str, str] = {}
    pref = f"pom:{artifact}"
    facts[f"{pref}/packaging"] = root.findtext("m:packaging", "jar", ns)
    # `<dependencyManagement>` fija versiones de transitivas (un `platform()` o un
    # BOM lo escribe ahi): cambia lo que resuelve el consumidor aunque
    # `<dependencies>` no se mueva.
    for kind, path in (("dep", "m:dependencies/m:dependency"),
                       ("depMgmt", "m:dependencyManagement/m:dependencies/m:dependency")):
        for d in root.findall(path, ns):
            g = d.findtext("m:groupId", "", ns)
            a = d.findtext("m:artifactId", "", ns)
            cl = d.findtext("m:classifier", "", ns)
            ga = f"{g}:{a}" + (f":{cl}" if cl else "")
            excl = sorted(f"{e.findtext('m:groupId', '', ns)}:{e.findtext('m:artifactId', '', ns)}"
                          for e in d.findall("m:exclusions/m:exclusion", ns))
            # Sin <version> (la resuelve un BOM de <dependencyManagement>): un token,
            # no un valor que empieza con espacio y no se podria declarar.
            value = (f"{normalize(d.findtext('m:version', '', ns), version) or NO_VERSION}"
                     f" scope={d.findtext('m:scope', 'compile', ns)}"
                     f" type={d.findtext('m:type', 'jar', ns)}"
                     f" optional={d.findtext('m:optional', 'false', ns)}"
                     f" excl=[{','.join(excl)}]")
            key = f"{pref}/{kind}:{ga}"
            if key in facts:
                raise ReadError(f"POM de {artifact}: {kind} repetida {ga}")
            facts[key] = value
    return facts


def _kv(d: dict | None, version: str) -> str:
    if not d:
        return ""
    return ",".join(f"{k}={normalize(str(d[k]), version)}" for k in sorted(d))


def _caps(caps: list | None, version: str) -> str:
    return ",".join(sorted(normalize(f"{c.get('group')}:{c.get('name')}:{c.get('version', '')}", version)
                           for c in caps or []))


def _dep_value(d: dict, version: str) -> str:
    excl = sorted(f"{e.get('group', '*')}:{e.get('module', '*')}" for e in d.get("excludes", []))
    parts = [f"version[{_kv(d.get('version'), version)}]", f"excl[{','.join(excl)}]"]
    if d.get("attributes"):
        parts.append(f"attrs[{_kv(d['attributes'], version)}]")
    if d.get("requestedCapabilities"):
        parts.append(f"caps[{_caps(d['requestedCapabilities'], version)}]")
    if d.get("endorseStrictVersions"):
        parts.append("endorseStrict")
    return " ".join(parts)


def module_facts(raw: bytes, artifact: str, version: str) -> dict[str, str]:
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as e:
        raise ReadError(f".module de {artifact}: {e}")
    facts: dict[str, str] = {}
    pref = f"module:{artifact}"
    facts[f"{pref}/formatVersion"] = str(doc.get("formatVersion"))
    facts[f"{pref}/createdBy.gradle"] = str(doc.get("createdBy", {}).get("gradle", {}).get("version"))
    comp = doc.get("component", {})
    facts[f"{pref}/component"] = normalize(
        f"{comp.get('group')}:{comp.get('module')}:{comp.get('version')} url={comp.get('url', '')}", version)
    facts[f"{pref}/component.attrs"] = _kv(comp.get("attributes"), version)
    variants = doc.get("variants", [])
    if not variants:
        raise ReadError(f".module de {artifact}: sin variantes")
    for var in variants:
        vp = f"{pref}/variant:{var['name']}"
        facts[vp] = "presente"
        for k, v in sorted(var.get("attributes", {}).items()):
            facts[f"{vp}/attr:{k}"] = str(v)
        if var.get("capabilities"):
            facts[f"{vp}/capabilities"] = _caps(var["capabilities"], version)
        # Indexadas por grupo:modulo, igual que el POM por GA: una repetida no se
        # PISA en silencio (la segunda taparia a la primera y el diff miraria una
        # sola), se rechaza.
        for kind, field in (("dep", "dependencies"), ("constraint", "dependencyConstraints")):
            for d in var.get(field, []):
                key = f"{vp}/{kind}:{d['group']}:{d['module']}"
                if key in facts:
                    raise ReadError(f".module de {artifact}: {kind} repetida {d['group']}:{d['module']} "
                                    f"en la variante {var['name']}")
                facts[key] = _dep_value(d, version)
        if "available-at" in var:
            at = var["available-at"]
            facts[f"{vp}/available-at"] = normalize(
                f"{at['group']}:{at['module']}:{at['version']} url={at.get('url', '')}", version)
        for f in var.get("files", []):
            facts[f"{vp}/file:{normalize(f['name'], version)}"] = normalize(f"url={f.get('url', '')}", version)
    return facts


def jar_facts(data: bytes, where: str) -> dict[str, str]:
    facts: dict[str, str] = {}
    majors: set[int] = set()
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for n in sorted(z.namelist()):
                if n.endswith("/"):
                    continue
                blob = z.read(n)
                if n.endswith(".class"):
                    facts[f"aar/class:{where}!{n}"] = "presente"
                    if blob[:4] != b"\xca\xfe\xba\xbe" or len(blob) < 8:
                        raise ReadError(f"{where}!{n}: no es un .class")
                    majors.add(struct.unpack_from(">H", blob, 6)[0])
                else:
                    facts[f"aar/jar-entry:{where}!{n}"] = sha(blob)
    except zipfile.BadZipFile as e:
        raise ReadError(f"{where}: {e}")
    if majors:
        facts[f"aar/class-major:{where}"] = ",".join(str(m) for m in sorted(majors))
    return facts


def aar_facts(raw: bytes, version: str) -> tuple[dict[str, str], dict[str, int], dict[str, str]]:
    """(hechos, tamaños, info). `info` se imprime y NO se compara: los bytes de
    UNPINNED_SO."""
    facts: dict[str, str] = {}
    info: dict[str, str] = {}
    sizes: dict[str, int] = {"(AAR entero)": len(raw)}
    try:
        z = zipfile.ZipFile(io.BytesIO(raw))
    except zipfile.BadZipFile as e:
        raise ReadError(f"AAR: {e}")
    names = sorted(n for n in z.namelist() if not n.endswith("/"))
    if not names:
        raise ReadError("AAR vacio")
    meta_seen = False
    so_seen = 0
    for n in names:
        data = z.read(n)
        sizes[n] = len(data)
        facts[f"aar/entry:{n}"] = "presente"
        if n.endswith("aar-metadata.properties"):
            meta_seen = True
            for line in data.decode("utf-8").splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                facts[f"aar/metadata:{k.strip()}"] = v.strip()
        elif n.endswith(".so"):
            so_seen += 1
            ef = elf_facts(data, n)
            no_build_id = ef.pop("sha-sin-build-id")
            if n.rsplit("/", 1)[-1] == UNPINNED_SO:
                info[n] = f"{ef.pop('sha')} (con el build-id en cero: {no_build_id})"
            for k, v in ef.items():
                facts[f"aar/so:{n}/{k}"] = v
        elif n.endswith(".jar"):
            facts.update(jar_facts(data, n))
        elif n.endswith(TEXT_SUFFIXES):
            text = normalize(data.decode("utf-8", "replace"), version)
            facts[f"aar/text:{n}"] = sha(text.encode())
        else:
            facts[f"aar/blob:{n}"] = sha(data)
    if not meta_seen:
        raise ReadError("AAR sin aar-metadata.properties")
    if so_seen == 0:
        raise ReadError("AAR sin ningun .so: este motor es nativo, un AAR asi no es el nuestro")
    return facts, sizes, info


# =============================================================================
# De donde salen los bytes
# =============================================================================

class Source:
    def __init__(self, label: str, version: str):
        self.label, self.version = label, version

    def get(self, artifact: str, ext: str) -> bytes:  # pragma: no cover - abstracta
        raise NotImplementedError


class LocalM2(Source):
    def __init__(self, root: Path, version: str):
        super().__init__(f"~/.m2 ({version})", version)
        self.root = root

    def get(self, artifact: str, ext: str) -> bytes:
        p = self.root / GROUP_PATH / artifact / self.version / f"{artifact}-{self.version}.{ext}"
        if not p.is_file():
            raise ReadError(f"falta {p} — ¿corriste publishToMavenLocal con esa version?")
        return p.read_bytes()


class Registry(Source):
    def __init__(self, version: str, cache: Path):
        super().__init__(f"registro ({version})", version)
        self.cache = cache
        key = os.environ.get("GPR_KEY", "")
        user = os.environ.get("GPR_USER", "") or "token"
        self._auth = ("Basic " + base64.b64encode(f"{user}:{key}".encode()).decode()) if key else None

    def get(self, artifact: str, ext: str) -> bytes:
        name = f"{artifact}-{self.version}.{ext}"
        cached = self.cache / name
        if cached.is_file():
            return cached.read_bytes()
        if not self._auth:
            raise ReadError(f"{name} no esta en cache y no hay GPR_KEY en el entorno")
        url = f"{REGISTRY}/{GROUP_PATH}/{artifact}/{self.version}/{name}"
        req = urllib.request.Request(url)
        # UNREDIRECTED a proposito: el registro redirige a un almacenamiento de
        # objetos en OTRO host con la URL ya firmada. Una cabecera comun viajaria
        # en ese redirect y le entregaria la credencial a un tercero.
        req.add_unredirected_header("Authorization", self._auth)
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
        except urllib.error.HTTPError as e:
            # Sin la cabecera: el mensaje no puede llevar la credencial.
            raise ReadError(f"{name}: HTTP {e.code} del registro")
        except urllib.error.URLError as e:
            raise ReadError(f"{name}: {e.reason}")
        self.cache.mkdir(parents=True, exist_ok=True)
        tmp = cached.with_suffix(cached.suffix + ".part")
        tmp.write_bytes(data)
        tmp.replace(cached)
        return data


class DirSource(Source):
    """Para el self-test: <dir>/<artifact>-<version>.<ext>."""

    def __init__(self, root: Path, version: str, label: str):
        super().__init__(label, version)
        self.root = root

    def get(self, artifact: str, ext: str) -> bytes:
        p = self.root / f"{artifact}-{self.version}.{ext}"
        if not p.is_file():
            raise ReadError(f"falta {p}")
        return p.read_bytes()


def collect(src: Source) -> tuple[dict[str, str], dict[str, int], dict[str, str]]:
    """Las CUATRO publicaciones, siempre: no hay forma de pedir menos.

    Que una publicacion aporte hechos no se chequea aparte porque no puede no
    hacerlo: `src.get` falla si falta el POM o el `.module`, el POM aporta siempre
    su `packaging` y el `.module` su `formatVersion` y al menos una variante (sin
    variantes es ReadError). Un guarda de "cero hechos" era codigo muerto (REQ-047
    S2, R8): no podia fallar, y un guarda que no puede fallar se lee como cobertura.
    """
    facts: dict[str, str] = {}
    sizes: dict[str, int] = {}
    info: dict[str, str] = {}
    for art in PUBLICATIONS:
        facts.update(pom_facts(src.get(art, "pom"), art, src.version))
        facts.update(module_facts(src.get(art, "module"), art, src.version))
        if art == AAR_PUBLICATION:
            f, s, i = aar_facts(src.get(art, "aar"), src.version)
            facts.update(f)
            sizes.update(s)
            info.update(i)
    return facts, sizes, info


# =============================================================================
# El diff y el veredicto
# =============================================================================

def diff_facts(a: dict[str, str], b: dict[str, str]) -> list[tuple[str, str, str]]:
    out = []
    for k in sorted(set(a) | set(b)):
        va, vb = a.get(k, ABSENT), b.get(k, ABSENT)
        if va != vb:
            out.append((k, va, vb))
    return out


def size_diff(sa: dict[str, int], sb: dict[str, int]) -> tuple[list[tuple[str, int, int]], tuple | None]:
    rows = [(k, sa.get(k, 0), sb.get(k, 0)) for k in sorted(set(sa) | set(sb))
            if k.endswith((".so", ".jar")) or k == "(AAR entero)"]
    total_a, total_b = sa.get("(AAR entero)", 0), sb.get("(AAR entero)", 0)
    if total_a <= 0 or total_b <= 0:
        raise ReadError("tamaño del AAR ilegible")
    if abs(total_b - total_a) / total_a > SIZE_TOLERANCE:
        return rows, (SIZE_KEY, SIZE_BASE, SIZE_UP if total_b > total_a else SIZE_DOWN)
    return rows, None


def print_size_verdict(rows, diffs) -> None:
    """Los bytes del AAR y el cruce del umbral, en claro: el trinquete ve el cruce, quien
    lee ve los bytes. El cruce se toma de `diffs` (lo que vio size_diff), no se recalcula:
    dos copias del umbral podrian decir cosas distintas."""
    a, b = next((a, b) for k, a, b in rows if k == "(AAR entero)")
    pct = (b - a) / a * 100
    cruce = next((vb for k, _va, vb in diffs if k == SIZE_KEY), "dentro del umbral")
    print(f"\n{SIZE_KEY}: {a} -> {b} bytes ({pct:+.2f} %), umbral {SIZE_TOLERANCE:.0%}: {cruce}")


ARROW = " -> "


def declarable_value(v: str) -> bool:
    """Si un valor observado se puede escribir en una declaracion y leer de vuelta
    igual. El vacio se escribe EMPTY; un valor que ES literalmente EMPTY no."""
    if v == "":
        return True
    return (v == v.strip() and "#" not in v and "->" not in v and v != EMPTY
            and "\n" not in v and "\r" not in v)


def written_value_ok(raw: str) -> bool:
    """Si un valor tal como esta ESCRITO en la declaracion es legible."""
    return raw == EMPTY or (raw != "" and declarable_value(raw))


def declarable_key(k: str) -> bool:
    return bool(k) and not any(c.isspace() for c in k) and "#" not in k


def encode_value(v: str) -> str:
    return EMPTY if v == "" else v


def decode_value(v: str) -> str:
    return "" if v == EMPTY else v


def declaration_lines(diffs) -> list[str]:
    """Lo observado en la sintaxis de --expect-file, con la razon por escribir.

    Un valor que no se puede escribir sin ambiguedad NO se imprime mal: es
    ReadError (codigo 2), y la diferencia queda sin declarar, o sea roja."""
    bad = [k for k, va, vb in diffs
           if not declarable_key(k) or not (declarable_value(va) and declarable_value(vb))]
    if bad:
        raise ReadError("no se puede declarar sin ambiguedad (clave con espacio o '#', o valor con "
                        f"'#', '->', espacio en un extremo o igual a {EMPTY}): {bad}")
    return [f"{k} = {encode_value(va)}{ARROW}{encode_value(vb)}   # {PLACEHOLDER_REASON}" for k, va, vb in diffs]


def parse_declaration(line: str) -> tuple[str, str, str, str] | None:
    body, _, reason = line.partition(" # ")
    body = body.strip()
    if not body or body.startswith("#"):
        return None
    key, eq, rest = body.partition(" = ")
    if not eq or not declarable_key(key):
        raise ReadError(f"declaracion sin la forma 'CLAVE = BASE -> HEAD': {line.strip()!r}")
    values = rest.split(ARROW)
    if len(values) != 2 or not all(written_value_ok(v) for v in values):
        raise ReadError(f"declaracion ambigua: tiene que haber exactamente un ' -> ', ningun valor puede "
                        f"llevar '#' ni '->' ni espacios en los extremos, y el vacio se escribe {EMPTY}: "
                        f"{line.strip()!r}")
    reason = reason.strip()
    if not reason or reason == PLACEHOLDER_REASON:
        raise ReadError(f"declaracion sin razon (o con el '{PLACEHOLDER_REASON}' de --print-declarations): "
                        f"{line.strip()!r}")
    return key, decode_value(values[0]), decode_value(values[1]), reason


def read_expectations(expect: list[str] | None, expect_file: str | None) -> dict[str, tuple[str, str, str]]:
    exp: dict[str, tuple[str, str, str]] = {}
    lines = list(expect or [])
    if expect_file:
        lines += Path(expect_file).read_text(encoding="utf-8").splitlines()
    for line in lines:
        d = parse_declaration(line)
        if d is None:
            continue
        key, base, head, reason = d
        if key in exp:
            raise ReadError(f"clave declarada dos veces: {key}")
        exp[key] = (base, head, reason)
    return exp


def verdict(diffs, expectations: dict[str, tuple[str, str, str]], quiet=False) -> tuple[int, dict]:
    """Devuelve (rc, detalle). detalle = {undeclared, wrong_value, unobserved} por clave."""
    observed = {k: (va, vb) for k, va, vb in diffs}
    undeclared = sorted(k for k in observed if k not in expectations)
    wrong_value = sorted(k for k in observed
                         if k in expectations and expectations[k][:2] != observed[k])
    unobserved = sorted(k for k in expectations if k not in observed)
    if not quiet:
        print(f"\ndiferencias: {len(observed)}  |  declaradas: {len(expectations)}")
        for k, (va, vb) in observed.items():
            if k in undeclared:
                mark = "NO DECLARADA"
            elif k in wrong_value:
                b, h, _ = expectations[k]
                mark = f"DECLARADA CON OTRO VALOR ({encode_value(b)} -> {encode_value(h)})"
            else:
                mark = "declarada"
            print(f"  [{mark}] {k}\n      {encode_value(va)}  ->  {encode_value(vb)}")
        for k in unobserved:
            print(f"  [DECLARADA Y NO SE REPRODUCE] {k}  # {expectations[k][2]}")
    detail = {"undeclared": undeclared, "wrong_value": wrong_value, "unobserved": unobserved}
    if undeclared or wrong_value or unobserved:
        if not quiet:
            print(f"\n\033[31mROJO\033[0m — {len(undeclared)} sin declarar, {len(wrong_value)} con otro valor, "
                  f"{len(unobserved)} declaradas de mas")
        return 1, detail
    if not quiet:
        print("\n\033[32mVERDE\033[0m — cada diferencia esta declarada con su valor y cada declaracion se reproduce")
    return 0, detail


def print_sizes(rows):
    print("\ntamaños (bytes):")
    for k, a, b in rows:
        pct = f"{(b - a) / a * 100:+.2f} %" if a else "nuevo"
        print(f"  {k:<60} {a:>10} -> {b:>10}  {pct}")


def print_unpinned(ia: dict[str, str], ib: dict[str, str]) -> None:
    print(f"\nbytes de {UNPINNED_SO} (se IMPRIMEN, no se declaran: embeben la version y el build-id "
          "depende de la ruta; la igualdad se afirma solo en un diff pareado de MISMA version, con el "
          "build-id en cero):")
    for k in sorted(set(ia) | set(ib)):
        print(f"  {k}\n      base {ia.get(k, ABSENT)}\n      head {ib.get(k, ABSENT)}")


class Observation:
    def __init__(self, diffs, rows, na, nb, info_a, info_b):
        self.diffs, self.rows, self.na, self.nb = diffs, rows, na, nb
        self.info_a, self.info_b = info_a, info_b


def observe(base: Source, head: Source) -> Observation:
    fa, sa, ia = collect(base)
    fb, sb, ib = collect(head)
    diffs = diff_facts(fa, fb)
    rows, size_row = size_diff(sa, sb)
    if size_row:
        diffs.append(size_row)
    return Observation(diffs, rows, len(fa), len(fb), ia, ib)


def compare(base: Source, head: Source, expectations, quiet=False) -> tuple[int, dict]:
    o = observe(base, head)
    if not quiet:
        print(f"base: {base.label}  —  {o.na} hechos")
        print(f"head: {head.label}  —  {o.nb} hechos")
        print_sizes(o.rows)
        print_size_verdict(o.rows, o.diffs)
        print_unpinned(o.info_a, o.info_b)
    return verdict(o.diffs, expectations, quiet)


# =============================================================================
# self-test: que el instrumento PUEDE fallar, comparador por comparador
# =============================================================================

def make_elf(bits: int, *, aligns=(16384,), comment: bytes = b"", needed=(), soname: str | None = None,
             pad: int = 0, build_id: bytes | None = None, dynamic_section: bool = True) -> bytes:
    """Un ELF minimo: un PT_LOAD por cada valor de `aligns` ENTRE un PT_GNU_STACK
    (p_align 0) y un PT_DYNAMIC (p_align 8), como los .so reales —asi, contar un
    segmento que no es PT_LOAD cambia el minimo—; .comment, .dynstr, .dynamic
    (salvo `dynamic_section=False`) y, si se pide, .note.gnu.build-id."""
    b64 = bits == 64
    ehsize, phentsize, shentsize = (64, 56, 64) if b64 else (52, 32, 40)
    dyn_fmt = "<qQ" if b64 else "<iI"
    dynstr = b"\0"
    entries = []
    for n in needed:
        entries.append((DT_NEEDED, len(dynstr)))
        dynstr += n.encode() + b"\0"
    if soname is not None:
        entries.append((DT_SONAME, len(dynstr)))
        dynstr += soname.encode() + b"\0"
    entries.append((DT_NULL, 0))
    dynamic = b"".join(struct.pack(dyn_fmt, t, v) for t, v in entries)
    # (nombre, sh_type, sh_flags, contenido, sh_link, sh_entsize). El indice 0 es
    # la seccion nula, asi que .dynstr es la 3 y .dynamic la linkea.
    secs = [(".shstrtab", 3, 0, b"", 0, 0), (".comment", 1, 0x30, comment, 0, 1),
            (".dynstr", 3, 2, dynstr, 0, 0)]
    if dynamic_section:
        secs.append((".dynamic", SHT_DYNAMIC, 3, dynamic, 3, struct.calcsize(dyn_fmt)))
    phdrs = [(PT_GNU_STACK, 0)] + [(PT_LOAD, a) for a in aligns] + [(PT_DYNAMIC, 8)]
    if build_id is not None:
        note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id
        secs.append((".note.gnu.build-id", 7, 2, note, 0, 0))
    shstr = b"\0"
    name_off = {}
    for s in secs:
        name_off[s[0]] = len(shstr)
        shstr += s[0].encode() + b"\0"
    contents = {s[0]: (shstr if s[0] == ".shstrtab" else s[3]) for s in secs}

    cur = ehsize + phentsize * len(phdrs)
    offsets = {}
    payload = b""
    for s in secs:
        pad8 = (-cur) % 8
        payload += bytes(pad8)
        cur += pad8
        offsets[s[0]] = cur
        payload += contents[s[0]]
        cur += len(contents[s[0]])
    payload += bytes(pad)
    cur += pad
    pad8 = (-cur) % 8
    payload += bytes(pad8)
    shoff = cur + pad8
    shnum = len(secs) + 1

    ident = b"\x7fELF" + bytes([2 if b64 else 1, 1, 1]) + bytes(9)
    if b64:
        eh = ident + struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, ehsize, shoff, 0,
                                 ehsize, phentsize, len(phdrs), shentsize, shnum, 1)
        ph = b"".join(struct.pack("<IIQQQQQQ", t, 5, 0, 0, 0, 0x1000, 0x1000, a) for t, a in phdrs)
        shf = "<IIQQQQIIQQ"
    else:
        eh = ident + struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, 0, ehsize, shoff, 0,
                                 ehsize, phentsize, len(phdrs), shentsize, shnum, 1)
        ph = b"".join(struct.pack("<IIIIIIII", t, 0, 0, 0, 0x1000, 0x1000, 5, a) for t, a in phdrs)
        shf = "<IIIIIIIIII"
    sh = bytes(shentsize)
    for name, typ, flags, _c, link, entsize in secs:
        sh += struct.pack(shf, name_off[name], typ, flags, 0, offsets[name], len(contents[name]), link, 0,
                          8 if typ == SHT_DYNAMIC else 1, entsize)
    return eh + ph + payload + sh


def class_bytes(major: int) -> bytes:
    return b"\xca\xfe\xba\xbe" + struct.pack(">HH", 0, major) + bytes(8)


def make_jar(classes: list[str], *, major=61, kotlin_module=b"\x00\x01pkg",
             manifest="Manifest-Version: 1.0\n", bad_class=False) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("META-INF/MANIFEST.MF", manifest)
        z.writestr("META-INF/com.watermellonstudios_audio_release.kotlin_module", kotlin_module)
        for c in classes:
            z.writestr(c, class_bytes(major))
        if bad_class:
            z.writestr("com/watermellonstudios/audio/Rota.class", b"no soy bytecode")
    return buf.getvalue()


CLANG = b"Android (13676358, +pgo, based on r522817d) clang version 18.0.3\0Linker: LLD 18.0.3\0"
CLANG_OTRO = b"Android (16248370, +pgo, based on r574158c) clang version 21.0.0\0Linker: LLD 21.0.0\0"
NEEDED = ["libc++_shared.so", "libc.so", "liblog.so", "liboboe.so"]
CLASSES = ["com/watermellonstudios/audio/api/AudioEngine.class"]
# El .so de 64 bits que mueven las perillas `so_*` es uno CON hash en el
# trinquete (liboboe); el nuestro va aparte, con sus perillas `own_*`.
SO64_PATH = "jni/arm64-v8a/liboboe.so"
OWN64_PATH = f"jni/arm64-v8a/{UNPINNED_SO}"
IMPOSTOR_PATH = f"jni/arm64-v8a/libimpostor_{UNPINNED_SO}"
RUNTIME_VARIANT = "releaseRuntimeElements-published"

# Lo que arma un lado del self-test. Cada caso cambia UNA de estas perillas del
# lado head (o del base) y exige ver EXACTAMENTE la clave de ese comparador.
DEFAULT_SIDE = dict(
    # AAR
    extra_entry=False, manifest="29", min_compile_sdk="36", meta=True, so=True,
    so_align=16384, so_comment=CLANG, so_needed=NEEDED, so_soname="liboboe.so", so_pad=0,
    own_align=16384, own_build_id=b"\x11" * 20, impostor_pad=0,
    blob=b"\x01\x02", classes=CLASSES, major=61, kotlin_module=b"\x00\x01pkg",
    jar_manifest="Manifest-Version: 1.0\n", bad_class=False, pad=0,
    # POM
    packaging="aar", core_ktx="1.18.0", scope="runtime", pom_type=None, pom_optional=None,
    pom_classifier=None, exclusions=("org.jetbrains.kotlin:kotlin-stdlib-common",), dep_mgmt=None,
    dup_pom_dep=False,
    # .module
    gradle="9.7.1", format_version="1.1", component_attr="release", component_module_suffix="",
    component_group=GROUP, available_at_group=GROUP, available_at_version_suffix="",
    component_url_dir="audio", variant_attr="java-runtime", variant_caps=None,
    runtime_variant_name=RUNTIME_VARIANT, module_excludes=("org.jetbrains.kotlin:kotlin-stdlib-common",),
    dep_attrs=None, dep_caps=None, dep_endorse=False, dup_module_dep=False,
    available_at_module="audio-android", available_at_url="x.module", file_suffix="aar", file_url_prefix="",
    constraint=None, dup_constraint=False, sources_variant=True, no_variants=False,
    publications=tuple(PUBLICATIONS),
)


def make_aar(k: dict, version: str) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:  # ZIP_STORED: el tamaño del AAR crece byte a byte con `pad`
        z.writestr("AndroidManifest.xml", '<manifest package="com.watermellonstudios.audio">'
                                          f' android:versionName="{version}">'
                                          f'<uses-sdk android:minSdkVersion="{k["manifest"]}"/></manifest>')
        if k["meta"]:
            z.writestr("META-INF/com/android/build/gradle/aar-metadata.properties",
                       "aarFormatVersion=1.0\naarMetadataVersion=1.0\n"
                       + (f"minCompileSdk={k['min_compile_sdk']}\n" if k["min_compile_sdk"] else "")
                       + "minAndroidGradlePluginVersion=1.0.0\n")
        z.writestr("classes.jar", make_jar(k["classes"], major=k["major"], kotlin_module=k["kotlin_module"],
                                           manifest=k["jar_manifest"], bad_class=k["bad_class"]))
        z.writestr("R.txt", "")
        z.writestr("assets/tabla.bin", k["blob"])
        # Relleno FIJO para que el AAR sintetico pese como para que los casos de un
        # solo comparador no crucen el 5 % por accidente y ensucien su clave.
        z.writestr("assets/relleno.bin", bytes(200_000))
        if k["so"]:
            z.writestr(SO64_PATH,
                       make_elf(64, aligns=(k["so_align"],), comment=k["so_comment"], needed=k["so_needed"],
                                soname=k["so_soname"], pad=k["so_pad"]) + bytes(k["pad"]))
            z.writestr(OWN64_PATH,
                       make_elf(64, aligns=(k["own_align"],), comment=CLANG, needed=NEEDED, soname=UNPINNED_SO,
                                build_id=k["own_build_id"]))
            z.writestr(f"jni/armeabi-v7a/{UNPINNED_SO}",
                       make_elf(32, aligns=(4096,), comment=CLANG, needed=NEEDED, soname=UNPINNED_SO))
            # Termina en el nombre del nuestro pero NO es el nuestro: sus bytes siguen
            # en el trinquete (R5 excluye por nombre de archivo exacto).
            z.writestr(IMPOSTOR_PATH,
                       make_elf(64, aligns=(16384,), comment=CLANG, needed=[], soname="libimpostor.so",
                                pad=k["impostor_pad"]))
        if k["extra_entry"]:
            z.writestr("jni/arm64-v8a/libsorpresa.so",
                       make_elf(64, aligns=(16384,), comment=CLANG, needed=[], soname="libsorpresa.so"))
    return buf.getvalue()


def make_pom(artifact: str, version: str, k: dict) -> bytes:
    deps = ""
    if artifact == AAR_PUBLICATION:
        extra = ""
        if k["pom_type"]:
            extra += f"<type>{k['pom_type']}</type>"
        if k["pom_optional"]:
            extra += f"<optional>{k['pom_optional']}</optional>"
        if k["pom_classifier"]:
            extra += f"<classifier>{k['pom_classifier']}</classifier>"
        excl = "".join(f"<exclusion><groupId>{e.split(':')[0]}</groupId><artifactId>{e.split(':')[1]}"
                       f"</artifactId></exclusion>" for e in k["exclusions"])
        stdlib = ("<dependency><groupId>org.jetbrains.kotlin</groupId><artifactId>kotlin-stdlib</artifactId>"
                  "<version>2.4.20</version><scope>compile</scope></dependency>")
        # Una dependencia hacia NUESTRA version: sin normalizar, "identico salvo la
        # version" deja de ser verde.
        stdlib += (f"<dependency><groupId>{GROUP}</groupId><artifactId>audio</artifactId>"
                   f"<version>{version}</version><scope>compile</scope></dependency>")
        deps = f"""<dependencies>
  <dependency><groupId>androidx.core</groupId><artifactId>core-ktx</artifactId>
    <version>{k['core_ktx']}</version><scope>{k['scope']}</scope>{extra}
    <exclusions>{excl}</exclusions></dependency>
  {stdlib}{stdlib if k['dup_pom_dep'] else ''}
</dependencies>"""
        if k["dep_mgmt"]:
            deps += ("<dependencyManagement><dependencies><dependency><groupId>androidx.core</groupId>"
                     f"<artifactId>core</artifactId><version>{k['dep_mgmt']}</version></dependency>"
                     "</dependencies></dependencyManagement>")
    packaging = k["packaging"] if artifact == AAR_PUBLICATION else "pom"
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0">
  <modelVersion>4.0.0</modelVersion><groupId>{GROUP}</groupId>
  <artifactId>{artifact}</artifactId><version>{version}</version>
  <packaging>{packaging}</packaging>{deps}
</project>""".encode()


def make_module(artifact: str, version: str, k: dict) -> bytes:
    variants = []
    comp = {"group": k["component_group"], "module": artifact + k["component_module_suffix"], "version": version,
            "attributes": {"org.gradle.status": k["component_attr"]}}
    if artifact == "audio":
        variants.append({"name": "releaseApiElements-published",
                         "attributes": {"org.gradle.usage": "java-api"},
                         "available-at": {"url": f"../../{k['available_at_module']}/{version}/{k['available_at_url']}",
                                          "group": k["available_at_group"], "module": k["available_at_module"],
                                          "version": version + k["available_at_version_suffix"]}})
    else:
        comp["url"] = f"../../{k['component_url_dir']}/{version}/audio-{version}.module"
        deps = []
        if artifact == AAR_PUBLICATION:
            dep = {"group": "androidx.core", "module": "core-ktx", "version": {"requires": k["core_ktx"]},
                   "excludes": [{"group": e.split(":")[0], "module": e.split(":")[1]}
                                for e in k["module_excludes"]]}
            if k["dep_attrs"]:
                dep["attributes"] = k["dep_attrs"]
            if k["dep_caps"]:
                dep["requestedCapabilities"] = k["dep_caps"]
            if k["dep_endorse"]:
                dep["endorseStrictVersions"] = True
            deps.append(dep)
            # Hacia NUESTRA version (la normalizacion de la spec de version).
            deps.append({"group": GROUP, "module": "audio", "version": {"requires": version}})
            if k["dup_module_dep"]:
                deps.append(dict(dep, version={"requires": "9.9.9"}))
        var = {"attributes": {"org.gradle.usage": k["variant_attr"]},
               "dependencies": deps,
               "files": [{"name": f"{artifact}-{version}.{k['file_suffix']}",
                          "url": f"{k['file_url_prefix']}{artifact}-{version}.{k['file_suffix']}",
                          "size": 123, "sha256": "x"}]}
        if k["runtime_variant_name"] is not None:
            var["name"] = k["runtime_variant_name"]
        # Por defecto, la capacidad implicita CON nuestra version (la normalizacion
        # de las capacidades); el caso de la variante la cambia por otra.
        var["capabilities"] = k["variant_caps"] or [{"group": GROUP, "name": artifact, "version": version}]
        if artifact == AAR_PUBLICATION and (k["constraint"] or k["dup_constraint"]):
            c = {"group": "androidx.core", "module": "core", "version": {"requires": k["constraint"] or "1.19.1"}}
            var["dependencyConstraints"] = [c, dict(c)] if k["dup_constraint"] else [c]
        variants.append(var)
        if k["sources_variant"]:
            variants.append({"name": "releaseSourcesElements-published",
                             "attributes": {"org.gradle.category": "documentation"}})
    if k["no_variants"]:
        variants = []
    return json.dumps({"formatVersion": k["format_version"], "component": comp,
                       "createdBy": {"gradle": {"version": k["gradle"]}},
                       "variants": variants}).encode()


def _side(overrides: dict) -> dict:
    k = dict(DEFAULT_SIDE)
    unknown = set(overrides) - set(k)
    if unknown:
        raise ValueError(f"perilla desconocida en el self-test: {unknown}")
    k.update(overrides)
    return k


def write_side(root: Path, version: str, **overrides) -> None:
    """Plano: <root>/<artifact>-<version>.<ext> (lo que lee DirSource)."""
    k = _side(overrides)
    root.mkdir(parents=True, exist_ok=True)
    for art in k["publications"]:
        (root / f"{art}-{version}.pom").write_bytes(make_pom(art, version, k))
        (root / f"{art}-{version}.module").write_bytes(make_module(art, version, k))
    (root / f"{AAR_PUBLICATION}-{version}.aar").write_bytes(make_aar(k, version))


def write_m2(m2: Path, version: str, **overrides) -> None:
    """Con la forma de ~/.m2 (lo que lee LocalM2), para probar run_main entero."""
    k = _side(overrides)
    for art in k["publications"]:
        d = m2 / GROUP_PATH / art / version
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{art}-{version}.pom").write_bytes(make_pom(art, version, k))
        (d / f"{art}-{version}.module").write_bytes(make_module(art, version, k))
        if art == AAR_PUBLICATION:
            (d / f"{art}-{version}.aar").write_bytes(make_aar(k, version))


SO64 = f"aar/so:{SO64_PATH}"
OWN64 = f"aar/so:{OWN64_PATH}"
ANDROID_DEP = "pom:audio-android/dep:androidx.core:core-ktx"
MODULE_VAR = f"module:audio-android/variant:{RUNTIME_VARIANT}"
MODULE_DEP = f"{MODULE_VAR}/dep:androidx.core:core-ktx"
NON_ROOT = [a for a in PUBLICATIONS if a != "audio"]

# (nombre, perillas del head, claves que TIENEN que aparecer — y NINGUNA otra).
# Un conjunto vacio exige VERDE: es lo que NO tiene que ser una diferencia.
COMPARATOR_CASES = [
    ("entradas del AAR", {"extra_entry": True},
     {"aar/entry:jni/arm64-v8a/libsorpresa.so"} | {f"aar/so:jni/arm64-v8a/libsorpresa.so/{f}" for f in
                                                    ("pt_load_align_min", "comment", "needed", "soname", "sha")}),
    ("hash de texto (manifest)", {"manifest": "30"}, {"aar/text:AndroidManifest.xml"}),
    ("aar-metadata", {"min_compile_sdk": "37"}, {"aar/metadata:minCompileSdk"}),
    (".so alineacion", {"so_align": 4096}, {f"{SO64}/pt_load_align_min", f"{SO64}/sha"}),
    (".so .comment", {"so_comment": CLANG_OTRO}, {f"{SO64}/comment", f"{SO64}/sha"}),
    (".so DT_NEEDED", {"so_needed": NEEDED + ["libsorpresa.so"]}, {f"{SO64}/needed", f"{SO64}/sha"}),
    (".so DT_SONAME", {"so_soname": "libotro.so"}, {f"{SO64}/soname", f"{SO64}/sha"}),
    (".so solo bytes", {"so_pad": 64}, {f"{SO64}/sha"}),
    ("un .so que solo TERMINA como el nuestro sigue en el trinquete", {"impostor_pad": 64},
     {f"aar/so:{IMPOSTOR_PATH}/sha"}),
    # R5: los bytes del nuestro NO son una diferencia; su alineacion SI.
    (f"{UNPINNED_SO} solo bytes (build-id) -> no es diferencia", {"own_build_id": b"\x22" * 20}, set()),
    (f"{UNPINNED_SO} alineacion (sigue en el trinquete)", {"own_align": 4096}, {f"{OWN64}/pt_load_align_min"}),
    ("blob binario", {"blob": b"\x01\x03"}, {"aar/blob:assets/tabla.bin"}),
    ("clases del jar", {"classes": CLASSES + ["com/watermellonstudios/audio/Nueva.class"]},
     {"aar/class:classes.jar!com/watermellonstudios/audio/Nueva.class"}),
    ("major del bytecode", {"major": 65}, {"aar/class-major:classes.jar"}),
    (".kotlin_module", {"kotlin_module": b"\x00\x02pkg"},
     {"aar/jar-entry:classes.jar!META-INF/com.watermellonstudios_audio_release.kotlin_module"}),
    ("jar: entrada no-clase (MANIFEST.MF)", {"jar_manifest": "Manifest-Version: 1.0\nX-Otro: si\n"},
     {"aar/jar-entry:classes.jar!META-INF/MANIFEST.MF"}),
    ("POM packaging", {"packaging": "jar"}, {"pom:audio-android/packaging"}),
    ("POM version", {"core_ktx": "1.19.1"}, {ANDROID_DEP, MODULE_DEP}),
    ("POM scope", {"scope": "compile"}, {ANDROID_DEP}),
    ("POM type", {"pom_type": "aar"}, {ANDROID_DEP}),
    ("POM optional", {"pom_optional": "true"}, {ANDROID_DEP}),
    ("POM classifier", {"pom_classifier": "sources"}, {ANDROID_DEP, f"{ANDROID_DEP}:sources"}),
    ("POM exclusiones", {"exclusions": ()}, {ANDROID_DEP}),
    ("POM dependencyManagement", {"dep_mgmt": "1.19.1"}, {"pom:audio-android/depMgmt:androidx.core:core"}),
    ("module formatVersion", {"format_version": "1.2"}, {f"module:{a}/formatVersion" for a in PUBLICATIONS}),
    ("module createdBy", {"gradle": "9.8.0"}, {f"module:{a}/createdBy.gradle" for a in PUBLICATIONS}),
    ("module component url", {"component_url_dir": "audio-otro"}, {f"module:{a}/component" for a in NON_ROOT}),
    ("module component grupo", {"component_group": "com.otro"}, {f"module:{a}/component" for a in PUBLICATIONS}),
    ("module component G:M:V", {"component_module_suffix": "-otro"},
     {f"module:{a}/component" for a in PUBLICATIONS}),
    ("module component atributos", {"component_attr": "integration"},
     {f"module:{a}/component.attrs" for a in PUBLICATIONS}),
    ("module atributo de variante", {"variant_attr": "java-api"},
     {f"module:{a}/variant:{RUNTIME_VARIANT}/attr:org.gradle.usage" for a in NON_ROOT}),
    ("module capacidades de la variante", {"variant_caps": [{"group": GROUP, "name": "extra", "version": "1"}]},
     {f"module:{a}/variant:{RUNTIME_VARIANT}/capabilities" for a in NON_ROOT}),
    ("module dep excludes", {"module_excludes": ()}, {MODULE_DEP}),
    ("module dep atributos", {"dep_attrs": {"org.gradle.category": "platform"}}, {MODULE_DEP}),
    ("module dep capacidades pedidas",
     {"dep_caps": [{"group": "androidx.core", "name": "core-ktx-fixtures", "version": "1"}]}, {MODULE_DEP}),
    ("module dep endorseStrictVersions", {"dep_endorse": True}, {MODULE_DEP}),
    ("module available-at grupo", {"available_at_group": "com.otro"},
     {"module:audio/variant:releaseApiElements-published/available-at"}),
    ("module available-at version", {"available_at_version_suffix": "-x"},
     {"module:audio/variant:releaseApiElements-published/available-at"}),
    ("module available-at", {"available_at_module": "audio-otro"},
     {"module:audio/variant:releaseApiElements-published/available-at"}),
    ("module url de available-at", {"available_at_url": "y.module"},
     {"module:audio/variant:releaseApiElements-published/available-at"}),
    ("module archivo", {"file_suffix": "zip"},
     {f"module:{a}/variant:{RUNTIME_VARIANT}/file:{a}-<VERSION>.{s}" for a in NON_ROOT for s in ("aar", "zip")}),
    ("module url de cada archivo", {"file_url_prefix": "otro/"},
     {f"module:{a}/variant:{RUNTIME_VARIANT}/file:{a}-<VERSION>.aar" for a in NON_ROOT}),
    ("module constraint", {"constraint": "1.19.1"}, {f"{MODULE_VAR}/constraint:androidx.core:core"}),
    ("module variante que desaparece", {"sources_variant": False},
     {f"module:{a}/variant:releaseSourcesElements-published{s}" for a in NON_ROOT
      for s in ("", "/attr:org.gradle.category")}),
]

# Lo que tiene que salir con codigo 2 (no poder leer) y no con 0 ni con 1.
UNREADABLE_CASES = [
    ("AAR sin aar-metadata", {"meta": False}),
    ("AAR sin .so", {"so": False}),
    ("falta una publicacion", {"publications": tuple(PUBLICATIONS[:-1])}),
    (".module sin variantes", {"no_variants": True}),
    ("POM con una dependencia repetida", {"dup_pom_dep": True}),
    (".module con una dependencia repetida", {"dup_module_dep": True}),
    (".module con un constraint repetido", {"dup_constraint": True}),
    (".class que no es bytecode", {"bad_class": True}),
]


def self_test() -> int:
    print("self-test de diff-published-artifact:")
    failures: list[str] = []

    def observe_sides(head_kw: dict, base_kw: dict | None = None) -> Observation:
        with tempfile.TemporaryDirectory() as t:
            write_side(Path(t) / "base", "2.21.0", **(base_kw or {}))
            write_side(Path(t) / "head", "2.21.0-local", **head_kw)
            return observe(DirSource(Path(t) / "base", "2.21.0", "base"),
                           DirSource(Path(t) / "head", "2.21.0-local", "head"))

    def run(head_kw: dict, expectations: dict, base_kw: dict | None = None) -> tuple[int, dict, set[str]]:
        o = observe_sides(head_kw, base_kw)
        rc, detail = verdict(o.diffs, expectations, quiet=True)
        return rc, detail, {k for k, _, _ in o.diffs}

    def check(name: str, ok: bool, why: str) -> None:
        print(f"  {'ok ' if ok else 'MAL'} {name}" + ("" if ok else f" — {why}"))
        if not ok:
            failures.append(name)

    def attempt(name: str, fn) -> None:
        """Un caso que REVIENTA es un caso MAL con su nombre, no un rojo anonimo."""
        try:
            ok, why = fn()
        except Exception as e:  # noqa: BLE001
            ok, why = False, f"lanzo {type(e).__name__}: {e}"
        check(name, ok, why)

    def raises_read_error(fn) -> tuple[bool, str]:
        try:
            fn()
        except ReadError:
            return True, ""
        return False, "no lanzo ReadError"

    def main_quiet(argv: list[str]) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            rc = run_main(argv)
        return rc, out.getvalue()

    # 0. Mismo artefacto, otra version: CERO diferencias. Si esto falla, la
    #    normalizacion de version esta rota y todo lo demas es ruido.
    def identical():
        rc, _d, seen = run({}, {})
        return rc == 0 and not seen, f"rc={rc} vio {sorted(seen)}"
    attempt("identico salvo la version -> verde", identical)

    # 1. Un caso por comparador: rojo, y EXACTAMENTE las claves de ese comparador,
    #    todas como "sin declarar". Un rc=1 de otro origen no cuenta.
    for name, kw, want in COMPARATOR_CASES:
        def one(kw=kw, want=want):
            rc, detail, seen = run(kw, {})
            ok = seen == want and set(detail["undeclared"]) == want and rc == (1 if want else 0)
            return ok, f"rc={rc} vio {sorted(seen)} esperaba {sorted(want)}"
        attempt(f"comparador: {name}", one)

    # R5: los bytes de UNPINNED_SO no son una diferencia, pero SE IMPRIMEN: en la
    #     salida de compare(), los dos hashes crudos y los dos sin build-id.
    def own_printed():
        o = observe_sides({"own_build_id": b"\x22" * 20})
        a, b = o.info_a.get(OWN64_PATH), o.info_b.get(OWN64_PATH)
        with tempfile.TemporaryDirectory() as t:
            write_side(Path(t) / "base", "2.21.0")
            write_side(Path(t) / "head", "2.21.0-local", own_build_id=b"\x22" * 20)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc, _d = compare(DirSource(Path(t) / "base", "2.21.0", "base"),
                                 DirSource(Path(t) / "head", "2.21.0-local", "head"), {})
        text = out.getvalue()
        ok = (a is not None and b is not None and a != b and rc == 0
              and set(o.info_a) == {OWN64_PATH, f"jni/armeabi-v7a/{UNPINNED_SO}"}
              and f"base {a}" in text and f"head {b}" in text)
        return ok, f"rc={rc} {o.info_a} {o.info_b}"
    attempt(f"{UNPINNED_SO}: sus bytes se imprimen (base != head)", own_printed)

    # R1: el umbral del 5 %, en las DOS direcciones y a los dos lados del borde.
    #     El AAR va sin comprimir, asi que `pad` lo agranda byte a byte; la razon
    #     REAL se mide del AAR construido, para que el caso no mienta sobre si mismo.
    size_a = len(make_aar(dict(DEFAULT_SIDE), "2.21.0"))
    for label, side, ratio, red in [("+4,9 % -> no es diferencia", "head", 0.049, False),
                                    ("+5,1 % -> aar/size", "head", 0.051, True),
                                    ("-5,1 % -> aar/size", "base", 0.051, True)]:
        def size_case(side=side, ratio=ratio, red=red):
            p = round(ratio * size_a) if side == "head" else round(ratio * size_a / (1 - ratio))
            kw = {"pad": p}
            o = observe_sides(kw if side == "head" else {}, kw if side == "base" else None)
            total = next((a, b) for k, a, b in o.rows if k == "(AAR entero)")
            real = (total[1] - total[0]) / total[0]
            rc, detail = verdict(o.diffs, {}, quiet=True)
            want = {f"{SO64}/sha"} | ({SIZE_KEY} if red else set())
            seen = {k for k, _, _ in o.diffs}
            # 4.11: el VALOR es el cruce y su sentido, nunca los bytes.
            value = next(((va, vb) for k, va, vb in o.diffs if k == SIZE_KEY), None)
            want_value = (SIZE_BASE, SIZE_UP if side == "head" else SIZE_DOWN) if red else None
            ok = seen == want and value == want_value and abs(abs(real) - ratio) < 0.0005 and rc == 1
            return ok, (f"razon real {real:+.4%}, vio {sorted(seen)} esperaba {sorted(want)}; "
                        f"valor {value} esperaba {want_value}")
        attempt(f"tamaño: {label}", size_case)

    def size_exact():
        # "Mas del 5 %": el 5 % JUSTO no es diferencia. Se rellenan los dos lados
        # para que la diferencia sea exactamente 1/20 del base, medido en bytes.
        size_b = len(make_aar(dict(DEFAULT_SIDE), "2.21.0-local"))
        q = (-size_a) % 20
        p = size_a + q - size_b + (size_a + q) // 20
        o = observe_sides({"pad": p}, {"pad": q})
        ta, tb = next((a, b) for k, a, b in o.rows if k == "(AAR entero)")
        seen = {k for k, _, _ in o.diffs}
        return (tb - ta) * 20 == ta and seen == {f"{SO64}/sha"}, f"{ta} -> {tb}, vio {sorted(seen)}"
    attempt("tamaño: +5,000 % exacto -> no es diferencia", size_exact)

    # 4.11: una declaracion de tamaño se escribe con el cruce, y ata al SENTIDO, no a
    # los bytes. `expect` arma la declaracion con el `.so` observado (que en estos
    # lados siempre difiere) y la linea de tamaño que se le pase.
    def size_expect(o, size_line):
        exp = {k: (va, vb, "caso") for k, va, vb in o.diffs if k != SIZE_KEY}
        if size_line is not None:
            exp[SIZE_KEY] = (*size_line, "caso")
        return exp

    up = (SIZE_BASE, SIZE_UP)

    def size_other_bytes():
        # El mismo cruce con otros bytes (+5,1 % y +7 %) -> la MISMA declaracion da verde
        # en los dos. Con bytes en el valor, el segundo quedaba "con otro valor".
        rcs = []
        for ratio in (0.051, 0.07):
            o = observe_sides({"pad": round(ratio * size_a)})
            rcs.append(verdict(o.diffs, size_expect(o, up), quiet=True)[0])
        return rcs == [0, 0], f"rc {rcs}"
    attempt("tamaño: el mismo cruce con otros bytes -> la misma declaracion, verde", size_other_bytes)

    def size_other_direction():
        # Declarado "crece" y el AAR achica mas del 5 % -> con otro valor.
        p = round(0.051 * size_a / (1 - 0.051))
        o = observe_sides({}, {"pad": p})
        rc, d = verdict(o.diffs, size_expect(o, up), quiet=True)
        return rc == 1 and d["wrong_value"] == [SIZE_KEY] and not d["unobserved"], f"rc={rc} {d}"
    attempt("tamaño: declarado crece, achica -> con otro valor", size_other_direction)

    def size_back_inside():
        # Declarado "crece" y el AAR vuelve adentro del umbral -> no se reproduce.
        o = observe_sides({"pad": round(0.049 * size_a)})
        rc, d = verdict(o.diffs, size_expect(o, up), quiet=True)
        return rc == 1 and d["unobserved"] == [SIZE_KEY] and not d["wrong_value"], f"rc={rc} {d}"
    attempt("tamaño: declarado crece, dentro del 5 % -> no se reproduce", size_back_inside)

    def size_bytes_declared():
        # La forma VIEJA (bytes) ya no puede dar verde: queda con otro valor.
        o = observe_sides({"pad": round(0.051 * size_a)})
        ta, tb = next((a, b) for k, a, b in o.rows if k == "(AAR entero)")
        rc, d = verdict(o.diffs, size_expect(o, (str(ta), str(tb))), quiet=True)
        return rc == 1 and d["wrong_value"] == [SIZE_KEY], f"rc={rc} {d}"
    attempt("tamaño: una declaracion en bytes -> con otro valor", size_bytes_declared)

    def size_printed():
        # Los bytes no entran al trinquete, pero se IMPRIMEN.
        o = observe_sides({"pad": round(0.051 * size_a)})
        ta, tb = next((a, b) for k, a, b in o.rows if k == "(AAR entero)")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            print_size_verdict(o.rows, o.diffs)
        text = out.getvalue()
        return f"{ta} -> {tb} bytes" in text and SIZE_UP in text, text.strip()
    attempt("tamaño: los bytes y el cruce se imprimen", size_printed)

    def size_vocabulary():
        # Las palabras del trinquete de tamaño estan escritas en los archivos de
        # declaracion (scripts/published-artifact-vs-*.txt). Cambiarlas deja cada
        # declaracion vieja "con otro valor" en la proxima corrida real, lejos de aca.
        want = ("referencia", "crece-mas-del-5%", "achica-mas-del-5%")
        return (SIZE_BASE, SIZE_UP, SIZE_DOWN) == want, f"{(SIZE_BASE, SIZE_UP, SIZE_DOWN)}"
    attempt("tamaño: el vocabulario de la declaracion es estable", size_vocabulary)

    # 2. El trinquete es sobre el VALOR (B1), del head Y de la base (R4).
    bump = {"min_compile_sdk": "37", "core_ktx": "1.19.1", "scope": "runtime"}
    exact = {k: (va, vb, "s2") for k, va, vb in observe_sides(bump).diffs}

    def bump_green():
        rc, _d, _s = run(bump, exact)
        return rc == 0 and len(exact) == 3, f"rc={rc} {sorted(exact)}"
    attempt("el bump declarado con sus valores -> verde", bump_green)

    def wrong(name, head_kw, key, base_kw=None):
        def fn():
            rc, d, _s = run(head_kw, exact, base_kw)
            return rc == 1 and key in d["wrong_value"], f"rc={rc} {d}"
        attempt(name, fn)
    wrong("A: declarado 36 -> 37, observado 36 -> 99 -> rojo",
          dict(bump, min_compile_sdk="99"), "aar/metadata:minCompileSdk")
    wrong("B: core-ktx declarado 1.19.1 runtime, observado 9.9.9 compile -> rojo",
          dict(bump, core_ktx="9.9.9", scope="compile"), ANDROID_DEP)
    wrong("C: declarado 36 -> 37, la clave desaparece (36 -> ausente) -> rojo",
          dict(bump, min_compile_sdk=None), "aar/metadata:minCompileSdk")
    wrong("R4: declarado 36 -> 37, observado 35 -> 37 (solo cambia la BASE) -> rojo",
          bump, "aar/metadata:minCompileSdk", base_kw={"min_compile_sdk": "35"})

    def moved():
        rc, d, _s = run({"pom_classifier": "sources", "core_ktx": "1.19.1"},
                        {ANDROID_DEP: ("1.18.0 scope=runtime type=jar optional=false "
                                       "excl=[org.jetbrains.kotlin:kotlin-stdlib-common]",
                                       "1.19.1 scope=runtime type=jar optional=false "
                                       "excl=[org.jetbrains.kotlin:kotlin-stdlib-common]", "x")})
        return rc == 1 and ANDROID_DEP in d["wrong_value"], f"rc={rc} {d}"
    attempt("C': la dependencia declarada se muda a otra clave (classifier) -> rojo", moved)

    def unobserved():
        rc, d, _s = run({}, {"aar/metadata:minCompileSdk": ("36", "37", "x")})
        return rc == 1 and d["unobserved"] == ["aar/metadata:minCompileSdk"], f"rc={rc} {d}"
    attempt("declaracion que no se reproduce -> rojo", unobserved)

    # 3. La sintaxis del archivo de declaraciones (R6, R9).
    def canonical():
        exp = read_expectations(["aar/metadata:minCompileSdk = 36 -> 37   # razon"], None)
        return exp == {"aar/metadata:minCompileSdk": ("36", "37", "razon")}, f"{exp}"
    attempt("parser: 'CLAVE = BASE -> HEAD   # razon'", canonical)

    def empty_value():
        exp = read_expectations([f"k = {EMPTY} -> 1   # r", f"k2 = {ABSENT} -> {EMPTY}   # r"], None)
        return exp == {"k": ("", "1", "r"), "k2": (ABSENT, "", "r")}, f"{exp}"
    attempt(f"parser: {EMPTY} es el valor vacio", empty_value)

    for label, bad in [("sin '='", "aar/metadata:minCompileSdk   # r"),
                       ("sin ' -> '", "aar/metadata:minCompileSdk = 37   # r"),
                       ("R6: sin razon", "aar/metadata:minCompileSdk = 36 -> 37"),
                       ("R6: razon vacia", "aar/metadata:minCompileSdk = 36 -> 37   # "),
                       ("R6: razon placeholder", f"aar/metadata:minCompileSdk = 36 -> 37   # {PLACEHOLDER_REASON}"),
                       ("R9: dos ' -> '", "k = a -> b -> c   # r"),
                       ("R9: head vacio sin (vacio)", "k = 36 ->    # r"),
                       ("R9: base vacia sin (vacio)", "k =  -> 37   # r"),
                       ("R9: un valor con '#'", "k = a#b -> c   # r"),
                       ("R9: un valor con '->'", "k = a -> ->b   # r"),
                       ("una clave con espacio", "k x = 1 -> 2   # r")]:
        attempt(f"parser rechaza {label}: {bad!r}",
                lambda bad=bad: raises_read_error(lambda: read_expectations([bad], None)))
    attempt("parser rechaza una clave declarada dos veces",
            lambda: raises_read_error(lambda: read_expectations(["k = 1 -> 2   # r", "k = 1 -> 3   # r"], None)))

    # --print-declarations: lo que imprime se lee de vuelta IGUAL, y lo que no se
    # puede escribir sin ambiguedad se rechaza en vez de imprimirse mal.
    def roundtrip():
        diffs = [("k1", "", "x"), ("k2", ABSENT, "a = b"), ("k3", "1.18.0 scope=runtime", "1.19.1 scope=runtime"),
                 ("k4", "1", "")]
        lines = [line.replace(f"# {PLACEHOLDER_REASON}", "# r") for line in declaration_lines(diffs)]
        exp = read_expectations(lines, None)
        return exp == {k: (va, vb, "r") for k, va, vb in diffs}, f"{lines} -> {exp}"
    attempt("print-declarations: ida y vuelta, (vacio) incluido", roundtrip)
    for label, diffs in [("un valor con ' -> '", [("k", "1", "a -> b")]),
                         ("un valor con ' # '", [("k", "1", "a # b")]),
                         ("un valor con espacio al final", [("k", "1 ", "2")]),
                         (f"un valor que es literalmente {EMPTY}", [("k", EMPTY, "2")]),
                         ("un valor con salto de linea", [("k", "1", "a\nb")]),
                         ("una clave con espacio", [("k x", "1", "2")])]:
        attempt(f"print-declarations rechaza {label}",
                lambda diffs=diffs: raises_read_error(lambda: declaration_lines(diffs)))

    # 4. Lo ilegible sale con codigo 2.
    for name, kw in UNREADABLE_CASES:
        attempt(f"ilegible: {name}", lambda kw=kw: raises_read_error(lambda: observe_sides(kw)))
    attempt("ilegible: un .so que no es ELF", lambda: raises_read_error(lambda: elf_facts(b"no soy un elf", "x.so")))
    attempt("ilegible: un ELF sin seccion dinamica",
            lambda: raises_read_error(lambda: elf_facts(make_elf(64, comment=CLANG, dynamic_section=False), "x.so")))
    attempt("ilegible: un ELF truncado",
            lambda: raises_read_error(lambda: elf_facts(b"\x7fELF\x02\x01" + bytes(10), "x.so")))

    # ... y por main(), que es lo que corre el usuario. Con control positivo: el
    # mismo arbol de ~/.m2 sin romper tiene que dar 0, o el 2 no prueba nada.
    def via_main(head_kw: dict, want_rc: int, extra: list[str] | None = None):
        def fn():
            with tempfile.TemporaryDirectory() as t:
                write_m2(Path(t), "2.21.0")
                write_m2(Path(t), "2.21.0-local", **head_kw)
                rc, out = main_quiet(["--against-local", "2.21.0", "--local-version", "2.21.0-local",
                                      "--m2", t] + (extra or []))
            return rc == want_rc, f"rc={rc}, esperaba {want_rc}"
        return fn
    attempt("main: control, ~/.m2 identico salvo la version -> 0", via_main({}, 0))
    attempt("main: una publicacion que no existe -> 2",
            lambda: (lambda rc: (rc == 2, f"rc={rc}"))(
                main_quiet(["--against-local", "no-existe", "--local-version", "tampoco", "--m2", "/nonexistent"])[0]))
    # R7: una excepcion que NO es ReadError (un .module sin `name` es KeyError).
    attempt("main: R7, un .module con una variante sin 'name' -> 2", via_main({"runtime_variant_name": None}, 2))
    attempt("main: --print-declarations, control declarable -> 0",
            via_main({"core_ktx": "1.19.1"}, 0, ["--print-declarations"]))
    attempt("main: --print-declarations con un valor no declarable -> 2",
            via_main({"core_ktx": "1.19.1#x"}, 2, ["--print-declarations"]))

    # 5. El parser de ELF lee lo que el generador escribio, en 64 y en 32 bits.
    def elf_parser():
        f64 = elf_facts(make_elf(64, aligns=(16384,), comment=CLANG, needed=NEEDED, soname="liba.so"), "a")
        f32 = elf_facts(make_elf(32, aligns=(4096,), comment=CLANG, needed=["libc.so"], soname=None), "b")
        ok = (f64["pt_load_align_min"] == "16384" and f32["pt_load_align_min"] == "4096"
              and "clang version 18.0.3" in f64["comment"] and "LLD" in f32["comment"]
              and f64["needed"] == ",".join(sorted(NEEDED)) and f32["needed"] == "libc.so"
              and f64["soname"] == "liba.so" and f32["soname"] == ABSENT)
        return ok, f"{f64} {f32}"
    attempt("parser ELF 64/32: alineacion, .comment, DT_NEEDED, DT_SONAME", elf_parser)

    def align_min():
        # Entre un PT_GNU_STACK de p_align 0 y un PT_DYNAMIC de 8 (make_elf): contar
        # un segmento que no es PT_LOAD daria 0 u 8, no el minimo de los PT_LOAD.
        got = {al: elf_facts(make_elf(64, aligns=al, comment=CLANG, needed=NEEDED, soname="a.so"),
                             "a")["pt_load_align_min"]
               for al in [(16384, 4096), (4096, 16384), (16384, 4096, 16384)]}
        return all(v == "4096" for v in got.values()), f"{got}"
    attempt("R2: parser ELF, PT_LOAD [16384, 4096], [4096, 16384] y [16384, 4096, 16384] -> 4096", align_min)

    def pom_without_version():
        pom = (f'<project xmlns="{POM_NS["m"]}"><dependencies><dependency><groupId>g</groupId>'
               '<artifactId>a</artifactId></dependency></dependencies></project>').encode()
        v = pom_facts(pom, "x", "1.0")["pom:x/dep:g:a"]
        return v.startswith(f"{NO_VERSION} ") and declarable_value(v), repr(v)
    attempt(f"POM: una dependencia sin <version> se escribe {NO_VERSION} y es declarable", pom_without_version)

    def needed_repeated():
        f = elf_facts(make_elf(64, comment=CLANG, needed=["libc.so", "libc.so"], soname="a.so"), "a")
        return f["needed"] == "libc.so,libc.so", f["needed"]
    attempt("parser ELF: una DT_NEEDED repetida se ve repetida", needed_repeated)

    def build_id():
        def facts(bid: bytes, pad: int = 0):
            return elf_facts(make_elf(64, comment=CLANG, needed=NEEDED, soname="a.so", build_id=bid, pad=pad), "a")
        f1, f2, f3 = facts(b"\x01" * 20), facts(b"\x02" * 20), facts(b"\x01" * 20, pad=8)
        ok = (f1["sha"] != f2["sha"] and f1["sha-sin-build-id"] == f2["sha-sin-build-id"]
              and f1["sha-sin-build-id"] != f3["sha-sin-build-id"])
        return ok, f"{f1} {f2} {f3}"
    attempt("parser ELF: el hash sin build-id ignora SOLO el build-id", build_id)

    if failures:
        print(f"\n\033[31mself-test ROJO\033[0m — {len(failures)} casos: {failures}")
        return 1
    print(f"\n\033[32mself-test ok\033[0m — el instrumento puede fallar "
          f"({len(COMPARATOR_CASES)} comparadores, cada uno con su caso).")
    return 0


# =============================================================================

def local_version_default() -> str:
    for line in (REPO / "gradle.properties").read_text(encoding="utf-8").splitlines():
        m = re.match(r"^version=(.+)$", line.strip())
        if m:
            return m.group(1)
    raise ReadError("gradle.properties sin version=")


def run_main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--against", help="version publicada en el registro (la base)")
    ap.add_argument("--against-local", metavar="VERSION",
                    help="usar una version de ~/.m2 como base en vez del registro")
    ap.add_argument("--local-version", help="version en ~/.m2 (default: la de gradle.properties)")
    ap.add_argument("--m2", default=str(Path.home() / ".m2" / "repository"))
    ap.add_argument("--cache", help="cache de lo bajado (default: build/published-artifacts/<version>)")
    ap.add_argument("--expect", action="append", help="'CLAVE = BASE -> HEAD' (repetible)")
    ap.add_argument("--expect-file", help="una declaracion por linea, con su ' # razon' (obligatoria)")
    ap.add_argument("--print-declarations", action="store_true",
                    help="imprime lo observado en la sintaxis de --expect-file y sale con 0; la razon "
                         f"sale como '{PLACEHOLDER_REASON}', que el parser rechaza hasta que se escriba")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.against and not args.against_local:
        ap.error("falta --against VERSION (o --against-local VERSION)")

    # N1: CUALQUIER excepcion leyendo sale con 2. Un traceback no es un veredicto.
    try:
        local_version = args.local_version or local_version_default()
        head = LocalM2(Path(args.m2), local_version)
        if args.against_local:
            base: Source = LocalM2(Path(args.m2), args.against_local)
        else:
            cache = Path(args.cache) if args.cache else REPO / "build" / "published-artifacts" / args.against
            base = Registry(args.against, cache)
        if args.print_declarations:
            # Todas o ninguna: si una no se puede escribir, ReadError -> 2.
            for line in declaration_lines(observe(base, head).diffs):
                print(line)
            return 0
        rc, _detail = compare(base, head, read_expectations(args.expect, args.expect_file))
        return rc
    except ReadError as e:
        print(f"\033[31mNO PUDE LEER\033[0m — {e}", file=sys.stderr)
        return 2
    except Exception as e:  # noqa: BLE001 — a proposito: nada ilegible puede salir con 0 o 1
        traceback.print_exc()
        print(f"\033[31mNO PUDE LEER\033[0m — {type(e).__name__}: {e}", file=sys.stderr)
        return 2


def main() -> int:
    return run_main(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
