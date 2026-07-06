# Requerimiento: Looper Evolution — DSP y motor

**Proyecto:** watermelon-audio (motor C++/Kotlin)
**Documento hermano:** `NoisyPad/docs/korg-req/phase16_looper_evolution.md` (app, UX, orquestación)
**Relacionado:** `docs/kmp/kmp_requirements.md` (reglas KMP/iOS de este repo)
**Estado:** PROPUESTO
**Fecha:** 2026-07-05

---

## 1. Objetivo

Proveer las primitivas DSP y de motor que hacen posible el looping "mágico" para principiantes definido en Phase 16 de NoisyPad: metrónomo garantizado, grabación desde input, edición de región sin glitches, time-stretch/pitch (elastic audio), efectos vocales (autotune, vocoder v2, harmonizer) y análisis (key, loudness, BPM de archivos). Todo **RT-safe, portable (C++20 puro) y expuesto vía C API primero** (regla KMP WA-2.5/2.6: el JNI es wrapper; iOS lo consume por cinterop sin trabajo extra).

---

## 2. Base existente (capitalizar, no duplicar)

| Ya construido y verificado | Ubicación |
|---|---|
| Transport sample-accurate (playFrame, nextBarBoundary, startMetronome) | `core/Transport.h` |
| Armado de grabación por frame: `armRecording`, `armSyncedToLoop` (phase-lock con cancelación de RTL), `PreRollRing`/`startRecordingWithPreRoll` (sin consumir aún por NoisyPad) | `looper/AudioLooper.h` |
| Onset detection (energy-flux log-domain, suavizado, piso absoluto) + `findContentBounds` + `finalizeFreeLoop` (trim + snap + seam bake en un pase) | `looper/TrackBuffer.h` |
| Perfil de costura por pista (crossfade largo vs corte duro) + wrap-mix circular horneado al grabar | `TrackBuffer`/`AudioLooper` |
| Render-guard (`mRendering` + `waitForRenderIdle`) para realloc seguro | `TrackBuffer.h` |
| Latencias medidas: `outputLatencyMs` (Oboe), `mInputLatencyFrames` (InputNode) | backends / `nodes/InputNode.cpp` |
| Metrónomo: `MetronomeClick` (sine burst 1200/900 Hz, 10 ms, RT) | `looper/MetronomeClick.h` |
| Resampler Catmull-Rom (import) y playhead fraccional Catmull-Rom (speed 0.5–2x, varispeed) | `looper/LooperExporter.cpp`, `TrackBuffer::mixInto` |
| FFT propia Cooley-Tukey (256–2048), magnitudes/smoothed/peak-hold | `analysis/SpectrumAnalyzer.h` |
| VocoderEffect (bandas, formant shift, carrier int/ext) + 24 efectos más | `effects/` |
| Suites gtest host-compilables (225+ tests) | `looper/tests/`, etc. |

**No existe hoy:** pitch detection, pitch shifting, time-stretch, harmonizer, autotune, detección de key, loudness por pista, tap de input hacia el looper, inserts por pista, cambio de región declickeado garantizado, rotate/offset de buffer.

---

## 3. Decisiones estratégicas

