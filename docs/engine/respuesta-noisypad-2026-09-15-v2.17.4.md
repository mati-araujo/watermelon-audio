---
title: "Aviso a NoisyPad — v2.17.4: las envolventes de tsf contra el spec"
type: reference
status: current
created: 2026-09-15
---

# Aviso a NoisyPad — 2026-09-15 · v2.17.4

**watermelon-audio → NoisyPad.** Sigue a los avisos de v2.17.2 y v2.17.3 del 14/09 (sin
respuesta todavía; no hace falta contestar los tres por separado). Este es el que **más se oye** de
los cuatro: léanlo antes de subir el bump.

> **Enviado el 2026-09-15. Contestado el 15/09** (carta del mediodía + corrección de la tarde):
> `carta-noisypad-2026-09-15-respuesta-v2.17.4.md`. Fix 1 ✅ en cinco presets con t40/t60 que cierran
> con los generadores efectivos de las cuatro capas. Fix 2: dijeron que en `8:63 Synth Brass 4` el
> filtro abre "desde los 5 ms" donde 2.17.3 abría a 54 ms, y que FluidSynth sería lineal; **medido
> contra FluidSynth 2.6.0 sobre la zona exacta, FluidSynth abre igual que 2.17.4** (convexa en el
> filtro también, `fluid_rvoice.c:369-370,447`). Sin MINI. Cotejado al final de esa carta.

## 1 · Lo que cambia

Cuarto caso de la misma clase que 2.17.1–2.17.3 (tsf resolvía un generador del SoundFont a otra
escala u otra forma que el spec), y esta vez son las **envolventes**. Cuatro cosas, todas medidas
primero contra FluidSynth 2.6.0 sobre el SoundFont-Spec-Test y después contra la fórmula de SF2
§8.1.2 sobre fonts mínimos:

1. **Decay y release de volumen recorren 100 dB en el tiempo declarado, no 80.** tsf traía una
   constante de LinuxSampler; el spec (#36/#38) dice 100. Toda pendiente de decay y release es
   ahora un **25 % más rápida en dB/s**: una release de 1 s que llegaba a −60 dB (inaudible) a los
   0,75 s llega a los 0,60. FluidSynth usa 96, así que contra él quedamos un 4 % más rápidos —
   declarado, no abierto.
2. **El ataque de la envolvente de modulación (filtro/pitch) ya no depende de la velocity.** tsf lo
   acortaba hasta 8× en las notas fuertes (a v=127 un ataque de 100 ms duraba 12,5). El spec no
   tiene tal cosa. **Los barridos de filtro de las notas fuertes duran ahora lo que el font
   declara.**
3. **Ese ataque es convexo** (spec #26): abre más rápido al principio y llega al final en el mismo
   tiempo. La curva es la de FluidSynth.
4. **Su release recorre el 100 % en el tiempo declarado** (#30): desde un sustain parcial vuelve al
   reposo en proporción, no en el tiempo entero.

Lo que **no** cambia: el sustain de volumen (tsf ya lo ponía en centibeles exactos), los niveles,
las curvas de velocity, la afinación (2.17.3) ni el LFO.

## 2 · En qué presets, medido

La nota completa está en `nota-de-bump-2026-09-15-envolventes-contra-el-spec.md`. Contado sobre
`GeneralUser_GS.sf3` recorriendo preset → instrumento:

| fix | generador | presets (de 269) | lo que se oye |
|---|---|---|---|
| 1 | `releaseVolEnv` / `decayVolEnv` | **259** / 207 | colas ~20 % más cortas en lo audible; decays al sustain un 20 % más rápidos. Sutil, en casi todo |
| 2–4 | `modEnvToFilterFc` | **176** — 80 de los 112 melódicos del banco 0 | **audible en notas fuertes**: el filtro abre en el tiempo programado en vez de casi instantáneo |
| 2–4 | `modEnvToPitch` | 30 | ídem sobre el pitch |

Los 176 incluyen los pianos (`0 Stereo Grand`, `1 Bright Grand`, `4/5 Electric Piano`), órganos,
guitarras (`25 Steel`, `26 Jazz`, `28 Muted`, `30 Distortion`), **todos los bajos** (32–39),
cuerdas, voces y bronces (`56 Trumpet`, `57 Trombone`). Los sintes de lead/pad con envolvente de
filtro son los que más lo notan a velocity alta.

## 3 · Lo que conviene hacer de su lado

Es la referencia que pidieron (*el font como lo programó su autor, FluidSynth como referencia*), y
esta vez lo van a oír sin buscarlo. Sugerencia concreta: **antes de subir el bump, toquen un bajo y
un piano a velocity máxima** en 2.17.3 y en 2.17.4 — el ataque del filtro es la diferencia más
evidente.

## 4 · La pregunta

**¿Algún preset "cambió demasiado" o una cola se percibe cortada?** Nombre del preset, tecla y
velocity: se mide contra FluidSynth sobre un font mínimo con los generadores exactos de esa zona,
como en las notas anteriores. Un "el Slap Bass sonaba mejor con el filtro instantáneo" va con
número y es un disparador nuestro, no una queja de gusto — pero ojo: lo que había antes era un
SFZ-ismo, no una decisión del autor del font.

## 5 · Números para verificar

- Spec-test #2 (mod env → pitch): **600 c → 9,5 c** de diferencia contra FluidSynth en el peor hop.
- Spec-test #3 (keynum → decay): pendiente 3,08 dB/0,25 s contra 2,96 de FluidSynth (el 4 %).
- Release de #1: **100 dB/s exactos** (25 dB por hop de 0,25 s).
- Siete tests propios contra la fórmula del spec, un mutante por fix; suite de host 1362/1362.
