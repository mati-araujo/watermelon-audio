#!/usr/bin/env bash
# fetch-spec-test.sh — REQ-039 S1. Baja el SoundFont-Spec-Test y lo VERIFICA.
#
# QUE ES
# ------
# `mrbumpy409/SoundFont-Spec-Test` v3.1 (MIT): un banco `.sf2` y un `.mid` con 22
# pruebas nombradas que ejercitan el spec de SoundFont 2.04 punto por punto —entre
# ellas #13 velocity -> atenuacion y #14 velocity -> corte del filtro, que son las
# dos que este REQ existe para arreglar—. Es el instrumento con el que se mide si
# el motor reproduce el font como fue programado, y es DE TERCEROS a proposito: un
# arreglo verificado contra su propia implementacion no esta verificado.
#
#   bash scripts/fetch-spec-test.sh          # baja lo que falte y verifica todo
#   bash scripts/fetch-spec-test.sh --verify # solo verifica lo que ya esta
#
# 🔴 SE FIJA POR COMMIT, NO POR RAMA. El repo NO tiene tags —igual que el de
# GeneralUser GS, y por la misma razon que `render-corpus.sh` fija el commit y el
# sha256 del .sf2—: `main` es una rama movil y "v3.1" es una linea del README, no
# una referencia de git. Con el commit fijado, si upstream publica v3.2 manana
# este script sigue bajando exactamente los mismos bytes, y actualizarlo es un
# diff que se revisa.
#
# 🔴 SE BAJAN DOS ARCHIVOS, NO EL REPO. El arbol completo pesa **118 MB** y son
# casi todos grabaciones de referencia. Lo que hace falta —el `.mid` y el `.sf2`—
# pesa **1,4 MB**. Ver abajo por que las grabaciones no se usan.
#
# 🔴 LA REFERENCIA SE RENDERIZA ACA, NO SE BAJA. El README recomienda comparar
# contra `FluidSynth 2.5.2, custom fx.flac`, y ese "custom fx" es literal: la
# grabacion lleva reverb y chorus aplicados. Nuestro motor renderiza SECO —no
# tiene sends, que son REQ-040— asi que compararlo contra una grabacion con
# efectos mide los efectos. La unica grabacion seca del repo es un mp3 (con
# perdida) de hardware viejo. Asi que la referencia se produce con FluidSynth
# **2.6.0**, que es la version que este repo ya fija en `render-corpus.sh` y que
# ademas se puede REGENERAR — una grabacion publicada no.
#
# 🔴 SIN EL MATERIAL LOS TESTS SALEN **SKIPPED**, NUNCA PASSED. Es la regla de
# REQ-032: una corrida que no verifico no se puede leer como cobertura.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="${WMA_SPEC_TEST_DIR:-$REPO_ROOT/audio/src/main/cpp/engines/tests/spec-test}"

# --- el artefacto, fijado ----------------------------------------------------
# HEAD de `main` al 2026-09-10. El README de ese commit se titula v3.1 y fecha
# 2025-12-31; el commit es lo que se fija, el nombre de version es informativo.
SPEC_TEST_COMMIT="07e9de77db6f4c9fdfeacdf69b9c576e1f5f481b"
SPEC_TEST_BASE="https://raw.githubusercontent.com/mrbumpy409/SoundFont-Spec-Test/${SPEC_TEST_COMMIT}"

# archivo:sha256 — medidos el 2026-09-10 sobre ese commit.
FILES="
sf_spec_test.mid:53995f9ab0ebec98ace9721da37e2c1488e10ebf88bdaf7dc3657d87545c712e
sf_spec_test.sf2:07260275995c51824527fcf655d3c9545fb909e6452b806d9678a2de5cdd6c54
"

VERIFY_ONLY=0
[ "${1:-}" = "--verify" ] && VERIFY_ONLY=1

sha256_of() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else printf 'no hay shasum ni sha256sum\n' >&2; exit 1
    fi
}

mkdir -p "$DEST_DIR"
entries=0
failed=0

for entry in $FILES; do
    name="${entry%%:*}"
    want="${entry##*:}"
    entries=$((entries + 1))
    dest="$DEST_DIR/$name"

    if [ ! -f "$dest" ] && [ "$VERIFY_ONLY" -eq 0 ]; then
        printf '  bajando %s\n' "$name"
        # A un temporal y despues se mueve: un `curl` cortado a la mitad no puede
        # dejar un archivo con el nombre bueno y el contenido malo.
        tmp="$dest.part"
        if ! curl -sSfL -o "$tmp" "$SPEC_TEST_BASE/$name"; then
            printf '  FALLO la descarga de %s\n' "$name" >&2
            rm -f "$tmp"
            failed=$((failed + 1))
            continue
        fi
        mv "$tmp" "$dest"
    fi

    if [ ! -f "$dest" ]; then
        printf '  falta %s\n' "$name" >&2
        failed=$((failed + 1))
        continue
    fi

    got="$(sha256_of "$dest")"
    if [ "$got" != "$want" ]; then
        # Ruidoso a proposito: un archivo que no es el que se fijo produce un rojo
        # que parece del motor, y no lo es.
        printf '  CHECKSUM DISTINTO en %s\n    esperado %s\n    obtenido %s\n' \
            "$name" "$want" "$got" >&2
        failed=$((failed + 1))
        continue
    fi
    printf '  ok %s\n' "$name"
done

printf '\n%d entrada(s), %d con problemas\n' "$entries" "$failed"
[ "$failed" -eq 0 ] || exit 1
