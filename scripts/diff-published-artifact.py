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
  - POM: packaging y cada dependencia, indexada por grupo:artefacto[:classifier],
    con version, scope, type, optional y sus exclusiones.
  - `.module` (Gradle Module Metadata): `formatVersion`, la version de Gradle de
    `createdBy`, el `component` (url, grupo, modulo, atributos) y, por variante,
    sus atributos, capacidades, dependencias (spec de version, excludes,
    atributos y capacidades pedidas), constraints, `available-at` y el nombre y
    la url de cada archivo. No compara tamaños ni hashes de archivos: eso lo
    hace el AAR, hecho por hecho.
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
    y el hash de sus BYTES. El ELF se lee aca, en Python: no depende de
    llvm-readelf ni del NDK.
El TAMAÑO se imprime siempre, entrada por entrada, y el del AAR entero es una
diferencia (`aar/size`) si se mueve mas del 5 % (el no funcional de REQ-047).

La VERSION se normaliza en todo lo que es texto (nombres de archivo, POM,
`.module`, referencias entre nuestras publicaciones). En los BYTES del `.so` no
se puede: `libwatermelon_audio.so` embebe la version como string, asi que su
hash cambia con cualquier `-Pversion` distinto y se declara con esa razon.

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

`(ausente)` es el valor de un hecho que no existe de ese lado. El veredicto es:
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
Una publicacion que falta, un `.so` que no parsea como ELF, un AAR sin
`aar-metadata.properties` o sin ningun `.so`, un lado con cero hechos, o
CUALQUIER excepcion al leer: sale con codigo 2, nunca con 0 ni con 1.

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
SIZE_KEY = "aar/size"


class ReadError(Exception):
    """Algo que no se pudo leer o bajar. Nunca se convierte en un pase."""


