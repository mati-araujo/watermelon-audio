# Cobertura C API vs JNI — WA-0.1

**Requerimiento:** `docs/kmp/kmp_requirements.md` § 5, WA-0.1
**Actualizado:** 2026-07-26 (al cerrar `análisis`) · **Reproducible con:**
`python3 scripts/c-api-gap.py`

---

## 1. Para qué sirve esto

El **JNI es la API completa de facto**: Android viene shippeando sobre esa
superficie. La **C API (`wma_*`) es a lo que Kotlin/Native va a bindear** vía
cinterop (D1). Entonces, toda función JNI que no sea USB necesita su contraparte
`wma_*` antes de que iOS pueda tener paridad — ese es el trabajo de **WA-2.5**, y
este documento lo dimensiona.

Mientras la brecha exista, Android e iOS ejecutan caminos distintos. **WA-2.6**
(pasar el JNI a ser un wrapper delgado de la C API) es lo que elimina esa clase
de bug por construcción; hasta entonces, esta tabla es el contrato.

## 2. Metodología y sus límites

El matching es por **conjunto de tokens**: las dos superficies usan convenciones
distintas (`nativeStartEngine` vs `wma_engine_start`) sobre el mismo vocabulario.
Se pasa a minúsculas, se parte en tokens, se descarta el prefijo `wma` y el
conector `from`, y se unifican algunos plurales. `get`/`set`/`is`/`has` **se
conservan**: distinguen un getter de un setter, que son funciones distintas.

También se pliegan las **abreviaturas** que la C API usa donde el JNI escribe la
palabra entera (`SoundFont` → `sf`). Sin eso las 10 funciones de SoundFont
figuraban como gap permanente —`LoadSoundFontFromPath` vs `wma_sf_load_path`,
existentes desde siempre— e inflaban el neto en ~14%. Se corrigió al cerrar
`oscillator/synth` (2026-07-26).

- **Cubierta** = igualdad exacta de conjuntos de tokens. Alta confianza.
- **Gap** = todo lo demás.
- **Near-match** (Jaccard ≥ 0.6) = probablemente la misma función con otro
  nombre. **Requiere confirmación humana** — el script no puede decidirlo.

> [!IMPORTANT]
> Los números de abajo son una **cota, no un censo exacto**. El gap portable real
> está entre **61** (si todos los near-match resultan ser la misma función) y
> **98** (si ninguno lo es).
>
> Un near-match como `SetNoiseGateEnabled ~ wma_input_set_noise_gate` es
> plausible (un solo setter con dos parámetros), pero
> `LooperGetArmedTrack ~ wma_looper_get_track_peak` es claramente un falso
> positivo. Hay que revisarlos uno por uno al encarar cada categoría en WA-2.5.
>
> **La experiencia de las cinco categorías cerradas dice que el extremo bajo es
> el realista, y por abajo.** En `lifecycle` los 8 "faltantes" eran 0 reales; en
> `input/monitor`, 14 nominales fueron 8; en `effects`, 6 fueron 1; en
> `oscillator/synth`, 4 fueron 1; en `voice`, 6 fueron **0**. Sumado: **38 de gap
> nominal, 10 de trabajo real.** Para dimensionar, mirar §4b, no esta tabla.

## 3. Resumen

| Métrica | Valor |
|---|---|
| JNIEXPORT (entry points) | 278 |
| Funciones `wma_*` | 245 |
| Cubiertas (match exacto) | 191 |
| **Gap total** | **87** |
| — USB, no se porta (D4) | 32 |
| — **Gap portable** | **55** |
| — con near-match (revisar) | 36 |
| — **neto a implementar** | **~19** |

### Gap portable por categoría

| Categoría | Funciones |
|---|---|
| Input / monitor | 12 |
| Otros | 8 |
| Engine / lifecycle | 8 |
| Looper | 7 |
| Voice / polyphony | 6 |
| Analysis | 5 |
| Benchmark / diagnostics | 4 |
| Oscillator / synth | 4 |
| Effects | 1 |

