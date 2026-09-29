#!/usr/bin/env python3
"""REQ-047 S2 — el artefacto que se publica, diffeado contra una version del registro.

POR QUE EXISTE
--------------
Un bump de build (AGP, NDK, Oboe, compileSdk) puede cambiar lo que recibe el
consumidor SIN tocar una linea de codigo y con todos los gates en verde: una
entrada de mas en el AAR, un `.so` alineado a 4 KB en vez de 16 KB, un
`minCompileSdk` que sube, una dependencia transitiva que cambia de version en el
POM. Nada de eso lo ve un test: lo ve comparar el artefacto.

Este script compara lo que `publishToMavenLocal` dejo en `~/.m2` contra una
version ya publicada en GitHub Packages, HECHO POR HECHO, y exige que cada
diferencia este DECLARADA. Lo usan S2, S3 y el cierre de REQ-047.

    python3 scripts/diff-published-artifact.py --self-test
    ./gradlew :audio:publishToMavenLocal -Pversion=2.21.0-local
    GPR_KEY=... python3 scripts/diff-published-artifact.py \\
        --against 2.21.0 --local-version 2.21.0-local \\
        --expect 'aar/metadata:minCompileSdk' --expect ...

QUE COMPARA
-----------
Por cada publicacion (`audio`, `audio-android`, `audio-iosarm64`,
`audio-iossimulatorarm64`):
  - POM: packaging y cada dependencia (version y scope).
  - `.module` (Gradle Module Metadata): por variante, sus atributos, sus
    dependencias (con `requires`/`strictly`/`prefers`), `available-at` y los
    NOMBRES de sus archivos. No compara tamaños ni hashes de archivos (cambian
    en cualquier rebuild) ni `createdBy` salvo la version de Gradle.
Y del AAR de `audio-android`:
  - el conjunto de entradas del zip;
  - `aar-metadata.properties`, clave por clave;
  - el contenido de cada archivo de texto (manifest, R.txt, proguard...);
  - las clases de cada `.jar` interno (la superficie compilada);
  - por cada `.so`: la alineacion minima de sus PT_LOAD y su seccion `.comment`
    (que nombra el compilador que lo genero). El ELF se lee aca, en Python: no
    depende de llvm-readelf ni del NDK.
El TAMAÑO se imprime siempre, entrada por entrada, pero solo es una diferencia
si el AAR crece o se achica mas del 5 % (el no funcional de REQ-047).

La VERSION se normaliza: `2.21.0` y `2.21.0-local` se leen igual en nombres de
archivo y en las referencias entre nuestras propias publicaciones.

DECLARAR NO ES SILENCIAR: ES UN TRINQUETE
-----------------------------------------
Cada `--expect CLAVE` (o una linea `CLAVE  # razon` en `--expect-file`) declara
que esa diferencia es esperada. El veredicto es:
  - diferencia NO declarada           -> ROJO
  - declaracion que NO se reproduce   -> ROJO (declarar de mas tapa un cambio
                                          futuro; es el mismo trinquete que
                                          rt-safety-baseline y mechanism-callers)
Asi una diferencia nueva aparece en el diff del PR que la agrega, no despues.

"NO PUDE BAJAR" / "NO PUDE LEER" NUNCA ES UN PASE
-------------------------------------------------
Una publicacion que falta de un lado, un `.so` que no parsea como ELF o un AAR
sin `aar-metadata.properties` FALLAN. Un diff de cero hechos no es un diff vacio.

LA CREDENCIAL
-------------
Se lee de `GPR_KEY` (y `GPR_USER`, opcional) en el entorno de ESTE proceso.
Nunca se imprime, nunca se escribe a disco, nunca va en la URL. Lo bajado se
cachea en `build/published-artifacts/<version>/` (ignorado por git).
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import os
import re
import struct
import sys
import tempfile
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
TEXT_SUFFIXES = (".xml", ".txt", ".pro", ".properties", ".json", ".MF", ".kotlin_module")
VERSION_TOKEN = "<VERSION>"


class ReadError(Exception):
    """Algo que no se pudo leer o bajar. Nunca se convierte en un pase."""


# =============================================================================
# ELF: lo minimo para PT_LOAD y .comment, de 32 y 64 bits, little endian.
# =============================================================================

PT_LOAD = 1


def elf_facts(data: bytes, where: str) -> dict[str, str]:
    if data[:4] != b"\x7fELF":
        raise ReadError(f"{where}: no es un ELF")
    ei_class, ei_data = data[4], data[5]
    if ei_data != 1:
        raise ReadError(f"{where}: ELF big endian, no soportado")
    if ei_class == 2:
        (e_phoff, e_shoff) = struct.unpack_from("<QQ", data, 0x20)
        (e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHHHH", data, 0x36)
        ph_fmt, sh_fmt = "<IIQQQQQQ", "<IIQQQQIIQQ"
    elif ei_class == 1:
        (e_phoff, e_shoff) = struct.unpack_from("<II", data, 0x1C)
        (e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHHHH", data, 0x2A)
        ph_fmt, sh_fmt = "<IIIIIIII", "<IIIIIIIIII"
    else:
        raise ReadError(f"{where}: EI_CLASS {ei_class} desconocido")

    aligns = []
    for i in range(e_phnum):
        ph = struct.unpack_from(ph_fmt, data, e_phoff + i * e_phentsize)
        # p_type es el campo 0 y p_align el 7 en las dos clases (el orden de
        # los campos del medio cambia entre 32 y 64 bits; esos dos no).
        if ph[0] == PT_LOAD:
            aligns.append(ph[7])
    if not aligns:
        raise ReadError(f"{where}: sin PT_LOAD")

    sections = []
    for i in range(e_shnum):
        sh = struct.unpack_from(sh_fmt, data, e_shoff + i * e_shentsize)
        # sh_name, sh_offset y sh_size son los campos 0, 4 y 5 en las dos clases.
        sections.append((sh[0], sh[4], sh[5]))
    comment = "(sin .comment)"
    if sections and e_shstrndx < len(sections):
        _n, str_off, str_size = sections[e_shstrndx]
        strtab = data[str_off:str_off + str_size]
        for name_off, off, size in sections:
            end = strtab.find(b"\0", name_off)
            if strtab[name_off:end] == b".comment":
                parts = [p.decode("utf-8", "replace") for p in data[off:off + size].split(b"\0") if p]
                comment = " | ".join(sorted(set(parts)))
    return {
        "pt_load_align_min": str(min(aligns)),
        "comment": comment,
    }


# =============================================================================
# Extraccion de hechos. Cada hecho es CLAVE -> valor (str). Un conjunto se
# expande a una clave por miembro con valor "presente".
# =============================================================================

def normalize(text: str, version: str) -> str:
    return text.replace(version, VERSION_TOKEN)


def pom_facts(raw: bytes, artifact: str, version: str) -> dict[str, str]:
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as e:
        raise ReadError(f"POM de {artifact}: {e}")
    ns = {"m": "http://maven.apache.org/POM/4.0.0"}
    facts: dict[str, str] = {}
    pref = f"pom:{artifact}"
    pk = root.find("m:packaging", ns)
    facts[f"{pref}/packaging"] = pk.text if pk is not None else "(default)"
    deps = root.findall("m:dependencies/m:dependency", ns)
    for d in deps:
        g = d.findtext("m:groupId", "", ns)
        a = d.findtext("m:artifactId", "", ns)
        v = normalize(d.findtext("m:version", "", ns), version)
        s = d.findtext("m:scope", "compile", ns)
        facts[f"{pref}/dep:{g}:{a}"] = f"{v} ({s})"
    return facts


def module_facts(raw: bytes, artifact: str, version: str) -> dict[str, str]:
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as e:
        raise ReadError(f".module de {artifact}: {e}")
    facts: dict[str, str] = {}
    pref = f"module:{artifact}"
    facts[f"{pref}/formatVersion"] = str(doc.get("formatVersion"))
    facts[f"{pref}/createdBy.gradle"] = str(doc.get("createdBy", {}).get("gradle", {}).get("version"))
    variants = doc.get("variants", [])
    if not variants:
        raise ReadError(f".module de {artifact}: sin variantes")
    for var in variants:
        vp = f"{pref}/variant:{var['name']}"
        facts[vp] = "presente"
        for k, v in sorted(var.get("attributes", {}).items()):
            facts[f"{vp}/attr:{k}"] = str(v)
        for d in var.get("dependencies", []):
            spec = d.get("version", {})
            spec_txt = ",".join(f"{k}={normalize(str(spec[k]), version)}" for k in sorted(spec))
            facts[f"{vp}/dep:{d['group']}:{d['module']}"] = spec_txt or "(sin version)"
        for d in var.get("dependencyConstraints", []):
            spec = d.get("version", {})
            spec_txt = ",".join(f"{k}={normalize(str(spec[k]), version)}" for k in sorted(spec))
            facts[f"{vp}/constraint:{d['group']}:{d['module']}"] = spec_txt
        if "available-at" in var:
            at = var["available-at"]
            facts[f"{vp}/available-at"] = normalize(f"{at['group']}:{at['module']}:{at['version']}", version)
        for f in var.get("files", []):
            facts[f"{vp}/file:{normalize(f['name'], version)}"] = "presente"
    return facts


def jar_classes(data: bytes, where: str) -> list[str]:
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            return sorted(n for n in z.namelist() if n.endswith(".class"))
    except zipfile.BadZipFile as e:
        raise ReadError(f"{where}: {e}")


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
            for c in jar_classes(data, n):
                facts[f"aar/class:{n}!{c}"] = "presente"
        elif n.endswith(TEXT_SUFFIXES):
            text = normalize(data.decode("utf-8", "replace"), version)
            facts[f"aar/text:{n}"] = "sha256:" + hashlib.sha256(text.encode()).hexdigest()[:16]
        else:
            facts[f"aar/blob:{n}"] = "sha256:" + hashlib.sha256(data).hexdigest()[:16]
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


def collect(src: Source, publications: list[str]) -> tuple[dict[str, str], dict[str, int]]:
    facts: dict[str, str] = {}
    sizes: dict[str, int] = {}
    for art in publications:
        facts.update(pom_facts(src.get(art, "pom"), art, src.version))
        facts.update(module_facts(src.get(art, "module"), art, src.version))
        if art == AAR_PUBLICATION:
            f, s = aar_facts(src.get(art, "aar"), src.version)
            facts.update(f)
            sizes.update(s)
    return facts, sizes


# =============================================================================
# El diff y el veredicto
# =============================================================================

def diff_facts(a: dict[str, str], b: dict[str, str]) -> list[tuple[str, str, str]]:
    out = []
    for k in sorted(set(a) | set(b)):
        va, vb = a.get(k, "(ausente)"), b.get(k, "(ausente)")
        if va != vb:
            out.append((k, va, vb))
    return out


def size_diffs(sa: dict[str, int], sb: dict[str, int]) -> tuple[list[tuple[str, int, int]], bool]:
    rows = []
    for k in sorted(set(sa) | set(sb)):
        if k.endswith(".so") or k.endswith(".jar") or k == "(AAR entero)":
            rows.append((k, sa.get(k, 0), sb.get(k, 0)))
    total_a, total_b = sa.get("(AAR entero)", 0), sb.get("(AAR entero)", 0)
    out_of_band = total_a > 0 and abs(total_b - total_a) / total_a > SIZE_TOLERANCE
    return rows, out_of_band


def read_expectations(args) -> dict[str, str]:
    exp: dict[str, str] = {}
    for e in args.expect or []:
        exp[e] = "(--expect)"
    if args.expect_file:
        for line in Path(args.expect_file).read_text(encoding="utf-8").splitlines():
            body = line.split("#", 1)
            key = body[0].strip()
            if key:
                exp[key] = body[1].strip() if len(body) > 1 else "(sin razon)"
    return exp


def verdict(diffs, size_oob, expectations: dict[str, str], quiet=False) -> int:
    keys = {k for k, _, _ in diffs}
    if size_oob:
        keys.add("aar/size")
    undeclared = sorted(k for k in keys if k not in expectations)
    unobserved = sorted(k for k in expectations if k not in keys)
    if not quiet:
        print(f"\ndiferencias: {len(keys)}  |  declaradas: {len(expectations)}")
        for k, va, vb in diffs:
            mark = "declarada" if k in expectations else "NO DECLARADA"
            print(f"  [{mark}] {k}\n      {va}  ->  {vb}")
        if size_oob:
            mark = "declarada" if "aar/size" in expectations else "NO DECLARADA"
            print(f"  [{mark}] aar/size  (fuera del {int(SIZE_TOLERANCE * 100)} %)")
        for k in unobserved:
            print(f"  [DECLARADA Y NO SE REPRODUCE] {k}  # {expectations[k]}")
    if undeclared or unobserved:
        if not quiet:
            print(f"\n\033[31mROJO\033[0m — {len(undeclared)} sin declarar, {len(unobserved)} declaradas de mas")
        return 1
    if not quiet:
        print("\n\033[32mVERDE\033[0m — cada diferencia esta declarada y cada declaracion se reproduce")
    return 0


def print_sizes(rows):
    print("\ntamaños (bytes):")
    for k, a, b in rows:
        pct = f"{(b - a) / a * 100:+.2f} %" if a else "nuevo"
        print(f"  {k:<60} {a:>10} -> {b:>10}  {pct}")


def compare(base: Source, head: Source, publications, expectations, quiet=False) -> int:
    fa, sa = collect(base, publications)
    fb, sb = collect(head, publications)
    if not quiet:
        print(f"base: {base.label}  —  {len(fa)} hechos")
        print(f"head: {head.label}  —  {len(fb)} hechos")
    diffs = diff_facts(fa, fb)
    rows, oob = size_diffs(sa, sb)
    if not quiet:
        print_sizes(rows)
    return verdict(diffs, oob, expectations, quiet)


# =============================================================================
# self-test: que el instrumento PUEDE fallar
# =============================================================================

def make_elf64(align: int, comment: bytes) -> bytes:
    """Un ELF64 minimo: un PT_LOAD con `align` y una seccion .comment."""
    ehsize, phentsize, shentsize = 64, 56, 64
    shstr = b"\0.shstrtab\0.comment\0"
    phoff = ehsize
    data_off = phoff + phentsize
    comment_off = data_off
    shstr_off = comment_off + len(comment)
    shoff = shstr_off + len(shstr)
    shoff += (-shoff) % 8
    ident = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
    eh = ident + struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, phoff, shoff, 0,
                             ehsize, phentsize, 1, shentsize, 3, 1)
    ph = struct.pack("<IIQQQQQQ", PT_LOAD, 5, 0, 0, 0, 0x1000, 0x1000, align)
    body = eh + ph + comment + shstr
    body += bytes(shoff - len(body))
    sh_null = bytes(shentsize)
    sh_str = struct.pack("<IIQQQQIIQQ", 1, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    sh_com = struct.pack("<IIQQQQIIQQ", 11, 1, 0x30, 0, comment_off, len(comment), 0, 0, 1, 1)
    return body + sh_null + sh_str + sh_com


def make_elf32(align: int, comment: bytes) -> bytes:
    ehsize, phentsize, shentsize = 52, 32, 40
    shstr = b"\0.shstrtab\0.comment\0"
    phoff = ehsize
    comment_off = phoff + phentsize
    shstr_off = comment_off + len(comment)
    shoff = shstr_off + len(shstr)
    shoff += (-shoff) % 4
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    eh = ident + struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, 0, phoff, shoff, 0,
                             ehsize, phentsize, 1, shentsize, 3, 1)
    ph = struct.pack("<IIIIIIII", PT_LOAD, 0, 0, 0, 0x1000, 0x1000, 5, align)
    body = eh + ph + comment + shstr
    body += bytes(shoff - len(body))
    sh_null = bytes(shentsize)
    sh_str = struct.pack("<IIIIIIIIII", 1, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    sh_com = struct.pack("<IIIIIIIIII", 11, 1, 0x30, 0, comment_off, len(comment), 0, 0, 1, 1)
    return body + sh_null + sh_str + sh_com


def make_jar(classes: list[str]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
        for c in classes:
            z.writestr(c, b"\xca\xfe\xba\xbe")
    return buf.getvalue()


CLANG = b"Android (13676358, +pgo, based on r522817d) clang version 18.0.3\0Linker: LLD 18.0.3\0"


def make_aar(version: str, *, extra_entry=False, so_align_arm64=16384, min_compile_sdk="36",
             pad=0) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("AndroidManifest.xml", '<manifest package="com.watermellonstudios.audio">'
                                          '<uses-sdk android:minSdkVersion="29"/></manifest>')
        z.writestr("META-INF/com/android/build/gradle/aar-metadata.properties",
                   f"aarFormatVersion=1.0\naarMetadataVersion=1.0\nminCompileSdk={min_compile_sdk}\n"
                   "minAndroidGradlePluginVersion=1.0.0\n")
        z.writestr("classes.jar", make_jar(["com/watermellonstudios/audio/api/AudioEngine.class"]))
        z.writestr("R.txt", "")
        z.writestr("jni/arm64-v8a/libwatermelon_audio.so", make_elf64(so_align_arm64, CLANG) + bytes(pad))
        z.writestr("jni/armeabi-v7a/libwatermelon_audio.so", make_elf32(4096, CLANG))
        if extra_entry:
            z.writestr("jni/arm64-v8a/libsorpresa.so", make_elf64(16384, CLANG))
    return buf.getvalue()


def make_pom(artifact: str, version: str, core_ktx="1.18.0") -> bytes:
    deps = ""
    if artifact == AAR_PUBLICATION:
        deps = f"""<dependencies>
  <dependency><groupId>androidx.core</groupId><artifactId>core-ktx</artifactId>
    <version>{core_ktx}</version><scope>runtime</scope></dependency>
  <dependency><groupId>org.jetbrains.kotlin</groupId><artifactId>kotlin-stdlib</artifactId>
    <version>2.4.20</version><scope>compile</scope></dependency>
