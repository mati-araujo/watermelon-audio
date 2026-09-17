---
title: "Aviso a NoisyPad — v2.19.2: un array de tamaño 0 es «sin dato» (MINI-030)"
type: reference
status: current
created: 2026-09-17
---

# Aviso a NoisyPad — 2026-09-17 · v2.19.2

**watermelon-audio → NoisyPad.** Contesta el §1 de su carta de hoy ("MINI-030 suelto y primero: sí").
Es un **patch** (`fix(kmp)`) **con nota de bump**: la firma no cambia, la semántica sí. Sin código de
REQ-043, que sigue detrás (spec en escritura, las tres ❓ cerradas con su carta).

> Redactado el 2026-09-17 antes del tag. `v2.19.2` **verificada en el registro, 4/4 coordenadas** (audio,
> audio-android, audio-iosarm64, audio-iossimulatorarm64), Publish run 35240128766. **SIN ENVIAR** al
> abrir el PR: marcar acá cuando salga por chat.

## 1 · Lo que cambia, en una frase

**`looperGetTrackWaveform(track, numBins)` devuelve `FloatArray(0)` cuando el motor no escribió ningún
bin** (pista inactiva o sin contenido). Hasta 2.19.1 devolvía `FloatArray(numBins)` en ceros: la
ausencia disfrazada de silencio. Vale en Android y en iOS.

## 2 · El contrato, por escrito (R-API-59)

| tamaño del array | significa |
|---|---|
| `0` | **no hay dato**: el motor no escribió ningún bin. No es silencio |
| `numBins` | hay dato; si el motor escribió menos que `numBins` (techo interno de 512), el resto queda en `0` y es **relleno de silencio**. El largo no varía: los llamadores de UI no reescalan |

Es la regla que **hereda toda lectura de análisis por pista** que se agregue después — en particular
`analyzePitch` y `getLevelEnvelope` de REQ-043 nacen con ella, como acordamos en la cláusula 0.

## 3 · Lo que les cambia a ustedes

- Un consumidor que indexa `bins[0]` sin mirar el tamaño se rompe con una pista inactiva. Dijeron que
  entran por `hasAudio` antes, así que no debería tocarlos; igual es la razón de la nota.
- El test que anunciaron en `LooperVideoMaterial` (tamaño 0 ⇒ "sin capa") ya tiene contra qué correr.
- Nada más se mueve: ni nivel, ni pitch, ni sends, ni la C API (diff vacío en `cpp/`).

## 4 · Lo que pedimos

Nada nuevo. Si al adoptar 2.19.2 alguna vista dibuja distinto una pista vacía, es hallazgo y lo
queremos saber; si no, con el acuse alcanza.