| # | Decisión | Recomendación | Justificación |
|---|---|---|---|
| D1 | **Motor de time-stretch/pitch-shift** | Vendorear **signalsmith-stretch** (MIT, C++ header-lib) en `thirdparty/` | Única opción de calidad con licencia compatible (Rubber Band=GPL, SoundTouch=LGPL, elastique=comercial). Sirve para stretch offline, pitch-shift del harmonizer y (opcional) el shifting del autotune. MIT como tsf/stb ya vendoreados. |
| D2 | **Detección de pitch** | Implementación propia de **YIN/MPM** en `dsp/PitchDetector.h` (header-only) | Algoritmos publicados y simples (~200 LOC); las libs existentes son GPL (aubio) o AGPL (Essentia). Monofónico alcanza (voz). |
| D3 | **Autotune: método de shifting** | **TD-PSOLA** para corrección de voz (monofónica, barata, RT-safe, preserva formantes razonablemente) con detector YIN; fallback/alternativa: signalsmith-stretch en modo per-block si PSOLA no da la calidad | PSOLA es el enfoque clásico de retune vocal en RT con CPU mínima — apto para low-tier. El caso de uso prioritario (retune rápido estilo trap) es donde PSOLA brilla. |
| D4 | **Time-stretch de pistas: offline, no RT** | `stretchTrack()` corre en UI/IO thread con el patrón pause/fence/realloc existente (`waitForRenderIdle`), nunca en el callback | Stretch de calidad no es viable RT en low-tier; el caso de uso (conformar a BPM, import) es naturalmente offline. Varispeed RT ya existe para lo creativo. |
| D5 | **FX por pista** | Fase 1: **print en captura** (input pasa por cadena de FX, el looper graba wet). Fase 2: **1 slot de insert por pista** en playback, como efecto compuesto "VocalChain" con presupuesto por tier | 8 pistas × cadena completa no entra en el presupuesto de CPU de low-tier. El print reutiliza toda la infraestructura de input-FX de guitar mode. |
| D6 | **Tap de input al looper** | El looper recibe **dos buffers** en `process()`: el mix del synth (actual) y el bus de input post-input-FX; la fuente de captura se selecciona por atomic (`RecordSource`) | Cambio mínimo al hot path; el input ya existe en el graph (InputNode). Definido sobre `IAudioBackend` → portable a CoreAudioBackend (iOS) sin cambios. |
| D7 | **FFT** | Mantener la FFT propia; si el chroma/análisis lo exige, migrar a **pffft** (BSD-like) como optimización, no como prerequisito | Evitar dependencia nueva sin necesidad medida. |
| D8 | **Superficie de API** | Toda función nueva: `wma_*` en `api/watermelon_audio.h` primero; JNI wrapper; bridge Kotlin en `IAudioNativeBridge` (commonMain) + impl androidMain | Regla KMP (WA-2.5/2.6): las features nacen multiplataforma. |

---

## 4. WS1 — Metrónomo garantizado

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WL-1.1 | **Click nunca horneado en grabación** | Auditar orden en `AudioLooper::process()`: hoy el click parece renderizarse sobre `audioData` **antes** del write de captura → riesgo de click grabado en pistas. Fix: el looper captura del buffer **pre-click**; el click se suma en un stage posterior (o a un scratch buffer mezclado post-captura). Contrato documentado en el header | gtest nuevo: grabar 2 bars con metrónomo ON → FFT del track sin picos en 1200/900 Hz; test de regresión permanente | P0 | S/M |
| WL-1.2 | Sonidos y volumen de click | `MetronomeClick`: 3 timbres sintetizados (beep actual, rimshot = burst de ruido filtrado + pitch env, clave = doble parcial corto), ganancia master del click (atomic), acento on/off. API: `wma_metronome_set_sound/volume/accent` | Cambio de timbre/volumen en vivo sin glitch | P1 | S |
| WL-1.3 | Estrés y jitter | Test de estrés (host): 8 pistas sonando + 6 FX + click continuo, N bloques → verificar que cada click cae exactamente en su frame de grilla (tolerancia 0) y sin underruns simulados | gtest de continuidad de clicks bajo carga verde | P1 | M |
| WL-1.4 | Beat/bar en el estado pusheado | Incluir `currentBeat`/`currentBar` (derivados de Transport, atomics) en el snapshot de estado que ya se pushea a Kotlin (RecordProgress/LooperStateListener) — elimina la derivación por coroutine en la UI | NoisyPad anima el beat desde el push (NL-2.2) | P1 | S |

---