---

## 4. Detalle del gap portable

### Input / monitor (12)

- `nativeGetMonitoringVolume` — near-match: `wma_input_get_monitoring_volume` (0.75)
- `nativeIsInputStreamRunning` — near-match: `wma_input_is_running` (0.75)
- `nativeIsMonitoringEnabled` — near-match: `wma_input_is_monitoring_enabled` (0.75)
- `nativeIsNoiseGateEnabled` — near-match: `wma_input_is_noise_gate_enabled` (0.80)
- `nativeIsNoiseGateOpen` — near-match: `wma_input_is_noise_gate_open` (0.80)
- `nativeReleaseInputNode` — near-match: `wma_input_release` (0.67)
- `nativeSetMonitoringEnabled`
- `nativeSetMonitoringVolume` — near-match: `wma_input_set_monitoring_volume` (0.75)
- `nativeSetNoiseGateEnabled` — near-match: `wma_input_set_noise_gate` (0.60)
- `nativeSetNoiseGateThreshold` — near-match: `wma_input_set_noise_gate_threshold` (0.80)
- `nativeStartInputStream` — near-match: `wma_input_start` (0.67)
- `nativeStopInputStream` — near-match: `wma_input_stop` (0.67)

### Otros (8)

- `nativeClearStreamError` — near-match: `wma_clear_error` (0.67)
- `nativeCreateSplitBackend`
- `nativeFallbackToOboeBackend`
- `nativeGetCurrentBackendType` — near-match: `wma_get_backend_type` (0.75)
- `nativeGetIsFading` — near-match: `wma_is_fading` (0.67)
- `nativeGetLastStreamErrorCode` — near-match: `wma_get_last_error_code` (0.80)
- `nativeHasStreamError` — near-match: `wma_has_error` (0.67)
- `nativeLoadSoundFont` — near-match: `wma_sf_load_data` (0.67)

### Engine / lifecycle (8)

- `nativeGetCurrentFadeVolume` — near-match: `wma_get_fade_volume` (0.75)
- `nativeGetIsPaused` — near-match: `wma_is_paused` (0.67)
- `nativeHasInitializationFailed`
- `nativeIsEngineInitialized` — near-match: `wma_is_initialized` (0.67)
- `nativePauseEngineWithFade`
- `nativeResumeEngineWithFade`
- `nativeStartEngineWithFade`
- `nativeStopEngineWithFade`

### Looper (7)

- `nativeLooperExportMixV2`
- `nativeLooperGetInputPeak` — near-match: `wma_looper_get_track_peak` (0.60)
- `nativeLooperGetTrackPeakLevel` — near-match: `wma_looper_get_track_peak` (0.80)
- `nativeLooperRegisterStateListener`
- `nativeLooperResetTrackPlayHead`
- `nativeLooperSaveUndoSnapshot` — near-match: `wma_looper_save_undo` (0.75)
- `nativeLooperUnregisterStateListener`

### Voice / polyphony (6)

- `nativeEnableVoiceSystem` — near-match: `wma_voice_enable` (0.67)
- `nativeIsVoiceSystemEnabled` — near-match: `wma_voice_is_enabled` (0.75)
- `nativeReleaseChordNotes`
- `nativeTriggerChordNotes`
- `nativeUpdateChordNotes`
- `nativeUpdateMultiTouch` — near-match: `wma_voice_update_multi_touch` (0.75)

### Analysis (5)

- `nativeGetOutputPeakLevel` — near-match: `wma_get_output_peak` (0.75)
- `nativeGetOutputPeakLevelDb` — near-match: `wma_get_output_peak_db` (0.80)
- `nativeGetOutputRmsLevel` — near-match: `wma_get_output_rms` (0.75)
- `nativeGetOutputRmsLevelDb` — near-match: `wma_get_output_rms_db` (0.80)
- `nativeSetMultipleEffectParameters` — near-match: `wma_effect_set_params_multi` (0.60)