def sha(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()[:16]


# =============================================================================
# ELF: PT_LOAD, .comment, DT_NEEDED y DT_SONAME, de 32 y 64 bits, little endian.
# =============================================================================

PT_LOAD = 1
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
    for name_off, s_type, off, size, link in sections:
        if _cstr(shstrtab, name_off) == ".comment":
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
    for d in root.findall("m:dependencies/m:dependency", ns):
        g = d.findtext("m:groupId", "", ns)
        a = d.findtext("m:artifactId", "", ns)
        cl = d.findtext("m:classifier", "", ns)
        ga = f"{g}:{a}" + (f":{cl}" if cl else "")
        excl = sorted(f"{e.findtext('m:groupId', '', ns)}:{e.findtext('m:artifactId', '', ns)}"
                      for e in d.findall("m:exclusions/m:exclusion", ns))
        value = (f"{normalize(d.findtext('m:version', '', ns), version)}"
                 f" scope={d.findtext('m:scope', 'compile', ns)}"
                 f" type={d.findtext('m:type', 'jar', ns)}"
                 f" optional={d.findtext('m:optional', 'false', ns)}"
                 f" excl=[{','.join(excl)}]")
        key = f"{pref}/dep:{ga}"
        if key in facts:
            raise ReadError(f"POM de {artifact}: dependencia repetida {ga}")
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
        for d in var.get("dependencies", []):
            facts[f"{vp}/dep:{d['group']}:{d['module']}"] = _dep_value(d, version)
        for d in var.get("dependencyConstraints", []):
            facts[f"{vp}/constraint:{d['group']}:{d['module']}"] = _dep_value(d, version)
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


def aar_facts(raw: bytes, version: str) -> tuple[dict[str, str], dict[str, int]]:
    facts: dict[str, str] = {}
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
            for k, v in elf_facts(data, n).items():
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
    return facts, sizes


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


def collect(src: Source) -> tuple[dict[str, str], dict[str, int]]:
    """Las CUATRO publicaciones, siempre: no hay forma de pedir menos."""
    facts: dict[str, str] = {}
    sizes: dict[str, int] = {}
    for art in PUBLICATIONS:
        before = len(facts)
        facts.update(pom_facts(src.get(art, "pom"), art, src.version))
        facts.update(module_facts(src.get(art, "module"), art, src.version))
        if art == AAR_PUBLICATION:
            f, s = aar_facts(src.get(art, "aar"), src.version)
            facts.update(f)
            sizes.update(s)
        if len(facts) == before:
            raise ReadError(f"{src.label}: {art} no aporto ningun hecho")
    if not facts:
        raise ReadError(f"{src.label}: cero hechos")
    return facts, sizes


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
        return rows, (SIZE_KEY, str(total_a), str(total_b))
    return rows, None


DECL_RE = re.compile(r"^(?P<key>\S+) = (?P<base>.*?) -> (?P<head>.*)$")


def parse_declaration(line: str) -> tuple[str, str, str, str] | None:
    body, _, reason = line.partition(" # ")
    body = body.strip()
    if not body or body.startswith("#"):
        return None
    m = DECL_RE.match(body)
    if not m:
        raise ReadError(f"declaracion sin la forma 'CLAVE = BASE -> HEAD': {line.strip()!r}")
    return m.group("key"), m.group("base"), m.group("head"), reason.strip() or "(sin razon)"


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
                mark = f"DECLARADA CON OTRO VALOR ({b} -> {h})"
            else:
                mark = "declarada"
            print(f"  [{mark}] {k}\n      {va}  ->  {vb}")
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


def observe(base: Source, head: Source) -> tuple[list[tuple[str, str, str]], list, int, int]:
    fa, sa = collect(base)
    fb, sb = collect(head)
    diffs = diff_facts(fa, fb)
    rows, size_row = size_diff(sa, sb)
    if size_row:
        diffs.append(size_row)
    return diffs, rows, len(fa), len(fb)


def compare(base: Source, head: Source, expectations, quiet=False) -> tuple[int, dict]:
    diffs, rows, na, nb = observe(base, head)
    if not quiet:
        print(f"base: {base.label}  —  {na} hechos")
        print(f"head: {head.label}  —  {nb} hechos")
        print_sizes(rows)
    return verdict(diffs, expectations, quiet)


# =============================================================================
# self-test: que el instrumento PUEDE fallar, comparador por comparador
# =============================================================================

def make_elf(bits: int, *, align: int, comment: bytes, needed: list[str], soname: str | None,
             pad: int = 0) -> bytes:
    """Un ELF minimo: un PT_LOAD con `align`, .comment, .dynstr y .dynamic."""
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
    shstr = b"\0.shstrtab\0.comment\0.dynstr\0.dynamic\0"
    names = {".shstrtab": 1, ".comment": 11, ".dynstr": 20, ".dynamic": 28}

    body_off = ehsize + phentsize
    blobs = [("comment", comment), ("dynstr", dynstr), ("dynamic", dynamic), ("shstr", shstr)]
    offsets = {}
    cur = body_off
    payload = b""
    for tag, blob in blobs:
        pad8 = (-cur) % 8
        payload += bytes(pad8)
        cur += pad8
        offsets[tag] = cur
        payload += blob
        cur += len(blob)
    payload += bytes(pad)
    cur += pad
    pad8 = (-cur) % 8
    payload += bytes(pad8)
    shoff = cur + pad8

    ident = b"\x7fELF" + bytes([2 if b64 else 1, 1, 1]) + bytes(9)
    if b64:
        eh = ident + struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, ehsize, shoff, 0,
                                 ehsize, phentsize, 1, shentsize, 5, 1)
        ph = struct.pack("<IIQQQQQQ", PT_LOAD, 5, 0, 0, 0, 0x1000, 0x1000, align)
        shf = "<IIQQQQIIQQ"
    else:
        eh = ident + struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, 0, ehsize, shoff, 0,
                                 ehsize, phentsize, 1, shentsize, 5, 1)
        ph = struct.pack("<IIIIIIII", PT_LOAD, 0, 0, 0, 0x1000, 0x1000, 5, align)
        shf = "<IIIIIIIIII"
    dyn_ent = struct.calcsize(dyn_fmt)
    sh = bytes(shentsize)                                                                  # 0: null
    sh += struct.pack(shf, names[".shstrtab"], 3, 0, 0, offsets["shstr"], len(shstr), 0, 0, 1, 0)   # 1
    sh += struct.pack(shf, names[".comment"], 1, 0x30, 0, offsets["comment"], len(comment), 0, 0, 1, 1)  # 2
    sh += struct.pack(shf, names[".dynstr"], 3, 2, 0, offsets["dynstr"], len(dynstr), 0, 0, 1, 0)   # 3
    sh += struct.pack(shf, names[".dynamic"], SHT_DYNAMIC, 3, 0, offsets["dynamic"], len(dynamic),
                      3, 0, 8, dyn_ent)                                                    # 4 -> link 3
    return eh + ph + payload + sh


