#!/usr/bin/env bash
# fetch-corpus.sh — REQ-001 S10 · 10.4. Baja el corpus grabado y lo VERIFICA.
#
# El corpus no vive en el repo: son archivos de audio y no tienen por que
# versionarse acá. Lo que sí vive en el repo es el MANIFIESTO —nombre, checksum y
# frecuencia verdadera de cada archivo— porque es lo que vuelve reproducible una
# corrida contra material grabado.
#
#   bash scripts/fetch-corpus.sh          # baja lo que falte y verifica todo
#   bash scripts/fetch-corpus.sh --verify # solo verifica lo que ya esta
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$REPO_ROOT/audio/src/main/cpp/analysis/tests/corpus-manifest.txt"
CORPUS_DIR="${WMA_CORPUS_DIR:-$REPO_ROOT/audio/src/main/cpp/analysis/tests/corpus}"
VERIFY_ONLY=0
[ "${1:-}" = "--verify" ] && VERIFY_ONLY=1

[ -f "$MANIFEST" ] || { printf 'no existe el manifiesto: %s\n' "$MANIFEST" >&2; exit 1; }

entries=0
failed=0
mkdir -p "$CORPUS_DIR"

while IFS= read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    # shellcheck disable=SC2086
    set -- $line
    name="$1"; want="$2"
    entries=$((entries + 1))
    dest="$CORPUS_DIR/$name"

    if [ ! -f "$dest" ] && [ "$VERIFY_ONLY" -eq 0 ]; then
        printf '  falta %s y no hay URL declarada para bajarlo\n' "$name" >&2
        failed=$((failed + 1))
        continue
    fi
    if [ ! -f "$dest" ]; then
        printf '  ausente: %s\n' "$name" >&2
        failed=$((failed + 1))
        continue
    fi

    got="$(shasum -a 256 "$dest" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        # Ruidoso a proposito: un archivo corrupto produce un resultado RARO en
        # vez de un error, y un afinador que falla raro es peor que uno que falla
        # fuerte.
        printf '  🔴 CHECKSUM NO COINCIDE en %s\n     esperado %s\n     obtenido %s\n' \
               "$name" "$want" "$got" >&2
        failed=$((failed + 1))
    else
        printf '  ok %s\n' "$name"
    fi
done < "$MANIFEST"

if [ "$entries" -eq 0 ]; then
    cat <<'MSG'
El manifiesto no declara ningun archivo.

Eso NO es un error: el corpus grabado todavia no existe, y el manifiesto lo dice
explicitamente. Los tests de robustez que dependen de el salen SKIPPED — nunca
PASSED— asi que una corrida sin corpus no se puede leer como cobertura completa.
MSG
    exit 0
fi

printf '\n%d entrada(s), %d con problemas\n' "$entries" "$failed"
[ "$failed" -eq 0 ]
