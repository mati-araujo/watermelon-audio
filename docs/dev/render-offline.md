# Render offline por handle (REQ-055 S2, ADR-0008)

Una segunda instancia del motor renderiza a un buffer del llamador, sin device, sin hilo de
audio y sin tocar el motor en vivo. La consume Studio (NoisyPad desktop): export, stems y
re-render de tomas (S3, S11, S15). Este documento guarda **lo medido**: el contrato de
determinismo, el estado estatico que dos instancias compartian, el costo, la memoria y los
umbrales que S11 y S15 citan. Los numeros tienen fecha; los tests que los vigilan, nombre.

## La superficie

- **C** (`api/watermelon_audio.h`, seccion 24): `wma_offline_start` / `wma_offline_render` /
  `wma_offline_stop` sobre el `WmaEngine*` de `wma_engine_create()`. La configuracion es la
  API C que ya existia: todo `wma_*` que toma un `WmaEngine*` configura una instancia offline
  igual que al vivo. Errores tipados: `NOT_INITIALIZED` (nulo), `PARAMETER_OUT_OF_RANGE`
  (argumento), `INVALID_OPERATION` (estado; y siempre que el motor corre en un device: la API
  offline nunca convierte ni detiene al vivo).
- **JNI** (`nativeOffline*`, 17): handle `jlong` opaco = id en un registro, no el puntero.
  El doble modelo (global para el vivo, handles para offline) esta en `jni/CLAUDE.md`.
- **Hilos**: las llamadas sobre UNA instancia offline no se solapan (la C API lo documenta;
  el JNI lo hace cumplir con un mutex por instancia en las 15 operaciones). En vivo un
  setter puede correr a la par del callback porque este es corto y RT —`clearAllEffects`
  espera 20 ms antes de liberar—; un render offline es un hilo comun con bloques de hasta
  85 ms, y un `clearAllEffects` a la par era un use-after-free (review de S2). Dos
  instancias distintas, en paralelo, si.
- **El registro de handles** vive en el heap y no se destruye nunca (como `g_wmaEngine`): un
  static con destructor destruiria los renderers no cerrados durante el `exit()`.
  `JNI_OnUnload` lo vacia.
- **Kotlin** (`jvmSharedMain`): `OfflineRenderer`, `AutoCloseable`, `close()` idempotente; un
  renderer cerrado falla con `EngineNotInitialized` sin cruzar.
- iOS: la C API nueva compila en `build-ios.sh` (no hace falta cinterop nuevo).

## Determinismo (ADR-0008 §2)

Contrato: **misma configuracion + mismas llamadas en los mismos frames + mismo bloque ⇒ el
mismo buffer bit a bit**, en una instancia fresca o reusada. `wma_offline_start` llama a
`AudioEngine::resetChainForOfflineRender()` despues de preparar. La cancion programada tambien
parte de vacio: el start apaga su agenda (hay que publicarla despues del start; los clips se
conservan), ver `cancion-programada.md`.

Medido el 2026-10-26, suite de host Debug y Release (-O3), 48 kHz, cadena de 5 efectos con
estado (chorus, random reso, delay, tape echo, hall reverb), agenda de 16 gestos
(`core/tests/test_offline_handle.cpp`):

| engine | 2 frescas | reiniciada | sucia y reconfigurada | bloques 64..1024 | no sembrable |
|---|---|---|---|---|---|
| CLASSIC | bit a bit | bit a bit | bit a bit | bit a bit | — |
| KARPLUS_STRONG | bit a bit | bit a bit | bit a bit | bit a bit | — |
| FM | bit a bit | bit a bit | bit a bit | bit a bit | — |
| WAVETABLE | bit a bit | bit a bit | bit a bit | bit a bit | — |
| GRANULAR | bit a bit | bit a bit | bit a bit | bit a bit | — |
| SUPERSAW | bit a bit | bit a bit | bit a bit | bit a bit | — |
| SOUNDFONT | bit a bit | bit a bit | bit a bit | bit a bit | — |

Mas el arpegiador aleatorio (probabilidad 0,5, variacion de velocidad 0,8): bit a bit al
reiniciar. Ningun engine quedo no sembrable: la unica fuente no reproducible era el LFO
(abajo), que no es de un engine sino de efectos y moduladores.

**La excepcion entre bloques**, medida: un cambio A MITAD del render de