</dependencies>"""
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0">
  <modelVersion>4.0.0</modelVersion><groupId>{GROUP}</groupId>
  <artifactId>{artifact}</artifactId><version>{version}</version>
  <packaging>{"aar" if artifact == AAR_PUBLICATION else "pom"}</packaging>{deps}
</project>""".encode()


def make_module(artifact: str, version: str, core_ktx="1.18.0") -> bytes:
    variants = []
    if artifact == "audio":
        variants.append({"name": "releaseApiElements-published",
                         "attributes": {"org.gradle.usage": "java-api"},
                         "available-at": {"url": f"../../audio-android/{version}/audio-android-{version}.module",
                                          "group": GROUP, "module": "audio-android", "version": version}})
    else:
        deps = []
        if artifact == AAR_PUBLICATION:
            deps.append({"group": "androidx.core", "module": "core-ktx", "version": {"requires": core_ktx}})
        variants.append({"name": "releaseRuntimeElements-published",
                         "attributes": {"org.gradle.usage": "java-runtime"},
                         "dependencies": deps,
                         "files": [{"name": f"{artifact}-{version}.aar", "size": 123, "sha256": "x"}]})
    return json.dumps({"formatVersion": "1.1", "component": {"group": GROUP, "module": artifact,
                       "version": version}, "createdBy": {"gradle": {"version": "9.7.1"}},
                       "variants": variants}).encode()


