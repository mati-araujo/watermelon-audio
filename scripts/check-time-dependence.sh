#!/usr/bin/env bash
#
# check-time-dependence.sh — que test cambia de veredicto cuando se le saca el tiempo.
#
# REQ-002 · S1. Tres de cinco pushes a master fallaron el 2026-08-20 por tests
# que sincronizan con el reloj de pared: verdes en una maquina ociosa, rojos en
# un runner con siete jobs. Este script los hace reproducibles a voluntad.
#
# COMO, Y POR QUE NO ES "CARGAR LA MAQUINA"
# ------------------------------------------
# Colapsa las esperas CIEGAS (`wma_test::sleepFixed`) a cero y deja intactas las
# esperas POR CONDICION (`wma_test::waitUntil`). Un test que cambia de veredicto
# bajo eso depende del reloj; uno que no, no.
#
# Cargar la maquina NO sirve, y esta medido en S1:
#     40 quemadores sobre 10 nucleos ...............  0/10
#     taskpolicy -c background + 20 quemadores .....  1/10, y ese uno fue TIMEOUT
#     colapsar la espera ...........................  10/10, en 2 segundos
# Un `sleep_for(120ms)` es tiempo ABSOLUTO: ralentizar el mundo no achica esa
# ventana, y encima le da mas margen al worker porque el render tambien tarda mas.
#
# LO QUE ESTE INSTRUMENTO NO VE, DICHO ACA PARA QUE NO SE LEA DE MAS
# ------------------------------------------------------------------
# Cubre una clase: la espera ciega insuficiente. NO cubre la otra que REQ-002
# encontro — una espera POR CONDICION cuya condicion puede volverse inalcanzable
# (`TunerApiTest.SwitchingSourceWhileTheAnalysisIntegratesDoesNotRace` espera un
# snapshot sin alimentar audio). Medido: 0/10 en las dos escalas. Un verde de
# este script NO dice que la suite sea independiente del tiempo; dice que no
# quedan esperas ciegas insuficientes.
#
# EL SELF-TEST CORRE PRIMERO Y NO ES OPCIONAL
# --------------------------------------------
# Si el instrumento deja de colapsar las esperas no reporta nada, y eso se lee
# como "no hay defectos". Verde por ceguera — este repo ya lo pago dos veces (el
# parser del arnes de mutacion, la cobertura del lint de RT). Por eso los
# controles corren ANTES y un fallo suyo ABORTA sin decir una palabra del resto.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${WMA_TEST_BUILD:-$ROOT/audio/src/main/cpp/tests/build}"
CORE="$BUILD/core_tests/core_tests"
PROBE_POS='TimeDependenceProbe.ABlindWaitIsDetectedByTheInstrument'
PROBE_NEG='TimeDependenceProbe.AConditionWaitSurvivesTheInstrument'

only_self_test=0
[ "${1:-}" = "--self-test" ] && only_self_test=1

if [ ! -x "$CORE" ]; then
    echo "check-time-dependence: no encuentro $CORE" >&2
    echo "  construi la suite primero:  bash scripts/run-cpp-tests.sh" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# 1. Self-test. Sin esto, nada de lo de abajo vale.
# ---------------------------------------------------------------------------
echo "== self-test del instrumento =="

if ! "$CORE" --gtest_filter="$PROBE_POS:$PROBE_NEG" >/dev/null 2>&1; then
    echo "  ROTO: con la escala normal los dos controles tienen que pasar, y no pasan." >&2
    echo "  El problema no es el instrumento: los controles mismos estan rotos." >&2
    exit 3
fi
echo "  escala normal: los dos controles pasan                    OK"

if WMA_TEST_WAIT_SCALE=0 "$CORE" --gtest_filter="$PROBE_POS" >/dev/null 2>&1; then
    echo "  ROTO: el control POSITIVO paso con la escala en 0." >&2
    echo "  El instrumento no esta colapsando las esperas, asi que un reporte" >&2
    echo "  vacio sobre la suite significaria 'no mire', no 'no hay defectos'." >&2
    exit 3
fi
echo "  escala 0: el control positivo FALLA, que es lo que debe    OK"

if ! WMA_TEST_WAIT_SCALE=0 "$CORE" --gtest_filter="$PROBE_NEG" >/dev/null 2>&1; then
    echo "  ROTO: el control NEGATIVO fallo con la escala en 0." >&2
    echo "  El instrumento esta achicando tambien los TECHOS de las esperas por" >&2
    echo "  condicion, asi que tiñe de rojo a los tests correctos y no discrimina." >&2
    exit 3
fi
echo "  escala 0: el control negativo sobrevive                    OK"

if [ $only_self_test -eq 1 ]; then
    echo "self-test OK."
    exit 0
fi

# ---------------------------------------------------------------------------
# 2. La suite, con el tiempo colapsado.
# ---------------------------------------------------------------------------
echo
echo "== suite con las esperas ciegas colapsadas =="
log="$(mktemp)"; trap 'rm -f "$log"' EXIT
set +e
WMA_TEST_WAIT_SCALE=0 ctest --test-dir "$BUILD" \
    --output-on-failure --no-tests=error \
    -E "TimeDependenceProbe" \
    ${CTEST_JOBS:+-j "$CTEST_JOBS"} >"$log" 2>&1
rc=$?
set -e

failed="$(sed -n '/The following tests FAILED:/,$p' "$log" | grep -E '^\s+[0-9]+ - ' || true)"

# ---------------------------------------------------------------------------
# El falso verde que este bloque existe para impedir.
#
# El instrumento solo ve lo que pasa por `wma_test::sleepFixed`. Un
# `std::this_thread::sleep_for` crudo le es INVISIBLE, asi que "ningun test
# depende del reloj" y "ningun test migrado depende del reloj" se imprimen
# igual — y son cosas muy distintas mientras quede algo sin migrar.
# ---------------------------------------------------------------------------
raw_files="$(grep -rl 'this_thread::sleep_for' "$ROOT/audio/src/main/cpp" \
        --include='*.cpp' --include='*.h' 2>/dev/null \
        | grep -v '/build' | grep -E '/tests?/' \
        | grep -v 'support/TestWait.h' || true)"
raw="$(printf '%s' "$raw_files" | grep -c . || true)"
raw_n=0
if [ -n "$raw_files" ]; then
    raw_n="$(printf '%s\n' "$raw_files" | tr '\n' '\0' \
             | xargs -0 grep -h 'this_thread::sleep_for' | grep -c . || true)"
fi

if [ "${raw:-0}" -gt 0 ]; then
    echo
    echo "  AVISO — cobertura parcial: quedan $raw_n esperas crudas en $raw archivos de test."
    echo "  Este instrumento no las ve: solo alcanza lo que pasa por wma_test::sleepFixed."
    echo "  Hasta que esten migradas, un verde de aca significa 'nada MIGRADO depende del"
    echo "  reloj', no 'nada depende del reloj'. Que el camino crudo deje de existir es S4."
fi

if [ -z "$failed" ]; then
    echo "  ningun test MIGRADO depende de una espera ciega."
    exit 0
fi

echo "  estos tests cambian de veredicto cuando se les saca el tiempo:"
echo "$failed" | sed 's/^/   /'
echo
echo "  Cada uno sincroniza con una duracion y afirma despues. Eso da verde en"
echo "  una maquina ociosa y rojo en un runner cargado, que es el defecto que"
echo "  REQ-002 persigue. La salida NO es agrandar la espera: es esperar por la"
echo "  condicion con techo (wma_test::waitUntil)."
exit "${rc:-1}"
