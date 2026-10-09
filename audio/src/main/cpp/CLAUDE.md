# Motor C++ — trampas locales

## Audio thread (RT)

- Callback 100 % **lock-free**: nunca mutex, new, malloc.
- `std::atomic` para parametros UI↔Audio; `incrementStateVersion()` despues de modificar estado
  observable; parametros con smoothing para evitar zipper noise.
- Logging via `platform/Logger.h` — NO es RT-safe, sólo fuera del hot path.

🔴 **Lo verifica `scripts/check-rt-safety.py`** (WD-1.1), en `gate.sh` y en el CI. Antes el
callback violaba estas reglas en **65 lugares**, y dos sobrevivian a `NDEBUG` por llamar a
`wma::logMessage` directo en vez de los macros `LOGI/LOGW`. Para no reintroducirlas:

- **NO loguees adentro del callback**, ni "periodicamente" ni "solo en debug". Los bloques
  `WMA_AUDIT` que habia eran un DEFECTO, no un precedente. Para observar, `wma::RtCounter`
  (`platform/RtCounter.h`): un `fetch_add` relajado que se lee desde el thread de control.
- **`reset()` es RT.** `EffectChain::reset()` y `Effect::reset()` los despacha `onAudioReady`.
- **Los handlers de voces son RT.** `VoiceManager::handleNoteOn/Off/ParamChange` los despacha la
  cola lock-free desde el thread de audio.
- **La captura es un SEGUNDO thread RT.** `InputNode::processInputBlock` corre en el thread del
  stream de entrada de Oboe con su propio DSP; lo que vale para la salida vale ahi.
- **`try_lock` si, `lock()` no.** Ojo con lo que parece un getter: `findVocoderIndex()` tomaba
  `chainMutex` y lo llamaban cuatro setters desde el thread de audio (WD-1.6).
- **El flush de denormales es POR THREAD.** `flushDenormalsRtSafe()` al principio de cada
  callback; `flushDenormals()` loguea y es sólo para el arranque, en el thread de control.
- **Un metodo nuevo con un nombre comun puede APAGAR parte del lint.** El walker sigue sólo las
  llamadas que resuelven a UNA definicion: un segundo `run`/`analyze`/`read` en CUALQUIER parte
  del arbol vuelve ambigua una llamada y le saca cobertura con el lint en verde (paso dos veces
  el 2026-08-19). `scripts/rt-coverage-baseline.txt` lo detecta: si sale en rojo una funcion que
  no tocaste, **renombra tu metodo**, no redeclares la cobertura.

## Portabilidad (iOS)

- Todo el motor cross-compila para iOS **salvo** `jni/`, `usb/`, `OboeBackend`, `LibusbBackend`
  y `PlatformAndroid.cpp`.
- Prohibido `#include <jni.h>` / `<android/...>` fuera de esas capas —
  `scripts/check-cpp-portability.sh` lo hace fallar (WA-0.4).
- Lo especifico de plataforma va detras de `IAudioBackend` o de `wma::platform`
  (`platform/Platform.h`); lo que depende sólo del ISA, en `platform/PlatformIsa.inc`.
- Un solo punto nombra backends concretos: `backends/PlatformBackends.cpp`.
- El thread RT **jamas** entra a Kotlin (el GC de Kotlin/Native no es RT-safe).
- El build de iOS vive en `ios/CMakeLists.txt`, **separado** del que maneja AGP: ese es Android
  de punta a punta (Oboe, libusb, JNI, flags de linker GNU que Apple ld rechaza). Separarlos deja
  el build que shippea en riesgo cero.

## Tests — como se espera (REQ-002)

- **Nunca sincronices con una duracion y afirmes despues.** Verde en una maquina ociosa, rojo en
  un runner con siete jobs: asi se cayo `master` tres veces el 2026-08-20.
- `wma_test::waitUntil(pred, techo)` para esperar a que algo **ocurra**; `wma_test::sleepFixed`
  para las esperas de **ausencia**. Las dos en `tests/support/TestWait.h`.
- Un `sleep_for` crudo dice que es (`// WAIT-OK: razon`) o el gate falla
  (`scripts/check-test-waits.py`; detalle en `scripts/CLAUDE.md`). El polling con deadline se
  reconoce solo.
- **Un receptor registrado en el motor tiene que vivir MAS que el motor.** Declararlo local en el
  cuerpo de un test es un use-after-free con abort (medido 5/5; KDoc de
  `wma_looper_set_event_callback`).
- 🔴 **Sacar una espera ciega no es el arreglo; poner una condicion en su lugar lo es.** Al
  quitar un `sleep` sin condicion, un test dejo de matar el mutante que antes mataba 20 contra 0.
- Un test que MIDE audio renderizado (nivel, dB, release, clicks): skill `medir-dsp`.

## Suite de host y sanitizers

- `bash scripts/run-cpp-tests.sh` corre ctest en PARALELO (149,7 s → 20,4 s el 18/08);
  `CTEST_JOBS=n` lo baja. El total lo imprime ctest; no se escribe en ningun doc (REQ-021).
- Los sanitizers NO son opcionales (el CI tiene un job para cada uno y encontraron dos bugs
  reales). Comandos en el `CLAUDE.md` raiz; **localmente van con `CTEST_JOBS=4`**: a full, TSan da
  timeout en `RateInvariance.*` y `NyquistLimits.*` por contencion de memoria. Ante un timeout,
  **bajar `CTEST_JOBS`, nunca subir el techo**. No es el default del script porque el CI no pasa
  `--timeout` y ahi el `-j` completo mide 45 % mejor. Numeros: `docs/dev/sanitizers.md`.
- `detect_leaks=1` (el de ci.yml) NO existe en macOS: rompe el discovery de gtest. Con
  `DISCOVERY_MODE PRE_TEST` el sintoma es `discover_tests failed to run command` (exit 8 de
  ctest), no un error de build.
- El TSan local (libc++) es MAS DEBIL que el del CI (libstdc++): para carreras manda el CI.
- `tests/hostjni/`: la libreria JNI para el host, con `FakeAudioBackend`. Valida la frontera
  JNI/Kotlin, NO audio en device. Ver `jni/CLAUDE.md`.
