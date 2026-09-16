---
title: "NoisyPad → watermelon-audio · WV-3: la capa de voz del video, y lo que ya consumimos"
type: reference
status: current
created: 2026-09-16
---

# Carta de NoisyPad: WV-3, la capa de voz del video

**Recibida el 2026-09-16 a la noche**, fuera del hilo de REQ-040/041. Pide orden y contrato para
WV-3.2 (`wma_looper_analyze_pitch`) y WV-3.1 (`wma_looper_get_level_envelope`), ya escritos en
`docs/visuals/visual_features_requirements.md` §WV-3, y deja una **nota sobre el contrato de
`getTrackWaveform`** (ceros ≡ sin señal del lado de Kotlin). Texto íntegro, como llegó (copia sin editar
en `specs/referencias/inbox-noisypad-2026-09-16-carta-wv3.md`).

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo verificado está al final.

---

# NoisyPad → watermelon-audio · WV-3: la capa de voz del video, y lo que ya consumimos

**2026-09-16, noche.** Aparte del hilo de REQ-040/041 (vimos el aviso de 2.19.1 en el repo: cuando esté el
tag lo tomamos con la ventana acordada, las siete sondas y las dos de Q alto). Esto es otra cosa: el
video ya se comparte (#270 en NoisyPad: audiograma para pistas sin captura + marca/badge/escena) y la
única fuente que hoy no dibuja nada es la voz. No pedimos superficie nueva — está en su
`docs/visuals/visual_features_requirements.md` §WV-3 — pedimos orden y contrato, para no medir contra un
modelo mental (nos pasó tres veces en un día con el SF2).

## 1 · Lo que consumimos hoy, y una nota sobre el contrato

| del motor | quién lo consume en NoisyPad | cómo |
|---|---|---|
| `looperGetTrackWaveform(track, 96)` | `LooperVideoMaterial.collect` → `VisualLayer.SampleWave` | una vez por pista, antes de componer (nunca por cuadro) |
| `looperDetectOnsets(track)` | ídem → ticks de onset | ídem |
| `getTrackLoopStart/End`, `getTrackSpeed` | `CompositionTrack` | el video wrapea en la REGIÓN y dibuja contra el BUFFER |

🔎 Nota, no pedido: `getTrackWaveform` devuelve 0 bins cuando `!isActive()` y el bridge igual entrega
`FloatArray(numBins)` en ceros — del lado de Kotlin "sin señal" y "silencio" son indistinguibles. Lo
absorbemos (una pista inactiva no entra al video por `hasAudio`), pero si WV-3.1 nace con el mismo
contrato, el video no va a poder decir "no analizado" ≠ "en silencio". Preferimos tamaño 0 / null para
"no hay".

## 2 · Lo que pedimos, en el orden que nos sirve

**WV-3.2 `wma_looper_analyze_pitch(track, hopMs, out)` → serie {frame, freqHz, confidence}.** Es lo único
que destraba `VisualComposition: VOICE -> null` (hoy explícito, con KDoc que dice por qué). Sabemos que
depende de WL-5.1 (PitchDetector) y que su ruta crítica es WL-2.1/2.2 → WL-5.1/5.2 → WL-4.1: no pedimos
saltearla; pedimos que WV-3.2 salga en el mismo bump que WL-5.1, aunque WL-5.2 (autotune) venga después.
Lo que necesitamos del contrato:
- `frame` en frames del BUFFER (el eje de `getTrackWaveform`/`detectOnsets`), no de la región.
- `freqHz = 0` o `confidence = 0` donde no hay pitch, nunca interpolado: el renderer corta la cinta ahí.
- `hopMs` respetado exacto (o devuelto): el video lo convierte a frames y cualquier redondeo se acumula.
- Determinista: dos llamadas sobre el mismo buffer, la misma serie byte a byte (nuestro test lo va a
  exigir).

**WV-3.1 `wma_looper_get_level_envelope(track, binsPerSecond, out)`**: hoy usamos la waveform de picos
como envolvente (96 bins por pista, sin normalizar). Un RMS decimado que respete la región nos deja el
audiograma "respirando" con nivel real y sin el truco de tomar el bin bajo el cabezal. Misma condición:
región vs buffer explícito en el nombre o en el doc.

**WV-3.3** (bandas del mix sobre archivo) puede esperar: el telón reactivo todavía no existe en el video.

## 3 · Cómo lo vamos a verificar (para que el AC sea el mismo de los dos lados)

**Pitch**: un WAV sintético de voz (glide 110 → 440 Hz en 2 s + 0,5 s de silencio + 220 Hz estable),
importado por la biblioteca (el mismo camino que el usuario). Esperamos: error < 1 % en el glide,
confidence ≈ 0 en el silencio (cero puntos interpolados ahí), y la serie idéntica en dos análisis
seguidos. Umbral de hallazgo: cualquier punto con `freqHz > 0` dentro del silencio.

**Envelope**: el mismo WAV de 8 golpes que usamos en #270 (4 s, pico −23 dBFS,
`Download/audiograma-prueba.wav` si lo quieren): 8 máximos locales en los frames de los onsets que ya
devuelve `detectOnsets` (±1 bin).

