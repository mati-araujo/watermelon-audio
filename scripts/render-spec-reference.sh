#!/usr/bin/env bash
# render-spec-reference.sh — REQ-039 S1. LA REFERENCIA del arnes de conformidad.
#
# Renderiza `sf_spec_test.mid` con FluidSynth, SECO, para comparar contra lo que
# produce nuestro renderizador.
#
# 🔴 POR QUE SE RENDERIZA Y NO SE BAJA. El README del spec-test recomienda usar
# `FluidSynth 2.5.2, custom fx.flac`, y ese "custom fx" es literal: lleva reverb y
# chorus aplicados. Nuestro motor renderiza SECO —no tiene sends, que son
# REQ-040—, asi que comparar contra esa grabacion mide los efectos. La unica
# grabacion seca del repo upstream es un mp3 (con perdida) de hardware viejo.
# Ademas: un render se REGENERA y una grabacion publicada no.
#
# 🔴 `-R 0 -C 0`: reverb y chorus apagados. Es la misma receta que
# `render-corpus.sh` usa por el mismo motivo.
#
# 🔴 `-g 1`: ganancia 1,0, no el 0,2 que FluidSynth trae por default. El spec-test
# tiene pruebas de NIVEL (#11 atenuacion, #12 atenuacion negativa) y una ganancia
# arbitraria las moveria. Aun asi queda un sesgo global de ~1,4 dB contra nuestro
# render —convenciones distintas—, y por eso el criterio del test compara niveles
# RELATIVOS dentro de cada prueba y no absolutos. Ver test_sf_spec_conformance.cpp.
#
#   bash scripts/render-spec-reference.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${WMA_SPEC_TEST_DIR:-$REPO_ROOT/audio/src/main/cpp/core/tests/spec-test}"
FLUIDSYNTH_VERSION="2.6.0"
OUT="$DIR/reference-fluidsynth-${FLUIDSYNTH_VERSION}.wav"

command -v fluidsynth >/dev/null || { echo "falta fluidsynth" >&2; exit 1; }
have="$(fluidsynth --version 2>&1 | head -1 | sed -E 's/.*version ([0-9.]+).*/\1/')"
if [ "$have" != "$FLUIDSYNTH_VERSION" ]; then
    # Se para en vez de avisar, misma regla que render-corpus.sh: un render con
    # otra version es OTRA referencia, y una referencia que no es la que el test
    # espera produce un rojo que parece del motor.
    echo "fluidsynth $have, la receta fija $FLUIDSYNTH_VERSION." >&2
    echo "Poné WMA_RENDER_ANY_FLUIDSYNTH=1 para renderizar igual (no va a ser la referencia)." >&2
    [ "${WMA_RENDER_ANY_FLUIDSYNTH:-0}" = "1" ] || exit 1
fi

for f in sf_spec_test.sf2 sf_spec_test.mid; do
    [ -f "$DIR/$f" ] || { echo "falta $DIR/$f — corré scripts/fetch-spec-test.sh" >&2; exit 1; }
done

fluidsynth -ni -R 0 -C 0 -g 1 -r 44100 -F "$OUT" \
    "$DIR/sf_spec_test.sf2" "$DIR/sf_spec_test.mid" >/dev/null 2>&1

[ -s "$OUT" ] || { echo "fluidsynth no produjo audio" >&2; exit 1; }
printf 'referencia: %s (%s bytes)\n' "$OUT" "$(wc -c < "$OUT" | tr -d ' ')"
