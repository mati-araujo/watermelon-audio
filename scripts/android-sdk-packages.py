#!/usr/bin/env python3
# ============================================================================
# Los paquetes del SDK de Android que el CI instala, LEIDOS del catalogo
# (REQ-047 S3, tarea 3.1).
#
# El NDK y el CMake que usa AGP los fija `gradle/libs.versions.toml` (los lee
# build-logic: `ndkVersion = libs.version("ndk")`, `version = libs.version("cmake")`).
# Hasta REQ-047 los workflows los repetian a mano en tres lugares (`ci.yml` x2 y
# `publish.yml`), y un bump del catalogo que no tocara los tres dejaba al CI
# instalando un NDK que el build no usa mientras AGP bajaba el otro en silencio.
# Ahora hay UNA fuente: los workflows piden la lista a este script.
#
# Imprime un paquete por linea, en el formato de `sdkmanager --install`:
#   ndk;<version>
#   cmake;<version>
#
# Sale con 1 si el catalogo no declara alguna de las dos versiones o si una no
# tiene forma de version: "no pude leer el pin" no puede terminar en un
# `sdkmanager --install` sin argumentos que da verde.
#
# Usage:
#   python3 scripts/android-sdk-packages.py [--catalog RUTA]
# ============================================================================
import argparse
import re
import sys
import tomllib

KEYS = ("ndk", "cmake")
VERSION = re.compile(r"^[0-9]+(\.[0-9]+)+$")


def packages(catalog_path):
    try:
        with open(catalog_path, "rb") as fh:
            catalog = tomllib.load(fh)
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise SystemExit(f"android-sdk-packages: no pude leer {catalog_path}: {exc}")

    versions = catalog.get("versions")
    if not isinstance(versions, dict):
        raise SystemExit(f"android-sdk-packages: {catalog_path} no tiene tabla [versions]")

    out = []
    for key in KEYS:
        value = versions.get(key)
        if not isinstance(value, str) or not VERSION.match(value):
            raise SystemExit(
                f"android-sdk-packages: [versions].{key} en {catalog_path} "
                f"es {value!r}, no una version"
            )
        out.append(f"{key};{value}")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", default="gradle/libs.versions.toml")
    args = parser.parse_args()
    print("\n".join(packages(args.catalog)))


if __name__ == "__main__":
    main()