### Benchmark / diagnostics (4)

- `nativeDrainCapturedLogs`
- `nativeGetAdaptiveBufferStats`
- `nativeGetLogCaptureDropped`
- `nativeSetLogCaptureEnabled`

### Oscillator / synth (4)

- `nativeRegenerateArpPattern` — near-match: `wma_arp_regenerate` (0.67)
- `nativeSetArpBaseFrequency` — near-match: `wma_arp_set_base_freq` (0.60)
- `nativeSetFrequencyAndAmplitude` — near-match: `wma_set_frequency_amplitude` (0.75)
- `nativeSetVocoderCarrierFrequency` — near-match: `wma_vocoder_set_carrier_freq` (0.60)

### Effects (1)

- `nativeGetEffectChainSize` — near-match: `wma_effect_chain_size` (0.75)

## 4b. Delegación del JNI (WA-2.6)

El gap de arriba responde la pregunta de **WA-2.5**: ¿existe una `wma_*` con
este nombre? **WA-2.6 es otra pregunta**: ¿el entry point del JNI *llama* a la C
API, o sigue entrando a `AudioEngine` por su cuenta? Una categoría puede tener
gap cero y estar duplicada de punta a punta.

**`lifecycle` fue exactamente eso.** Sus 8 "faltantes" son un artefacto del
matcher por tokens — las 8 ya existían con otro nombre
(`nativeHasInitializationFailed` ↔ `wma_has_init_failed`,
`nativeStartEngineWithFade` ↔ `wma_engine_start`, …). El gap real de la
categoría era **0**, y aun así el JNI transcribía las 22 funciones a mano. Por
eso el número de abajo se mide aparte, mirando adentro del cuerpo de cada
función JNI.

```
WA-2.6 — JNI delegando: 224/278      (228/290 contando los 4 archivos JNI)
```

| Categoría (heurística del script) | Delegan |
|---|---|
| Input / monitor | 21/21 |
| Oscillator / synth | 21/21 |
| Voice / polyphony | 18/18 |
| Otros | 17/27 |
| Effects | 16/16 |
| Engine / lifecycle | 14/14 |
| Analysis | 13/13 |
| Metronome | 11/11 |
| Mode transitions | 10/12 |
| Modulation | 3/3 |
| Benchmark / diagnostics | 2/6 |
| Looper | 1/79 |
| Mixer / Regions | 1/1 |
| el resto | 0 |

**Las 224 son las diez categorías cerradas**: 22 de `lifecycle`, 21 de `input/monitor`,
14 de `effects`, 40 de `oscillator/synth`, 21 de `voice`, 8 de `mode`, 10 de `análisis`,
13 de `metronome` y **77 del `looper`**. Las 3 de `benchmark` **no están contadas ahí** —
viven en `jni_benchmark.cpp`, que el script no lee (ver el aviso de abajo).

> [!IMPORTANT]
> **Cerrar las diez categorías no cerró WA-2.6.** De los 62 entry points que no delegan,
> **46 son deliberados** (39 USB por D4, 5 de Oboe/stubs en `benchmark`, 2 del state listener
> del looper) y **15 son una cola que la lista de categorías nunca nombró**: routing (5),
> backend (3 + 2 a revisar), XY mapping (2) y captura de logs (3). **12 ya tienen su `wma_*`.**
> Vivían en la fila "Otros" y aparecieron sólo al enumerar el complemento en vez de la lista.
> Detalle en `kmp_requirements.md` §16, "Hallazgo al contar el final".
**Ninguna coincide con su fila de la tabla**, porque la heurística del script
clasifica por keyword del nombre y no por la sección real de la C API:

