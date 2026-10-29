# Mezcla de la cancion, bus «Espacio», master y taps (REQ-055 S12)

Lo que Studio usa para mezclar la cancion programada (`docs/dev/cancion-programada.md`), escucharla
con un master y exportar WAV y stems por render offline (`docs/dev/render-offline.md`). Contrato de la
C API: seccion 25 de `api/watermelon_audio.h` (mas `wma_offline_render_taps` en la 24). El diseno de
NoisyPad es su ADR-0013 (privado); aca esta lo que cambia en watermelon-audio, en lenguaje de este
repo. Version: **2.25.0**.

## El grafo

```
por pista t (solo con la cancion activa):
  SongVoice_t -> ganancia_t (smoother) -> paneo_t (EqualPowerPanLUT) -> seco_t --+--------------> Σ seco
                                                                                 \- x envio_t -> Σ envio
                       Σ envio -> DelayEffect 100 % wet (ms)  x retorno_delay  --+
                                  ReverbEffect 100 % wet      x retorno_reverb --+-> retorno FX
  premezcla = Σ seco + retorno FX  ->  master del looper (wma_looper_set_master_volume)  ->  bus del motor
  bus del motor -> perfil de salida:
     PROTECTED (default)  OutputStage de siempre                 -> device
     MASTER               MasterStage -> volumen de monitoreo    -> device
     RAW (solo offline)   nada                                   -> buffer del llamador
```

## Cambio de contrato de la voz (2.25.0)

Hasta 2.23.0 la voz de la cancion (`SongPlayer::renderSongVoice`) tomaba volumen, paneo, mute y
`sendToFx` del `TrackBuffer` de su pista, la misma que toca la escucha de tomas. Desde 2.25.0:

- **Con la cancion activa, esos cuatro controles del `TrackBuffer` no se aplican a las voces.** La voz
  se gobierna solo por `wma_mix_set_track` (ganancia lineal, paneo, envio) y por la agenda. El mute es
  ganancia 0.
- **Ninguna voz entra a la cadena de escena** (`EffectChain`): todas suenan en la pasada no-FX. Una
  pista marcada `sendToFx` no cambia eso mientras la cancion este activa.
- Los smoothers de la voz arrancan **en su target** al activarse la cancion (inactiva -> activa) y en
  `wma_offline_start`: el primer bloque no rampea desde ganancia 1 / paneo 0.
- Un smoother se clava en el target a menos de 1e-6: un envio «0» no deja un residuo que mantenga vivo
  el bus.
- Sin cancion activa nada cambia: las tomas siguen por `TrackBuffer::mixInto`.

Anexo a ADR-0007 (cancion sobre el looper): la frase «una `SongVoice` por pista respeta volumen,
paneo, mute de usuario y envio al FX» queda reemplazada por lo de arriba. La memoria de clips duplicada
mientras corre un trabajo offline (D-S11-6) es aceptada: cada instancia offline es independiente.

Anexo a ADR-0008 (render offline por handle): el contrato de determinismo cubre los taps
(`wma_offline_render_taps`: misma configuracion + mismas llamadas + mismo bloque => mismos taps, bit a
bit), el perfil RAW (solo offline) y el procesador de master (`wma_master_*`): tras `reset`, la misma
entrada da la misma salida con cualquier tamano de bloque.

## La API

