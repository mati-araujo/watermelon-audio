# Auditoría Looper & Administración de Pistas

**Fecha:** 2026-05-11
**Autor:** Auditoría técnica (Claude Opus 4.7)
**Branch base:** `feature/usb_refactor_codex`
**Alcance:** `audio/src/main/cpp/looper/`, capa JNI, bindings Kotlin, integración con `AudioEngine`, pipeline de export.

---

## 0. Resumen ejecutivo

El looper actual cumple los requisitos básicos de un sampler multi-pista RT-safe (8 tracks, 48 MB cap, lock-free, undo, crossfade anti-click, soft-clip en overdub, export WAV 16-bit). La cabeza C++ está bien escrita en cuanto a *thread safety* y memoria. **Pero** está varios pasos por debajo de "profesional" en seis frentes que coinciden uno-a-uno con tus puntos:

| # | Problema reportado | Veredicto técnico | Severidad |
|---|---|---|---|
| 1 | No graba en `input_fx` | **Funciona en el path "legacy", pero el path AudioGraph no rutea input → looper grabaría silencio si se activa el grafo. Además el tap es POST-master-volume → si bajás vol global, grabás bajo o nada.** | Alta |
| 2 | No hay BPM-sync | **Cero awareness de BPM en el looper. `setBpm` sólo afecta efectos. No hay quantize loop-length, no hay cuenta de barras, no hay sync entre tracks.** | Alta |
| 3 | Sustain cortado al fin de loop | **El crossfade de 128 frames (~2.7 ms) es para anti-click, no para preservar tail. No hay tail buffer, no hay overlap-record.** | Alta |
| 4 | Export multi-track a único archivo | **`exportMix` ya hace mixdown, pero (a) sin sincronización con audio thread → race, (b) sin true-peak limiter, (c) sin opción N-bars, (d) sólo 16-bit, (e) Kotlin no envuelve en Dispatchers.IO.** | Media |
| 5 | Metrónomo intermitente | **`triggerClick(...)` existe en C++/JNI/Kotlin pero NO hay scheduler. El consumer (NoisyPad) lo programa manualmente. Si la UI se pausa o el frame de Compose cae, el click se pierde. El tone-burst usa `48000` hardcoded en lugar del SR real → puede sonar destemplado.** | Media |
| 6 | Memoria / performance | **Buffer fijo `mLooperMixBuf[1024*2]` no se valida vs `numFrames` en runtime. Smoothing per-sample con `cos`/`sin`/`fmod` por canal, por track, por frame es caro. Allocaciones lazy correctas pero `clear()` libera y re-aloca → fragmentación tras N grabaciones.** | Media |

Hay además **deudas estructurales** transversales: looper no está expuesto en `commonMain` (sólo `androidMain`), no hay tests unitarios C++ del looper (a diferencia de USB que sí los tiene), y no existe un doc de diseño activo. Detalle abajo.

---

## 1. Inventario y arquitectura actual

### 1.1 Archivos
```
audio/src/main/cpp/looper/
  AudioLooper.h    620 LOC   facade: 8 tracks + recording + click
  TrackBuffer.h    423 LOC   single track: buffer, mixInto, overdub, undo, loop region
  WavFile.h        218 LOC   WAV reader/writer header-only
  CMakeLists.txt
```

### 1.2 Superficie pública

| Capa | Funciones | Notas |
|---|---|---|
| C++ `AudioLooper` | ~50 métodos (control, params, queries, export) | RT-safe paths claros |
| JNI `Java_..._nativeLooper*` | 37 (jni_audio_bridge.cpp:2217–2568) | Mapping 1:1 limpio |
| Kotlin `AudioNativeBridge` | 47 declaraciones externas + 43 wrappers públicos (2550–2669) | Espejo perfecto del JNI |
| C API `wma_looper_*` | 38 funciones (watermelon_audio.h:665–745) | Incluye `export_mix`, `export_track`, `import_track`, `trigger_click` |
| Kotlin `IAudioNativeBridge` (commonMain) | **0** — interface explícitamente excluye looper | Bloquea uso multiplatform |

### 1.3 Integración con AudioEngine

`mAudioLooper.process(output, numFrames)` se llama:
- **Path legacy** (AudioEngine.cpp:1550) — después de `applyEffectsAndOutput`, después de master-vol y fade.
- **Path AudioGraph** (AudioEngine.cpp:1137) — después de copiar output del `OutputNode`.

**Default:** `mUseAudioGraph = false` (AudioEngine.h:906) → en producción se usa el path legacy.