Lo medimos en el Moto G42 con nuestro canal de debug (`cmd video` con material real) y reportamos en el
formato de tabla que acordamos.

## 4 · Lo que NO pedimos

Ni renderers ni nada de UI: `VoiceRibbonRenderer` es nuestro y ya está diseñado (cinta de pitch +
amplitud, color de pista, misma composición determinista). El día que el bridge exponga `analyzePitch`,
la capa entra en un PR chico de NoisyPad.

---

## Lo que verificamos de nuestro lado (2026-09-16)

- **La nota sobre `getTrackWaveform` es correcta y es nuestra**: `wma_looper_get_track_waveform`
  devuelve el número de bins escritos (0 con la pista inactiva), pero `AudioNativeBridge.looperGetTrackWaveform`
  (`AudioNativeBridge.kt:3099`) **descarta el retorno** y entrega `FloatArray(numBins)` en ceros. Es la
  clase de "un array de ceros no es dato ausente" que este repo ya pagó dos veces (`OfflineAnalysis.h`
  lo dice en su contrato). Candidato a **MINI**: el bridge devuelve `FloatArray(0)` cuando el motor
  escribe 0 bins, y WV-3.1/3.2 nacen con ese contrato.
- **WL-5.1 ya existe a medias**: el detector MPM del afinador (`dsp/McLeodPitch.h`, REQ-001) es "YIN o
  MPM, monofónico, {freqHz, confidence}" — lo que la spec pide. Lo que NO existe es el recorrido
  **por hop sobre un buffer**: el puerto offline (`analysis::analyzeBuffer`, REQ-015/029) devuelve UN
  snapshot por buffer, no una serie. WV-3.2 es ese recorrido + la C API + el bridge; no depende de
  WL-2.x ni de WL-5.2.
- **Los cuatro puntos del contrato que piden son razonables y uno ya tiene precedente**: `frame` en
  frames del buffer es el eje de `detectOnsets`; "nunca interpolado" es lo que el afinador ya hace
  (NO_SIGNAL/NO_LOCK con `detectedHz` intacto, REQ-031); el determinismo es lo que `analyzeBuffer`
  garantiza (ring propio, sin estado compartido); `hopMs` exacto se resuelve devolviendo el hop en
  frames. Lo que no es gratis: el afinador **no tiene confidence** como tal (tiene claridad NSDF y la
  compuerta de ausencia/soporte espectral); hay que decidir qué se publica como `confidence`.
- **WV-3.1** es un RMS decimado sobre la región: `TrackBuffer` ya tiene `getLiveWaveform` (max-pool)
  y `detectOnsets` (energy-flux); es S, sin dependencias. Su AC de ellos (8 máximos en los onsets ±1
  bin) es afirmable en host con el WAV que ofrecen.
- **Decisión pendiente del humano**: abrir un REQ para WV-3.2 + WV-3.1 (+ el MINI del bridge) y su
  orden respecto de REQ-041 S2/S3. La carta pone la voz por encima del stretch, y por encima de la
  interpolación de S3 no lo dice pero lo implica: S3 nadie lo pidió.