| funcion | rangos (fuera de rango o no finito => `PARAMETER_OUT_OF_RANGE`, nada se aplica) |
|---|---|
| `wma_mix_set_track(track, gain, pan, send)` | pista < pistas activas; ganancia 0..2; paneo -1..1; envio 0..1 |
| `wma_mix_set_space(WmaMixSpace*)` | retornos 0..1; tamano 0,1..2; decay 0,1..10 s; damping 0..0,95; predelay 0..100 ms; cortes 20..500 / 1000..20000 Hz; delay 1..2000 ms **y que quepa en la linea del delay al sample rate vigente** (192000 muestras: 2000 ms hasta 96 kHz, ~1000 ms a 192 kHz); feedback 0..0,9 |
| `wma_mix_set_master(WmaMixMaster*)` | ganancia -60..+40 dB; techo -20..0 dBTP; release 1..2000 ms |
| `wma_mix_set_output_profile(p)` | PROTECTED / MASTER / RAW; RAW fuera de un render offline => `INVALID_OPERATION` |
| `wma_offline_render_taps(mix, fx, tracks, track_count, frames)` | frames 1..max_block; track_count 0..pistas activas; sin start => `INVALID_OPERATION` |
| `wma_mix_get_master_latency_frames`, `wma_get_xrun_count` | lecturas; 0 para NULL |
| `wma_master_create/configure/reset/process/get_latency_frames/destroy` | sample rate 8000..192000; frames 1..`WMA_MASTER_MAX_FRAMES` (2^20) |

- **Hilos.** Los setters son stores atomicos de escalares independientes (sin consistencia entre si: el
  suavizado tapa el desfase de un bloque); el callback los lee una vez por bloque. Los parametros del
  espacio los **aplica el hilo que procesa** (los setters de reverb/biquad no son atomicos): un contador de
  version avisa. No hay mutex ni alloc en el camino RT.
- **El bus se crea perezosamente**, en el hilo que hace la primera `wma_mix_set_track/space`, se publica
  por un puntero atomico y **no se libera mientras viva el motor**. Un motor que nunca llama (el movil) no
  paga memoria ni CPU. `publishSongBus` lo prepara **al rate vigente, bajo el mismo lock con que
  `setSampleRate` publica el rate**: un cambio de rate concurrente con la creacion deja el bus en el rate
  final (si gana la creacion, lo re-prepara `setSampleRate`; si gana el cambio, la creacion lee el rate
  nuevo). Un cambio de sample rate lo re-prepara con el RT quiesced (como al resto).
- **Capacidad.** El bus tiene SU capacidad en frames (`ISongSpaceBus::spaceBusCapacityFrames`, el
  `mDelayOut` del bus): crece con `prepareMixBuffer` (`reserveSpaceBus`, sin reiniciar el estado) y el
  guardia de cada bloque exige que alcancen los tres buffers del looper **y** esa capacidad. Un bloque
  mas grande salta el bus y se cuenta (`AudioLooper::getSongBusOverflowBlocks`), no aloca. Antes el
  guardia miraba solo los buffers del looper: con `prepareMixBuffer(8192)` y un bloque de 6000 el delay
  del bus (fijo en 4096) escribia fuera de su memoria.
- **El primer juego de parametros de un bus fresco** clava el suavizado del tiempo del delay en su
  objetivo (`DelayEffect::snapDelayTimeToTarget`): sin eso el suavizado de 50 ms lo desliza desde el
  default de 250 ms. Las lineas ya las vacio `prepareSpaceBus` en control; en el RT no hay memset (el
  `reset()` que habia ahi eran ~1,5 MB: 850 us en Debug en el host, contra < 1 us ahora).
- **Tiempo del delay y sample rate.** La linea del `DelayEffect` es de largo fijo en muestras; la C API
  rechaza (`PARAMETER_OUT_OF_RANGE`, nada se aplica) un `delay_time_ms` que no cabe al rate vigente
  (`AudioEngine::songDelayMaxMs`). Si despues el rate sube, el `DelayEffect` acota en silencio al
  maximo de su linea hasta la proxima llamada de `wma_mix_set_space`.