## 5. WS2 — Captura desde input

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WL-2.1 | `RecordSource` en el motor | Enum `{SYNTH_MIX, INPUT_DRY, INPUT_FX}` + atomic en `AudioLooper`; `process()` recibe además el puntero al bus de input (dry y post-FX ya existen en el graph de input). La rama de grabación elige el buffer fuente según el atomic. API: `wma_looper_set_record_source` | Grabar mic en una pista mientras el synth suena en otras; el synth NO se cuela en la toma de input | P0 | M |
| WL-2.2 | Latencia del path de input | El armado con compensación (`armInFrames`/`armSyncedToLoop`) debe usar RTL correcto según la fuente: SYNTH_MIX → solo out-latency semantics actuales; INPUT_* → out + in (`mInputLatencyFrames`). Exponer la latencia efectiva por fuente (`wma_looper_get_latency_for_source`) | Toma de mic sobre pista de referencia queda en grilla (test de device con loopback físico documentado) | P0 | M |
| WL-2.3 | Monitoreo | Asegurar que el path de monitoreo de input existente (guitar mode) es utilizable con el looper grabando: monitoreo on/off + ganancia sin afectar lo capturado (captura pre-fader de monitoreo) | Monitoreo mudo ≠ toma muda | P1 | S |
| WL-2.4 | Pre-roll para input | Verificar que `PreRollRing` opera sobre la fuente seleccionada (hoy siembra post-FX del mix); generalizar al bus de input | REC tardío rescata ~500 ms de la fuente correcta | P2 | S |
| WL-2.5 | Portabilidad iOS | El tap de input se define contra el graph/`IAudioBackend` (no contra Oboe). Requisito cruzado con WA-2.4 (CoreAudioBackend full-duplex): mismo contrato de buffers | El diseño no referencia tipos Oboe; revisado en PR | P0 | — |

---

## 6. WS3 — Primitivas de edición de región

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WL-3.1 | Cambio de región declickeado | Al mover loopStart/loopEnd con la pista sonando: si el playhead queda fuera de la nueva región (o el salto es audible), aplicar micro-crossfade (~256 frames, buffer scratch preasignado) o diferir el jump al próximo wrap (política por parámetro). Hoy los atomics aplican al vuelo sin declick garantizado | gtest: barrido de cambios de región durante playback → sin discontinuidades > umbral en la salida | P0 | M |
| WL-3.2 | `findNearestZeroCrossing` | `TrackBuffer::findNearestZeroCrossing(frame, maxRadius)` (UI-thread, read-only, busca cruce por cero de energía mínima en ambos canales). API `wma_looper_find_zero_crossing` | Cortes snapeados a zero-cross sin click (test con seno) | P1 | S |
| WL-3.3 | Rotate/offset del loop (nudge) | `TrackBuffer::rotateContent(offsetFrames)` con el patrón pause/fence/realloc-in-place (render-guard existente): rota el contenido circularmente para corregir costura/feel. API `wma_looper_rotate_track`. Cierra el pendiente "Fase 4" del plan de sync | Nudge ±10 ms audiblemente correcto; gtest de rotación exacta | P2 | M |
| WL-3.4 | Waveform por región | Si NoisyPad lo necesita para el editor con zoom: `wma_looper_get_waveform_region(track, start, end, bins)` (peak-per-bin, UI-thread) — extensión del getTrackWaveform actual | Bins correctos para sub-rangos | P2 | S |

---

