# :audio — build de Gradle, trampas locales

- 🔴 **`dependsOn` SOLO ORDENA.** La libreria de host (`buildHostJniLib`) esta declarada como
  `inputs.files` de `testDebugUnitTest`: sin eso, romper un simbolo del lado C++ dejaba la task
  UP-TO-DATE y el arnes JNI en VERDE sin ejecutar nada (medido; misma trampa que
  `cinteropWatermelonAudio`). No lo "limpies" por redundante. Igual con el `outputs.dir` de la
  cobertura: sin el, su finalizador se queda sin nada.
- `testDebugUnitTest` construye sola la libreria de host y le pone el `java.library.path`.
- **`forkEvery = 1`**: una JVM por clase del arnes, porque el motor nativo es singleton de proceso.
- `jniHarnessCoverage` es **finalizador** de `testDebugUnitTest` a proposito: asi se imprime la
  cobertura sin tocar `gate.sh` ni `ci.yml`. Detalle del arnes: `src/androidUnitTest/CLAUDE.md`.
- `kotlin.mpp.enableCInteropCommonization` (gradle.properties) lo necesita
  `compileIosMainKotlinMetadata`, que ningun gate compila.
- `build-ios.sh` corre suelto y antes de Gradle en `gate.sh`: ver `KmpNativeConventionPlugin.kt`.