- **La cola del bus (D-S12-18).** Con la cancion inactiva (agenda sacada o transporte parado) el bus
  sigue procesando **con envio cero**: la cola suena y decae como en un DAW, en vez de cortarse en seco
  y quedar congelada hasta la proxima activacion. Se apaga cuando **todo lo que hay dentro del bus** queda
  bajo -120 dBFS (pico de bloque < 1e-6) durante **2,5 s seguidos** (mas que el delay maximo de 2 s, para
  no confundir el hueco entre dos ecos con el final). El silencio se mide **antes de los retornos** (la
  salida 100 % wet de la reverb y del delay, `spaceBusPreReturnPeak`): con un retorno en 0 la salida es
  muda pero las lineas pueden seguir llenas, y apagar ahi dejaba la realimentacion congelada (a 0,95 el
  eco volvia al reactivar). **No hay tope por tiempo
  para una cola audible sana**: cortarla obligaria a vaciar las lineas (`resetSpaceBus`, ~1,5 MB de memset) en el
  callback al reactivar, y Ambient (delay 1,5 tiempos, realimentacion 0,5) a <= 60 bpm ya pasaba los 30 s del tope
  viejo. Lo que garantiza que la cola se agota son **los rangos de la C API**: realimentacion <= **0,9** y damping
  de la reverb <= **0,95**. El interpolador cubico del delay (`cubicInterpolate`, Bourke, `a2 = y2 - y0` sin 0,5)
  tiene ganancia maxima **1,0887** (fraccion 0,5, ~9,4 kHz a 48 kHz): con 0,95 y un tiempo fraccional (250,0104 ms)
  cada vuelta ganaba y la cola divergia (3,8e-4 a 40 s, 22398 a 180 s); con 0,9 · 1,0887 = 0,98 la linea es estable
  con cualquier fraccion (un tono normal pierde ~35 s por cada 250 ms de delay a 0,9; el peor caso, un tono de ~9,4 kHz con fraccion 0,5, pierde solo el 2 % por vuelta: ~190 s por cada 250 ms, y con delays largos pasa los 10 min del tope, que lo corta con fundido). Con
  damping 1 los combs de la reverb quedan en DC y el bus nunca baja de 1e-6. **Saneo de la entrada del
  bus** (`sanitizeSongSend`): antes de `runSpaceBus`, toda muestra no finita del envio (un clip con NaN/Inf) se
  escribe 0, y toda de modulo > **`kSongSendLimit` = 64** se recorta a ±64; el bloque se cuenta en los dos casos
  (`AudioLooper::getSongBusGuardTrips`, un solo contador). Reverb y delay ya ponen en cero su salida no
  finita, pero sus lineas internas quedaban en NaN para siempre y el bus se apagaba «por silencio» sin marcarse viejo.
  El limite hace falta porque un float WAV crudo con una muestra 3e38 es finito y aun asi la reverb suma L+R
  (`(inL + inR) * 0,5`) y desborda a Inf (con 5e37–2e38 el FX quedaba clavado en ±4 durante segundos). 64 =
  16 pistas x ganancia 2 x clip ~1, con margen para un float > 1: ningun envio legitimo lo alcanza. Con el envio
  en ±64 las lineas no desbordan: el delay a 0,9 llega a ~50x el envio (3200) y la reverb tiene ganancia < 1 por
  vuelta, ordenes de magnitud bajo el limite del float (los dos tests con el bus real lo miden). No hay reinicio del bus en el RT. **No hay guarda por
  pico de la SALIDA** (la de pico > 16 se saco): `DelayEffect` no acota su salida y con realimentacion 0,9 el wet llega a
  ~10x el envio con material periodico (~50x cerca de 9,4 kHz con fraccion 0,5), asi que no hay un umbral fisico que no
  corte material legitimo (ganancia 2, paneo −1, seno de 100 Hz a escala plena, 250 ms, realimentacion 0,9 lo pasaba
  cada ~4 s); con realimentacion <= 0,9 la linea es estable (verificado a 44,1/48/96/192 kHz con fraccion 0,5). El
  **tope de 10 min de cola** cierra con un fundido lineal de **duracion fija, 50 ms**, repartido en los bloques que
  hagan falta (con bloques de 1 frame dura lo mismo que con 512), cuenta el tope (`getSongBusTailCaps`) y da el bus por
  viejo. La cuenta de la cola vuelve a 0 con cada activacion de la cancion. **Reactivar en medio del fundido**
  cancela el fundido (sin contar tope ni marcar el bus viejo) y el retorno vuelve a 1 con una rampa lineal de 50 ms
  desde la ganancia en que iba (`startSongBusRise`), sin escalon; `resetSongForRender` borra fundido y subida. La proxima activacion reinicia un bus
  viejo: ese memset en el RT queda solo para el tope y para el bloque mayor que la capacidad del bus (falla ya
  contada). Si la cancion
  se reactiva durante la cola, la cola sigue (no se reinicia). **Costo de mantenerlo vivo** (medido en
  Release, ver abajo): ~24 us por callback de 256 frames, 0,44 % del periodo. Los unicos `resetSpaceBus`
  que quedan en el RT son los de las fallas contadas: un bloque mayor que la capacidad del bus y el tope de la cola (el
  bus se da por viejo y la proxima activacion lo reinicia); con la capacidad de `publishSongBus` el primero no ocurre
  por configuracion, y esa falta ya salta el bloque (sin fundido: no hay estado del bus con que hacerlo). El
  tap de FX recibe el retorno de la cola, asi que `premezcla == Σ pistas + FX` tambien ahi. Offline:
  `resetSongForRender` reinicia todo (instancia fresca) y el determinismo no cambia.