---

## 2. Hallazgos por área

### 2.1 Recording en `input_fx` (Punto 1)

**Estado actual:** Técnicamente funciona en el path legacy. `renderInputFx` (AudioEngine.cpp:1143) lee del input, aplica efectos, fade y master-vol → escribe a `output`. El looper se ejecuta a continuación sobre ese mismo `output`, así que graba la señal post-FX.

**Problemas detectados:**

1. **Tap incorrecto: POST-master-volume.** Si el usuario baja volumen global durante una grabación, la pista queda grabada bajo o silenciada. Profesional: el looper debería tener su **propio bus de captura** anterior al fade/master-vol pero posterior a los efectos.
2. **Path AudioGraph no soporta input_fx.** `renderViaGraph` (AudioEngine.cpp:1111) sólo lee del `OutputNode`. No hay `mGraphInputHandle`. Si en el futuro alguien activa `mUseAudioGraph=true`, el looper grabará silencio en input_fx. Es bomba de tiempo silenciosa.
3. **Sin diagnóstico de "por qué framesRead==0".** El log de input_fx (AudioEngine.cpp:1148–1173) sólo cuenta peaks; no diferencia "stream no corriendo" vs "monitoring deshabilitado" vs "nullptr". Cuando el usuario reporta "no graba", no hay forma rápida de saber qué falla.
4. **Sin gating de UI.** No hay forma de la UI de saber si el looper está grabando *señal real* (peak > umbral) vs *silencio*. Nada en JNI/Kotlin retorna el peak del signal entrante al looper (el `getTrackPeakLevel` mide post-mixInto).
5. **Sin pre-roll / look-back para input.** Si el usuario apreta REC justo cuando empieza a tocar, los primeros 50–100 ms se pierden por reacción humana. Profesional: pre-roll buffer circular de 200–500 ms.

### 2.2 Sincronización por BPM (Punto 2)

**Estado actual:** El looper es **completamente time-agnostic**.

- `AudioEngine::setBpm(float)` (AudioEngine.h:409) sólo propaga a `mEffectChain.setBpm()`. **No llega al looper.**
- `AudioLooper` no tiene campo `mBpm`, no tiene `setBpm`, no expone "bars" ni "beats".
- `prepareTrack(idx, lengthFrames, sr)` recibe **frames crudos**. La conversión BPM→frames está delegada al cliente (NoisyPad).
- `triggerClick(isDownbeat)` se invoca manualmente desde la UI, sin scheduler interno.
- **No existe un "transport" central** (playhead global, posición en barras, downbeats, swing).

**Problemas profesionales:**

1. **Sin loop quantization.** Si grabás 4 bars a 120 BPM con micrófono y tu pulso humano da 4 bars + 87 ms, el loop queda desalineado. Debería poder cuantizar al beat más cercano automáticamente.
2. **Sin start/stop sync entre tracks.** Cada track tiene su `mPlayHead` independiente. Si grabás Track 2 con Track 1 ya en loop, no hay garantía de que Track 2 quede en fase con Track 1. El comportamiento actual depende de cuándo el usuario suelta REC.
3. **Sin tempo change con stretch.** Si cambia el BPM, los loops grabados no se re-pitch ni re-stretch (sólo `setSpeed` lineal por track con resample de baja calidad).
4. **No hay integración con Ableton-Link / MIDI clock.** Para "profesional" es esperable.

### 2.3 Tail / sustain del loop cortado (Punto 3)

**Estado actual:** En `TrackBuffer::mixInto` (TrackBuffer.h:211–222) hay un crossfade de `CROSSFADE_FRAMES = 128` (~2.7 ms @ 48 kHz) que mezcla los últimos N frames del loop con los primeros N. **Esto suaviza el click pero no preserva el sustain de notas con tail largo.**

**Causa raíz:** El buffer es exactamente `lengthFrames` largo. La grabación se detiene en `mRecordFramesRemaining <= 0` y `finalizeCurrentRecording` se llama. Si en el frame N+1 todavía hay sonido (delay tail, reverb, sustain de cuerda), se descarta.

**Soluciones profesionales (ordenadas por costo/beneficio):**

