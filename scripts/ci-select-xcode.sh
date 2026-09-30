#!/usr/bin/env bash
# ============================================================================
# Fija el Xcode de un job de macOS del CI y lo deja escrito en el log (REQ-047 S4).
#
#   bash scripts/ci-select-xcode.sh
#
# Hasta REQ-047 el CI usaba el Xcode DEFAULT de la imagen `macos-latest` y ningun
# log decia cual: el dia que la imagen cambiara de default, el job `ios` pasaria a
# compilar con otro Xcode en silencio. Un verde con Xcode 26.6 no dice nada sobre
# Xcode 27 (el mismo argumento de .github/toolchain-pins.json).
#
# LA UNICA FUENTE es la clave `xcode` de .github/toolchain-pins.json —la misma que
# valida la atestacion local—, con la forma "26.6 (17F113)". De ahi sale:
#   - la app que se selecciona: /Applications/Xcode_<version>.app, que es la
#     convencion de las imagenes de actions/runner-images;
#   - la comprobacion final: `xcodebuild -version`, normalizado con EL MISMO sed
#     que usa scripts/gate.sh al observar el toolchain local, tiene que dar
#     EXACTAMENTE el pin, build incluido. Otro build con el mismo numero de
#     version es otro Xcode.
#
# Falla (exit 1, con `::error::`) si la app no existe en la imagen o si el Xcode
# seleccionado no es el del pin: un cambio de Xcode es una decision que se toma
# bumpeando toolchain-pins.json, no una deriva de la imagen.
# ============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PINS="$REPO_ROOT/.github/toolchain-pins.json"

PIN="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["xcode"])' "$PINS")"
VERSION="${PIN%% *}"
if ! printf '%s' "$PIN" | grep -Eq '^[0-9]+(\.[0-9]+)+ \([0-9A-Za-z]+\)$'; then
    echo "::error::la clave xcode de $PINS no tiene la forma 'X.Y (BUILD)': '$PIN'"
    exit 1
fi

APP="${WMA_XCODE_APPS_DIR:-/Applications}/Xcode_${VERSION}.app"
if [ ! -d "$APP" ]; then
    echo "::error::no existe $APP en esta imagen (pin: $PIN). Instalados:"
    ls -d "${WMA_XCODE_APPS_DIR:-/Applications}"/Xcode*.app 2>/dev/null || true
    exit 1
fi

echo "Xcode fijado por .github/toolchain-pins.json: $PIN -> $APP"
sudo xcode-select -s "$APP"

# Lo que imprime el log, crudo: es la evidencia de que Xcode uso el job.
xcodebuild -version

GOT="$(xcodebuild -version | tr '\n' ' ' | sed -E 's/Xcode ([^ ]+) Build version ([^ ]+).*/\1 (\2)/')"
if [ "$GOT" != "$PIN" ]; then
    echo "::error::el Xcode seleccionado es '$GOT', el pin es '$PIN'"
    exit 1
fi
echo "Xcode verificado: $GOT"