def write_side(root: Path, version: str, **kw) -> None:
    root.mkdir(parents=True, exist_ok=True)
    core_ktx = kw.pop("core_ktx", "1.18.0")
    for art in PUBLICATIONS:
        (root / f"{art}-{version}.pom").write_bytes(make_pom(art, version, core_ktx))
        (root / f"{art}-{version}.module").write_bytes(make_module(art, version, core_ktx))
    (root / f"{AAR_PUBLICATION}-{version}.aar").write_bytes(make_aar(version, **kw))


def self_test() -> int:
    print("self-test de diff-published-artifact:")
    failures = []

    def case(name, head_kw, expectations, want_rc, must_see=()):
        with tempfile.TemporaryDirectory() as t:
            base = Path(t) / "base"
            head = Path(t) / "head"
            write_side(base, "2.21.0")
            write_side(head, "2.21.0-local", **head_kw)
            fa, _ = collect(DirSource(base, "2.21.0", "base"), PUBLICATIONS)
            fb, _ = collect(DirSource(head, "2.21.0-local", "head"), PUBLICATIONS)
            seen = {k for k, _, _ in diff_facts(fa, fb)}
            rc = compare(DirSource(base, "2.21.0", "base"), DirSource(head, "2.21.0-local", "head"),
                         PUBLICATIONS, expectations, quiet=True)
            missing = [m for m in must_see if m not in seen and m != "aar/size"]
            ok = rc == want_rc and not missing
            print(f"  {'ok ' if ok else 'MAL'} {name}: rc={rc} (esperado {want_rc})"
                  + (f", no vio {missing}" if missing else ""))
            if not ok:
                failures.append(name)

    # 1. Mismo artefacto, otra version: CERO diferencias. Si esto falla, la
    #    normalizacion de version esta rota y todo lo demas es ruido.
    case("identico salvo la version -> verde", {}, {}, 0)
    # 2. Una entrada de mas en el AAR.
    case("una entrada de mas -> rojo", {"extra_entry": True}, {}, 1,
         must_see=["aar/entry:jni/arm64-v8a/libsorpresa.so"])
    # 3. Un .so de arm64 alineado a 4 KB en vez de 16 KB.
    case("un .so a 4 KB -> rojo", {"so_align_arm64": 4096}, {}, 1,
         must_see=["aar/so:jni/arm64-v8a/libwatermelon_audio.so/pt_load_align_min"])
    # 4. minCompileSdk y core-ktx, SIN declarar: rojo.
    bump = {"min_compile_sdk": "37", "core_ktx": "1.19.1"}
    bump_keys = ["aar/metadata:minCompileSdk",
                 "pom:audio-android/dep:androidx.core:core-ktx",
                 "module:audio-android/variant:releaseRuntimeElements-published/dep:androidx.core:core-ktx"]
    case("el bump de S2 sin declarar -> rojo", dict(bump), {}, 1, must_see=bump_keys)
    # 5. El mismo bump DECLARADO: verde.
    case("el bump de S2 declarado -> verde", dict(bump), {k: "s2" for k in bump_keys}, 0)
    # 6. Declarar de mas es rojo (trinquete): la declaracion no se reproduce.
    case("una declaracion que no se reproduce -> rojo", {}, {"aar/metadata:minCompileSdk": "x"}, 1)
    # 7. El AAR crece mas del 5 %: rojo aunque ningun hecho cambie.
    case("el AAR crece > 5 % -> rojo", {"pad": 400_000}, {}, 1, must_see=["aar/size"])

    # 8. Un ELF que no es ELF no puede leerse como "sin diferencias".
    try:
        elf_facts(b"no soy un elf", "x.so")
        failures.append("un .so ilegible paso")
        print("  MAL un .so ilegible no fallo")
    except ReadError:
        print("  ok  un .so ilegible -> ReadError, no pase")
    # 9. El parser de ELF lee lo que el generador escribio, en 64 y en 32 bits.
    f64 = elf_facts(make_elf64(16384, CLANG), "a")
    f32 = elf_facts(make_elf32(4096, CLANG), "b")
    if f64["pt_load_align_min"] != "16384" or f32["pt_load_align_min"] != "4096" \
            or "clang version 18.0.3" not in f64["comment"] or "LLD" not in f32["comment"]:
        failures.append("parser ELF")
        print(f"  MAL parser ELF: {f64} {f32}")
    else:
        print("  ok  parser ELF 64/32: alineacion y .comment")

    if failures:
        print(f"\n\033[31mself-test ROJO\033[0m — {len(failures)} casos: {failures}")
        return 1
    print("\n\033[32mself-test ok\033[0m — el instrumento puede fallar.")
    return 0