- `lifecycle` = secciones 1 (Lifecycle), 2 (State) y 3 (Volume & Fade). Se
  reparte en tres filas: `nativeGetStateVersion` cae en "Benchmark" por `stat`,
  `nativeHasStreamError` en "Otros". Los dos pendientes de "Engine / lifecycle"
  son `nativeGetEngineType` / `nativeSetEngineType`, que son el *synth engine*,
  no el ciclo de vida: van con `oscillator/synth`.
- `input/monitor` = sección 12 (Input). `nativeGetInputMeteringSnapshot` cae en
  "Analysis" por `meter` y `nativeIsInputClipping` en "Mixer / Regions" por
  `clip`. Los dos pendientes de la fila son `nativeIsArpGateOpen` y
  `nativeSetArpGateLength`, que son del arpegiador y entraron por `gate`.
- `effects` = sección 8 (Effects), 14 funciones: las 10 de la fila "Effects" más
  las 4 de parámetros, que caen en "Analysis" porque **`parameter` contiene
  `meter`**.
- `oscillator/synth` = secciones 4, 5, 6, 15 y 18 juntas, 40 funciones. Es la que
  más se desparrama: el arpegiador y los osciladores caen en su fila, pero los
  5 `SoundFontPreset*` y `nativeHasVocoderEffect` estaban en "Effects" (por
  `preset` y `effect`), `nativeGetEngineType`/`SetEngineType` en
  "Engine / lifecycle" (por `engine`), los `LoadSoundFont*` en "Otros", y
  `nativeSetSecondaryOscillatorType` vive en el bloque de dual-touch del JNI, que
  es por dónde casi se escapa.

- `voice` = secciones 7 (Voice Filter), 13 (Dual Touch) y 14 (Voice System), más
  los cuatro `SfNote*` de la 6. Las de dual touch caen en "Mode transitions"
  porque su nombre lleva `mode`, y por eso esa fila pasó de 0 a 4 sin que se
  tocara una sola transición de modo.
> [!CAUTION]
> **La métrica de delegación tiene un punto ciego: el script lee UN solo archivo.**
> `JNI_SRC` es `jni/jni_audio_bridge.cpp` y nada más, así que el denominador 278 deja
> afuera los otros tres archivos JNI — `jni_benchmark.cpp` (8 entry points),
> `jni_usb.cpp` (3) y `jni_engine.cpp`. Son **13 entry points invisibles** a la métrica.
>
> Lo destapó la categoría `benchmark`, que vive casi entera en `jni_benchmark.cpp`:
> se migraron 3 funciones y **el número no se movió ni uno**. Medido a mano sobre los
> cuatro archivos da **152/290**, contra el 148/278 que imprime el script.
>
> No está mal que el script mire un archivo —ahí está el 96% de la superficie— pero
> **al cerrar una categoría hay que preguntarse en qué archivo vive** antes de creerle
> al delta. Los 5 de `jni_benchmark.cpp` que siguen sin delegar son a propósito y están
> justificados en el encabezado de ese archivo; los 3 de `jni_usb.cpp` son D4.

- `benchmark` = **sección 21 (Diagnostics & Latency), nueva.** Sólo **2 funciones** de C
  API, contra un gap nominal de 4: `wma_get_recommended_buffer_size` y
  `wma_get_latency_report`. El resto de la categoría **no porta y no se forzó**:
  `runLatencyOptimizationTest` e `isAAudioAvailable` abren un `oboe::AudioStreamBuilder`
  para preguntar algo que sólo existe en Android; las colas de `getDetailedLatencyInfo`
  ([4..7]) y del reporte describen un `oboe::AudioStream`; tres son stubs deprecados que
  no tocan el motor; y `getAdaptiveBufferStats` lee el `LibusbBackend` (D4).
  **Es la primera categoría que migra a medias a propósito**, con el porqué escrito en el
  encabezado de `jni_benchmark.cpp`. No se agregó un `wma_get_latency_info` batch: sus
  números ya son `wma_get_stream_info` (§2) + `wma_input_get_latency_ms` (§12), y nadie
  polea la latencia por frame como sí se polea el metering.
