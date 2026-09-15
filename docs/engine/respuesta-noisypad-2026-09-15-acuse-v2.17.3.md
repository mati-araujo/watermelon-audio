---
title: "Respuesta a NoisyPad — acuse de su carta de v2.17.2/v2.17.3: tres matices y un pedido"
type: reference
status: current
created: 2026-09-15
---

# Respuesta a NoisyPad — 2026-09-15 · acuse de v2.17.2 y v2.17.3

**watermelon-audio → NoisyPad.** Contesta su carta del 14/09 a la noche
(`carta-noisypad-2026-09-15-respuesta-v2.17.2-v2.17.3.md`). No avisa ninguna versión nueva: la
última es **v2.17.4** (las envolventes, aviso del 15/09), que su carta no alcanzó a ver y es la que
**más se oye** de la serie — cuando la suban, ese aviso trae su propio tambor con nombre y tecla.

> **Redactada el 2026-09-15. NO se envía por separado: absorbida en el envío de v2.18.0**
> (`respuesta-noisypad-2026-09-15-v2.18.0.md`, §0), que además contesta la carta de 2.17.4 que llegó
> después de redactar esto. Queda como el cotejo largo de los tres matices.

## 1 · Lo que cierra, y cómo lo cotejamos

Cada número de su carta se cotejó contra el archivo (`read-sf2-modulators.py --pitch` y `--presets`
sobre GeneralUser 1.471, el `.sf3` que shippean) y contra la suite de host. Todo lo que midieron
cierra con lo que el archivo declara:

- `8:116` a la 36: **−780** es la capa `Taiko Drum` (root 60, scale 40, coarse −13). Los toms del
  `Electronic`: `Synth Drum` con coarse 5/7/12/14 y scale 50 = **+250/+350/+600/+700**. El
  `808 Tom`: **−361,5** (coarse −7, fine −23, scale 50). Las 18 teclas de kits y los 2 melódicos
  que dieron 0 c no tienen offset con `scaleTuning ≠ 100`: 0 es lo que corresponde.
- Los **−15,1 dB** entre sus dos golpes de Saw Lead son la curva de 800 cB cóncava del archivo a
  MIDI 43 contra 122, calculada con `fluid_concave`: **15,10**. El +26 % de centroide a
  velocity ≈ 0,35 es nuestro +29 % a velocity 38, más cerca del piso. Y el snare de `Standard`
  1246 → 1895 Hz a velocity ≈ 0,62 es el default #2 desapareciendo en las zonas que lo borran
  (−907 c de corte a MIDI 79): MINI-028 alcanzando a los kits, como dicen.
- Cero superficie nueva: confirmado sobre `git diff v2.17.1..v2.17.3 -- '*.kt'`. Y de 2.17.3 a
  **2.17.4 tampoco cambia ningún `.kt`**: el bump siguiente no tiene nada que adoptar.

Es la segunda vez seguida que una nota de bump se confirma en el dispositivo preset por preset, y
la primera con la pregunta I-3 contestada con número en las dos direcciones (fuera de la lista y
"peor"). Gracias por medir lo que no cambiaba: es lo que vale.

## 2 · Tres matices que el archivo agrega a su carta

1. **El bombo de concierto es dos capas, no un sample.** `Concert Bass Drum 2` apila `Taiko Drum`
   (−780, todas las velocities, sin atenuación) con `Timpani Hard` (**−810**, en cuatro capas de
   velocity: 0–75 / 76–93 / 94–110 / 111–127, con 150 / 100 / 50 / 0 cB). A su velocity ≈ 0,62
   suena la capa 76–93, 4 dB abajo del Taiko. El −780 exacto por correlación es el Taiko
   dominando; los −775 / −745 de dos teclas son lo que una correlación sobre una mezcla de dos
   desplazamientos puede dar, y no un residuo del motor (en el host las dos voces resuelven
   3740 y 3690 c sobre el mismo `.sf3`).
2. **Su keytrack "ya estaba bien" viene un 2 % corto, y es consistente.** Los diez valores
   respecto de la 36 (78/235/392/353/471/550/275/511/589/118 contra 80/240/…/120) dan un cociente
   de 0,975–0,983, **media 0,981**: 39,2 c por tecla donde el archivo declara 40. Diez de diez en la
   misma dirección no es ruido. No es el motor —en el host el keytrack mide exacto al cent contra
   una senoide con scale 50, y la fórmula se cotejó con FluidSynth 2.6.0 a 0,00 c— y está fuera de
   lo que 2.17.3 cambió; lo anotamos porque un 2 % sistemático en el estimador es lo que después
   disfraza de "±20 c" a un residuo real. Nuestra hipótesis: la correlación del espectro entero
   sobre un tambor de dos capas con un `initialFilterFc` que no sigue a la tecla sesga el
   corrimiento hacia abajo. **Se cierra con dos WAV**: la 36 y la 51 del bombo en la build de
   2.17.1 — de los 14 que ofrecen, esos dos sí sirven; el resto no hace falta.
3. **El Wood Block va al revés.** "Teclas 76/77, −1594/−1319" es **76 → −1319** y **77 → −1594**
   (`Wood Block_1`), y cada tecla apila además un `Wood Block_2` a −75 / −350. Nuestro aviso lo
   escribió sin teclas, así que el orden no salió de ahí. Sin consecuencia: no lo pueden alcanzar
   desde el Drum Grid, y eso queda de su lado.

## 3 · Un dato de su doc del bump que sí nos toca

Su arnés da *"un golpe de sacrificio (el motor recorta el primero ~0,1 s)"* al grabar en el looper.
Si eso es reproducible con número —cuánto se recorta, y si se cuenta desde `Record` o desde el
primer note-on—, **es un disparador nuestro**, no un detalle del arnés: un looper que pierde los
primeros 100 ms de la primera nota es un defecto con nombre. Mándenlo como mandaron el resto:
build, gesto, y el largo de lo que falta.

## 4 · Nada más pendiente

- De ustedes: la respuesta al aviso de **v2.17.4** cuando suban el bump (trae su propio tambor con
  nombre: hasta 2.17.3 las envolventes recorrían 80 dB donde el spec dice 100, y el mod env de
  filtro era un SFZ-ismo: audible en notas fuertes de los 176 presets con envolvente de
  filtro, sutil en casi todos los demás).
- De nosotros: nada. Los JSON por golpe no hacen falta; los dos WAV del bombo, sí.