- **Los taps** se toman despues del master del looper (cada uno con la misma rampa), asi que
  `premezcla == Σ pistas + FX` por construccion del grafo, hasta el redondeo de float (~1e-7: la suma se
  hace en otro orden). Un bloque sin cancion deja en cero los taps de pista; el de FX lleva la cola del bus mientras dura.
- `wma_get_xrun_count` lo refresca el callback cada ~500 bloques: va atrasado. Una instancia offline da 0.

## `MasterStage` (`dsp/MasterStage.h`)

Ganancia (dB, rampa lineal por bloque) -> limitador brickwall true-peak. No reemplaza a `LookaheadLimiter`
(el movil lo sigue usando: mide pico de muestra y no garantiza su techo).

- **Deteccion:** sobremuestreo 4x con el FIR polifasico de ITU-R BS.1770-4 anexo 2 (4 fases x 12 taps),
  enlazado entre canales. El registro del instante `j` describe las posiciones `[j-6, j-5)`.
- **Lookahead fijo de 5 ms** (`D` frames; 240 a 48 kHz). La salida es la entrada demorada `D` frames por la
  ganancia: **latencia declarada = `D`**.
- **Ganancia:** requerida `techo / pico`, **minimo sobre una ventana de `D+3` registros** (deque monotona de
  capacidad fija), limitada en subida por una release exponencial y suavizada por una **media movil entera
  (Q24) de `D-12` muestras**. La media de `N` valores cada uno <= lo requerido cubre toda muestra que un pico
  puede tocar (el rizado del FIR incluido, +-7 muestras): la ganancia baja *antes* de que el pico salga de la
  linea de retardo. No hay residuo de redondeo, asi que **por debajo del umbral la ganancia es exactamente 1 y la
  salida es la entrada demorada, bit a bit** (tambien tras una release completa).
- **Guarda final:** clamp al techo (`clampHits()` cuenta las veces que actua: en operacion normal, 0).
- **RT:** todo el estado (anillo de 2048 frames, deque, anillo de la media) se dimensiona en `prepareMaster`,
  una vez, para el sample rate maximo (192 kHz). `processMaster` evoluciona muestra a muestra: la salida es
  la misma con bloques de 1, 64 o 4096 (con parametros fijos). Entradas no finitas se anulan.
- **Desvio del diseno, medido:** la deteccion suma **+0,1 dB de holgura**. El FIR de 4x subestima la
  reconstruccion ideal: hasta ~0,07 dB con ruido limitado a 0,2 fs, ~0,4 dB a 0,45 fs, y hasta **9 dB** con
  ruido blanco pleno (la reconstruccion ideal de ruido blanco se excursiona mucho mas que sus muestras). Por eso
  la garantia contra una referencia de 16x (<= techo + 0,1 dB) vale para material con la banda de un programa
  (la bateria de `test_master_stage.cpp` usa ruido de banda limitada a 0,25 / 0,35 fs), y para ruido blanco pleno
  vale contra el medidor BS.1770 de 4x, que es la definicion de dBTP.