- `metronome` = **la sección 20 no existía**. Fue la primera categoría sin sección
  propia en `watermelon_audio.h`: la numeración saltaba de 19 (Looper) a 20
  (Waveform), y las 10 `nativeTransport*` no tenían contraparte ninguna. Se creó
  la 20 (Transport) y las de abajo corrieron un número. La fila del script dice
  11 pero la categoría son **13**: `nativeTransportFramesPerBar` cae en
  "Mixer / Regions" (por `bar`) y `nativeLooperTriggerClick` en "Looper" — y por
  eso esa fila arranca en 1/79 sin que se haya tocado el looper. `nativeSetBpm` /
  `nativeGetBpm` sí estaban en la fila, y su C API ya existía en la sección 3.

Dicho de otra forma: de las 130 que no delegan, **una buena parte está en filas que
no llevan el nombre de su categoría**. La tabla sirve para ver por dónde va la
cosa, no para planificar.

> [!IMPORTANT]
> Lección para las categorías que siguen: **el gap no dimensiona WA-2.6**. Antes
> de dar una categoría por barata porque su gap es chico, hay que mirar cuántos
> de sus entry points delegan. Y las filas de esta tabla son un mapa aproximado,
> no la unidad de trabajo: la unidad es la sección de `watermelon_audio.h`.

## 5. Lecturas del análisis

**El looper es el 45% del gap portable** (39 de 87) y **no se movió ni una
función** en ocho categorías: es un bloque intacto y, por decisión de producto,
entra completo. Va último a propósito, para llegar con el mecanismo rodado.

**USB son 32 funciones que no se portan** (D4: iOS no permite acceso USB
genérico sin DriverKit + entitlements). Sacarlas del cálculo baja el gap de 119
a 87 — vale la pena tenerlo presente para no sobredimensionar WA-2.5.

**51 funciones `wma_*` no se alcanzan desde el JNI.** No son un problema: la C
API ya cubre terreno que el JNI resuelve de otra forma (por ejemplo
`wma_engine_create`/`wma_engine_destroy`, que del lado Android son manejo de
ciclo de vida implícito). Al migrar cada categoría conviene mirarlas: algunas
son el camino canónico que el JNI todavía no usa.

**El gap sobrestima el trabajo — salvo cuando lo subestima.** En las siete
primeras categorías cerradas el gap nominal fue 39 y el trabajo real 11 funciones
nuevas; `voice` necesitó **cero**, con 6 de gap nominal. La causa es siempre la
misma —la C API abrevia (`sf`, `param`, `freq`) donde el JNI escribe entero— y
cada categoría destapa una abreviatura nueva.

**`metronome` rompió la regla y fue en la otra dirección**: gap nominal 9,
trabajo real **10**, porque su sección de `watermelon_audio.h` no existía y no
había ni una abreviatura que descontar. Es el caso opuesto y confirma la misma
receta: antes de dimensionar, **abrir la sección y contar a mano**. Cuando no hay
sección que abrir, el gap es el piso, no el techo.

**"Otros" (9) hay que clasificarlo a mano.** La heurística de categorías del
script es por keywords y no acierta siempre; ese cajón es el residuo.

## 6. Cómo actualizar este documento

```bash
python3 scripts/c-api-gap.py             # resumen numérico + delegación (§4b)
python3 scripts/c-api-gap.py --markdown  # secciones 3 y 4 de este doc
```

El script lee directamente `jni/jni_audio_bridge.cpp` y `api/watermelon_audio.h`,
así que los números siguen al código. Al cerrar cada categoría en WA-2.5/2.6,
re-correrlo y actualizar acá.

**El doc no se regenera solo.** `--markdown` emite las secciones 3 y 4 en
stdout; hay que pegarlas. El resto (§1, §2, §4b, §5, §6) es prosa a mano.