- **synth volume**: rampa por bloque (`mSynthVolumePrev`), -15 a -23 dB de error relativo
  entre bloque 64 y 1024 (la cola lo integra);
- **un parametro de engine**: `SynthEngine::smoothParam()` lo lee una vez por bloque, -4 dB
  (Granular, el parametro reprograma los granos) a -28 dB;

depende del tamaño de bloque. Master volume y parametros de efecto no (bit a bit). Con el
MISMO bloque, el cambio a mitad de render sigue siendo bit a bit
(`AMidRenderParameterChangeIsStillBitExactForTheSameBlock`). Conclusion para Studio: **un
export fija su bloque**. Al arrancar no hay excepcion: el reseteo arranca la rampa de synth
volume EN el volumen pedido (con la del constructor, 1,0, el arranque daba -26 dB entre
bloques).

Que resetea `resetChainForOfflineRender` y que mutante lo demuestra (cada uno se quito por
separado y se corrio la suite offline):

| linea quitada | cae |
|---|---|
| todo el reseteo | 15: reiniciada (7), sucia (7), arpegiador |
| `mEffectChain.reset()` | reiniciada (7), arpegiador |
| `mEngineDispatcher.reset()` | reiniciada KS/FM/Wavetable/Supersaw, sucia CLASSIC |
| `mOutputStage.reset()` | reiniciada (7), sucia (7), arpegiador |
| `mArpSequencer.resetForRender()` | arpegiador |
| `mVoiceManager->reset()` | **ninguno** — ver pendientes |

Tambien resetea fases y smoothers de los osciladores clasicos (`AudioSource::resetForRender`),
moduladores, `mArpSfPrevMidiNote`, la captura de forma de onda y la envolvente del dual touch.
NO toca configuracion (engine, parametros, preset, cadena) ni el CONTENIDO del looper, que
es entrada: el reseteo del looper por seccion es de S11.

**Pendiente, no medido**: el sistema de voces polifonico (`wma_voice_enable` +
`wma_voice_update_multi_touch`) no produce audio en una instancia offline del host (pico
~4e-5 con 2 voces activas, en 4 engines); ningun test del repo afirma que produzca audio,
tampoco en vivo. El reseteo de voces queda sin mutante que lo mate.

## Estado estatico que dos instancias compartian

| estado | donde | veredicto | como |
|---|---|---|---|
| `g_globalInstance` del `BackendManager` | `BackendManager.cpp` | **arreglado** | `wma_engine_create` registra solo si no hay (CAS), `destroy` solo des-registra el suyo, y una instancia offline lo suelta (`nativeOfflineCreate`, `wma_offline_start`); el JNI fuerza el del vivo; cada `AudioEngine` usa SU manager (`setBackendManager`), y la C API con handle tambien (`managerOf`). Antes: la instancia offline le robaba el global al vivo y, destruida, lo dejaba en nulo; y el motor offline leia el rate del stream del vivo. Tests: `OfflineStaticState.*`, `TheLiveEngineSoundsTheSame...`; mutantes M2/M2b |
| `defaultInstance` (fallback de `getInstance()`) | `BackendManager.cpp` | documentado | solo si no hay global; preexistente |
| `thread_local std::mt19937` + `random_device` | `LFO::randomFloat` | **arreglado** | xorshift32 por instancia, semilla fija, `reset()` la restaura. Era no sembrable: RANDOM_RESO, flutter del TAPE_ECHO, AM/FM con forma RANDOM. Tests: `LFO.RandomWaveforms...`, golden `RandomResoIsDeterministic...`; mutante M3. **Consecuencia en el vivo**: todos los LFO aleatorios arrancan con la MISMA semilla, asi que dos en el mismo motor (dos RANDOM_RESO en la cadena, el flutter del tape echo junto a un random reso) sortean la misma secuencia; antes el generador por hilo los intercalaba. Pendiente de decision: sembrar por posicion en la cadena (sigue determinista) |
| `crc_table` de stb_vorbis | `tsf_impl.cpp` (vendorizado) | **arreglado, con costo** | `crc32_init()` la reescribe en cada apertura Vorbis (SF3): dos cargas concurrentes = carrera. `SoundFontManager::decodeMutex()` serializa `tsf_load_memory`. Es un lock de PROCESO que comparten el vivo y la offline: nunca lo toma el callback, pero una carga de SoundFont en el vivo espera a que termine la decodificacion de una carga offline en curso (con GeneralUser .sf3, del orden de un segundo en Debug). El arreglo fino —`crc32_init` una sola vez— es un parche al stb_vorbis vendorizado, que hoy esta verificado identico a upstream: va con REQ-048, que ya lo reemplaza por un fork parcheado. Sin test (la suite no tiene un SF3 propio) |
| registro de `getNumParams` | `EffectChain.cpp` | **arreglado, sin test** | `static bool initialized` → inicializador magic-static. Su unico camino (`savePreset`/`loadPreset`) no tiene llamador de produccion |
| `g_cLogCallback` | `watermelon_audio.cpp` | **arreglado** | `std::atomic`; test `TheCLogCallbackCanBeSwapped...` (TSan) |
| `g_logCallback`, `LogCaptureBuffer::instance()`, `os_log` | `platform/` | seguro, documentado | de proceso POR DISEÑO: los logs de las dos instancias van al mismo sumidero; atomico / mutex no-RT / magic static |
| `g_wmaEngine`, `g_jniState`, `g_jniCache`, `g_javaVm`, `g_floatArrayPool`, `g_looperListenerMutex`, `g_rtInstalled` | `jni/` | seguro | son del modelo global; ninguna `nativeOffline*` los toca |
| volumenes y estado USB (`g_usb*`, `gUsbDeviceState`), contadores `static` de `LibusbBackend`, `thread_local` de `UsbTransferManager` | `jni/`, `backends/`, `usb/` | seguro | solo el camino USB del vivo; una instancia offline no tiene backend |
| `WavFile` counter | `looper/WavFile.h` | seguro | atomico |
| hooks `g*Hold*/Is*` de analisis e InputNode | `analysis/`, `nodes/` | seguro | solo con `WMA_TEST_HOOKS` |
| tablas `static const`/`constexpr` (stb_vorbis, PanLUT, etc.) | varios | seguro | inmutables o magic statics |

