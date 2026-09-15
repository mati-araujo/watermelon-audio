---
title: "Respuesta a NoisyPad — acuse de su carta de v2.18.0: las dos preguntas, y la perilla va"
type: reference
status: current
created: 2026-09-15
---

# Respuesta a NoisyPad — 2026-09-15 · acuse de v2.18.0

**watermelon-audio → NoisyPad.** Contesta su carta de la noche
(`carta-noisypad-2026-09-15-respuesta-v2.18.0.md`). No avisa versión nueva: la última es **2.18.0**.

> **Enviada el 2026-09-15.** Respuesta pendiente (los WAV del bombo, el número del looper).

## 1 · Sus dos preguntas, medidas — ninguna es un hallazgo

Las dos se separan con **tres renders en el host** sobre el mismo `.sf3`: 2.17.4, 2.18.0 **seco** (los
sends en 0, que es lo que la perilla les va a dar) y 2.18.0 con sends. Tecla 72, v 44 y 122.

**Pregunta 1 — el +2,5 dB del Grand suave.** Las dos mitades que proponían pesan así: el **wet suma
+0,6 dB en las dos velocities** (el send es proporcional a la voz, así que no pesa más contra una nota
chica), y el resto, **+1,7 dB, es el dry** — sólo en la suave. Es el corte modulado: la zona 0–49 del
Grand declara `initialFilterFc` 2660 Hz con **Q 2 dB** y velocity → Fc −3800 c; a v 44 el corte baja a
**634 Hz**, justo encima de la fundamental de la 72 (523 Hz), y la resonancia queda montada sobre la
fundamental, que domina el RMS. El centroide cae 667 → 535 en el host (ustedes 658 → 531). A v 122 la
zona es otra (Fc 798, Q 0) y no se mueve nada. Grand no tiene chorus.

Lo que la referencia sí va a mostrar distinto, y tiene dueño: FluidSynth 2.6.0 define el Q como la
altura sobre la respuesta **sin** resonancia (`q_lin = 10^((Q−3,01)/20)`, `fluid_iir_filter.c:62-90`);
tsf usa `10^(Q/20)`: **3 dB más de resonancia a cualquier Q**. Con Q = 2 dB, FluidSynth subiría la nota
suave ~+0,6 donde nosotros +1,7. Es la fila #10 del spec-test (*Filter resonance*) y ya es de
**REQ-041** (el low-pass y la interpolación); su Grand suave es la primera evidencia con preset y número.

**Pregunta 2 — el fuerte de Trumpet "10 → 17–20 ms".** No se corrió. `0:56 Trumpet` **no tiene chorus**
(reverb 7 %), así que la copia retrasada no existe en ese preset, y sus moduladores a v 122 pesan 3,9 %
(`attackVolEnv` ×1,07, como decía la nota). En el host el dry del fuerte es **idéntico entre 2.17.4 y
2.18.0 al 0,2 dB** en los seis instantes; con sends, las seis columnas bajan **−0,8 dB parejo**: el
régimen de 0,3–0,4 s ya lleva el wet (+0,7 dB) y los primeros 25 ms no (el comb más corto tarda 1116
muestras). En su tabla es lo mismo: −0,6 / −1,8 / −0,5 / −1,1 / −0,8 / −0,9 debajo, un desplazamiento
constante. El "17–20 ms" es leer el cruce de −1 dB sobre una curva corrida 0,8 dB. La referencia con
efectos hace exactamente lo mismo, porque su régimen también lleva wet.

**Regla para su arnés, hasta la perilla**: el régimen de una medición de ataque se toma **antes de los
25 ms**, o sobre el render seco.

**Wood Block, cerrado**: 825 → 618 Hz entre la 76 y la 77 son **−500 c**, y es exactamente lo que el
archivo declara para la misma capa (`Wood Block_1`, root 60, scale 50: −1319 y −1594 sobre un keytrack
de +800 / +850). Antes de 2.17.3 la 77 sonaba +50 c *arriba* de la 76. Gracias por alcanzarlo.

## 2 · La perilla: va

Aceptada con sus tres razones. Es **REQ-042**, abierto hoy, una etapa. Lo que van a recibir, salvo que
la amplificación mueva algo:

- `wma_sf_set_ambience(engine, reverb, chorus)` y su getter; en Kotlin `ISoundFontBridge.sfSetAmbience` /
  `sfGetAmbience`; iOS por cinterop. **Por instancia, sin CC.**
- Rango **0..1 lineal sobre la amplitud del send** (0,5 = −6 dB de wet), saturado; **default 1/1**
  (= FluidSynth = 2.18.0). Escala **todos** los sends —generador y default #8/#9 con CC91/CC93 en reset—
  **antes** de las unidades. Con 0/0 el render es, muestra a muestra, el seco del arnés de conformidad.
- Es un ajuste del **instrumento**: sobrevive a `reset()`, al cambio y a la descarga de font (la cola ya
  era del cuarto; la perilla también). Cambiarlo en vuelo no aloca ni clickea (se mide el escalón antes
  de decidir si lleva rampa).
- Lo que **no** trae: parámetros de las unidades (room, damp, width, level, voces del chorus siguen en
  los defaults de FluidSynth), seguir CC91/CC93 en vuelo, ni apagar el DSP de las unidades en 0 (la
  compuerta de 2 s ya las duerme).

Sale como **minor** con su nota de bump y aviso. Para el A/B que quieren, el arnés lo pone en 0/0 y
compara contra 2.17.4 directamente: el seco de 2.18.0 con la perilla en 0 tiene que dar byte a byte lo
que daría 2.18.0 sin sends, y sólo difiere de 2.17.4 en lo que MINI-027 cambió.

## 3 · Pendiente

- De ustedes: los dos WAV del bombo (36 y 51 en 2.17.1) y el número del looper. Su hipótesis (*el
  `loopStart` cae después del primer note-on*; Warm Pad entra a −46 dB y llega al pico 0,5 s después)
  es medible con Record y note-on con marca de tiempo; si se confirma, es un MINI nuestro con nombre.
- De nosotros: REQ-042 y su release.
