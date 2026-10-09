# Scripts y lints del gate — trampas locales

Regla comun: **los lints con `--self-test` lo corren ANTES** (en `gate.sh` y en el CI): si el parser
se rompe, el lint queda en verde para siempre. Y cada baseline es un **TRINQUETE**: falla si
aparece deuda nueva Y si una entrada declarada ya no se reproduce. Se redeclara con su comando y
**su diff es la revision**.

## Atestacion (`gate.sh`)

`.github/local-gate.json` lo escribe `gate.sh` y NADIE MAS (ver el `CLAUDE.md` raiz).
`bash scripts/test-attestation.sh` es el verificador contra arboles mutados (~10 s). Diseño:
`docs/ci/local_first.md`. Ojo: con `ios` atestandose en 9 s, los tres jobs de ubuntu **son** el
camino critico de un PR atestado (`local_first.md` §2).

## Los lints

- `check-rt-safety.py` (WD-1.1): camina el call-graph del callback y falla por logging,
  allocation, lock que bloquee o `shared_ptr`. `--graph` imprime lo alcanzado. Excepciones:
  `// RT-SAFE-ALLOW: razon` para lo INOFENSIVO; `rt-safety-baseline.txt` para deuda con dueño.
  SEGUNDO trinquete, sobre la COBERTURA: `rt-coverage-baseline.txt` declara QUE funciones alcanza
  el walker y falla si el conjunto cambia en cualquier direccion (`--update-coverage`). Existe
  porque la cobertura se encoge SOLA: un metodo nuevo con nombre comun vuelve ambigua una llamada.
- `check-test-waits.py` (REQ-002): toda espera cruda en un test esta CLASIFICADA, no detectada por
  su forma (eso se evade sin querer). Cada `sleep_for` es una de cuatro cosas:
  `polling` (bucle con deadline, lo reconoce solo) · `estimulo` (la duracion ES el experimento:
  jitter, intervalo, forzar un orden; `// WAIT-OK: razon`) · `presencia` (espera y afirma →
  `wma_test::waitUntil`) · `ausencia` (espera a que NO pase → `wma_test::sleepFixed`).
  🔴 Si es PRESENCIA, agrandar el sleep NO lo arregla: alcanza aca y se queda corto en el runner.
- `check-time-dependence.sh`: corre la suite con las esperas CIEGAS colapsadas y dice que test
  cambia de veredicto. Lo corre el job `cpp-tests` de ubuntu, no `gate.sh` (cuesta una corrida).
  🔴 NO carga la maquina a proposito: esta MEDIDO que la contencion no reproduce esta clase (40
  quemadores sobre 10 nucleos: 0/10; `taskpolicy -c background` + carga: 1/10, y fue timeout; un
  `sleep_for(120ms)` es tiempo absoluto y el render lento le da MAS margen). Colapsar la
  espera: 10/10 en 2 s. Cubre UNA clase; no ve una espera POR CONDICION que se vuelve
  inalcanzable ni las esperas de AUSENCIA (falso VERDE). El script lo imprime.
- `check-cpp-portability.sh` (WA-0.4): `jni.h` / `android/` fuera de su capa.
- `check-jni-signatures.py` (REQ-025): la FIRMA del cruce JNI. 🔴 Su guarda de COMPLETITUD lo
  sostiene: si el parser abarca menos prototipos de los que hay en el arbol, FALLA. Sin baseline:
  nacio en cero desajustes. Las tres preguntas: `audio/src/main/cpp/jni/CLAUDE.md`.
