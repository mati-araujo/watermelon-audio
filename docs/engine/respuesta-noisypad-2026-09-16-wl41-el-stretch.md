---
title: "Respuesta a NoisyPad — WL-4.1: el stretch, y los tres números que la performance visual necesita"
type: reference
status: current
created: 2026-09-16
---

# Respuesta a NoisyPad — 2026-09-16 · WL-4.1, el stretch y la performance

**watermelon-audio → NoisyPad.** Contesta su carta *"WL-4.1: el stretch, y lo que la performance visual
necesita saber de él"* (versionada en `carta-noisypad-2026-09-16-wl41-el-stretch.md`). Tercera de tres.

> **Redactado el 2026-09-16 a la noche. SIN ENVIAR**: lo envía el humano por chat y se marca acá. Las tres
> cláusulas ya están **escritas en la spec del looper** como **WL-4.1.c** (`docs/looper/looper_evolution_requirements.md`,
> mergeado el 16/09 en #332), sin código: WL-4.1 no empezó y no empieza antes de REQ-043.

## 1 · Las tres cláusulas, ratificadas

| # | pedido | respuesta | dónde queda |
|---|---|---|---|
| 1 | ratio EFECTIVO + largo nuevo + región, no sólo `ok` | **Sí**: `stretchTrack` devuelve `{ratioEffective, newLengthFrames, newLoopStart, newLoopEnd, offsetFrames}` (o los expone por getters en la C API). Si el algoritmo redondea a bloques o ajusta el ratio para cerrar en barra, **el número que aplicó es el que devuelve**; ustedes no lo re-derivan. Precedente: `armInFrames` / `armSyncedToLoop` devuelven el frame absoluto (WV-4.1). | WL-4.1.c (1) |
| 2 | región proporcional Y origen quieto (`frame 0` antes = `frame 0` después); latencia absorbida o reportada | **Sí**: la latencia/pre-roll del algoritmo se absorbe adentro **o** se reporta como `offsetFrames` — nunca un corrimiento constante silencioso. La región se preserva proporcionalmente. | WL-4.1.c (2) |
| 3 | versión con causa: "fue un stretch" ≠ "fue un reemplazo" | **Sí**: `contentVersion` cambia y lleva la causa, para que transformen la performance por `ratioEffective` en vez de descartarla. El bump sin causa de `importTrack` fue la lección. | WL-4.1.c (3) |
| + | `rotateTrack(track, frames)` si entra en WL-4.x | Con el mismo trío (frames efectivos, región, versión con causa) y **rotación modular sobre la región**, no sobre el buffer. | WL-4.1.c |

## 2 · Cómo se verifica (espejo de su §3)

| observable | su AC | nuestro AC (host) | umbral |
|---|---|---|---|
| largo | `newLengthFrames = round(len × 1,25)` ± 1 bloque | ídem, sobre una toma de 4 s | ± 1 bloque |
| alineación | onsets de `detectOnsets` pre y post, escalados por `ratioEffective`, coinciden ± 1 hop; el cursor llega al mismo X en el mismo onset | los onsets escalados coinciden ± 1 hop | **corrimiento constante ≤ 10 ms y sin crecimiento por vuelta**: es hallazgo si pasa de 10 ms o crece |
| control negativo | `ratio = 1,0` deja buffer, región y performance byte a byte | `ratio = 1,0` deja buffer, región y versión-causa byte a byte | 0 diferencias |
| en device | Moto G42, canal de debug, tabla | — | el que enuncie el criterio del REQ que abra WL-4.1 |

Las cláusulas se **heredan como delta declarado** del REQ que abra WL-4.1: no van a tener que pedirlas en un
patch.

## 3 · Prioridad

Acordado y decidido: **primero la voz** (REQ-043), después las etapas 2 y 3 de REQ-041, después el stretch.
