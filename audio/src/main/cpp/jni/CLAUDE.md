# Capa JNI — trampas locales

- Toda funcion nueva va en `jni_audio_bridge.cpp` + `AudioNativeBridge.kt`.
- Category mutexes: `lifecycleMutex`, `effectsMutex`, `modeMutex`, `inputMutex`.
- Lock-free paths para real-time params (`setXY`, `setFrequency`).
- Return `Result<T>` para operaciones que pueden fallar.

## Nueva funcion JNI

1. C++: agregar en `jni/jni_audio_bridge.cpp`.
2. Kotlin: `private external fun` en `AudioNativeBridge.kt` (androidMain).
3. Wrapper con mutex apropiado y `Result<T>`.
4. Si es API publica: agregar a `IAudioNativeBridge` (commonMain).
5. Considerar la C API `watermelon_audio.h/cpp` (`python3 scripts/c-api-gap.py` mide el gap).

## Las TRES preguntas del camino JNI (REQ-016, REQ-025)

Ninguna sola alcanza, y son complementarias:

- *¿el simbolo existe?* — `scripts/check-jni-symbols.py` (MINI-001): compara **sólo NOMBRES**
  contra el `.so`. Da verde con funciones jamas ejecutadas.
- *¿la FIRMA coincide?* — `scripts/check-jni-signatures.py` (REQ-025): aridad, anchos y retorno
  entre la `external fun` y su prototipo. Un `Int` donde el C++ espera `jlong` compila, linkea,
  pasa el gate de nombres y corrompe memoria en el device. Source-only (sin `.so` ni NDK).
- *¿alguien lo ejecuta?* — el arnes de `androidUnitTest` (ver su `CLAUDE.md`), que cruza la
  frontera de verdad. Hasta REQ-016 la respuesta era **nadie**.

🔴 La FIRMA se compra **de una sola vez para todas**, con el gate. REQ-024 intento comprarla de a
una funcion (un `--add-opens=java.base/java.io` permanente + reflexion sobre un campo privado del
JDK para ejercer `nativeLoadSoundFontFromFd`) y MINI-015 lo revirtio. Lo que el arnes compra y el
gate de firmas NO ve es la **semantica**: que el pinneo de un array se libere, que el largo sea el
correcto, que un `null` se maneje, que no quede una excepcion pendiente.

🔴 El arnes cubre una FRACCION, sobre un backend FALSO. No reemplaza al smoke en device; nada de
lo que corre en el host lo hace.

## Libreria de host

`bash scripts/build-host-jni.sh` construye `tests/hostjni/` suelta. `WMA_JAVA_HOME`/`JAVA_HOME`
manda el JDK y tiene que ser **el mismo** que corre los tests. NO usa `find_package(JNI)` (busca
AWT y falla contra un JDK headless, medido en ubuntu) y compila `jni/` con
`-Wall -Wextra -Werror`, el unico gate de warnings de esta capa.