def class_bytes(major: int) -> bytes:
    return b"\xca\xfe\xba\xbe" + struct.pack(">HH", 0, major) + bytes(8)


def make_jar(classes: list[str], *, major=61, kotlin_module=b"\x00\x01pkg") -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
        z.writestr("META-INF/com.watermellonstudios_audio_release.kotlin_module", kotlin_module)
        for c in classes:
            z.writestr(c, class_bytes(major))
    return buf.getvalue()


CLANG = b"Android (13676358, +pgo, based on r522817d) clang version 18.0.3\0Linker: LLD 18.0.3\0"
CLANG_OTRO = b"Android (16248370, +pgo, based on r574158c) clang version 21.0.0\0Linker: LLD 21.0.0\0"
NEEDED = ["libc++_shared.so", "libc.so", "liblog.so", "liboboe.so"]
CLASSES = ["com/watermellonstudios/audio/api/AudioEngine.class"]

# Lo que arma un lado del self-test. Cada caso cambia UNA de estas perillas del
# lado head y exige ver EXACTAMENTE la clave de ese comparador.
DEFAULT_SIDE = dict(
    extra_entry=False, manifest="29", min_compile_sdk="36", meta=True, so=True,
    so_align=16384, so_comment=CLANG, so_needed=NEEDED, so_soname="libwatermelon_audio.so", so_pad=0,
    blob=b"\x01\x02", classes=CLASSES, major=61, kotlin_module=b"\x00\x01pkg", pad=0,
    packaging="aar", core_ktx="1.18.0", scope="runtime", pom_type=None, pom_optional=None,
    pom_classifier=None, exclusions=("org.jetbrains.kotlin:kotlin-stdlib-common",),
    gradle="9.7.1", component_attr="release", variant_attr="java-runtime", module_excludes=(
        "org.jetbrains.kotlin:kotlin-stdlib-common",), available_at_module="audio-android",
    file_suffix="aar", constraint=None, sources_variant=True, publications=tuple(PUBLICATIONS),
)


