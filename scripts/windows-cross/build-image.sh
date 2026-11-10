#!/usr/bin/env bash
# Construye la imagen del cruce a Windows con los pins de .github/toolchain-pins.json
# (`_windows_cross.container`); el Dockerfile no repite ninguno. El tag se deriva del pin
# (GCC + 12 hex del digest de la base): un pin nuevo es una imagen nueva, nunca se pisa una vieja.
#
#   bash scripts/windows-cross/build-image.sh            # imprime el tag al final
#   docker run --rm -v "$PWD":/src -w /src <tag> bash scripts/gate.sh --only windows-cross
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
PINS="$REPO_ROOT/.github/toolchain-pins.json"

command -v docker >/dev/null 2>&1 || { echo "falta docker" >&2; exit 1; }
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python

# Una sola lectura: base, paquetes, herramientas, prefijo y tag, uno por linea.
mapfile -t F < <("$PY" - "$PINS" <<'PY'
import json, sys
c = json.load(open(sys.argv[1], encoding="utf-8"))["_windows_cross"]["container"]
digest = c["base_image"].split("sha256:")[1][:12]
print(c["base_image"])
print(" ".join(c["packages"]))
print(" ".join(c["tools"]))
print(c["prefix"])
print(f"wa-windows-cross:gcc{c['gcc']}-{digest}")
PY
)
[ "${#F[@]}" -eq 5 ] || { echo "no pude leer _windows_cross.container de $PINS" >&2; exit 1; }
BASE="${F[0]}"; PACKAGES="${F[1]}"; TOOLS="${F[2]}"; PREFIX="${F[3]}"; TAG="${F[4]%$'\r'}"
PREFIX="${PREFIX%$'\r'}"; BASE="${BASE%$'\r'}"; PACKAGES="${PACKAGES%$'\r'}"; TOOLS="${TOOLS%$'\r'}"

docker build -f "$HERE/Dockerfile" -t "$TAG" \
    --build-arg "BASE_IMAGE=$BASE" --build-arg "PACKAGES=$PACKAGES" \
    --build-arg "TOOLS=$TOOLS" --build-arg "CROSS_PREFIX=$PREFIX" "$HERE"
echo "$TAG"
