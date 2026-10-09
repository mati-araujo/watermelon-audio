# Arnes JNI (REQ-016 + REQ-018) — trampas locales

Corre en la JVM del host y EJECUTA funciones `JNIEXPORT` reales contra un `JNIEnv` real, entrando
por `AudioNativeBridge`. Las tres preguntas del camino JNI: `audio/src/main/cpp/jni/CLAUDE.md`.

- **Una JVM por clase (`forkEvery = 1`)**: el motor nativo es un singleton de proceso y los tests
  de ausencia necesitan uno virgen. Por eso ninguna clase ve el total.
- **Como se suma la cobertura**: cada clase publica lo suyo a `audio/build/jni-coverage/` y la
  task `jniHarnessCoverage` —finalizador de `testDebugUnitTest`— lo suma e imprime
  `[REQ-016] arnes JNI - TOTAL: N de M ... hueco: M-N`. Numerador anotado al cruzar la frontera,
  denominador contado del arbol. Sin archivos o con cero funciones, FALLA: "no pude sumar" no es
  un pase. El numero no se escribe en ningun doc (REQ-021).
- 🔴 **Cada clase declara el conjunto que cubre: TRINQUETE BIDIRECCIONAL.** Ejercer de menos es
  rojo y ejercer de mas tambien, para que sumar cobertura aparezca en el diff (lo trajo un
  mutante: sacar una funcion bajaba el conteo y nadie se ponia rojo).
- Las trampas de Gradle (`dependsOn` sólo ordena, el finalizador): `audio/CLAUDE.md`.
- **Huecos DECLARADOS, con el mutante que los demuestra**: el stream de entrada VIVO no se abre en
  el host (`InputNode::createInputStream` no tiene camino sin Oboe), y `nativeIntonationReset` /
  el camino CON dato de `nativeGetTunerSnapshot` no son observables sin audio. Los cubre la suite
  de C++, que si maneja el backend.
