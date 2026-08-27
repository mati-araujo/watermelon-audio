#!/usr/bin/env bash
# ============================================================================
# REQ-016 — construye `libwatermelon_audio.{so,dylib}` PARA EL HOST: la librería
# nativa que carga el arnés JNI de `:audio:testDebugUnitTest`.
#
# Por qué existe un script y no sólo una task de Gradle: es el mismo criterio
# que `scripts/build-ios.sh`. Un script se corre suelto para iterar, se declara
# como input de la task (si no, cambiarle las banderas deja la task UP-TO-DATE y
# sobrevive el binario viejo — medido en `buildIosNativeLib`, 2026-07-29), y lo
# puede invocar el CI sin pasar por Gradle.
#
#   bash scripts/build-host-jni.sh            # construye e imprime la ruta
#   WMA_JAVA_HOME=/ruta/jdk  bash scripts/build-host-jni.sh
#   CLEAN=1 bash scripts/build-host-jni.sh    # borra el árbol de build primero
#
# 🔴 El .so lleva adentro un `FakeAudioBackend`. Valida la frontera JNI/Kotlin,
# NO el audio en dispositivo. Ver el encabezado del CMakeLists.
# ============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO_ROOT/audio/src/main/cpp/tests/hostjni"

# El árbol de build va FUERA de audio/src/main/cpp a propósito. Los guardrails
# que caminan el C++ (`check-rt-safety.py`, `check-mechanism-callers.py`) excluyen
# `thirdparty/`, `ios/build/` y `.deps/` y NADA MAS: un `build/` nuevo bajo cpp/
# les metería el `CMakeCXXCompilerId.cpp` que genera CMake en el universo de
# fuentes, y esos dos lints tienen trinquetes que fallan si el conjunto cambia.
BUILD_DIR="$REPO_ROOT/audio/build/hostjni"

# El JDK del que salen jni.h y jni_md.h. Tiene que ser EL MISMO que va a correr
# los tests: headers de una versión y runtime de otra es la clase de desajuste
# que este REQ existe para no tener. Gradle lo pasa explícito (ver
# audio/build.gradle.kts); suelto, se deduce.
if [[ -z "${WMA_JAVA_HOME:-}" ]]; then
    if [[ -n "${JAVA_HOME:-}" ]]; then
        WMA_JAVA_HOME="$JAVA_HOME"
    elif [[ -x /usr/libexec/java_home ]]; then
        WMA_JAVA_HOME="$(/usr/libexec/java_home)"
    elif command -v javac >/dev/null 2>&1; then
        WMA_JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
    else
        echo "error: no encuentro un JDK. Exportá JAVA_HOME o WMA_JAVA_HOME." >&2
        exit 2
    fi
fi

if [[ ! -f "$WMA_JAVA_HOME/include/jni.h" ]]; then
    echo "error: '$WMA_JAVA_HOME' no tiene include/jni.h — ¿es un JRE y no un JDK?" >&2
    exit 2
fi

[[ "${CLEAN:-0}" == "1" ]] && rm -rf "$BUILD_DIR"

GEN_ARGS=()
command -v ninja >/dev/null 2>&1 && GEN_ARGS=(-G Ninja)

# La misma danza "${arr[@]+...}" que run-cpp-tests.sh: bash 3.2 (macOS) aborta
# con `set -u` sobre un array vacío, y GEN_ARGS lo está siempre que falte ninja.
cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${GEN_ARGS[@]+"${GEN_ARGS[@]}"}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DWMA_JAVA_HOME="$WMA_JAVA_HOME"

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
cmake --build "$BUILD_DIR" -j"$JOBS"

# Sin `ls a b | head`: con `pipefail`, un `ls` sobre el nombre que NO existe en
# esta plataforma devuelve 2 y `set -e` mata el script justo despues de un build
# exitoso. Se pregunta por cada archivo.
LIB=""
for candidate in "$BUILD_DIR/libwatermelon_audio.so" "$BUILD_DIR/libwatermelon_audio.dylib"; do
    [[ -f "$candidate" ]] && LIB="$candidate"
done
if [[ -z "$LIB" ]]; then
    echo "error: el build terminó pero no dejó libwatermelon_audio.{so,dylib} en $BUILD_DIR" >&2
    exit 1
fi
echo "host-JNI: $LIB"