Inventario por `grep` de `static`/`thread_local`/globales no-const en `audio/src/main/cpp`
(fuera de `thirdparty/` salvo tsf y stb_vorbis, y de `tests/`), el 2026-10-26.

**Pendiente (C API sin JNI, iOS)**: si una instancia offline se crea ANTES que el vivo, el
CAS la deja como global y el vivo creado despues no se registra; al arrancar la offline el
global queda nulo y las funciones de backend sin handle caen al manager de fallback. El JNI
no tiene el problema (fuerza su vivo). El header pide crear el vivo primero; hoy iOS no
expone render offline. Arreglo posible: que el vivo reclame el global, con un camino que
sepa cual es el vivo antes de `wma_select_backend`.

## Dos instancias y el vivo

- Dos instancias renderizando en dos hilos dan, cada una, lo mismo que sola
  (`TwoInstancesRenderingAtOnce...`, tambien bajo TSan).
- **El vivo no se entera** (`TheLiveEngineSoundsTheSame...`): con el vivo sonando sobre el
  `FakeAudioBackend`, una sesion offline completa (crear, cargar SoundFont, 2 s de render,
  destruir) en paralelo deja la salida del vivo **bit a bit igual** a la de control, el
  manager global y el callback del backend en el vivo.
- **Afirmacion causal de "no registra xruns"** (`TheLiveKeepsCompletingCallbacks...`): con el
  hilo offline CONGELADO adentro de la carga del SoundFont (publicandolo, ya decodificado y
  con el `decodeMutex` suelto) y adentro de `startOffline` (con los locks de su instancia; el
  gancho es el callback de log), el vivo completa 50 callbacks. Si
  compartieran un lock, el vivo esperaria a un hilo detenido. Ninguna duracion en la
  afirmacion. (El gancho de `startOffline` es un LOGI: solo existe en Debug.)

## Costo: >= 10x tiempo real

`OfflineHandleEngines.AC_055_15_RendersAtLeastTenTimesFasterThanRealTime`: 10 s de audio,
48 kHz, bloque 256, cadena de 5 efectos, **tiempo de CPU del hilo** (`CLOCK_THREAD_CPUTIME_ID`;
el reloj de pared mediria la carga de la maquina). Medido el 2026-10-26, M-series, carga 12-17:

| engine | Release (-O3) | Debug (-O0) |
|---|---|---|
| CLASSIC | 61,5x | 6,9x |
| KARPLUS_STRONG | 60,7x | 6,8x |
| FM | 57,9x | 6,9x |
| WAVETABLE | 60,9x | 6,9x |
| GRANULAR | 60,3x | 6,7x |
| SUPERSAW | 61,3x | 6,9x |
| SOUNDFONT | 54,5x | 6,5x |

La suite del gate es Debug: ahi el test sale **SKIPPED** (como bajo sanitizers), nunca con
el umbral bajado. Se reproduce con la suite en Release:

```bash
cmake -S audio/src/main/cpp/tests -B /tmp/wma-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_BASE_DIR=$PWD/audio/src/main/cpp/tests/.deps
cmake --build /tmp/wma-rel --target core_tests
/tmp/wma-rel/core_tests/core_tests --gtest_filter='*TenTimes*'
```

## Memoria con dos instancias

`OfflineHandle.MeasureTheMemoryOfTwoInstancesWithASoundFontEach` (a pedido:
`WMA_MEASURE_OFFLINE_MEMORY=1 WMA_GENERALUSER_SF3=<ruta>`), `phys_footprint` del proceso,
GeneralUser GS `.sf3` (9,5 MB en disco) cargado en las dos. Dos corridas el 2026-10-26:

| paso | MB |
|---|---|
| un motor (`wma_engine_create`) | +3,0 a +3,1 |
| su SoundFont | +64,6 a +76,1 |
| segundo motor | +3,7 a +4,6 |
| su SoundFont | +66,0 a +66,1 |
| renderizar 200 bloques | +0,3 a +0,7 |

**Una instancia offline con el mismo SoundFont cuesta ~70 MB**: el SF3 se decodifica a PCM
por instancia (no se comparte entre motores). Cuatro ciclos crear-cargar-destruir: -40 MB en
total (no hay fuga; macOS retiene paginas y las devuelve despues). Compartir el font entre
instancias (`tsf_copy`) es la palanca si Studio lo necesita; no se hizo (sin consumidor).

## Umbrales para S11 y S15 (`core/tests/support/RenderChecks.h`)

Los calibra y vigila `core/tests/test_offline_render_checks.cpp`.

**S11 — discontinuidades en las uniones.** `findJunctionDiscontinuities(render, uniones)`:
en cada union conocida, un salto |x[n] - x[n-1]| > `kJumpFloor` (0,005) que supera
`kJumpRatio` = **2,0** veces el salto maximo de los 32 frames de cada lado.

- Por que RELATIVO: un umbral absoluto no separa nada. Un render limpio de FM, Granular o
  Supersaw a volumen de exportacion salta hasta 1,73 de una muestra a otra.
- Por que SOLO en las uniones: un render limpio de Supersaw o Granular tiene saltos de
  100x-400x su entorno por construccion (reset de la sierra, borde de grano).
- Datos: uniones limpias (40 frames x 14 renders) razon maxima **1,28** (margen x1,57);
  80 cortes inyectados (union de dos renders sin fundido, y corte a silencio): p10 0,36,
  p25 0,78, mediana 2,37; detectados **43 de 80 (54 %)**. Con 1,5: 48 (margen x1,17); con
  4,0: 29. Lo que no ve: un corte cuyo salto es menor que el doble del movimiento propio de
  la señal (banda ancha, o cerca de un cruce por cero). Es necesario, no suficiente: S11
  hace el fundido igual.

**S15 — onsets del re-render.** `onsetsOf` (el `TrackBuffer::detectOnsets` del looper, hop
256) y `maxOnsetDifferenceMs`; umbral `kMaxOnsetDifferenceMs` = **10 ms** (el de ADR-0008).

- Datos: toma con los gestos en su frame exacto (bloques de 32) contra re-render con los
  gestos en el borde de bloque de 256 siguiente, corridos 0..224 frames; KS, FM, Supersaw,
  SoundFont, 10 notas: peor **5,33 ms = un hop** (las dos cuantizaciones caen en la misma
  grilla). Margen x1,9.
- Vale para re-render con bloque <= 256 y hop 256 (con bloque 512 la cuantizacion sola llega
  a 10,7 ms). No cubre el muestreo de gestos a ritmo de UI ni el coalescer: eso es de los
  gestos, no del render, y S15 lo mide contra la toma real.
