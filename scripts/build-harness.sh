#!/usr/bin/env bash
#
# WA-5.5 — construye :harness para las dos plataformas y verifica que el
# framework de iOS traiga el motor adentro.
#
# Por que un script y no dos tasks sueltas en el gate: un framework que "se
# construyo" pero llega vacio pasa `linkDebugFramework...` sin chistar. Es el
# mismo modo de falla que ci.yml ya cubre para el XCFramework de WA-4.1 con
# "Verify the XCFramework carries the engine", y por el mismo motivo: el motor
# viaja como archivo estatico adentro del klib, asi que "linkeo" y "trae el
# motor" son dos afirmaciones distintas.
#
# En Linux la parte de iOS se saltea con un mensaje claro, igual que
# buildIosNativeLib.
#
# Uso:  bash scripts/build-harness.sh

set -euo pipefail

cd "$(dirname "$0")/.."

# Piso deliberadamente bajo. La cuenta exacta se mueve con cada wma_* que se
# agrega; lo que este numero detecta es la diferencia entre "el archivo entro"
# (cientos) y "no entro" (cero). ci.yml usa el mismo piso para el XCFramework.
readonly MIN_WMA_SYMBOLS=100

printf '=== :harness — Android ===\n'
./gradlew :harness:assembleDebug

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf '\n=== :harness — iOS: se saltea (requiere macOS) ===\n'
    printf '\nOK — Android construido.\n'
    exit 0
fi

printf '\n=== :harness — iOS (simulador) ===\n'
./gradlew :harness:linkDebugFrameworkIosSimulatorArm64

readonly FRAMEWORK="harness/build/bin/iosSimulatorArm64/debugFramework/HarnessKit.framework/HarnessKit"

if [[ ! -f "$FRAMEWORK" ]]; then
    printf '\nFAIL — no se produjo %s\n' "$FRAMEWORK" >&2
    exit 1
fi

wma_count="$(nm -gU "$FRAMEWORK" 2>/dev/null | grep -c ' T _wma_' || true)"
printf '  HarnessKit.framework: %s simbolos wma_*\n' "$wma_count"

if (( wma_count < MIN_WMA_SYMBOLS )); then
    printf '\nFAIL — el framework del harness no trae la C API (%s < %s).\n' \
        "$wma_count" "$MIN_WMA_SYMBOLS" >&2
    printf '       El archivo estatico no viajo adentro del klib. El harness\n' >&2
    printf '       arrancaria y moriria en la primera llamada al motor.\n' >&2
    exit 1
fi

# El punto de entrada que envuelve el proyecto de Xcode. Si no se exporta, el
# shell de Swift compila y no encuentra nada que instanciar.
#
# Se afirma contra el HEADER GENERADO y no contra la tabla de simbolos, y la
# diferencia no es de estilo. Kotlin/Native nombra la clase ObjC por el ARCHIVO
# (MainViewController.kt -> HarnessKitMainViewControllerKt), no por la funcion:
# renombrar `MainViewController` a cualquier otra cosa deja el simbolo intacto y
# el check pasaba igual. O sea que la version anterior afirmaba "el archivo
# existe", no "el punto de entrada existe". El header sí lleva la firma real, y
# es ademas exactamente lo que compila Swift.
readonly HEADER="harness/build/bin/iosSimulatorArm64/debugFramework/HarnessKit.framework/Headers/HarnessKit.h"

if [[ ! -f "$HEADER" ]]; then
    printf '\nFAIL — no se genero el header del framework (%s).\n' "$HEADER" >&2
    exit 1
fi

# La firma completa, con tipo de retorno: un `MainViewController` que devolviera
# otra cosa no le sirve al UIViewControllerRepresentable de Swift.
entry_count="$(grep -c '+ (UIViewController \*)MainViewController' "$HEADER" || true)"
if (( entry_count == 0 )); then
    printf '\nFAIL — el framework no exporta MainViewController(): UIViewController.\n' >&2
    printf '       El shell de Swift compilaria sin nada que instanciar.\n' >&2
    printf '       Lo que el header sí declara:\n' >&2
    grep -E '^\+ \(' "$HEADER" | sed 's/^/         /' >&2 || true
    exit 1
fi
printf '  MainViewController(): UIViewController — exportado\n'

printf '\nOK — :harness construye en las dos plataformas y el framework trae el motor.\n'
