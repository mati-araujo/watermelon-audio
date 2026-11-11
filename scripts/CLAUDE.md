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
- `check-cpp-portability.sh` (WA-0.4, REQ-061 AC-061.7): regla 1, `jni.h` / `android/` fuera de su capa; regla 2, una API
  sólo-POSIX o sólo-Windows fuera de su capa (`platform/`, el backend de su plataforma, `usb/`, `jni/`), por header Y por
  SÍMBOLO suelto medido (`lstat`, `link(`, `O_NOFOLLOW`...: viven en headers que MinGW sí tiene, así que un lint de headers no
  los ve; comentarios y strings se ignoran). Dos baselines, TRINQUETES en las dos direcciones (una violación no listada falla,
  una entrada que ya no se reproduce falla), hoy vacíos: `cpp-portability-baseline.txt` (`ruta|API`) y
  `windows-cross/baseline.txt` (fuentes cuyo objeto no compila para Windows). Su diff es la revisión; nunca se agrega una
  entrada para acallar un rojo. `--self-test` primero.
- `check-windows-not-run.py` (REQ-061 AC-061.1): lo que NO corre en Windows se declara, no se esconde. El listado
  `tests/windows-not-run.txt` lleva `Suite.Test | razon` (test POSIX por naturaleza: `WMA_WINDOWS_NOT_RUN("razon")`,
  `tests/support/WindowsNotRun.h`) y las secciones `variante` (cuerpo con un `#if` de plataforma), `ambiente`
  (prefijo exacto de un skip de ambiente), `solo-windows`, `deshabilitado` (TEST con prefijo `DISABLED_`),
  `posix-solo | target | Suite.Test | razon` (se compila en POSIX y no en Windows) y las dos de HALLAZGO
  `cmake | ruta | construccion | razon` y `fuera | ruta | condicion | razon` (todo `#if` de plataforma FUERA de los cuerpos
  de TEST, sin numeros de linea; N apariciones = N entradas); TODA entrada con razon y trinquete en las dos direcciones.
  Son TRES partes y cada una ve una cosa distinta:
  1. **Diff de registro** (`windows-cross/check.py`, abajo): la verdad. Que TESTS se compilan en cada lado, por `nm`.
  2. **Estatica** (`python3 scripts/check-windows-not-run.py`, en `gate.sh`, CI y `run-cpp-tests.sh`, con su `--self-test`
     antes): lo que (1) NO ve porque el TEST SI se registra pero queda vacio o filtrado. Fuentes: ningun TEST/INSTANTIATE
     bajo un `#if` de plataforma/compilador (`_WIN32`, `__APPLE__`, `__GNUC__`, `__clang__`, `_POSIX_*`, `GTEST_OS_*`...),
     ningun `#if` asi en un cuerpo sin declarar (`variante`) ni fuera de los cuerpos sin declarar (`fuera`). CMake (los
     CMakeLists de tests Y los `.cmake` que incluyen, `thirdparty/googletest.cmake` incluido; parentesis balanceados):
     bajo un `if()` de plataforma o de compilador (`WIN32`, `UNIX`, `APPLE`, `MINGW`, `MSVC`, `CMAKE_SYSTEM_*`,
     `CMAKE_HOST_*`, `CMAKE_*_COMPILER*`, `ENV{MSYSTEM}`) todo `return()`, target, `list(FILTER|REMOVE_*)`, fuente de test,
     definicion (`target_compile_definitions`, `add_definitions`, `-D`) o propiedad (`set_*_properties`, `TIMEOUT`); en
     cualquier lado un genex con `PLATFORM_ID`/`*_COMPILER_ID`, un `${WIN32}`, y `DISABLED`/`TEST_FILTER`/`EXTRA_ARGS`/
     `SKIP_REGULAR_EXPRESSION`/`WILL_FAIL`/`EXCLUDE_FROM_ALL` sobre algo que ctest registra. Cada hallazgo se declara
     `cmake | ruta | clave | razon`. 🔴 Es una lista de construcciones: lo que se le escape lo caza (1) si saca el
     TEST, y NADIE si lo deja registrado y lo vacia por una variable que el parser no sigue (`set(X ${Y})`).
  3. **Resultados** (`--results DIR --build-dir BUILD`, lo corre `run-cpp-tests.ps1` en Windows): cada test que lista
     cada exe de la suite (`<exe> --gtest_list_tests`; suites parametrizadas y typed incluidas) tiene su XML (sin XML = ctest
     no lo corrio: `-E`, `TEST_FILTER`, `DISABLED`), ningun `<failure>` aunque ctest haya salido 0, todo skip declarado
     (marcador contra el listado, ambiente por prefijo, sin mensaje = rojo). `--partial` (`-Filter`/`-Fast`) tolera los
     registrados sin XML; un `<failure>` sigue rojo. Si Smart App Control no deja ejecutar un exe para listarlo, es ROJO.
     🔴 TODO es POR TARGET: el XML de `gtest-xml/<target>/` ata cada test a su exe (`looper_tests` y `looper_tests_dense`
     registran los mismos 207 nombres: un conjunto global dejaba que el XML de uno tapara al otro). Un test registrado por
     un exe necesita SU XML en SU directorio; el skip del listado se compara por target (corre en uno y se saltea en otro =
     rojo). Rojo tambien: un `*_include.cmake` sin `TEST_XML_OUTPUT_DIR` (o que no termina en el nombre del target) y,
     estatico, un `gtest_discover_tests` directo fuera de `function(wma_gtest_discover_tests)` (se declara como `cmake`).
  🔴 El inventario `fuera` obliga a DECLARAR cada `#if` de plataforma fuera de un cuerpo, pero no juzga si la razon es
  cierta: que no cambie lo que el test afirma se revisa a mano.
