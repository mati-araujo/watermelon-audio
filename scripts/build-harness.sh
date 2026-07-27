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

# ---------------------------------------------------------------------------
# El shell de Xcode: compilar, instalar y VER SI SOBREVIVE.
#
# Los dos pasos son distintos y el segundo es el que gana su lugar.
#
# `xcodebuild` agarra lo de siempre: el proyecto no parsea, el framework no
# linkea, Swift no compila contra el header generado. Todo eso es valioso y
# barato.
#
# Lo que NO agarra —ni el, ni Gradle, ni el link check, ni nada de lo que ya
# habia en el gate— es un Info.plist incompleto. Compose Multiplatform corre
# `PlistSanityCheck` AL ARRANCAR y aborta el proceso: sin
# CADisableMinimumFrameDurationOnPhone la app moria con SIGABRT antes de dibujar
# un pixel, mientras los ocho comandos del gate daban verde. Es exactamente la
# clase de falla silenciosa que este repo persigue, y la unica forma de verla es
# lanzarla y preguntar si sigue viva.
#
# Se saltea con un mensaje claro si no hay simulador; no se saltea en silencio.
# ---------------------------------------------------------------------------
readonly XCODE_PROJECT="harness/iosApp/iosApp.xcodeproj"
readonly BUNDLE_ID="com.watermellonstudios.audio.harness"

printf '\n=== :harness — shell de Xcode ===\n'
xcodebuild -project "$XCODE_PROJECT" -scheme iosApp -configuration Debug \
    -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' \
    -derivedDataPath harness/iosApp/build/DerivedData \
    build > /tmp/wma-harness-xcodebuild.log 2>&1 || {
        printf '\nFAIL — no compila el shell de Xcode. Ultimas lineas:\n' >&2
        grep -E 'error:|BUILD FAILED' /tmp/wma-harness-xcodebuild.log | head -15 >&2
        exit 1
    }
printf '  xcodebuild: OK\n'

readonly APP="harness/iosApp/build/DerivedData/Build/Products/Debug-iphonesimulator/iosApp.app"

# Un simulador ya booteado, o el primer iPhone disponible.
device="$(xcrun simctl list devices booted -j 2>/dev/null \
    | python3 -c 'import json,sys;d=json.load(sys.stdin)["devices"];print(next((x["udid"] for v in d.values() for x in v),""))' 2>/dev/null || true)"

if [[ -z "$device" ]]; then
    printf '  no hay simulador booteado — se saltea el chequeo de arranque.\n'
    printf '    (booteá uno con `xcrun simctl boot <udid>` para que corra)\n'
    printf '\nOK — :harness construye en las dos plataformas, el framework trae el\n'
    printf '     motor, y el shell de Xcode compila. Arranque NO verificado.\n'
    exit 0
fi

xcrun simctl terminate "$device" "$BUNDLE_ID" >/dev/null 2>&1 || true
xcrun simctl install "$device" "$APP" >/dev/null 2>&1 || {
    printf '\nFAIL — no se pudo instalar la app en el simulador.\n' >&2
    exit 1
}

pid="$(xcrun simctl launch "$device" "$BUNDLE_ID" 2>/dev/null | awk -F': ' '{print $2}')"
if [[ -z "$pid" ]]; then
    printf '\nFAIL — la app no arranco.\n' >&2
    exit 1
fi

# Tres segundos alcanzan: PlistSanityCheck corre en el primer frame, y un
# SIGABRT de arranque llega mucho antes de eso.
sleep 3

# `ps -p` sobre el PID que devolvio simctl, y no `simctl spawn ... launchctl
# list | grep`, por dos razones. La primera es que pregunta exactamente lo que
# importa —¿sigue vivo ESTE proceso?— en vez de si algun listado menciona el
# bundle. La segunda es que no hay pipeline: la version con `| grep -q` daba
# FAIL con la app corriendo, porque grep -q corta al primer match y bajo
# `set -o pipefail` el SIGPIPE del productor hunde el pipeline entero.
#
# Es la SEGUNDA vez que ese mismo error aparece en este archivo — ya estaba
# arreglado quince lineas mas arriba, para `nm`, y volvi a escribirlo igual.
# Regla para este repo: `grep -q` no va en un pipeline bajo pipefail. Nunca.
# Los procesos del simulador son procesos del host, asi que `ps -p` los ve.
if ! ps -p "$pid" > /dev/null 2>&1; then
    printf '\nFAIL — la app arranco (pid %s) y MURIO en los primeros 3 segundos.\n' "$pid" >&2
    printf '       Compilar no alcanza: mira el crash report mas nuevo en\n' >&2
    printf '       ~/Library/Logs/DiagnosticReports/iosApp-*.ips\n' >&2
    printf '       Sospechoso numero uno: una clave que falta en el Info.plist —\n' >&2
    printf '       Compose aborta el proceso desde PlistSanityCheck.\n' >&2
    exit 1
fi
xcrun simctl terminate "$device" "$BUNDLE_ID" >/dev/null 2>&1 || true
printf '  arranca y sobrevive los primeros 3s (pid %s)\n' "$pid"

printf '\nOK — :harness construye en las dos plataformas, el framework trae el\n'
printf '     motor, y la app de iOS arranca sin morirse.\n'
