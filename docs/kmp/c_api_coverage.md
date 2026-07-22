# Cobertura C API vs JNI — WA-0.1

**Requerimiento:** `docs/kmp/kmp_requirements.md` § 5, WA-0.1
**Generado:** 2026-07-22 · **Reproducible con:** `python3 scripts/c-api-gap.py`

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
Se pasa a minúsculas, se parte en tokens, se descarta el prefijo `wma` y se
unifican algunos plurales. `get`/`set`/`is`/`has` **se conservan**: distinguen un
getter de un setter, que son funciones distintas.

- **Cubierta** = igualdad exacta de conjuntos de tokens. Alta confianza.
- **Gap** = todo lo demás.
- **Near-match** (Jaccard ≥ 0.6) = probablemente la misma función con otro
  nombre. **Requiere confirmación humana** — el script no puede decidirlo.

> [!IMPORTANT]
> Los números de abajo son una **cota, no un censo exacto**. El gap portable real
> está entre **79** (si todos los near-match resultan ser la misma función) y
> **110** (si ninguno lo es). La estimación original del requerimiento (~89) cae
> dentro de ese rango y se confirma como razonable para planificar.
>
> Un near-match como `SetNoiseGateEnabled ~ wma_input_set_noise_gate` es
> plausible (un solo setter con dos parámetros), pero
> `LooperGetArmedTrack ~ wma_looper_get_track_peak` es claramente un falso
> positivo. Hay que revisarlos uno por uno al encarar cada categoría en WA-2.5.

## 3. Resumen

| Métrica | Valor |
|---|---|
| JNIEXPORT (entry points) | 278 |
| Funciones `wma_*` | 187 |
| Cubiertas (match exacto) | 136 |
| **Gap total** | **142** |
| — USB, no se porta (D4) | 32 |
| — **Gap portable** | **110** |
| — con near-match (revisar) | 31 |
| — **neto a implementar** | **~79** |

### Gap portable por categoría

| Categoría | Funciones |
|---|---|
| Looper | 39 |
| Input / monitor | 14 |
| Otros | 13 |
| Metronome | 9 |
| Engine / lifecycle | 8 |
| Voice / polyphony | 6 |
| Effects | 6 |
| Analysis | 6 |
| Benchmark / diagnostics | 4 |
| Oscillator / synth | 4 |
| Mode transitions | 1 |

---

## 4. Detalle del gap portable

### Looper (39)

- `nativeLooperAbortRecording`
- `nativeLooperArmAtNextBar`
- `nativeLooperArmInFrames`
- `nativeLooperArmSyncedToLoop`
- `nativeLooperArmSyncedToLoopQuantized`
- `nativeLooperCancelArm`
- `nativeLooperCancelExport`
- `nativeLooperCaptureTrack`
- `nativeLooperDetectOnsets`
- `nativeLooperExportMixV2`
- `nativeLooperExportStems`
- `nativeLooperFinalizeFreeLoop`
- `nativeLooperFindContentBounds`
- `nativeLooperGetArmedTrack` — near-match: `wma_looper_get_track_peak` (0.60)
- `nativeLooperGetArmedTriggered`
- `nativeLooperGetDroppedEvents`
- `nativeLooperGetExportProgress`
- `nativeLooperGetExportsCompleted`
- `nativeLooperGetExportsFailed`
- `nativeLooperGetFramesDropped`
- `nativeLooperGetInputPeak` — near-match: `wma_looper_get_track_peak` (0.60)
- `nativeLooperGetStemsWritten`
- `nativeLooperGetTailMs`
- `nativeLooperGetTrackPeakLevel` — near-match: `wma_looper_get_track_peak` (0.80)
- `nativeLooperIsExportInProgress`
- `nativeLooperIsTrackPercussionMode`
- `nativeLooperPrepareTrackBars`
- `nativeLooperRegisterStateListener`
- `nativeLooperResetTelemetry`
- `nativeLooperResetTrackPlayHead`
- `nativeLooperSaveUndoSnapshot` — near-match: `wma_looper_save_undo` (0.75)
- `nativeLooperSetCapabilities`
- `nativeLooperSetExportSampleRate`
- `nativeLooperSetTailMs`
- `nativeLooperSetTrackPercussionMode`
- `nativeLooperSetTrackPlayCount`
- `nativeLooperStartRecordingWithPreRoll`
- `nativeLooperTrimTrack`
- `nativeLooperUnregisterStateListener`

### Input / monitor (14)

- `nativeGetInputLatencyMs`
- `nativeGetInputLevelLinear`
- `nativeGetMonitoringVolume`
- `nativeIsInputStreamRunning` — near-match: `wma_input_is_running` (0.75)
- `nativeIsMonitoringEnabled`
- `nativeIsNoiseGateEnabled` — near-match: `wma_input_is_noise_gate_enabled` (0.80)
- `nativeIsNoiseGateOpen`
- `nativeReleaseInputNode` — near-match: `wma_input_release` (0.67)
- `nativeSetMonitoringEnabled`
- `nativeSetMonitoringVolume`
- `nativeSetNoiseGateEnabled` — near-match: `wma_input_set_noise_gate` (0.60)
- `nativeSetNoiseGateThreshold` — near-match: `wma_input_set_noise_gate` (0.60)
- `nativeStartInputStream` — near-match: `wma_input_start` (0.67)
- `nativeStopInputStream` — near-match: `wma_input_stop` (0.67)