## 7. WS4 — Time & pitch (elastic audio)

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WL-4.1 | Integración signalsmith-stretch | Vendor en `thirdparty/signalsmith-stretch/` (MIT, header-only) + CMake include. `AudioLooper::stretchTrack(track, ratio, semitones=0)`: offline (UI/IO thread), pause/fence, escribe a buffer nuevo, swap atómico, preserva loop region proporcionalmente. Progreso por callback/polling. API `wma_looper_stretch_track` + `wma_looper_get_stretch_progress` | Stretch 0.8–1.25x sin artefactos groseros en batería y voz; 8 pistas conformadas < 3 s en device mid-tier | P1 | L |
| WL-4.2 | Modo speed Stretch por pista | Alternativa al varispeed: al soltar el control de speed en modo Stretch, se ejecuta WL-4.1 con ratio acumulado y speed vuelve a 1.0 (pitch constante). El modo Tape (actual) no cambia | Toggle por pista; ida y vuelta sin degradación acumulada notable (guardar original o límite de re-stretch documentado) | P2 | M |
| WL-4.3 | Resampler sinc para import | Subir `LooperExporter::resampleStereo` de Catmull-Rom a sinc windowed polifásico (tabla precomputada, ~16 taps) para conversiones 44.1↔48 k. Mantener Catmull-Rom para el playhead RT (correcto ahí) | THD+N mejorado en sweep 44.1→48 k vs actual (medición en test) | P2 | M |
| WL-4.4 | SIMD `mixInto` + fast-path | Pendiente heredado de perf: vectorizar el hot loop (NEON/SSE via `SIMDUtils.h`) y fast-path para speed==1.0 sin región (copia+gain directa). Habilita el presupuesto de CPU para inserts (WS7) | Benchmark: ≥ 2x en el mix de 8 pistas en arm64; gtests de paridad bit-a-bit en fast-path | P1 | M |

---

## 8. WS5 — Pitch detection, Autotune y Harmonizer

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WL-5.1 | `dsp/PitchDetector.h` | YIN (o MPM) header-only, monofónico: ventana ~25–40 ms, threshold configurable, salida {freqHz, confidence}, hop configurable. RT-safe (buffers preasignados). Test con senos, voz sintética y sweeps | Error < 1% en 80–1000 Hz con confidence útil; gtest verde | P0 | M |
| WL-5.2 | **AutotuneEffect** | Nuevo efecto (patrón `Effect` estándar + EffectRegistry): detector (WL-5.1) → target = nota más cercana de la escala activa → corrección por **TD-PSOLA** (D3). Parámetros: `keyRoot`, `scaleMask` (interop con el sistema de escalas existente — mismo formato que ScaleMode), `retuneSpeedMs` (0 = hard/trap, 400 = natural), `amount`, `formantPreserve` (on/off). Latencia reportada correctamente al chain | Voz desafinada → afinada a escala; retune 0 ms produce el carácter "trap" característico; CPU < 8% de un core mid-tier | P0 | L |
| WL-5.3 | **HarmonizerEffect** | 1–2 voces pitch-shifted (PSOLA o signalsmith per-block) en intervalos diatónicos de la escala activa (3ª/5ª/octava), mezcla y spread estéreo. Comparte el detector con WL-5.2 | Voz + 3ª diatónica correcta en escala mayor/menor; CPU < 10% | P2 | L |
| WL-5.4 | Registro y constantes | `EffectType` + `EffectParameter` + `EffectConstants` para ambos (workflow estándar de nuevo efecto); presets de fábrica (Natural, Hard Trap, Sutil / 3ª arriba, 5ª abajo) | Consumibles desde NoisyPad como cualquier efecto | P0 | S |

---

## 9. WS6 — Vocoder v2

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WL-6.1 | Carrier desde el engine | Nueva fuente de carrier: la salida del synth engine activo (tap pre-master), además de osc interno/input actuales. Param `CARRIER_SOURCE += ENGINE` | Cantar y que "hable" el Supersaw/FM del proyecto | P1 | M |
| WL-6.2 | Calidad escalable | Bandas configurables por tier (8/16/24), envolventes con attack/release mejorados (por banda), ruido de sibilancia (banda HF unvoiced pass-through) para inteligibilidad | Inteligibilidad claramente mejor con sibilancia; CPU por tier documentada | P2 | M |

---