1. **Tail buffer (recomendado).** Pre-allocar `lengthFrames + tailFrames` (e.g. +500 ms). Seguir grabando los últimos `tailFrames` *después* de `lengthFrames`. En playback, mezclar el tail con fade-out sobre los primeros `tailFrames` del loop.
2. **Overlap-add carry-over.** En el wrap del playhead, dejar que el frame de "última grabación" (tail) se mezcle con fade-out sobre los primeros frames del próximo ciclo, en *runtime* (no en buffer).
3. **Crossfade con duración configurable** (ya existe `CROSSFADE_FRAMES` pero hardcoded; subirlo a 25–200 ms y exponer por param soluciona el 80% de casos sin tail buffer).
4. **Pre-roll en grabación.** Empezar a grabar 100–200 ms *antes* del start declarado (mantener un ring buffer de "pasado reciente"). Conjugado con tail buffer, da round-trip perfecto.
5. **Auto-trim a zero-crossings.** Detectar el primer y último zero-crossing dentro de una ventana ±20 ms del loop boundary y ajustar.

### 2.4 Export multi-pista a un archivo (Punto 4)

**Estado actual:** `AudioLooper::exportMix(filePath)` (AudioLooper.h:276–320) ya hace mixdown a un único WAV.

**Problemas:**

1. **Race condition no protegida.** Lee `mTracks[t].data()`, `getLengthFrames()`, etc. **sin pausar el audio thread**. Si el usuario está grabando o haciendo overdub durante el export, hay data race. `importTrack` sí usa `atomic_thread_fence(seq_cst)` (AudioLooper.h:410), `exportMix` no.
2. **Limiter no profesional.** Aplica `tanh(x*0.666f)*1.5f` global (AudioLooper.h:315). Esto comprime *todo* el material, incluso si no satura. Profesional: true-peak limiter con look-ahead, o al menos un soft-knee con threshold/release.
3. **Sólo 16-bit PCM.** WavFile.h:80 hardcodea `int16_t`. La capa de lectura sí soporta 24-bit y 32-bit float, pero no la de escritura. Pérdida de calidad en round-trip.
4. **Single loop length (longest active).** Tracks más cortos se loopean en módulo. No hay opción "exportar N barras", "exportar hasta que pare", "exportar con count-in", "exportar STEMS (uno por track al mismo length)".
5. **Sin metadata.** WAV sin `cue` chunks, sin `bext`, sin `LIST/INFO`. Profesional debería embeber al menos BPM, fecha, nombre del proyecto.
6. **Sin Dispatchers.IO en Kotlin.** `AudioNativeBridge.kt:2667` es JNI directo blocking. El consumidor debe envolverlo, lo cual no está garantizado.
7. **Sin progreso ni cancelación.** Para tracks largos (decenas de MB), no hay callback de progreso ni forma de cancelar. UX bloqueada.
8. **Path no validado.** No hay verificación de permisos / espacio en disco / path válido. Falla silenciosa con `false`.
9. **Sin "stems" individuales.** No hay `exportStems(directory)` que escriba `track_0.wav`...`track_N.wav` con la misma duración para mezcla externa.

### 2.5 Metrónomo intermitente (Punto 5)

**Estado actual:** El click se genera en `AudioLooper::process` (AudioLooper.h:108–128) con `sin(2π*freq*phase/48000.0f)`. El `triggerClick(isDownbeat)` se llama desde la UI vía JNI.

**Causas probables del comportamiento intermitente:**

1. **`48000.0f` está hardcoded** (AudioLooper.h:120). Si el dispositivo corre a 44.1 kHz o 96 kHz, el click suena destemplado pero **suena**. Más grave: si el sample rate cambia (USB device hot-plug), la frecuencia del click se vuelve incorrecta.
2. **Scheduler en UI.** `triggerClick` debe ser invocado por el cliente (NoisyPad) en cada beat. Si el thread de UI está bloqueado, GC pause, o jank de Compose, **se pierde el beat**. Este es probablemente el síntoma "a veces suena, a veces no".
3. **No hay scheduling RT-safe en C++.** Lo correcto sería: la UI dice "BPM=120, count_in=4 beats" y el motor C++ programa los clicks contando frames internamente. Así el click es **exacto** y nunca se pierde.
4. **Atomic store `release` en `mClickRemaining`** (AudioLooper.h:514) puede ser observado más tarde por el audio thread si hay delay en JNI dispatch — primer beat puede llegar 1 callback tarde (~10 ms a 480 frames).
5. **Click NO afectado por master-vol** (correcto, AudioLooper.h:108) **pero sí mezclado en el mismo bus que tracks**. Si el usuario tiene tracks fuertes, el click se enmascara perceptivamente. Convendría tap separado o ducking.
6. **Sin diferenciación visual de downbeat.** El JNI propaga `isDownbeat` pero la UI no recibe callback de "este beat sonó".