- `check-jni-symbols.py` (MINI-001): sólo NOMBRES contra el `.so`.
- `check-release-flags.py` (MINI-036): las flags EFECTIVAS del `.so` de `assembleRelease` en las 4
  ABIs: -O3, FP seguras, `-ftree-vectorize`, `-flto=thin` y `-g` en cada objeto, con la semantica
  del driver (GANA LA ULTIMA: un `-fno-lto` al final anula); SIN `-ffinite-math-only` ni lo que lo
  implique (`-ffast-math`, `-ffp-model=*`); link LTO a -O3; la copia sin strip CON
  `.symtab/.debug_info/.debug_line` y el `.so` del AAR SIN `.debug_*`, leidos con `readelf`. Le
  pregunta a ninja (`-n`) si el `.so` esta al dia. Corre despues de assemble-release.
  🔴 NO lee el CMakeLists, y ese es el punto: pedia "Release", AGP construye RelWithDebInfo, y
  todo salio a -O2 sin LTO hasta MINI-036. Lee el `build.ninja` que produjo el `.so`
  EMPAQUETADO, atado por contenido: `.cxx/` acumula un hash por configuracion.
- `check-dep-pins.py` (REQ-047 S4): UN lugar por pin (googletest, NDK/CMake del SDK,
  `java-version`, el Xcode del CI —aplicado por `ci-select-xcode.sh` en todo job de macOS— y el
  SHA+tag de cada action).
- `diff-published-artifact.py --self-test` (REQ-047 S4, 4.10): el instrumento de AC-047.2 puede
  fallar. El diff REAL contra el registro NO es gate (necesita publish y credencial): **lo corre la
  etapa que toca lo publicado**.
- `check-mechanism-callers.py` (REQ-013): falla si una funcion de produccion tiene sus UNICOS
  llamadores en tests (REQ-012 entrego un mecanismo verificado que nadie llamaba). El baseline
  lleva CATEGORIA y RAZON por entrada (sin razon falla). El reparto se DERIVA:
  `grep -v '^#' scripts/mechanism-callers-baseline.txt | grep -v '^$' | sed 's/ | .*//' | awk -F'::' '{print $NF}' | sort | uniq -c`.
  🔴 NO es detector de codigo muerto (si no la llama NADIE, no se reporta) y NO ve el hueco del
  JNI. Falso negativo MEDIDO: nombres simples con homonimos en produccion quedan tapados (`reset`
  tiene ~96 definiciones; 117 nombres tapados al medirlo). La salida es RENOMBRAR.
- `check-doc-counts.py` (REQ-021): la tabla «Conteos medidos» del `CLAUDE.md` raiz. Lee el bloque
  entre sus marcas `BEGIN/END conteos-medidos`: no lo muevas de archivo.
- `check-no-ui-in-library.sh` (WA-5.5): ver `harness/CLAUDE.md`.
- `check-literal-rate.py` (REQ-006.4): ningun subsistema se prepara con un sample rate LITERAL.
- `check-ituner-implementations.py` (MINI-004): `TunerContractTest` ejerce TODA implementacion
  de `ITuner`; un test parametrico sólo protege a las que alguien registro.
- `check-jni-results.py` (REQ-045): ninguna `JNIEXPORT` descarta un `WmaResult`.
- `check-jni-preinit.py` (REQ-045): una configuracion llamada antes del init no se pierde muda.
- `check-jni-usb-access.py` (MINI-042): cada `JNIEXPORT` entra al `LibusbBackend` por el accesor
  con el alcance que le toca; ningun test de host lo ve (el arnes compila un backend vacio).

## Otros

- `build-ios.sh`: `gate.sh` lo corre SUELTO y ANTES de Gradle, y no es redundante (ver el
  comentario en `KmpNativeConventionPlugin.kt`).
- `c-api-gap.py`: gap C API vs JNI + delegacion (WA-2.6). Imprime; `docs/kmp/c_api_coverage.md`
  se actualiza a mano con esa salida.
- Los del corpus y del afinador: `audio/src/main/cpp/analysis/CLAUDE.md`. `sf-delta-host.py`:
  `audio/src/main/cpp/engines/CLAUDE.md`. `regen-golden.sh`: `audio/src/main/cpp/effects/CLAUDE.md`.
