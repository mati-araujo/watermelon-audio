---
title: "Acuse a NoisyPad — la tabla de 2.19.1 (I-2): su lectura es la correcta, y lo que encontró de paso"
type: reference
status: current
created: 2026-09-16
---

# Acuse a NoisyPad — 2026-09-16 · la tabla de v2.19.1 (I-2 de REQ-041)

**watermelon-audio → NoisyPad.** Contesta su carta de esta noche (*"v2.19.1 adoptada y medida (I-2): el
término llega; la tabla no es el nivel"*, versionada en `carta-noisypad-2026-09-16-respuesta-v2.19.1.md`
con nuestra verificación al pie).

> **Redactado el 2026-09-16 a la noche. SIN ENVIAR**: lo envía el humano por chat y se marca acá. NoisyPad
> pidió las tres respuestas **por separado**: WV-3 en `respuesta-noisypad-2026-09-16-wv3-la-voz.md` y
> WL-4.1 en `respuesta-noisypad-2026-09-16-wl41-el-stretch.md`.

## 1 · Su lectura es la correcta: la tabla de la nota era el término, no el nivel

Tenían razón en las dos mitades. Rendimos las nueve sondas en host con el mismo arnés sobre el tag
`v2.19.0` y sobre 2.19.1 (0/0, su ventana: RMS 0,1–0,4 s tras el note-on), y el host da lo que su device
midió, **también en las cuatro que la fórmula no predecía**:

| sonda | fórmula | **host rendido** | **su device** | device − host |
|---|---|---|---|---|
| Saw Lead 60·127 | +1,5 | +1,50 | +1,3 | −0,2 |
| Strings 60·127 | +1,5 | +1,50 | +1,7 | +0,2 |
| Trumpet 72·122 | +1,5 | +1,46 | +1,4 | −0,1 |
| Grand 72·122 | +1,5 | +1,48 | +1,3 | −0,2 |
| Music Box 60·127 (Q 170) | −7,0 | −6,99 | −7,4 | −0,4 |
| Warm Pad 60·127 | +1,5 | **−0,00** | −0,1 | −0,1 |
| Trumpet 72·42 | +1,5 | **+0,29** | +0,1 | −0,2 |
| Grand 72·42 (Q 20) | +0,5 | **−0,96** | −1,3 | −0,3 |

Máximo 0,4 dB, en la de Q alto. **I-2 evaluado: vivo, N = 8.** La nota de bump ya dice lo que pidieron: la
columna "vs 2.19.0" es el término 1/√q por región, vale en la banda de paso, y donde el espectro vive sobre
el corte 2.19.0 tenía además la joroba de tsf a Q = 0 (0 dB en fc y +1,25 de pico, contra −3,01 del
Butterworth), que 2.19.1 no tiene. El Δ_host del criterio pasa a ser **el render**, no la tabla.

## 2 · Warm Pad: 2.19.1 **es** FluidSynth

Su pregunta, contestada con el oráculo: `0:89` k60 v127 seco, FluidSynth 2.6.0 `-R 0 -C 0` sobre un `.sf2`
mínimo con el sample real y la zona exacta (nuestro motor rinde el mínimo y el `.sf3` al 0,01 dB), nota de
3 s porque ese preset tiene 0,8 s de ataque:

| | 0,1–0,4 s | 1,5–2,5 s (hold) | centroide |
|---|---|---|---|
| 2.19.0 | −24,80 | −17,05 | 475 Hz |
| 2.19.1 | −24,81 | **−17,20** | 450 Hz |
| FluidSynth | −24,86 | **−17,20** | 449 Hz |

FluidSynth **no** sube +1,5 respecto de 2.19.0: da −0,15, lo mismo que 2.19.1. Su −0,1 es el comportamiento
correcto. Trumpet 72·42 a 0,1 dB de FluidSynth, Saw Lead a 0,05. (Trampa para su lado, si alguna vez rinden
contra FluidSynth: sin `CC7` en el `.mid` arranca en 100 y su modulador #3 resta 4,15 dB; nosotros
arrancamos en 127.)

## 3 · Shooting Star no se compara con 2.19.0

En host **2.19.0 rinde el régimen a +10,6 dBFS**: el resonador de 96 dB sobre ruido blanco satura. En su
device el "antes" no pudo pasar de 0 dBFS (su absoluto de 2.19.1 −43,8 más el Δ +38,0 da un "antes" de
−5,8, que es una toma recortada). Con 2.19.1 como "antes" la sonda sirve; contra 2.19.0 no mide el filtro.

## 4 · Lo que su tabla sí encontró, y no es de REQ-041: el Grand suave está 1,7 dB abajo de FluidSynth

El −1,3 del Grand 72·42 coincide con nuestro host (−0,96), pero **el host no coincide con FluidSynth** en esa
sonda: 2.19.1 −32,93 dBFS contra −31,26, plano en todas las bandas (no es el filtro; es una ganancia). Lo
aislamos mutando el font mínimo: con la atenuación neta del preset en 0 o positiva los dos motores dan lo
mismo al 0,07; con la del font —instrumento 40 + preset **−80 = −40 cB**— FluidSynth sube 1,6 dB y
nosotros no. **tsf recorta en 0 la atenuación del generador antes de sumar los moduladores; FluidSynth
suma los moduladores y recorta después**, así que un neto negativo es un refuerzo que sólo se cobra donde
la atenuación por velocity lo supera: a v42 vale 1,6 dB, a v122 nada (por eso sus sondas fuertes cerraron).

En GeneralUser: **570 regiones en 39 presets** con neto negativo — en las diez baterías (120/128) son **los
crashes (49/57)**: a velocity ≤ 41 neto −120 cB = **4,8 dB** de menos, 42–51 2,8; Xylophone (vel ≤ 51) y
Clavinet (vel ≤ 81) 3,2; la capa suave (vel ≤ 49) de Stereo/Bright Grand y de los Piano & … 1,6…2,4; Brass
Section / Shamisen / Shenai 2,0. Preexistente a 2.19.1 (en 2.19.0 la joroba lo tapaba a medias). Abrimos
**MINI-031**; sale como `fix` con nota de bump por preset y velocity. Donde lo van a oír al adoptarlo: los
crashes tocados suave y las teclas suaves del piano y del xilófono, **más fuertes**; nada en las fuertes.

## 5 · Una línea sobre los absolutos

Sus absolutos de 2.19.1 quedan 3–4,5 dB por debajo de nuestro host en siete sondas, pero Warm Pad −0,6 y
Shooting Star 7,8. No es criterio de nada y puede ser de la toma (pan por tecla, mono vs estéreo); si les
sirve, los del host están en la carta versionada.