## Perfiles de salida

`AudioEngine::applyMasterAndProtectOutput` elige por perfil. **PROTECTED es el default y es exactamente la cadena
de 2.23.0** (volumen maestro, limitador con lookahead, soft clip TANH, dither, hard limit): lo verifica
`OutputProfiles.ProtectedIsTheTwoTwentyThreeOutputBitForBit` (hash FNV-1a de 60 bloques, capturado compilando
`core/tests/support/OutputGoldenScenario.h` contra `96ca8d61` antes de tocar la cadena; solo macOS arm64, porque
`tanh` y `sin` de otra libm difieren en el ultimo bit) y, en todas las plataformas,
`ProtectedEqualsTheOldChainAppliedToTheRawMix` (con volumen maestro 0,6: con 1,0 un cambio de lugar del volumen no se ve;
el `setMasterVolume(1.4f)` inicial del escenario del golden lo acota el motor a 1,0, tanto en 2.23.0 como ahora, y se deja tal cual
para no cambiar la captura). Para recapturar el hash: compilar el mismo header contra el
arbol de 2.23.0 (un ejecutable `golden_gen` como `sf_render_preset`), no contra este.

- **MASTER** reemplaza a `OutputStage::processOutput` por `MasterStage` y aplica **despues** el volumen de
  monitoreo (`wma_set_master_volume`): bajar el volumen no cambia lo que hace el limitador ni lo que mide un export.
  La salida es identica a `wma_master_process` sobre la misma entrada (`MasterProfileEqualsTheStandaloneMaster...`).
  Al entrar a MASTER el master arranca de cero (no suena lo que quedo en su linea de retardo).
- **RAW** no aplica nada (ni el volumen de monitoreo). Solo se acepta en una instancia offline; con device da
  `INVALID_OPERATION`. Defensa en profundidad: el callback trata RAW como PROTECTED si el motor no esta en modo
  offline, y `stop()` de una instancia offline devuelve el perfil a PROTECTED.
- Los medidores de salida estan en la cola de los tres perfiles.

## JNI y Kotlin

- **Vivo:** `nativeMixSetTrack / SetSpace(FloatArray[10]) / SetMaster(FloatArray[3]) / SetOutputProfile`,
  `nativeGetXRunCount`. **Por handle:** las mismas con `nativeOfflineMix*` (se agrego `nativeOfflineMixSetMaster`
  para que `OfflineRenderer` implemente el mismo `ISongMixBridge`), mas `nativeOfflineRenderTaps(handle,
  FloatArray, frames, trackCount)`. **Master:** `nativeMasterCreate/Configure/Reset/Process/Latency/Destroy`.
- **Taps:** UN `FloatArray` `[premezcla | FX | pista 0 | ... | pista N-1]`, cada region `frames*2` floats (estereo
  intercalado). El largo se valida (`>= frames*2*(2+trackCount)`, en 64 bits) **antes** de renderizar y de
  escribir: un array corto da `PARAMETER_OUT_OF_RANGE` y el motor no avanza ni un bloque.
- **El procesador de master es un id, no un puntero:** un registro (en el heap, vaciado por `JNI_OnUnload`) lo
  mapea al `WmaMaster*`; un id desconocido, cerrado o ya destruido da `NOT_INITIALIZED` y nunca toca memoria; los
  ids no se reusan. Cada slot tiene su mutex. `process` copia a un scratch, procesa y copia de vuelta.
- **Arrays de espacio y master** se leen completos a la pila: nulo o de otro largo => `PARAMETER_OUT_OF_RANGE`. El
  perfil entra como `jint` y se valida **antes** de convertirlo al enum.
