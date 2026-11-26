# Capa JNI — trampas locales

- Toda funcion nueva va en `jni_audio_bridge.cpp` + `AudioNativeBridge.kt`.
- Category mutexes: `lifecycleMutex`, `effectsMutex`, `modeMutex`, `inputMutex`.
- Lock-free paths para real-time params (`setXY`, `setFrequency`).
- Return `Result<T>` para operaciones que pueden fallar.

## Nueva funcion JNI

1. C++: agregar en `jni/jni_audio_bridge.cpp`.
2. Kotlin: `private external fun` en `AudioNativeBridge.kt` (`jvmSharedMain`, compartido con el
   target JVM: cero `android.*` ahi; la ruta que leen los gates esta en
   `scripts/jni_bridge_source.py`).
3. Wrapper con mutex apropiado y `Result<T>`.
4. Si es API publica: agregar a `IAudioNativeBridge` (commonMain).
5. Considerar la C API `watermelon_audio.h/cpp` (`python3 scripts/c-api-gap.py` mide el gap).

## Dos modelos: el motor global y los handles offline (REQ-055 S2, ADR-0008)

- **Global** (todo lo previo): un solo motor por proceso, `g_wmaEngine`, creado por
  `ensureEngine()` y serializado por `g_jniState.engineMutex`. Es el VIVO, el que suena.
- **Handles** (`nativeOffline*`, al final de `jni_audio_bridge.cpp`): instancias de render
  offline, cada una un `WmaEngine*` propio de `wma_engine_create()`, identificadas por un
  `jlong` opaco. Las posee `OfflineRenderer` (jvmShared, `AutoCloseable`, `close()` idempotente).
- 🔴 **Nunca se cruzan**: una `nativeOffline*` no lee `g_wmaEngine` ni toma `engineMutex`, y
  una del modelo global no recibe handle. Una funcion que necesite las dos cosas es un desvio
  de diseño, no un atajo.
- **El handle es un id, no el puntero**: un registro (`offlineRegistry()`, en el heap y nunca
  destruido; lo vacia `JNI_OnUnload`) lo mapea a la instancia. Un puntero crudo hace de un handle destruido un use-after-free, y de
  uno reciclado (misma direccion, otra instancia) algo peor: operar en silencio sobre otra.
  Un id que no se reusa da `WMA_ERROR_NOT_INITIALIZED`. El mutex del registro sólo cubre
  buscar/insertar/sacar; la operacion corre con el lock suelto sobre un `shared_ptr`, asi que
  ni el vivo (que no lo toma) ni otra instancia esperan un render. Cada instancia tiene un
  `opMutex` que serializa TODAS sus operaciones (start, render, stop, setters): un setter a la
  par de un render offline no tiene el margen que el callback corto y RT le da en vivo.
- **El global sigue siendo del vivo**: `ensureEngineLocked()` fuerza
  `BackendManager::setGlobalInstance` con el manager de `g_wmaEngine`, porque las entradas de
  USB/backend de este directorio van por `getInstance()`. `wma_engine_create()` solo
  registra si no hay ninguno, y `wma_engine_destroy()` solo des-registra el suyo.
- **Configurar una instancia offline** es un pasamanos 1:1 a la `wma_*` que ya toma un
  `WmaEngine*`. No se duplican las 300 del modelo global: se agrega la que un consumidor pide.

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