def make_aar(k: dict) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("AndroidManifest.xml", '<manifest package="com.watermellonstudios.audio">'
                                          f'<uses-sdk android:minSdkVersion="{k["manifest"]}"/></manifest>')
        if k["meta"]:
            z.writestr("META-INF/com/android/build/gradle/aar-metadata.properties",
                       "aarFormatVersion=1.0\naarMetadataVersion=1.0\n"
                       + (f"minCompileSdk={k['min_compile_sdk']}\n" if k["min_compile_sdk"] else "")
                       + "minAndroidGradlePluginVersion=1.0.0\n")
        z.writestr("classes.jar", make_jar(k["classes"], major=k["major"], kotlin_module=k["kotlin_module"]))
        z.writestr("R.txt", "")
        z.writestr("assets/tabla.bin", k["blob"])
        # Relleno FIJO para que el AAR sintetico pese como para que los casos de un
        # solo comparador no crucen el 5 % por accidente y ensucien su clave.
        z.writestr("assets/relleno.bin", bytes(200_000))
        if k["so"]:
            z.writestr("jni/arm64-v8a/libwatermelon_audio.so",
                       make_elf(64, align=k["so_align"], comment=k["so_comment"], needed=k["so_needed"],
                                soname=k["so_soname"], pad=k["so_pad"]) + bytes(k["pad"]))
            z.writestr("jni/armeabi-v7a/libwatermelon_audio.so",
                       make_elf(32, align=4096, comment=CLANG, needed=NEEDED, soname="libwatermelon_audio.so"))
        if k["extra_entry"]:
            z.writestr("jni/arm64-v8a/libsorpresa.so",
                       make_elf(64, align=16384, comment=CLANG, needed=[], soname="libsorpresa.so"))
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
        deps = f"""<dependencies>
  <dependency><groupId>androidx.core</groupId><artifactId>core-ktx</artifactId>
    <version>{k['core_ktx']}</version><scope>{k['scope']}</scope>{extra}
    <exclusions>{excl}</exclusions></dependency>
  <dependency><groupId>org.jetbrains.kotlin</groupId><artifactId>kotlin-stdlib</artifactId>
    <version>2.4.20</version><scope>compile</scope></dependency>
</dependencies>"""
    packaging = k["packaging"] if artifact == AAR_PUBLICATION else "pom"
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0">
  <modelVersion>4.0.0</modelVersion><groupId>{GROUP}</groupId>
  <artifactId>{artifact}</artifactId><version>{version}</version>
  <packaging>{packaging}</packaging>{deps}