- **Kotlin** (`audio/api`, commonMain): `ISongMixBridge` (lo implementan `AudioNativeBridge` y `OfflineRenderer`),
  `IOfflineTapRenderer` (solo `OfflineRenderer`), `IMasterProcessor` y los tipos `SongSpace`, `MasterParams`,
  `OutputProfile`. `MasterProcessor` (jvmSharedMain, `AutoCloseable`). **No** se inflaron `ISongBridge` ni
  `ILooperBridge`; iOS no las implementa en v1. Cerrado el dueno: `EngineNotInitialized` sin cruzar.
- Arnes: `SongMixJniTest` (frontera: taps ida y vuelta, arrays cortos, ids, handle cerrado).

## Guardas -> el test que cae


| guarda | test |
|---|---|
| pico de muestra <= techo y true-peak <= techo + 0,1 dB | `MasterStage.BatteryNeverExceeds...`, `CeilingHoldsAtOtherRates...`, `FullBandWhiteNoiseHoldsTheBs1770Meter` |
| deteccion 4x (no solo pico de muestra) | las mismas (un mutante de solo pico de muestra las pone rojas) |
| ventana del minimo / lookahead | las mismas (mutante `mWindow = D/4`) |
| ganancia exactamente 1 bajo el umbral y tras la release | `BelowThresholdIsTheDelayedInput...`, `GainReturnsToExactUnity...` |
| latencia declarada = medida | `DeclaredLatencyEqualsMeasuredImpulseDelay`, `TheDeclaredMasterLatency...` |
| salida invariante al bloque; fresca = reusada | `OutputIsBitExactAcrossBlockSizes`, `FreshAndResetInstancesAgree` |
| rampa lineal por bloque; reset salta al objetivo | `GainRampsLinearlyPerBlockAndResetSnaps` |
| 0 alloc en `processMaster` | `ProcessDoesNotAllocate` |
| send 0 => FX 0 y salida = Σ secos | `SongMix.SendZeroGives...` |
| salida RAW = Σ taps + FX; taps con la rampa del master | `RawOutputEqualsTheSumOfTheTapsPlusTheFxReturn` |
| envio post-fader y post-paneo | `SendIsPostFaderAndPostPan` |
| ley de paneo del looper | `PanLawIsTheLooperEqualPowerOne` |
| snap al target (activacion y start; la voz suena ya en el primer bloque) | `FirstBlockStartsAtTheMixTarget...`, `SongVoice.ReactivatingTheSong...` |
| el `TrackBuffer` no alcanza a la cancion; ninguna voz a la cadena | `TrackBufferControlsHaveNoEffect...`, `SongVoice.TrackBufferControlsDoNotReach...` |
| fresca = reusada con bus | `AReusedInstanceWithTheBusGivesTheBitsOfAFreshOne` |
| el envio apagado no corta la cola ni alimenta el bus para siempre | `TurningTheSendOffLetsTheTailDecay...` |
| la cola sigue tras sacar la agenda y decae; entra al tap de FX | `TheTailKeepsDecayingAfterTheScheduleIsRemoved`, `SongBusTail.TheBusKeepsRunning...`, `SongBusTail.TheTailReturnReachesTheOutput` |
| reprogramar durante la cola no la encuentra congelada ni reiniciada | `AScheduleReprogrammedDuringTheTailFindsTheDecayedTail...` |
| la cola se apaga a los -120 dBFS (2,5 s), medido antes de los retornos; sin tope ni reset en el RT | `SongBusTail.AQuietTailStops...`, `SongBusTail.ABusWithSilentOutputButFullLinesStaysAlive`, `SongBusTail.ALoudTailIsNotCutAtThirtySeconds...`, `AfterTheTailIsGoneReactivating...` |
| un retorno en 0 no congela la linea; la cola pasa los 30 s sin salto | `SongMix.AReturnAtZeroDoesNotSwitchOff...`, `AMutedReturnKeepsTheLineDecaying...`, `ALoudTailGoesPastThirtySecondsWithoutAStep` |
| un bus creado tras `prepareMixBuffer` toma la capacidad mayor | `SongBus.ABusCreatedAfterPrepareMixBufferTakesTheLargerCapacity` |
| el bloque mayor que la capacidad del bus lo salta y se cuenta (nunca escribe fuera) | `SongBus.ABlockLargerThanTheBusCapacity...`, `SongBusTail.ABlockLargerThanTheBusCapacityDuringTheTail...`, `SongMix.ABlockLargerThanThePreparedMixBuffer...` (ASan) |
| `prepareMixBuffer` agranda tambien el bus | `SongBus.PrepareMixBufferGrowsTheBus...` |
| el bus se prepara al rate vigente al crearse y al cambiar el rate | `SongBus.APublishedBusIsPreparedAtTheCurrentRate`, `SongBus.ARateChangeRacingTheBusCreation...`, `SongMix.TheDelayTimeFollowsASampleRateChange...` |
| el master se prepara al rate del motor | `OutputProfiles.TheMasterIsPreparedAtTheRateTheEngineStartsAt` |
| el primer apply no vacia la linea del delay | `SongMix.TheFirstParameterApplyPinsTheDelayTime...` |
| el tiempo del delay cabe en la linea al rate vigente | `MixApi.TheDelayTimeIsRejectedWhenItDoesNotFit...` |
| 0 alloc / sin carrera / costo del callback | `ABurstOfMixSettersAgainstTheCallback...` (TSan en la suite de sanitizers) |
| PROTECTED = 2.23.0 | `ProtectedIsTheTwoTwentyThreeOutputBitForBit`, `ProtectedEqualsTheOldChain...` |
| MASTER = `wma_master_process`; el volumen va despues | `MasterProfileEqualsTheStandaloneMaster...` |
| RAW nunca con device ni tras dejar de ser offline | `RawIsRefusedOnAnEngineThatIsNotOffline`, `AnInstanceThatStopsBeingOffline...` |
| medidores en la cola de los tres perfiles | `TheOutputMetersAreAtTheTailOfAllThreeProfiles` |
| rangos y NULL de la C API | `test_c_api_mix.cpp` |
| JNI: array de taps corto, handle/ids cerrados, arrays mal formados | `SongMixJniTest` |