### Otros (13)

- `nativeClearStreamError` — near-match: `wma_clear_error` (0.67)
- `nativeCreateSplitBackend`
- `nativeFallbackToOboeBackend`
- `nativeGetCurrentBackendType` — near-match: `wma_get_backend_type` (0.75)
- `nativeGetIsFading` — near-match: `wma_is_fading` (0.67)
- `nativeGetLastStreamErrorCode` — near-match: `wma_get_last_error_code` (0.80)
- `nativeHasStreamError` — near-match: `wma_has_error` (0.67)
- `nativeIsSoundFontLoaded`
- `nativeLoadSoundFont`
- `nativeLoadSoundFontFromFd`
- `nativeLoadSoundFontFromPath`
- `nativeTransportFramesPerBar`
- `nativeUnloadSoundFont`

### Metronome (9)

- `nativeTransportFramesPerBeat`
- `nativeTransportGetBeatsPerBar`
- `nativeTransportGetRemainingBeats`
- `nativeTransportIsMetronomeContinuous`
- `nativeTransportIsMetronomeRunning`
- `nativeTransportSetBeatsPerBar`
- `nativeTransportStartMetronome`
- `nativeTransportStartMetronomeContinuous`
- `nativeTransportStopMetronome`

### Engine / lifecycle (8)

- `nativeGetCurrentFadeVolume` — near-match: `wma_get_fade_volume` (0.75)
- `nativeGetIsPaused` — near-match: `wma_is_paused` (0.67)
- `nativeHasInitializationFailed`
- `nativeIsEngineInitialized` — near-match: `wma_is_initialized` (0.67)
- `nativePauseEngineWithFade`
- `nativeResumeEngineWithFade`
- `nativeStartEngineWithFade`
- `nativeStopEngineWithFade`

### Voice / polyphony (6)

- `nativeEnableVoiceSystem` — near-match: `wma_voice_enable` (0.67)
- `nativeIsVoiceSystemEnabled` — near-match: `wma_voice_is_enabled` (0.75)
- `nativeReleaseChordNotes`
- `nativeTriggerChordNotes`
- `nativeUpdateChordNotes`
- `nativeUpdateMultiTouch` — near-match: `wma_voice_update_multi_touch` (0.75)

### Effects (6)

- `nativeGetEffectChainSize` — near-match: `wma_effect_chain_size` (0.75)
- `nativeGetSoundFontPresetBankProgram`
- `nativeGetSoundFontPresetCount`
- `nativeGetSoundFontPresetKeyRange`
- `nativeGetSoundFontPresetName`
- `nativeSetSoundFontPreset`

### Analysis (6)

- `nativeGetInputMeteringSnapshot`
- `nativeGetOutputPeakLevel` — near-match: `wma_get_output_peak` (0.75)
- `nativeGetOutputPeakLevelDb` — near-match: `wma_get_output_peak_db` (0.80)
- `nativeGetOutputRmsLevel` — near-match: `wma_get_output_rms` (0.75)
- `nativeGetOutputRmsLevelDb` — near-match: `wma_get_output_rms_db` (0.80)
- `nativeSetMultipleEffectParameters`

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

### Mode transitions (1)

- `nativeGetModeName`
---

## 5. Lecturas del análisis

**El looper es el 35% del gap portable** (39 de 110) — es, por lejos, la
categoría más pesada y la que más va a costar en WA-2.5. Conviene atacarla
primero o, si el cronograma aprieta, evaluar si NoisyPad iOS v1 necesita el
looper completo o un subconjunto.

**USB son 32 funciones que no se portan** (D4: iOS no permite acceso USB
genérico sin DriverKit + entitlements). Sacarlas del cálculo baja el gap de 142
a 110 — vale la pena tenerlo presente para no sobredimensionar WA-2.5.

**53 funciones `wma_*` no se alcanzan desde el JNI.** No son un problema: la C
API ya cubre terreno que el JNI resuelve de otra forma (por ejemplo
`wma_engine_create`/`wma_engine_destroy`, que del lado Android son manejo de
ciclo de vida implícito). Pero conviene revisarlas al hacer WA-2.6: si el JNI va
a pasar a ser un wrapper de la C API, algunas de estas pueden quedar como el
camino canónico.

**"Otros" (13) hay que clasificarlo a mano.** La heurística de categorías del
script es por keywords y no acierta siempre; ese cajón es el residuo.

## 6. Cómo actualizar este documento

```bash
python3 scripts/c-api-gap.py             # resumen numérico
python3 scripts/c-api-gap.py --markdown  # secciones 3 y 4 de este doc
```

El script lee directamente `jni/jni_audio_bridge.cpp` y `api/watermelon_audio.h`,
así que los números siguen al código. Al cerrar cada categoría en WA-2.5,
re-correrlo y actualizar acá.