- `windows-cross/check.py` (REQ-061, `gate.sh --only windows-cross`; también compila la DLL Release de `desktop/` (`--dll`: -O3, -Werror, -static) y corre `check-jni-symbols.py --pe/--imports` sobre ella; Mac con Homebrew o contenedor Fedora, `build-image.sh`):
  (a) compila los OBJETOS de `desktop/` y de la suite de host para Windows (MinGW-w64, Debug, `-Werror`), baseline-trinquete
  sobre FUENTES (`windows-cross/baseline.txt`); (b) compila la suite POSIX con el compilador NATIVO del host (Mac: el de
  Xcode; contenedor: `gcc-c++`, pineado en `toolchain-pins.json`) en `build/windows-cross/<prefijo>/tests-posix`, y compara
  con `nm` las clases `<Suite>_<Test>_Test::TestBody` que define cada target de test (TEST_P y plantillas de TYPED_TEST
  incluidas, el espacio de nombres y los argumentos de plantilla se descartan): POSIX - Windows == `posix-solo` y
  Windows - POSIX == `solo-windows`, por target. Un target que el host no construye es «no verificable en este host» (se
  informa, en verde). 🔴 NO ve: un `INSTANTIATE_TEST_SUITE_P` que solo existe en un lado (la clase del TEST_P esta en los
  dos; lo caza la estatica), ni nada que cambie sin cambiar la lista de clases (eso es el `--results` de Windows), ni
  corre nada. Una fuente de tests que NO compila para Windows (en el baseline) no frena el diff: se excluyen sus objetos de
  los DOS lados (sus clases no se cuentan en ninguno) y se informa cual; un target de test sin NINGUN objeto vivo es ROJO.
  TYPED_TEST_P (`gtest_suite_<S>_::<N><T>::TestBody()`) se mapea a `S_N_Test`; los nombres se JUNTAN, nunca se parten por
  `_` (el arbol tiene tests con `_` y ninguna suite con `_`; `Suite_With_Underscores` esta en el self-test). En la Mac no se pudo medir desde la PC Windows: ahi el diff usa `nm` + `c++filt` de Xcode.
- `gate-windows.ps1` (REQ-061): el gate LOCAL de la PC Windows (MSYS2 UCRT64), PowerShell 5.1; no atesta, no baja nada y
  no escribe en el árbol (compara `git status --porcelain --ignored` antes y después). Pasos: `pin` (el GCC es el de
  `toolchain-pins.json`, lógica compartida en `windows-toolchain.ps1` con `run-cpp-tests.ps1`), `lint`, `suite`
  (`run-cpp-tests.ps1 -TimeoutSec 180`: ctest con timeout por test), `dll` (build Release en `%LOCALAPPDATA%\Temp\wma-gate-windows`,
  `--pe`/`--imports`), `jvm` (`System.loadLibrary` + `nativeIsEngineInitialized()==false` con PATH reducido; `-SkipJvm` lo
  salta y lo dice), `repo`. Fija `SetErrorMode` antes de lanzar hijos: un crash o `abort()` vuelve solo, sin diálogo de WER.
  `-Only <paso>` sirve para iterar y NO cuenta como corrida completa. La suite construye en `tests/build` (ignorado).
- `check-jni-signatures.py` (REQ-025): la FIRMA del cruce JNI. 🔴 Su guarda de COMPLETITUD lo
  sostiene: si el parser abarca menos prototipos de los que hay en el arbol, FALLA. Sin baseline:
  nacio en cero desajustes. Las tres preguntas: `audio/src/main/cpp/jni/CLAUDE.md`.
- `check-jni-symbols.py` (MINI-001): sólo NOMBRES contra el `.so`. REQ-061: `--pe DLL` exige exports == `external fun` menos
  USB (el `.def`/`--export-all-symbols` no existen: salen sólo los `JNIEXPORT`) e `--imports DLL` exige que la DLL importe
  sólo `KERNEL32.dll` y `api-ms-win-crt-*` (un runtime que no viaja dentro, como `libwinpthread-1.dll`, es rojo). Ambos leen
  `objdump -p` (`--objdump X` o `$OBJDUMP`).
- La ruta del bridge Kotlin que leen `check-jni-symbols`, `check-jni-signatures` y
  `check-doc-counts` es UNA constante: `jni_bridge_source.py` (el source set, REQ-055 S1).
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
