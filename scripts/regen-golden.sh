#!/usr/bin/env bash
# ============================================================================
# WD-2.2 — regenerar los golden de DSP.
#
# Recapturar NO es un efecto colateral de correr los tests: es una tarea
# explicita, con su propio comando, que deja el diff a la vista. Es la misma
# regla que `.github/local-gate.json` — la prueba de que algo corrio la escribe
# quien lo corrio, y nadie mas.
#
#   bash scripts/regen-golden.sh              # todos los golden
#   bash scripts/regen-golden.sh -R GoldenEq  # filtro de ctest
#
# Despues de correr esto, `git diff --stat` sobre testdata/golden/ tiene que
# mostrar SOLO lo que se esperaba que cambiara. Si aparece un preset que el
# cambio no tocaba, eso es el hallazgo.
#
# Los `.resp` son texto: su diff se lee directo en el PR y es la parte
# revisable de la recaptura. Los `.f32` son binarios y solo dicen "cambio".
# ============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GOLDEN_DIR="$REPO_ROOT/audio/src/main/cpp/effects/tests/testdata/golden"

mkdir -p "$GOLDEN_DIR"

echo "==> Regenerando golden en $GOLDEN_DIR"
echo

# Los tests de golden se marcan SKIPPED en modo regeneracion, a proposito: una
# corrida que ESCRIBE no puede pasar por una corrida que VERIFICA.
WMA_GOLDEN_REGEN=1 bash "$REPO_ROOT/scripts/run-cpp-tests.sh" -R 'GoldenPresets' "$@"

echo
echo "==> Diff de los golden de respuesta (texto, revisable):"
git -C "$REPO_ROOT" --no-pager diff --stat -- "$GOLDEN_DIR" || true
echo
echo "==> Ahora VERIFICA que el diff sea el esperado, y recien despues commitealo."
echo "    Para comprobar que los nuevos golden pasan:"
echo "      bash scripts/run-cpp-tests.sh -R Golden"