# =============================================================================

def local_version_default() -> str:
    for line in (REPO / "gradle.properties").read_text(encoding="utf-8").splitlines():
        m = re.match(r"^version=(.+)$", line.strip())
        if m:
            return m.group(1)
    raise ReadError("gradle.properties sin version=")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--against", help="version publicada en el registro (la base)")
    ap.add_argument("--against-local", metavar="VERSION",
                    help="usar una version de ~/.m2 como base en vez del registro")
    ap.add_argument("--local-version", help="version en ~/.m2 (default: la de gradle.properties)")
    ap.add_argument("--m2", default=str(Path.home() / ".m2" / "repository"))
    ap.add_argument("--cache", help="cache de lo bajado (default: build/published-artifacts/<version>)")
    ap.add_argument("--expect", action="append", help="clave de una diferencia esperada (repetible)")
    ap.add_argument("--expect-file", help="archivo con una clave por linea, '# razon' opcional")
    ap.add_argument("--publications", default=",".join(PUBLICATIONS))
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.against and not args.against_local:
        ap.error("falta --against VERSION (o --against-local VERSION)")

    try:
        pubs = [p for p in args.publications.split(",") if p]
        local_version = args.local_version or local_version_default()
        head = LocalM2(Path(args.m2), local_version)
        if args.against_local:
            base: Source = LocalM2(Path(args.m2), args.against_local)
        else:
            cache = Path(args.cache) if args.cache else REPO / "build" / "published-artifacts" / args.against
            base = Registry(args.against, cache)
        return compare(base, head, pubs, read_expectations(args))
    except ReadError as e:
        print(f"\033[31mNO PUDE LEER\033[0m — {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