</project>""".encode()


RUNTIME_VARIANT = "releaseRuntimeElements-published"


def make_module(artifact: str, version: str, k: dict) -> bytes:
    variants = []
    comp = {"group": GROUP, "module": artifact, "version": version,
            "attributes": {"org.gradle.status": k["component_attr"]}}
    if artifact == "audio":
        variants.append({"name": "releaseApiElements-published",
                         "attributes": {"org.gradle.usage": "java-api"},
                         "available-at": {"url": f"../../{k['available_at_module']}/{version}/x.module",
                                          "group": GROUP, "module": k["available_at_module"],
                                          "version": version}})
    else:
        comp["url"] = f"../../audio/{version}/audio-{version}.module"
        deps = []
        if artifact == AAR_PUBLICATION:
            deps.append({"group": "androidx.core", "module": "core-ktx", "version": {"requires": k["core_ktx"]},
                         "excludes": [{"group": e.split(":")[0], "module": e.split(":")[1]}
                                      for e in k["module_excludes"]]})
        var = {"name": RUNTIME_VARIANT,
               "attributes": {"org.gradle.usage": k["variant_attr"]},
               "dependencies": deps,
               "files": [{"name": f"{artifact}-{version}.{k['file_suffix']}",
                          "url": f"{artifact}-{version}.{k['file_suffix']}", "size": 123, "sha256": "x"}]}
        if k["constraint"] and artifact == AAR_PUBLICATION:
            var["dependencyConstraints"] = [{"group": "androidx.core", "module": "core",
                                             "version": {"requires": k["constraint"]}}]
        variants.append(var)
        if k["sources_variant"]:
            variants.append({"name": "releaseSourcesElements-published",
                             "attributes": {"org.gradle.category": "documentation"}})
    return json.dumps({"formatVersion": "1.1", "component": comp,
                       "createdBy": {"gradle": {"version": k["gradle"]}},
                       "variants": variants}).encode()


def write_side(root: Path, version: str, **overrides) -> None:
    k = dict(DEFAULT_SIDE)
    unknown = set(overrides) - set(k)
    if unknown:
        raise ValueError(f"perilla desconocida en el self-test: {unknown}")
    k.update(overrides)
    root.mkdir(parents=True, exist_ok=True)
    for art in k["publications"]:
        (root / f"{art}-{version}.pom").write_bytes(make_pom(art, version, k))
        (root / f"{art}-{version}.module").write_bytes(make_module(art, version, k))
    (root / f"{AAR_PUBLICATION}-{version}.aar").write_bytes(make_aar(k))


SO64 = "aar/so:jni/arm64-v8a/libwatermelon_audio.so"
ANDROID_DEP = "pom:audio-android/dep:androidx.core:core-ktx"
MODULE_VAR = f"module:audio-android/variant:{RUNTIME_VARIANT}"

# (nombre, perillas del head, claves que TIENEN que aparecer — y NINGUNA otra)
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
    ("blob binario", {"blob": b"\x01\x03"}, {"aar/blob:assets/tabla.bin"}),
    ("clases del jar", {"classes": CLASSES + ["com/watermellonstudios/audio/Nueva.class"]},
     {"aar/class:classes.jar!com/watermellonstudios/audio/Nueva.class"}),
    ("major del bytecode", {"major": 65}, {"aar/class-major:classes.jar"}),
    (".kotlin_module", {"kotlin_module": b"\x00\x02pkg"},
     {"aar/jar-entry:classes.jar!META-INF/com.watermellonstudios_audio_release.kotlin_module"}),
    ("POM packaging", {"packaging": "jar"}, {"pom:audio-android/packaging"}),
    ("POM version", {"core_ktx": "1.19.1"}, {ANDROID_DEP, f"{MODULE_VAR}/dep:androidx.core:core-ktx"}),
    ("POM scope", {"scope": "compile"}, {ANDROID_DEP}),
    ("POM type", {"pom_type": "aar"}, {ANDROID_DEP}),
    ("POM optional", {"pom_optional": "true"}, {ANDROID_DEP}),
    ("POM classifier", {"pom_classifier": "sources"}, {ANDROID_DEP, f"{ANDROID_DEP}:sources"}),
    ("POM exclusiones", {"exclusions": ()}, {ANDROID_DEP}),
    ("module createdBy", {"gradle": "9.8.0"},
     {f"module:{a}/createdBy.gradle" for a in PUBLICATIONS}),
    ("module component", {"component_attr": "integration"},
     {f"module:{a}/component.attrs" for a in PUBLICATIONS}),
    ("module atributo de variante", {"variant_attr": "java-api"},
     {f"module:{a}/variant:{RUNTIME_VARIANT}/attr:org.gradle.usage" for a in PUBLICATIONS if a != "audio"}),
    ("module excludes", {"module_excludes": ()}, {f"{MODULE_VAR}/dep:androidx.core:core-ktx"}),
    ("module available-at", {"available_at_module": "audio-otro"},
     {"module:audio/variant:releaseApiElements-published/available-at"}),
    ("module archivo", {"file_suffix": "zip"},
     {f"module:{a}/variant:{RUNTIME_VARIANT}/file:{a}-<VERSION>.{s}" for a in PUBLICATIONS if a != "audio"
      for s in ("aar", "zip")}),
    ("module constraint", {"constraint": "1.19.1"}, {f"{MODULE_VAR}/constraint:androidx.core:core"}),
    ("module variante que desaparece", {"sources_variant": False},
     {f"module:{a}/variant:releaseSourcesElements-published{s}" for a in PUBLICATIONS if a != "audio"
      for s in ("", "/attr:org.gradle.category")}),
    ("AAR > 5 %", {"pad": 400_000}, {SIZE_KEY, f"{SO64}/sha"}),
]

# Lo que tiene que salir con codigo 2 (no poder leer) y no con 0 ni con 1.
UNREADABLE_CASES = [
    ("AAR sin aar-metadata", {"meta": False}),
    ("AAR sin .so", {"so": False}),
    ("falta una publicacion", {"publications": tuple(PUBLICATIONS[:-1])}),
]


def self_test() -> int:
    print("self-test de diff-published-artifact:")
    failures: list[str] = []

    def run(head_kw: dict, expectations: dict) -> tuple[int, dict, set[str]]:
        with tempfile.TemporaryDirectory() as t:
            write_side(Path(t) / "base", "2.21.0")
            write_side(Path(t) / "head", "2.21.0-local", **head_kw)
            base = DirSource(Path(t) / "base", "2.21.0", "base")
            head = DirSource(Path(t) / "head", "2.21.0-local", "head")
            diffs, _rows, _na, _nb = observe(base, head)
            rc, detail = verdict(diffs, expectations, quiet=True)
            return rc, detail, {k for k, _, _ in diffs}

    def check(name: str, ok: bool, why: str) -> None:
        print(f"  {'ok ' if ok else 'MAL'} {name}" + ("" if ok else f" — {why}"))
        if not ok:
            failures.append(name)

    # 0. Mismo artefacto, otra version: CERO diferencias. Si esto falla, la
    #    normalizacion de version esta rota y todo lo demas es ruido.
    rc, _d, seen = run({}, {})
    check("identico salvo la version -> verde", rc == 0 and not seen, f"rc={rc} vio {sorted(seen)}")

    # 1. Un caso por comparador: rojo, y EXACTAMENTE las claves de ese comparador,
    #    todas como "sin declarar". Un rc=1 de otro origen no cuenta.
    for name, kw, want in COMPARATOR_CASES:
        rc, detail, seen = run(kw, {})
        ok = rc == 1 and seen == want and set(detail["undeclared"]) == want
        check(f"comparador: {name}", ok, f"rc={rc} vio {sorted(seen)} esperaba {sorted(want)}")

    # 2. El trinquete es sobre el VALOR (B1).
    bump = {"min_compile_sdk": "37", "core_ktx": "1.19.1", "scope": "runtime"}
    rc, _d, seen = run(bump, {})
    exact = {}
    with tempfile.TemporaryDirectory() as t:
        write_side(Path(t) / "base", "2.21.0")
        write_side(Path(t) / "head", "2.21.0-local", **bump)
        diffs, *_ = observe(DirSource(Path(t) / "base", "2.21.0", "b"),
                            DirSource(Path(t) / "head", "2.21.0-local", "h"))
        exact = {k: (va, vb, "s2") for k, va, vb in diffs}
    rc, _d, _s = run(bump, exact)
    check("el bump declarado con sus valores -> verde", rc == 0 and len(exact) == 3, f"rc={rc} {sorted(exact)}")
    # A: el mismo valor base, OTRO head (minCompileSdk 99).
    rc, d, _s = run(dict(bump, min_compile_sdk="99"), exact)
    check("A: declarado 36 -> 37, observado 36 -> 99 -> rojo",
          rc == 1 and d["wrong_value"] == ["aar/metadata:minCompileSdk"], f"rc={rc} {d}")
    # B: la dependencia con otra version Y otro scope.
    rc, d, _s = run(dict(bump, core_ktx="9.9.9", scope="compile"), exact)
    check("B: core-ktx declarado 1.19.1 runtime, observado 9.9.9 compile -> rojo",
          rc == 1 and ANDROID_DEP in d["wrong_value"], f"rc={rc} {d}")
    # C: la clave declarada DESAPARECE del head: 36 -> (ausente) no es 36 -> 37.
    rc, d, _s = run(dict(bump, min_compile_sdk=None), exact)
    check("C: declarado 36 -> 37, la clave desaparece (36 -> ausente) -> rojo",
          rc == 1 and d["wrong_value"] == ["aar/metadata:minCompileSdk"], f"rc={rc} {d}")
    rc, d, _s = run({"pom_classifier": "sources", "core_ktx": "1.19.1"},
                    {ANDROID_DEP: ("1.18.0 scope=runtime type=jar optional=false "
                                   "excl=[org.jetbrains.kotlin:kotlin-stdlib-common]",
                                   "1.19.1 scope=runtime type=jar optional=false "
                                   "excl=[org.jetbrains.kotlin:kotlin-stdlib-common]", "x")})
    check("C': la dependencia declarada se muda a otra clave (classifier) -> rojo",
          rc == 1 and ANDROID_DEP in d["wrong_value"], f"rc={rc} {d}")
    # D: una declaracion que no se reproduce.
    rc, d, _s = run({}, {"aar/metadata:minCompileSdk": ("36", "37", "x")})
    check("declaracion que no se reproduce -> rojo",
          rc == 1 and d["unobserved"] == ["aar/metadata:minCompileSdk"], f"rc={rc} {d}")

    # 3. La sintaxis del archivo de declaraciones.
    try:
        exp = read_expectations(["aar/metadata:minCompileSdk = 36 -> 37   # razon"], None)
        ok = exp == {"aar/metadata:minCompileSdk": ("36", "37", "razon")}
    except ReadError:
        ok = False
    check("parser: 'CLAVE = BASE -> HEAD # razon'", ok, "no parsea la forma canonica")
    for bad in ["aar/metadata:minCompileSdk", "aar/metadata:minCompileSdk = 37"]:
        try:
            read_expectations([bad], None)
            check(f"parser rechaza {bad!r}", False, "la acepto")
        except ReadError:
            check(f"parser rechaza {bad!r}", True, "")

    # 4. Lo ilegible sale con codigo 2 — por main(), que es lo que corre el usuario.
    for name, kw in UNREADABLE_CASES:
        with tempfile.TemporaryDirectory() as t:
            write_side(Path(t) / "base", "2.21.0")
            write_side(Path(t) / "head", "2.21.0-local", **kw)
            try:
                observe(DirSource(Path(t) / "base", "2.21.0", "b"), DirSource(Path(t) / "head", "2.21.0-local", "h"))
                check(f"ilegible: {name}", False, "no lanzo ReadError")
            except ReadError:
                check(f"ilegible: {name}", True, "")
    try:
        elf_facts(b"no soy un elf", "x.so")
        check("ilegible: un .so que no es ELF", False, "no lanzo ReadError")
    except ReadError:
        check("ilegible: un .so que no es ELF", True, "")
    try:
        elf_facts(b"\x7fELF\x02\x01" + bytes(10), "x.so")
        check("ilegible: un ELF truncado", False, "no lanzo ReadError")
    except ReadError:
        check("ilegible: un ELF truncado", True, "")
    except Exception as e:  # noqa: BLE001 — tiene que ser ReadError, no cualquier cosa
        check("ilegible: un ELF truncado", False, f"lanzo {type(e).__name__}, no ReadError")
    with contextlib.redirect_stderr(io.StringIO()):
        rc = run_main(["--against-local", "no-existe", "--local-version", "tampoco", "--m2", "/nonexistent"])
    check("main: lo ilegible sale con 2", rc == 2, f"rc={rc}")

    # 5. El parser de ELF lee lo que el generador escribio, en 64 y en 32 bits.
    f64 = elf_facts(make_elf(64, align=16384, comment=CLANG, needed=NEEDED, soname="liba.so"), "a")
    f32 = elf_facts(make_elf(32, align=4096, comment=CLANG, needed=["libc.so"], soname=None), "b")
    ok = (f64["pt_load_align_min"] == "16384" and f32["pt_load_align_min"] == "4096"
          and "clang version 18.0.3" in f64["comment"] and "LLD" in f32["comment"]
          and f64["needed"] == ",".join(sorted(NEEDED)) and f32["needed"] == "libc.so"
          and f64["soname"] == "liba.so" and f32["soname"] == ABSENT)
    check("parser ELF 64/32: alineacion, .comment, DT_NEEDED, DT_SONAME", ok, f"{f64} {f32}")

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
    ap.add_argument("--expect-file", help="una declaracion por linea, ' # razon' opcional")
    ap.add_argument("--print-declarations", action="store_true",
                    help="imprime lo observado en la sintaxis de --expect-file y sale con 0")
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
            diffs, *_ = observe(base, head)
            for k, va, vb in diffs:
                print(f"{k} = {va} -> {vb}   # RAZON")
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