## Lo medido

Host macOS arm64 (M-series), 2026-10-28. Los numeros no se escriben a mano en otro doc.

| medicion | Debug (-O0) | Release (-O3) |
|---|---|---|
| callback con 8 pistas de cancion + bus + master MASTER, 256 frames, rafaga de setters (6000 callbacks): maximo | 1408 us | 147-173 us |
| idem, p99 | 601 us | 117-123 us |
| periodo del callback a 48 kHz | 5333 us | 5333 us |
| pasada offline con 8 taps de pista + bus, bloque 4096, 10 s de audio, CPU del hilo | skipped | **118-123x** tiempo real (0,081-0,084 s de CPU) |
| cola del bus viva (reverb + delay 1,5 s, realimentacion 0,5, envio 0), callback de 256 frames, CPU del hilo | skipped | **28,5 us** contra 4,9 us con el bus apagado: +23,6 us = **0,44 %** del periodo |

El maximo de Release es el 3 % del periodo. El piso de 10x del diseno se cumple con >10x de margen.
La rafaga (`SongMix.ABurstOfMixSettersAgainstTheCallbackAllocatesNothingAndStaysInsideThePeriod`) imprime
`[ mix ]`; el costo del export, `[ COSTO   ]`; el de la cola, `[ COLA    ]`. Para reproducirlo con la suite en Release:

```bash
cmake -S audio/src/main/cpp/tests -B /tmp/wma-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_BASE_DIR=$PWD/audio/src/main/cpp/tests/.deps
cmake --build /tmp/wma-rel --target core_tests
/tmp/wma-rel/core_tests/core_tests --gtest_filter='SongMix.APassWith*:SongMix.ABurst*:SongMix.Keeping*'
```