## 10. WS7 — Cadenas de FX por pista

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WL-7.1 | Print en captura (fase 1) | Garantizar que INPUT_FX (WL-2.1) captura post-cadena-de-input con la cadena preset activa; sin trabajo adicional del looper (la cadena vive en el graph de input) | Toma wet correcta con preset de cadena vocal | P0 | — |
| WL-7.2 | Insert por pista (fase 2) | 1 slot de `Effect` por `TrackBuffer`, procesado en `mixInto` post-lectura/pre-pan (usa el presupuesto ganado en WL-4.4). Diseñado para el efecto compuesto "VocalChain" (comp+EQ+autotune+delay+reverb send simplificada) pero acepta cualquier Effect. Límite de inserts simultáneos por tier (atomic config, patrón `LooperConfig`) | 2–8 inserts según tier sin underruns; bypass sample-safe | P2 | L |

---

## 11. WS8 — Análisis

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WL-8.1 | Key/escala (chroma) | `analysis/KeyDetector.h`: chromagram (reusa FFT existente, ventanas sobre el contenido de la pista, UI-thread) → perfil Krumhansl → {root, major/minor, confidence}. API `wma_looper_detect_key(track)` | Key correcta en material tonal simple (test con progresiones sintetizadas); confidence baja en percusión | P1 | M |
| WL-8.2 | BPM de archivos | Correr el pipeline onset (existente) + estimador de período sobre buffers importados/archivos (no solo pistas grabadas): `wma_analyze_file_tempo(path)` reutilizando WavFile + detectOnsets + un port nativo del comb-template IOI (hoy en Kotlin `LooperTimingMath`) — o exponer onsets del archivo y dejar el comb en Kotlin (decisión: **port nativo**, así iOS lo tiene igual) | BPM ±2 en loops de batería estándar | P2 | M |
| WL-8.3 | Loudness por pista | RMS ventana completa + short-term max por pista (UI-thread, read-only): `wma_looper_get_track_loudness(track)` → base del auto-level ("Magic Mix") | Valores estables y monótonos con la ganancia | P1 | S |
| WL-8.4 | VAD simple | Umbral adaptativo sobre energía+ZCR para bounds de voz (variante de `findContentBounds` con perfil "voice": no corta colas suaves) — `findContentBounds(profile)` | Trim de voz conserva finales de frase (fixture de voz real) | P2 | S |

---

## 12. WS9 — API, bridge y tests

| ID | Requerimiento | Detalle | Prio |
|---|---|---|---|
| WL-9.1 | C API primero | Todas las funciones nuevas de este doc entran a `api/watermelon_audio.h` (`wma_*`, handles opacos, error codes, thread-safety documentada por función) antes o junto con su JNI. Suma al gap-closure WA-2.5 | P0 |
| WL-9.2 | Bridge Kotlin | Métodos nuevos en `IAudioNativeBridge` (commonMain) + impl en `AudioNativeBridge` (androidMain); mutex de categoría `looperMutex` para operaciones offline (stretch, rotate); nunca suspender las RT (setRecordSource es atomic-set) | P0 |
| WL-9.3 | Suites gtest nuevas | `test_metronome_capture.cpp` (WL-1.1/1.3), `test_record_source.cpp` (WL-2.1/2.2), `test_region_edit.cpp` (WL-3.1/3.2/3.3), `test_stretch.cpp` (WL-4.1, con golden files), `test_pitch_detector.cpp`, `test_autotune.cpp` (senos desafinados → afinados), `test_key_detector.cpp`, `test_loudness.cpp`. Todos host-compilables (corren con `scripts/run-cpp-tests.ps1`, sin gradlew) | P0 |
| WL-9.4 | Benchmarks de CPU | Micro-benchmarks host de: mixInto (pre/post SIMD), autotune, vocoder v2, insert chain — tabla de presupuesto por tier en `docs/looper/cpu_budget.md` | P1 |
| WL-9.5 | Licencias | `THIRD_PARTY_LICENSES` actualizado: signalsmith-stretch (MIT). Prohibido: GPL/AGPL (aubio, Rubber Band, Essentia); LGPL solo dinámico (no aplica) | P0 |