### 2.6 Memoria, performance y RT-safety (Punto 6)

**Lo que está bien:**

- Lock-free correctamente implementado: todos los stores/loads usan `memory_order_acquire/release`.
- Memory budget de 48 MB es razonable.
- `clear()` ordena las stores correctamente con `seq_cst` fence antes de liberar buffer (TrackBuffer.h:75) — clave para evitar use-after-free.
- Buffers preasignados (`mLooperMixBuf` en stack como miembro).
- Allocación lazy via `prepareTrack`, no en constructor.

**Problemas detectados:**

1. **`MAX_BUFFER_FRAMES = 1024` hardcoded** (AudioLooper.h:38). Si Oboe da un callback con `numFrames > 1024` (que ocurre en USB y en algunos exclusive modes), `framesToProcess = min(numFrames, 1024)` → **se pierden frames del mix output**. Debería ser dinámico o validar al `prepare()`.
2. **`clear()` llama a `mBuffer.shrink_to_fit()`** (TrackBuffer.h:92) → libera memoria al heap. Re-grabar fuerza nueva alloc. Tras N ciclos record/clear → fragmentación. Mejor: no shrink, retener capacity hasta destrucción del looper.
3. **Smoothing per-sample con `cos`/`sin`** (TrackBuffer.h:187–189) por cada track activo, por cada frame. Con 8 tracks y 480 frames: 8 × 480 × 2 trig = 7680 transcendentales por callback. Reemplazable por LUT o algoritmo recursivo (rotación 2x2). Costo actual ~1–3% CPU evitable.
4. **`fmod` en cada frame** (TrackBuffer.h:192) — costoso. Si `loopLen` es power-of-2, máscara bitwise. Si no, mantener `playHeadF` y rest sólo cuando supera `loopLen`.
5. **Mezcla escalar (no SIMD).** `mixInto` opera frame-por-frame en C++ puro. El resto del motor usa `simd::applyStereoGainRamp` (AudioEngine.cpp:1044). Looper queda atrás.
6. **`getTrackWaveform`** (AudioLooper.h:449) lee `data()` desde UI thread sin sincronización. Mientras mezcla está activa, race latente (lectura sólo, sin tearing crítico para visualización, pero técnicamente UB).
7. **`exportMix` aloca `vector<float>(maxLen*2)`** que para 60 segundos a 48 kHz son 23 MB. No reutiliza buffer entre exports.
8. **`importTrack` resampler lineal** (AudioLooper.h:373–392) — interpolación lineal sólo. Para 44.1→48 sería esperable polifase / sinc.
9. **`mPeakLevel` se actualiza sólo en `mixInto`** — si track está paused, el peak no decae. Visualmente queda "stuck" si pausás un track con peak alto.
10. **No hay back-pressure / underrun detection** específica del looper. Si el motor satura, el looper igualmente intenta grabar — sin telemetría de cuántos frames se perdieron.
11. **`mEnabled` es global** — un único bool detiene todo el looper. No hay enable per-track, ni "freeze" (track snapshot inmutable).
12. **8 tracks fijo** — `MAX_TRACKS = 8` hardcoded. Para profesional: 16 o 32 con LRU/eviction.

### 2.7 Otras deudas técnicas

1. **No hay tests unitarios** del looper en `audio/src/main/cpp/usb/tests/` (que sí tiene cobertura USB). Crear `audio/src/main/cpp/looper/tests/` con: `test_track_buffer.cpp`, `test_audio_looper.cpp`, `test_wav_file.cpp`, `test_loop_quantization.cpp`.
2. **`commonMain/IAudioNativeBridge.kt` excluye looper** explícitamente. Bloquea futura UI multiplatform.
3. **`AudioLooper.h` y `TrackBuffer.h` son header-only de 620+423 LOC**. Aumenta tiempo de compilación (incluido en muchos `.cpp` indirectamente). Mover impl a `.cpp`.
4. **No hay doc de diseño activo del looper** (`docs/00_master_plan.md` lo menciona como Phase 11 pero sin specs vivas). Crear `docs/looper/` con stages análogos a `docs/usb-audio/`.
5. **Logging con macros `P12_*`** (AudioLooper.h:10–12) — el "P12" alude al phase 11/12 histórico; debería ser `LOOPER_LOG` consistente con otros módulos.
6. **`overdubFrame` aplica tanh-clip por sample** — destruye dinámica en overdubs apilados. Profesional: gain-staging con limiter una sola vez al final, no por sample.
7. **Sin notificación de "loop wrapped"** — la UI no sabe cuándo cierra un ciclo (útil para flash visual, sync UI, etc.). Hace falta callback / atomic counter.
8. **Sin ARM/disarm explícito.** `startRecording` arma + dispara inmediatamente. Falta estado intermedio "armed, esperando downbeat".