En el gate (Debug) el test de costo sale SKIPPED, nunca con el umbral bajado; el del periodo solo se afirma sin
sanitizers (con ellos el callback es mas lento por construccion).

## Limites conocidos

- **Deuda preexistente (issue aparte, no se toca aca):** `cubicInterpolate` (`dsp/DSPMath.h`) tiene ganancia maxima 1,0887 y
  `DelayEffect` lo usa con tiempo fraccional: con realimentacion > 0,9186 diverge. Afecta tambien al `DelayEffect` de la cadena
  de escena del movil (PROTECTED tiene que seguir igual bit a bit a 2.23.0, por eso el bus se protege con el rango de la C API).
- El bus se crea para `max(4096, lo que prepareMixBuffer ya dimensiono)` y crece con `prepareMixBuffer`; un callback vivo mas grande que eso
  salta el bus ese bloque (cuenta, no aloca).
- La carrera "cambio de rate vs. creacion del bus" tiene un test de estres (`ARateChangeRacingTheBusCreation...`)
  que es probabilistico: la extension del lock de `setSampleRate` la cubre el diseno, no un test determinista.
- El master del export (`MasterStage` por `wma_master_*`) y el vivo comparten codigo, no estado: Studio los
  mantiene en el mismo punto con `reset` + `configure` antes de procesar.
- El hash de 2.23.0 es de macOS arm64; en otras plataformas vale la prueba estructural.

**Guarda -> test que cae** (bus REAL salvo donde se aclara):

| Guarda | Test |
|---|---|
| Saneo del envio no finito (NaN, Inf, −Inf; reverb y delay) y cancion limpia posterior bit a bit igual | `SongMix.ANonFiniteSampleInAClipNeverPoisonsTheRealBusSoALaterCleanSongSoundsTheSame` |
| Recorte del envio a ±64 (3e38, 1e37, −3e38; reverb y delay): cancion limpia posterior = control (≤ 5e-4; no es un residuo bajo −120 dBFS: el bus sigue vivo con la cola del pico recortado a 64, ≈ −82 dBFS) y bloque contado | `SongMix.AHugeFiniteSampleInAClipIsClampedSoALaterCleanSongSoundsLikeTheControl` |
| Reactivar a mitad del fundido: sin escalon, sin tope contado, la cola siguiente dura el tope menos 1 s (los tests acortan el tope con una sonda) | `SongBusTail.ReactivatingMidFadeRaisesTheReturnWithoutAStepAndLeavesNoStaleFade` |
| `resetSongForRender` borra fundido y subida a medias | `SongBusTail.AnOfflineRenderStartsFromScratchAfterAHalfFinishedFadeOrRise` |
| La subida del retorno sigue en los bloques de cola si la cancion para antes de los 50 ms | `SongBusTail.TheRiseKeepsGoingThroughTheTailIfTheSongStopsBeforeItEnds` |
| La subida dura 50 ms con bloques de 1 frame a 192 kHz (contador entero de frames) | `SongBusTail.TheRiseLastsFiftyMillisecondsWithOneFrameBlocksAt192kHz` |
| Un envio finito enorme llega al bus recortado al limite con su signo, no anulado | `SongBus.AHugeFiniteSendReachesTheBusClampedToTheLimitAndNotZeroed` |
| Sin guarda por pico: material fuerte por el delay a 0,9 no se corta, no se reinicia y es continuo | `SongMix.LoudPeriodicMaterialThroughTheDelayAtMaximumFeedbackIsNeverCutOrResetAndIsContinuous` |
| Tope de cola (600 s; los tests lo acortan a 4 s con una sonda) con fundido de 50 ms, con bloques de 1, 256 y 512 frames (`RecordingBus`) | `SongBusTail.ATailThatNeverEndsIsCappedAtTenMinutesWithAFixedFiftyMillisecondFade` |
| La cuenta de la cola se pone a 0 al activar (`finishSongMix`) (`RecordingBus`) | `SongBusTail.SeveralShortTailsDoNotAccumulateTowardsTheTenMinuteCap` |