---

## 13. Impacto KMP/iOS (obligatorio)

1. **Portabilidad por construcción:** todo el DSP nuevo va a `dsp/`, `effects/`, `analysis/`, `looper/` — sub-libs CMake puras que la Fase 2 KMP (WA-2.1) compila para iOS sin cambios. Prohibido `#include <android/*>`/`<jni.h>` fuera de las zonas permitidas (guardrail WA-0.4 los detecta).
2. **C API primero (WL-9.1)** es la misma regla WA-2.5/2.6: cada primitiva de este doc reduce el gap JNI↔C API en lugar de agrandarlo. Las funciones nuevas del looper deben nacer en la tabla de cobertura `docs/kmp/c_api_coverage.md`.
3. **Input tap (WL-2.x)** se especifica contra el graph/`IAudioBackend`: el `CoreAudioBackend` (WA-2.4) debe alimentar el mismo bus de input — requisito cruzado explícito; revisar juntos los contratos de buffer (interleaving/formato) al diseñar ambos.
4. **Nada de esto toca `commonMain` de forma Android-specific:** los métodos nuevos del bridge van a `IAudioNativeBridge` (común) y su impl Android; `IosAudioBridge` (WA-3.2) los obtiene del mismo contrato.
5. **Análisis en nativo, no en Kotlin (WL-8.2):** portar el comb-template de tempo a C++ evita divergencia Android/iOS y duplica menos lógica que mantener la versión Kotlin como fuente de verdad.
6. **Secuenciación:** este requerimiento puede ejecutarse antes de que exista el target iOS; si se respetan 1–5, el looper evolucionado queda disponible en iOS en cuanto WA-2/3/4 aterricen, sin retrabajo.

---

## 14. Orden recomendado y quick wins

**Quick wins (primera tanda, independientes):**
- WL-1.1 (auditoría/fix click-en-grabación) — el más crítico del doc.
- WL-3.2 (zero-crossing) y WL-3.1 (región declickeada) — desbloquean el fix de trim de NoisyPad (NL-1.x).
- WL-1.4 (beat en el push de estado).
- WL-8.3 (loudness) — trivial y habilita Magic Mix.

**Refactors de alto valor:**
- WL-4.4 (SIMD mixInto) — paga el presupuesto de CPU de todo lo demás (inserts, autotune en vivo).
- WL-9.1 (C API primero) — convierte este requerimiento en un acelerador del plan KMP en vez de deuda nueva.
- WL-8.2 (tempo a nativo) — una sola fuente de verdad de tempo para ambos OS.

**Ruta crítica de features:** WL-2.1/2.2 (input) → WL-5.1/5.2 (autotune) → WL-4.1 (stretch). El resto paraleliza.

---

## 15. Riesgos

| Riesgo | Impacto | Mitigación |
|---|---|---|
| PSOLA insuficiente para retune "natural" | Calidad del autotune lento | El caso prioritario es retune rápido (trap) donde PSOLA rinde; fallback D3 a signalsmith per-block para el modo natural |
| CPU de autotune+inserts en low-tier | Glitches | WL-4.4 primero; gating por tier; benchmarks WL-9.4 como gate de merge |
| Cambio del hot path de grabación (WL-1.1/2.1) introduce regresiones RT | Bugs de audio | Los gtests e2e existentes (record→finalize→seam) corren en cada PR; cambios chicos y por etapas; render-guard ya probado |
| signalsmith-stretch: calidad/latencia en material polifónico denso | Conformado de mezclas | Stretch es por pista (material más simple que un mix); ratios acotados 0.75–1.33 en UI |
| Detección de key/BPM con falsos positivos | Confianza del usuario | Umbrales de confidence + siempre presentado como sugerencia cancelable (patrón Fase D ya validado) |