---

## 3. Roadmap recomendado (priorizado)

### Sprint 1 — Fundamentos (alto impacto, costo bajo-medio)

| ID | Acción | Archivos | Esfuerzo |
|---|---|---|---|
| L-01 | Crear `Transport` C++ central: BPM + bars/beats + downbeat scheduler RT-safe | `cpp/looper/Transport.h` (nuevo) | M |
| L-02 | Looper graba **PRE-master-vol**: tap nuevo en `applyEffectsAndOutput` antes de `simd::applyStereoGainRamp` | `core/AudioEngine.cpp:1034–1048` | S |
| L-03 | Reemplazar `48000.0f` hardcoded en click por `mSampleRate` real | `looper/AudioLooper.h:120` | S |
| L-04 | Click scheduler RT-safe en C++: `Transport::scheduleClicks(beats, bpm)` programa frames internos | `looper/AudioLooper.h` + `Transport.h` | M |
| L-05 | Validar `numFrames <= MAX_BUFFER_FRAMES` o hacer mix buffer dinámico | `looper/AudioLooper.h:38, 89–94` | S |
| L-06 | Quitar `shrink_to_fit()` en `clear()` para reusar capacity | `looper/TrackBuffer.h:92` | S |
| L-07 | Agregar JNI `nativeLooperGetInputPeak()` para metering pre-record | JNI + bridge | S |
| L-08 | Logging unificado `LOOPER_LOG` reemplazando `P12_*` | `looper/AudioLooper.h:10–12` | S |

### Sprint 2 — Profesionalización tail/sync

| ID | Acción | Archivos | Esfuerzo |
|---|---|---|---|
| L-10 | Tail buffer: alocar `lengthFrames + tailFrames(default 500ms)` y mezclar con fade en wrap | `looper/TrackBuffer.h` | L |
| L-11 | Pre-roll: ring buffer global de 500 ms de input/output. `startRecording` copia los últimos N ms al inicio del track | `looper/AudioLooper.h` + `RingBuffer.h` (nuevo) | M |
| L-12 | Loop quantize: `prepareTrackByBars(idx, bars, beatsPerBar)` calcula frames vía `Transport` | `looper/AudioLooper.h` | S |
| L-13 | Estado "armed → waiting downbeat → recording" para sync entre tracks | `looper/AudioLooper.h` + JNI | M |
| L-14 | `setSpeed` con resampler polifase en lugar de lineal (importTrack también) | `looper/TrackBuffer.h:373` | M |

### Sprint 3 — Export profesional

| ID | Acción | Archivos | Esfuerzo |
|---|---|---|---|
| L-20 | True-peak limiter en `exportMix` (look-ahead 5 ms, threshold/release params) | `looper/Limiter.h` (nuevo) | M |
| L-21 | Soporte 24-bit y 32-bit float en `wav::writeWav` | `looper/WavFile.h:51–87` | S |
| L-22 | `exportStems(directory)` — N tracks con mismo length | `looper/AudioLooper.h` | S |
| L-23 | `exportMix` con N bars (mezcla `barsCount * masterLoopFrames`) y count-in opcional | `looper/AudioLooper.h:276` | S |
| L-24 | Pausar audio thread (o snapshot atómico de buffers) antes de `exportMix` para eliminar race | `looper/AudioLooper.h:276` | M |
| L-25 | Callback de progreso + cancelación cooperativa (`std::atomic<bool> mCancelExport`) | `looper/AudioLooper.h` + JNI | M |
| L-26 | Wrapper `suspend fun exportMix(...)` en Dispatchers.IO en androidMain | `androidMain/AudioNativeBridge.kt` | S |
| L-27 | Metadata WAV: chunk `LIST/INFO` con BPM, fecha, project name | `looper/WavFile.h` | S |

### Sprint 4 — Performance / cobertura

