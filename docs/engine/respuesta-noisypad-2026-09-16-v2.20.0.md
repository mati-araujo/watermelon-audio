---
title: "Aviso a NoisyPad — v2.20.0: el low-pass de la voz es el de FluidSynth (toda nota +1,5 dB)"
type: reference
status: current
created: 2026-09-16
---

# Aviso a NoisyPad — 2026-09-16 · v2.20.0

**watermelon-audio → NoisyPad.** Sigue a la respuesta de hoy sobre REQ-040 (cerrado evaluado con su
ventana). Es un **patch** (`fix`) **sin superficie nueva** y **con cambio de nivel en todo preset**:
REQ-041 S1, el filtro de la voz pasa a las convenciones de FluidSynth 2.6.0. La nota de bump entera,
con la tabla de los 90 presets, está en `nota-de-bump-2026-09-16-el-filtro-de-la-voz.md`.

> **Redactado el 2026-09-16, antes del tag**; se envía cuando `v2.20.0` esté verificada en el registro
> (4/4 coordenadas). Su respuesta es **I-2** del criterio de muerte de REQ-041.

## 1 · Lo que cambia, en una frase

**Toda nota de todo preset sube +1,5 dB** (+1,505 exactos). Es la corrección del −1,43 "global, sin
dueño" que las filas #1/#11 del spec-test tenían anotado desde MINI-026: el low-pass de cada voz
aporta un término de nivel 1/√q (SF2 p. 59) que tsf no tenía; con `initialFilterQ = 0` —toda voz—
vale +1,505 dB. Medido: el motor estaba −1,44 dB por debajo de FluidSynth sobre los siete tonos de
#11; ahora +0,07, con control absoluto en el gate (≤ 0,2).

## 2 · Lo que cambia además, donde el font declara resonancia

1. q = 10^((Q − 3,01)/20): Q es la altura del pico **sobre la respuesta sin resonancia** (Q = 0 ⇒
   Butterworth, sin joroba). tsf tenía el pico 3 dB arriba a cualquier Q.
2. La voz baja −(Q − 3,01)/2 dB donde hay Q, **respecto de 2.19.0**: Q = 50 cB ⇒ −1,0 (2,5 por
   debajo del +1,5 de las demás); 190 cB ⇒ −8,0; 960 cB ⇒ −46,5.
3. El corte se clampea a [5 Hz, 0,45·sr] y el filtro corre siempre. A 44,1/48 kHz no mueve nada más
   que el clamp.

**En qué presets** (regiones, verificado contra tsf preset por preset): **1787 regiones con Q > 0 en
90 de 269 presets**; los otros 179 presets suben el +1,5 parejo. Por Q máximo: 26 presets ≤ 50 cB
(+1,5 a −1,0), 14 en 51–100 (−1,0 a −3,5), **49 en 101–200 (−3,5 a −8,0: los kits de bank 120 y
pads/leads)**, 1 > 200 (`12:127` Shooting Star, −46,5).

**Lo que NO cambia**: el corte (el 634 Hz del Grand suave sigue; MINI-027 no se toca), pitch,
envolventes, sends, la perilla (0/0 sigue siendo el seco, ahora +1,5). Spec-test: #10 (resonancia)
**46,22 → 0,39 dB** contra FluidSynth; #9 (corte) 2,81 → 1,08 y el residuo que queda es la
interpolación (S3), no el filtro. Superficie pública sin diff.

## 3 · Lo que les cambia a ustedes

- **Sus líneas de base de nivel se corren +1,5 dB en todo preset sin Q, y hasta −8 en los kits.**
  Re-tomar las cinco sondas con 0/0 en 2.20.0 con la ventana acordada (régimen 0,1–0,4 s tras el
  note-on) ANTES de comparar cualquier otra cosa. Es el mismo caso que el 0,4 dB/dB de MINI-024.
- Las tomas de REQ-040 con 1/1 no se invalidan: el wet escala con la voz.
- El Grand suave 72·42 (Q = 20 cB): esperamos **+0,5 dB neto y el centroide igual** (532 Hz). Si el
  centroide se mueve > 3 %, es hallazgo.

## 4 · Lo que pedimos (I-2 de nuestro criterio de muerte)

Por preset, con **0/0**, 2.19.0 vs 2.20.0, régimen absoluto (dBFS, ventana motor) y su Δ: Saw Lead
60·127, Trumpet 72·42 y 72·122, Strings 60·127, Warm Pad 60·127, Grand 72·122 y 72·42 — más **dos
sondas con Q alto que hoy no tienen**: un hit de un kit de bank 120 (Q 190 cB ⇒ −8,0) y `12:127`
Shooting Star (⇒ −46,5). Nuestro Δ esperado por preset está en la tabla de la nota (columna
"nivel vs 2.19.0"); el umbral es |Δ_device − Δ_host| > 1 dB. Y si algún preset "se apagó" de oído
aunque el número cierre, díganlo: es F-3.

## 5 · Lo que sigue de este lado

S2 de REQ-041: la rampa del corte modulado (hoy los coeficientes saltan por bloque de 64; los 111
presets del corte por velocity de MINI-027 lo pueden mostrar). S3: la interpolación de 4 puntos, sólo
si el costo entra. Cada una con su tag y su nota.
