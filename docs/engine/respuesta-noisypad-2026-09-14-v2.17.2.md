---
title: "Aviso a NoisyPad — v2.17.2: el borrado del default #2 con identidad SF 2.01 se honra"
type: reference
status: current
created: 2026-09-14
---

# Aviso a NoisyPad — 2026-09-14 · v2.17.2

**watermelon-audio → NoisyPad.** Sigue a su respuesta del 14/09 (`carta-noisypad-2026-09-14-respuesta-v2.17.1.md`).

> **Enviado el 2026-09-14.** Respuesta pendiente.

Corto, porque es lo que pidieron en su §3 y ya lo tienen medido.

## 1 · Lo que cambia

Su posición —*el font como lo afina su autor, FluidSynth como referencia*— entró tal cual en
MINI-028, y **v2.17.2** lo cierra: el default #2 del spec (velocity → corte del filtro, −2400 cents)
que GeneralUser 1.471 **borra** en 1422 zonas con la identidad del spec 2.01, y que el motor mantenía
vivo porque sólo reconocía la 2.04, **ya no se aplica donde el archivo lo borra**. Un font que calla
lo sigue recibiendo (el spec-test #14 A/C siguen el spec 2.04); un font que lo borra con cualquiera
de las dos identidades no.

Lo que van a oír: **más brillo a baja velocity**, en el grid con velocity del golpe. En el XY
(velocity 1,0 + expresión) nada cambia: a velocity 127 ese default valía 0. El nivel no se mueve
(`Saw Lead` sigue en −17,05 dB a velocity 38 respecto de 124), ni la expresión por toque ni la global.

## 2 · En qué presets, medido

La nota de bump completa, por preset, está en `nota-de-bump-2026-09-14-default-2-identidad-2-01.md`.
En una tabla:

| qué | presets | ejemplos |
|---|---|---|
| **el timbre deja de moverse con la velocity** (el archivo no declara curva propia) | **140** de 269 · 66 del banco 0 | `6 Harpsichord`, `12 Marimba`, `38 Synth Bass 1`, `80 Square Lead`, `87 Bass & Lead`, `5 FM Electric Piano`, `30 Distortion Guitar`, `32 Acoustic Bass`, `46 Orchestral Harp`, `68 Oboe`, `116 Taiko` — el mismo brillo suave o fuerte, como en 2.16.4 y como en FluidSynth |
| **queda sólo la curva del archivo**, 2400 cents menos de cierre | **46** | `81 Saw Lead` (−4400 → **−2000**), `62 Synth Brass 1` y `73 Flute` (−6000 → −3600), `65 Alto Sax`, `50 Synth Strings 1` |
| **mezcla por región** | **83** | los tres pianos (`0 Stereo Grand` −6200/−2400 → −3800/0: las capas fuertes conservan su curva), `40 Violin`, `110 Fiddle`, `60 French Horns`, `42 Cello`, `20 Reed Organ` |
| no cambian (declaran su curva con identidad 2.04) | 0 | `89 Warm Pad`, `44/48/49 Stereo Strings`, `56 Trumpet`, `57 Trombone` |

Su número: `Saw Lead` tecla 72, velocity 124 → 38, centroide **1640 → 1195 Hz** hasta 2.17.1;
**1638 → 1546** en 2.17.2 — muestra a muestra igual a lo que el archivo declara. Su 1123 → 880 debería
quedar en ~1120 → 1050 a la tecla que midieron.

## 3 · Nada que hacer de su lado

No hay compensación posible ni deseable: es la referencia que pidieron. Si en el grid una nota suave
suena ahora **demasiado brillante** en un preset con nombre, eso no es una queja de gusto para
nosotros: es el disparador del falsador de MINI-028 (medir ese preset contra FluidSynth), y va con
número.

## 4 · La pregunta, la misma

**¿Algún preset suena distinto de lo que esperan —nivel, afinación, envolvente o brillo— y no está
en esta lista?** Sus WAV de `Warm Pad` siguen siendo bienvenidos (su −8,4 / −16,0 contra nuestro
−7,09 / −17,12 en 2.17.1, sin cambio en 2.17.2).