| ID | Acción | Archivos | Esfuerzo |
|---|---|---|---|
| L-30 | Reemplazar `cos`/`sin` per-sample por LUT pan o algoritmo recursivo | `looper/TrackBuffer.h:187–189` | M |
| L-31 | SIMD para `mixInto` (NEON / SSE) | `looper/TrackBuffer.h:180–237` | M |
| L-32 | Tests unitarios: `looper/tests/{test_track_buffer,test_audio_looper,test_wav_file,test_loop_quantization}.cpp` | nuevos | L |
| L-33 | Mover `AudioLooper.h` y `TrackBuffer.h` a `.cpp` (impl) + headers slim | refactor | M |
| L-34 | Telemetría runtime: contador de "frames perdidos en grabación", "exports completados", expuesto vía JNI | `AudioLooper.h` | S |
| L-35 | Eliminar `getTrackWaveform` race usando snapshot doble-buffer | `AudioLooper.h:449` | S |

### Sprint 5 — Multiplatform / arquitectura

| ID | Acción | Archivos | Esfuerzo |
|---|---|---|---|
| L-40 | Exponer interfaz `ILooperBridge` en `commonMain/api/` con todos los ops del looper | `commonMain` | M |
| L-41 | Crear `Looper` (commonMain) con state machine (Idle/Armed/Recording/Playing/Overdubbing) | `commonMain/internal/looper/` | M |
| L-42 | Doc de diseño: `docs/looper/00_audit.md`, `01_transport.md`, `02_tail_handling.md`, `03_export_pipeline.md` | nuevos | M |
| L-43 | Considerar Ableton-Link / MIDI clock sync via `Transport` | `core/Transport.h` | L |

---

## 4. Riesgos y consideraciones

- **L-02 (tap pre-master-vol)** cambia comportamiento existente: tracks ya grabadas con el comportamiento viejo no se ven afectadas, pero los usuarios que dependían de "grabar con vol 0 para mute" perderán esa interacción. Documentar.
- **L-10 (tail buffer)** sube uso de memoria por track ~10% (500 ms a 48 kHz stereo float = 192 KB). Con 8 tracks: +1.5 MB. Aceptable dentro del budget de 48 MB.
- **L-13 (armed state)** puede generar regresiones en NoisyPad si la UI espera el estado anterior "rec immediato". Coordinar.
- **L-24 (export sin race)** con pausa de audio thread genera glitch audible. Alternativa: doble-buffer interno o copia atómica frame-por-frame.
- **L-31 (SIMD)** complica builds para 4 ABIs. Validar en x86_64 (emulador) y arm64.

---

## 5. Métricas de éxito propuestas

Antes de declarar el looper "profesional":

1. **Latency end-to-end** (record → playback) < 25 ms en pixel-tier device.
2. **Zero clicks** en loop wrap con material sintético (test signal: senoide 220 Hz sin envelope).
3. **Tail preservado** ≥ 300 ms en loops con sustained pad.
4. **Click metrónomo** con jitter < 1 ms desde primer beat hasta el N-ésimo.
5. **Export bit-perfect** verificado con DAW externa: round-trip import → export → diff < -90 dBFS.
6. **CPU del looper** (8 tracks activos, todos con FX) < 8% en device de referencia.
7. **Cobertura tests** > 80% líneas en `looper/`.
8. **Sin data races** detectados con TSAN en stress test 5 min.

---

## 6. Anexo: ubicaciones clave referenciadas

- Mix RT-safe: `audio/src/main/cpp/looper/TrackBuffer.h:140–260`
- Click generator: `audio/src/main/cpp/looper/AudioLooper.h:108–128`
- Render input_fx: `audio/src/main/cpp/core/AudioEngine.cpp:1143–1191`
- Apply master vol: `audio/src/main/cpp/core/AudioEngine.cpp:1034–1048`
- Mix-loop processing: `audio/src/main/cpp/core/AudioEngine.cpp:1549–1551`
- BPM API: `audio/src/main/cpp/core/AudioEngine.h:402–423` (no llega al looper)
- JNI looper: `audio/src/main/cpp/jni/jni_audio_bridge.cpp:2217–2568`
- C API looper: `audio/src/main/cpp/api/watermelon_audio.h:665–745`
- WAV writer: `audio/src/main/cpp/looper/WavFile.h:51–87` (sólo 16-bit)
- Memory budget: `audio/src/main/cpp/looper/AudioLooper.h:35` (48 MB)
- `MAX_BUFFER_FRAMES`: `audio/src/main/cpp/looper/AudioLooper.h:38` (1024 hardcoded)
- Crossfade frames: `audio/src/main/cpp/looper/TrackBuffer.h:28` (128 = ~2.7 ms)
