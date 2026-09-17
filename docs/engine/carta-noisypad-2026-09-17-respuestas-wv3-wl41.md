---
title: "NoisyPad → watermelon-audio · respuestas a WV-3 (REQ-043) y acuse de WL-4.1.c"
type: reference
status: current
created: 2026-09-17
---

# Carta de NoisyPad del 2026-09-17: las tres ❓ de WV-3, el fixture de 8 golpes y el acuse de WL-4.1.c

**Recibida el 2026-09-17 a la mañana.** Contesta nuestras dos cartas del 16/09 (PR #332):
`carta-noisypad-2026-09-16-wv3-la-capa-de-voz.md` y `carta-noisypad-2026-09-16-wl41-el-stretch.md`.
Cierra las **tres preguntas abiertas** de la spec de REQ-043 con la evidencia del consumidor, trae el
**fixture de 8 golpes** que pedimos (versionado en este PR, ver §3) y da por leído el contrato de
`stretchTrack` (WL-4.1.c) sin agregar nada. Texto íntegro, sin editar.

> **Lo que resuelve para REQ-043** (y queda enlazado como cláusulas resueltas en
> `docs/visuals/visual_features_requirements.md` §WV-3):
>
> - **hop**: lo fija el consumidor (`hopMs`, arranca en 10) y el motor devuelve `hopFrames` real,
>   redondeado una vez; si el MPM tiene un mínimo, lo aplica y lo devuelve.
> - **WV-3.1 es RMS**, cruda, lineal `[0, 1]`, sin normalizar ni en dB, sobre la región, con
>   `firstFrame` (= `loopStart`) + `hopFrames`; `binsPerSecond` lo fija el consumidor y el conteo
>   lo devuelve el motor.
> - **`speed ≠ 1` no afecta la serie**: el eje es el buffer y `layerFor` ya escala antes del módulo.
>   Lo único que se pide es que `stretchTrack` invalide o re-analice.
>
> El orden (REQ-043 antes de REQ-041 S2/S3 y del stretch; MINI-030 suelto y primero) queda acordado.
> El fixture del glide lo generamos y versionamos nosotros, como se dijo.

---

# NoisyPad → watermelon-audio · 2026-09-17 · respuestas a WV-3 (REQ-043) y acuse de WL-4.1.c

Contesta sus dos cartas del 16/09. Las tres ❓ van con la evidencia del consumidor, no con
preferencia: lo que se pide es lo que el código de hoy ya hace.

## 1 · Orden y MINI-030 — acordado

REQ-043 = WV-3.2 + WV-3.1 en un bump, antes de REQ-041 S2/S3 y del stretch; WL-5.2 después.
MINI-030 (`getTrackWaveform` con 0 bins ⇒ `FloatArray(numBins)` en ceros) suelto y primero: sí.

## 2 · Contrato — cláusula por cláusula

| # | su respuesta | la nuestra |
|---|---|---|
| 0 | `FloatArray(0)` cuando el motor no escribió ningún bin; R-API-59 para toda lectura de análisis por pista | **Aceptado.** Entramos por `hasAudio`, así que no indexamos `[0]` a ciegas; igual el bump entra con un test en `LooperVideoMaterial` que alimenta tamaño 0 y exige "sin capa", para que la regla quede escrita del lado del consumidor. |
| 1 | `frame` en frames del buffer | Confirmado. Es el eje de `CompositionTrack.lengthFrames`, `waveformBins` y `onsetFrames`. |
| 2 | `freqHz = 0` y `confidence = 0` exactos donde no hay pitch (NO_SIGNAL / NO_LOCK), nunca interpolado; `confidence` = claridad NSDF cruda | Confirmado. El umbral de dibujo es nuestro (`VoiceRibbonRenderer`); la cinta se corta en el punto con `freqHz == 0`. |
| 3 | ❓ ¿hop lo fijan ustedes y el motor devuelve el real en frames, o lo fija el motor? | **Lo fijamos nosotros (`hopMs`) y el motor devuelve `hopFrames` real, redondeado una vez.** Y como cada punto lleva su `frame` absoluto, nada se acumula: `hopFrames` nos sirve para el AC de determinismo y para dimensionar, no para reconstruir el eje. Nuestro valor de arranque: `hopMs = 10` (≈ 480 frames a 48 k; 3 puntos por cuadro a 30 fps, que es lo que la cinta necesita para no serruchar). Si el MPM tiene un mínimo por ventana, que lo aplique y lo devuelva: preferimos un hop más grande y verdadero a uno pedido y falso. |
| 4 | determinista, ring propio, offline en UI/IO | Confirmado. Nuestro test compara dos series byte a byte, con el afinador en vivo corriendo (el mismo AC suyo). |
| 5 | ❓ RMS en WV-3.1 | **RMS, por escrito.** Y **crudo, lineal `[0, 1]`, sin normalizar ni en dB**: del lado de NoisyPad la serie se normaliza al pico de la pista (`NormalizedToPeak`, piso 0,01) para que un WAV a −23 dBFS respire igual que uno a 0. Si el motor la normalizara, perderíamos el nivel real y el piso dejaría de tener sentido. Eje: buffer, igual que la waveform; que la serie **cubra sólo la región** pero venga con `firstFrame` (= `loopStart`) y `hopFrames`, o con el frame por bin — cualquiera de las dos, mientras esté en el doc. `binsPerSecond` lo fijamos nosotros; el conteo lo devuelve el motor (misma regla que el hop). |
| 6 | WV-3.3 fuera de REQ-043 | De acuerdo. |
| 7 | ❓ `speed ≠ 1`: el eje es el buffer, el análisis no cambia | **Confirmado — es lo que ya hacemos.** `VisualComposition.layerFor` escala `audioFrame × speed` ANTES del módulo por la región y lee el buffer en `loopStart + …` (el mismo frame que `TrackBuffer::process`). Una serie en frames del buffer entra ahí sin una línea nueva. Lo único que pedimos: que `stretchTrack` (WL-4.1) invalide o re-analice — ahí el buffer sí cambia, y una serie vieja contra un buffer nuevo es el bug silencioso de siempre. |

## 3 · El fixture de 8 golpes — adjunto

`audiograma-prueba.wav` — 4,000 s, 48 000 Hz, estéreo, 16 bit, 192 000 frames, pico **−23,2 dBFS**,
SHA-256 `82f44736e3b28bf81836bba7f3d89df1e059e5535cec394f83cbedac5804eb75`.

> **Dónde quedó (nota nuestra, 2026-09-17):**
> `audio/src/main/cpp/looper/tests/testdata/audiograma-prueba.wav`, con su sha256 en
> `audio/src/main/cpp/looper/tests/testdata/MANIFEST.txt`. El directorio nació en este PR y espeja
> `effects/tests/testdata/`; se versiona porque pesa 768 KB y es de origen propio — el corpus del
> afinador NO se versiona por tamaño y va por `scripts/fetch-corpus.sh`. El sha256 se verificó antes
> de copiarlo y coincide con el de arriba. Y el header y los ocho eventos se **re-midieron** al
> versionarlo (RMS por 10 ms sobre `(L+R)/2`): los frames, el escalón −28 → −32 dB a los 200 ms de los
> pares, los ~150 ms de los impares, el piso de −56 dB y el pico de −23,2 dBFS coinciden con lo que
> declara la carta.

Medido sobre el archivo (RMS por 10 ms): **8 eventos en grilla de 0,5 s (120 BPM)**, en los frames
**0 · 24000 · 48000 · 72000 · 96000 · 120000 · 144000 · 168000**. Los pares (0,0 / 1,0 / 2,0 / 3,0 s)
duran ~300 ms y tienen un **escalón interno −28 → −32 dB a los 200 ms**; los impares (0,5 / 1,5 / 2,5 /
3,5 s) duran ~150 ms a −30 dB. Entre eventos, −56 dB.

🪤 **El escalón es una trampa para "8 máximos locales"**: un barrido ingenuo sobre la envolvente da 12
(el pico de cada largo se cuenta dos veces). El AC que proponemos, entonces, no es "8 máximos" sino:
**el bin de la envolvente en cada uno de los 8 frames de `detectOnsets` ± 1 bin es un máximo local
por encima de −35 dB, y no hay ningún bin por encima de −45 dB fuera de [onset, onset + 350 ms)**.
Con eso los dos lados miden lo mismo y el escalón no cuenta.

El del glide (110 → 440 Hz en 2 s + 0,5 s silencio + 220 Hz) lo generan y versionan ustedes, como
dijeron; nosotros lo importamos por `importTrack` y lo medimos con `cmd video` en el G42 (I-2).

## 4 · WL-4.1.c — acuse

Las tres cláusulas leídas en `docs/looper/looper_evolution_requirements.md` tal como las pedimos:
`{ratioEffective, newLengthFrames, newLoopStart, newLoopEnd, offsetFrames}` devueltos y no
re-derivados; frame 0 quieto y la latencia como `offsetFrames` nunca silenciosa; `contentVersion`
con causa. `rotateTrack` con el mismo trío. Nada que agregar; PV-4.4 espera a REQ-043.

## 5 · Lo que no cambia de nuestro lado

Renderers y UI son nuestros. NoisyPad libera hoy sobre **2.19.1** (el tag más alto en el repo al
17/09); REQ-043 entra en la release siguiente cuando salga su tag, con la capa VOICE en un PR chico.
