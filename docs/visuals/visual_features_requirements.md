# Requerimiento: Visual Features — soporte del motor para Pad Visual Evolution

**Proyecto:** watermelon-audio (motor C++/Kotlin)
**Documento hermano:** `NoisyPad/docs/korg-req/phase17_pad_visual_evolution.md` (pad, cursores, video arte)
**Relacionado:** `docs/kmp/kmp_requirements.md` (reglas KMP), `docs/looper/looper_evolution_requirements.md` (Phase 16 — varios requerimientos se comparten)
**Estado:** PROPUESTO
**Fecha:** 2026-07-05

---

## 1. Objetivo y límite de alcance

Proveer al sistema visual de NoisyPad (Phase 17) los datos de audio que necesita, con dos calidades distintas:

1. **En vivo:** features por pista y de transporte, pusheadas de forma RT-safe, para que el pad visualice la música que suena (capas por pista, pulso de compás).
2. **Offline/determinista:** análisis por pista (envelopes, onsets, pitch) y timestamps exactos de grabación, para que el render de video sea reproducible y esté en sync sample-exacto con el mix exportado.

**Límite explícito (decisión D1):** el motor **no hace video**. Codificación/mux de video es responsabilidad de la app por plataforma (MediaCodec/AVAssetWriter). watermelon-audio aporta audio, análisis y tiempo.

---

## 2. Base existente

| Ya existe | Uso para Phase 17 |
|---|---|
| Peak level por pista, ya computado en C++ y polleado a 33 ms (mixer Phase 13C) | Base de WV-1 (falta onset y push) |
| `Transport::getPlayFrame()` / grilla de bars; frame de trigger real en `armRecording` | Timestamps de performance (WV-4) |
| `TrackBuffer::detectOnsets` (energy-flux, calibrado y verificado en device) y `findContentBounds` | Envelopes/onsets offline (WV-3.1) |
| `getTrackWaveform` (bins por pista) | Renderer de samples (app) |
| Export render-once con repeat + metadata BPM | Audio del video (WV-5) |
| **Compartidos con Phase 16:** WL-1.4 (beat/bar en el push), WL-5.1 (PitchDetector YIN), WL-8.3 (loudness por pista), WL-8.2 (tempo de archivos) | Referenciados, no duplicados |

**Nota de arquitectura:** el bus de features en vivo del pad (`AudioFeatureExtractor`, FFT Kotlin sobre `IWaveformProvider`) vive en NoisyPad y **se mantiene** — funciona, está calibrado y es Kotlin puro (KMP-friendly). Este documento solo agrega lo que ese bus no puede dar: features **por pista** (el waveform provider es master-only) y análisis **determinista** para el video.

---

## 3. Requerimientos

### WV-1 — Features por pista en vivo (push)

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WV-1.1 | Onset flag por pista | En `TrackBuffer::mixInto` (o un tap barato post-lectura): detector de ataque simplificado por pista (delta de energía por bloque contra media móvil, atomics; NO el detectOnsets offline). Flag consumible-once por pista | Golpes de un drum loop generan flags en el frame correcto; costo < 1% CPU con 8 pistas | P1 | M |
| WV-1.2 | Level por pista en el push | Los peaks por pista existen pero se obtienen por polling: incluir peak/RMS por pista en el push de estado existente (`LooperStateListener`/RecordProgress), junto con los onset flags (WV-1.1) y beat/bar (WL-1.4) | NoisyPad anima capas por pista sin polling adicional | P1 | S |
| WV-1.3 | Presupuesto | Todo lo de WV-1 es opt-in (atomic enable, apagado si el "Modo Show" está off) para no gastar en usuarios que no lo usan | Overhead cero con el modo apagado (medido) | P1 | S |

### WV-3 — Análisis offline por pista (determinista, para el video)

