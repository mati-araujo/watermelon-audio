---
title: "Aviso a NoisyPad — v2.17.3: la afinación fina y el coarseTune salen del keytrack"
type: reference
status: current
created: 2026-09-14
---

# Aviso a NoisyPad — 2026-09-14 · v2.17.3

**watermelon-audio → NoisyPad.** Sigue al aviso de v2.17.2 del mismo día (sin respuesta todavía;
no hace falta contestar los dos por separado).

> **Enviado el 2026-09-14. Contestado el 14/09 a la noche** (recibido el 15/09):
> `carta-noisypad-2026-09-15-respuesta-v2.17.2-v2.17.3.md` — el bombo de concierto a la 36 bajó −780 c
> al cent, los toms del Electronic +250/+350/+600/+700, nada fuera de la lista se movió (18 teclas de
> kits + 2 melódicos, 0 c) y ninguno de la lista suena peor con número; I-3 contestada. Cotejado
> contra el archivo al final de esa carta.

## 1 · Lo que cambia

Tercer caso de la misma clase que 2.17.1 y 2.17.2: tsf resolvía un generador del SoundFont a otra
escala que el spec. Esta vez son los **tres offsets de pitch** —`coarseTune`, `fineTune` y la
corrección del sample—, que entraban **adentro** del keytrack (`scaleTuning`): en una zona con
`scaleTuning` 50 se aplicaban a la mitad, con 0 desaparecían. SF2 §8.1.2 y FluidSynth los aplican
**afuera**, como offsets absolutos, y **v2.17.3** hace lo mismo. Medido contra el spec-test (#8:
23 c → 0,00 contra FluidSynth 2.6.0) y contra FluidSynth sobre fonts mínimos (da lo mismo con
scaleTuning 0, 50 y 100).

Lo que van a oír: **cambia el pitch de tambores y efectos**, y de nada más. GeneralUser sólo usa
`scaleTuning ≠ 100` ahí. **Ningún preset melódico del banco 0 se mueve**; el nivel, las curvas de
velocity y la expresión tampoco.

## 2 · En qué presets, medido

La nota completa, por preset y con el Δ en cents, está en
`nota-de-bump-2026-09-14-pitch-fuera-del-keytrack.md`. En una tabla:

| qué | dónde | ejemplos (Δ = pitch nuevo − viejo, cents) |
|---|---|---|
| **las once baterías**, en sus dos bancos | 120 y 128 (22 presets, 404 zonas) | `128:0 Standard`: Wood Block −1594/−1319, otras −350/−75 · `128:25 808/909`: −1594…−75 · `128:56 SFX`: Thunder −1675, otras hasta +330 |
| **percusión y efectos GM** | programas 115–127 de los bancos 0–12 (45 presets, 166 zonas) | `0:115 Wood Block` −1344 · `0:116 Taiko Drum` −12 · `0:117 Melodic Tom` −300…+550 según la tecla · `0:124 Telephone 1` −750 · `0:127 Gun Shot` −700 · `8:116 Concert Bass Drum` **−780/−810** · `8:118 808 Tom` −362 · `12:127 Shooting Star` +2450 |
| no cambian | los 202 presets restantes, todo el banco 0 melódico | pianos, cuerdas, `81 Saw Lead`, `89 Warm Pad`, `56 Trumpet`… |

**Total: 570 zonas en 67 presets.** El caso típico es un tambor con `coarse` negativo que hasta
2.17.2 sonaba **más agudo** de como lo afinó el autor: el bombo de concierto a la tecla 36, más de
una quinta arriba.

## 3 · Nada que hacer de su lado

Es la referencia que pidieron (*el font como lo afina su autor, FluidSynth como referencia*). Si
usan baterías en el grid, un tambor con nombre y tecla alcanza para verificarlo: `8:116` a la tecla
36 tiene que sonar 780 c más grave que en 2.17.2.

## 4 · La pregunta, la misma

**¿Algún preset de baterías o efectos suena distinto de lo que esperan y no está en la lista? ¿Y
alguno de la lista suena PEOR que antes?** Un "el 808 sonaba mejor agudo" va con número y es un
disparador nuestro (medir ese preset contra FluidSynth), no una queja de gusto.