*(La numeración salta WV-2 reservado: beat/bar push = WL-1.4, requerimiento de Phase 16.)*

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WV-3.1 | Envelope de nivel por pista | `wma_looper_get_level_envelope(track, binsPerSecond, out)` — RMS decimado sobre el contenido (UI/IO thread, read-only, respeta loop region). Reusar/compartir con WL-8.3. Onsets: exponer `detectOnsets` por región si no está ya en la C API | Envelope determinista (mismo buffer → mismos valores); usado por SampleWaveRenderer y Modo Show | P0 | S |
| WV-3.2 | Curva de pitch por pista (voz) | `wma_looper_analyze_pitch(track, hopMs, out)` — corre el `PitchDetector` (WL-5.1, **gate**) offline sobre el buffer → serie {frame, freqHz, confidence} decimada. Para pistas grabadas con RecordSource=INPUT (voz) alimenta el renderer Ribbon | Curva correcta en voz real (fixture); confidence baja marca tramos no tonales | P1 | M (post WL-5.1) |
| WV-3.3 | Envelope de bandas del mix | Para el fondo reactivo del video: análisis banded (low/mid/high por ventana) sobre un **archivo** WAV (el mix exportado): `wma_analyze_file_bands(path, hopMs, out)` — comparte pipeline con WL-8.2 (análisis de archivos) y la FFT existente | El fondo del video reacciona al mix final exacto (no al render en vivo) | P1 | M |

### WV-4 — Tiempo exacto de grabación

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WV-4.1 | Frame de inicio real por pista | Exponer el frame de Transport en el que la grabación de una pista realmente arrancó (el trigger de `armRecording`/`armSyncedToLoop`, ya conocido internamente): `wma_looper_get_record_start_frame(track)`. Es el ancla de todos los timestamps de `VisualPerformance` (PV-4.2) | La app alinea eventos de UI al sample; verificación: golpe de drum grid vs onset del audio ≤ 1 bloque | P0 | S |
| WV-4.2 | PlayFrame consultable barato | Confirmar/exponer `wma_transport_get_play_frame()` en la C API (existe internamente) — la app lo usa para timestampear eventos de UI durante la toma | Lectura atómica, sin locks, llamable desde UI thread | P0 | S |
| WV-4.3 | (Opcional) Tap de note events | SPSC queue de eventos noteOn/off/velocity con frame (fuentes: voice pool, sfNoteOn — piano/drums/chords touchIds, arp) drenada por UI thread durante la grabación. Cubre lo que la UI no ve (arpegiador, chords generados). Solo si la captura UI (PV-4.2) resulta insuficiente | Eventos del arp visibles en el video con timing correcto | P2 | M |

### WV-5 — Audio para el video

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WV-5.1 | Mix con N repeticiones | Ya soportado (export con repeat); verificar que el largo exacto en frames del render sea consultable/predecible (la app calcula la duración del video con eso) — exponer `wma_looper_get_export_length_frames(repeats)` si no existe | Duración de audio y video coinciden al sample | P0 | S |

### WV-6 — API, tests y reglas

| ID | Requerimiento | Detalle | Prio |
|---|---|---|---|
| WV-6.1 | C API primero | Todo lo anterior entra a `watermelon_audio.h` (`wma_*`) con JNI wrapper — regla WA-2.5/2.6; suma a la tabla de cobertura `docs/kmp/c_api_coverage.md` | P0 |
| WV-6.2 | Determinismo testeado | gtests: mismos buffers → mismos envelopes/onsets/pitch bit-a-bit (los renderers golden-frame de NoisyPad dependen de esto) | P0 |
| WV-6.3 | Portabilidad | Nada de esto toca Android APIs: análisis en `looper/`/`analysis/`/`dsp/` (sub-libs portables); disponible en iOS con WA-2/3/4 sin trabajo extra | P0 |

---

## 4. Impacto de los otros requerimientos

- **Phase 16 (looper):** WV-3.2 depende de WL-5.1 (PitchDetector); WV-3.1 comparte implementación con WL-8.3; el push de WV-1.2 viaja en el mismo mensaje que WL-1.4 (beat/bar) — diseñar el snapshot de estado una sola vez para las dos phases. Si `stretchTrack` (WL-4.1) modifica una pista, sus envelopes cacheados app-side se invalidan por contentVersion (responsabilidad de la app).
- **KMP:** todos los requerimientos siguen las reglas del plan (C API primero, C++ portable, bridge en `IAudioNativeBridge` común). El análisis offline en nativo (no en Kotlin) garantiza que Android e iOS produzcan videos idénticos para la misma sesión.

## 5. Riesgos

| Riesgo | Mitigación |
|---|---|
| Costo del onset por pista en el hot path (WV-1.1) | Detector O(1) por bloque, opt-in (WV-1.3), medición como gate de merge |
| Crecimiento del mensaje de push de estado | Un solo snapshot versionado compartido con Phase 16; campos opcionales |
| Divergencia de análisis en vivo (Kotlin) vs offline (C++) | Aceptada por diseño: el bus Kotlin es estético/tiempo real; el video usa SOLO análisis nativo determinista |
