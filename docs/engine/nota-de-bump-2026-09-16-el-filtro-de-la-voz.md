# Nota de bump para NoisyPad — el low-pass de la voz es el de FluidSynth

**Fecha**: 2026-09-16 · **Cambio**: REQ-041 S1 · **Release**: la siguiente, `fix` ·
**Font medido**: `GeneralUser_GS.sf3` (GeneralUser GS 1.471, el que shipea NoisyPad).

Esta nota dice **qué cambia de nivel y en qué presets**, medido sobre el archivo
(`scripts/read-sf2-modulators.py --filter-q`, en regiones) y contra FluidSynth 2.6.0 sobre el
SoundFont-Spec-Test. Si algo de acá no coincide con lo que se oye en el dispositivo, eso es un
hallazgo y se quiere saber.

## Lo que cambia, en una frase

**Toda nota de todo preset sube +1,5 dB** (+1,505 exactos). Es la corrección del **−1,43 dB
"global, sin dueño"** que las filas #1 y #11 del spec-test tenían anotado desde MINI-026: el
low-pass de cada voz aporta un término de nivel `1/sqrt(q)` (SF2 2.01 p. 59, *"gain reduction
equal to half the height of the resonance peak"*) que tsf no tenía, y con `initialFilterQ = 0`
—o sea en toda voz— ese término vale **+1,505 dB**. Medido sobre los siete tonos de #11 (senoide
sin filtro que la toque): el motor estaba **−1,44 dB** por debajo de FluidSynth; ahora **+0,07**,
y ese número tiene desde hoy su control absoluto en el gate (≤ 0,2 dB).

## Lo que cambia además, donde el font declara resonancia

Las tres convenciones del filtro pasan a ser las de FluidSynth 2.6.0 (`fluid_iir_filter`):

1. **q = 10^((Q − 3,01)/20)** con Q = clip(`initialFilterQ`/10, 0, 96) dB. Q es la altura del pico
   **sobre la respuesta sin resonancia**: con Q = 0 el filtro es un Butterworth (−3,01 dB en el
   corte, sin joroba). tsf usaba 10^(Q/20): **el pico de resonancia estaba 3 dB por encima** a
   cualquier Q, y a Q = 0 quedaba una joroba de +3 dB cerca del corte que el spec dice que no va.
2. **La voz baja −(Q − 3,01)/2 dB** donde hay Q. Toda esta nota mide **respecto de 2.19.0** (que
   no tenía el término): una región con Q = 50 cB queda **−1,0 dB** (o sea 2,5 dB por debajo del
   +1,5 de las regiones sin Q); con 190 cB, **−8,0 dB**; con 960 cB, **−46,5**.
3. **El corte se clampea a [5 Hz, 0,45·sr] y el filtro corre siempre.** tsf lo apagaba si el corte
   pasaba de 0,499·sr: a 22 050 Hz el default (13 500 c = 19 912 Hz) dejaba la voz sin filtro y sin
   el +1,5. A 44,1/48 kHz el default ya estaba activo, así que en el teléfono esto no mueve nada más
   que el clamp; a rates bajos es el anti-alias, como en FluidSynth.

## En qué presets, medido en regiones (no en zonas)

Sobre las **12 311 regiones** de GeneralUser (la región es lo que tsf toca: zona de instrumento ×
zona de preset con rangos solapados; `--generators` cuenta 323 **zonas** con `initialFilterQ`, que
no es lo mismo): **1787 regiones con Q > 0 en 90 de 269 presets** (verificado contra tsf cargando el
mismo archivo: preset por preset, idéntico). Las otras 10 524 regiones, y los 179 presets restantes
enteros, suben el +1,5 parejo.

Por el Q máximo de cada preset (cuánto baja **su región de más Q** respecto de 2.19.0):

| Q máximo del preset | presets | nivel vs 2.19.0 en esa región |
|---|---|---|
| ≤ 50 cB | 26 | entre +1,5 y −1,0 dB (hasta 2,5 dB por debajo del +1,5 de las regiones sin Q) |
| 51 – 100 cB | 14 | −1,0 a −3,5 dB |
| 101 – 200 cB | 49 | −3,5 a −8,0 dB — son los **kits de percusión** de bank 120 (190 cB en algunos hits) y pads/leads |
| > 200 cB | 1 | `12:127` Shooting Star: 960 cB, **−46,5 dB** en su única región |

La tabla completa, preset por preset, al final.

## Qué tan igual a FluidSynth, con número

Sobre el SoundFont-Spec-Test, contra el render seco de FluidSynth 2.6.0:

| prueba | qué | antes | ahora |
|---|---|---|---|
| #10 | resonancia: ruido blanco a 4 kHz con Q = 0 … 96 dB | **46,22 dB** en la Q más alta | **0,39** (0,16 hasta Q = 40 dB). Pasa a F |
| #11 | ganancia global, en absoluto (nuevo control) | **−1,44 dB** | **+0,07** (+0,06 … +0,09 en los siete tonos) |
| #9 | corte: ruido blanco con el corte en 20 Hz … 20 kHz | 2,81 | **1,08**, y lo que queda **no es del filtro**: es la interpolación lineal de tsf sobre un sample a 48 kHz tocado a 44,1 (−0,8 dB de contenido a 20 kHz contra los 4 puntos de FluidSynth; un modelo sin el motor da la misma curva). Dueño: S3 de este REQ |
| #1 | envolvente en absoluto | "pico −1,43 (offset global)" | el offset se fue con esto; la fila se re-declara |

Los otros trinquetes que no son de este cambio no se movieron: la línea de base de pitch, el
reparto de moduladores (64 de fuente de canal), las escaleras de velocity #13/#14 (mismo veredicto;
los rangos de #14 A/C, que tienen Q = 10 dB, se movieron 0,07 dB por la forma nueva del pico) y los
golden de DSP.

## Lo que les pedimos

- **Re-tomen sus líneas de base de nivel** antes de comparar cualquier otra cosa: toda toma con
  SoundFont sube +1,5 dB, y las 1787 regiones con Q bajan lo de la tabla. Es la tercera vez que el
  régimen se mueve en una semana (0,4 dB/dB de MINI-024, los sends de REQ-040, y esto): una
  comparación contra 2.19.0 sin re-tomar la base va a leer este +1,5 como cualquier otra cosa.
- Nada que adoptar y nada que apagar: cero superficie nueva (`git diff` vacío sobre
  `watermelon_audio.h`, `IAudioNativeBridge`, `ISoundFontBridge`). El +1,5 no es una perilla:
  es el nivel del font como lo programó su autor y como suena en FluidSynth.
- Si un preset con resonancia (los kits de bank 120, `Electric Grand`, `Space Voice`, los pads)
  les suena **distinto de lo que esperan** —más flojo por el −(Q−3)/2, o con menos "silbido" por el
  pico 3 dB abajo—, eso es exactamente lo que cambió. Si les suena distinto de FluidSynth 2.6.0
  con el mismo font, eso es un hallazgo: **pídanlo con carta**, con el preset y la tecla.

## Tabla por preset (90 con `initialFilterQ` > 0; el resto sube +1,5 parejo)

`regiones` = las de tsf para ese preset; `con Q` = cuántas declaran Q > 0; `Q (cB)` = el rango
entre ellas; la última columna es el nivel de la región de más Q **respecto de 2.19.0**
(`(3,01 − Q/10)/2` dB; las regiones sin Q del mismo preset van a +1,5).

| preset | regiones | con Q | Q (cB) | región de más Q, vs 2.19.0 |
|---|---|---|---|---|
| `0:0` Stereo Grand | 200 | 58 | 10 … 40 | -0.5 dB |
| `0:2` Electric Grand | 164 | 63 | 50 … 150 | -6.0 dB |
| `0:3` Honky-Tonk | 218 | 218 | 20 … 80 | -2.5 dB |
| `0:5` FM Electric Piano | 117 | 57 | 40 … 90 | -3.0 dB |
| `0:10` Music Box | 7 | 7 | 170 | -7.0 dB |
| `0:20` Reed Organ | 24 | 12 | 40 | -0.5 dB |
| `0:23` Bandoneon | 26 | 13 | 80 | -2.5 dB |
| `0:32` Acoustic Bass | 24 | 6 | 70 | -2.0 dB |
| `0:33` Finger Bass | 25 | 12 | 15 … 45 | -0.8 dB |
| `0:34` Pick Bass | 38 | 5 | 105 | -3.8 dB |
| `0:38` Synth Bass 1 | 80 | 80 | 48 | -0.9 dB |
| `0:39` Synth Bass 2 | 5 | 2 | 50 | -1.0 dB |
| `0:40` Violin | 145 | 84 | 130 | -5.0 dB |
| `0:51` Synth Strings 2 | 8 | 4 | 50 | -1.0 dB |
| `0:62` Synth Brass 1 | 16 | 16 | 20 … 30 | +0.0 dB |
| `0:63` Synth Brass 2 | 20 | 10 | 50 | -1.0 dB |
| `0:76` Bottle Blow | 6 | 1 | 180 | -7.5 dB |
| `0:77` Shakuhachi | 4 | 1 | 180 | -7.5 dB |
| `0:80` Square Lead | 64 | 32 | 50 | -1.0 dB |
| `0:83` Chiffer Lead | 6 | 5 | 40 | -0.5 dB |
| `0:84` Charang | 18 | 8 | 80 | -2.5 dB |
| `0:87` Bass & Lead | 60 | 60 | 27 … 30 | +0.0 dB |
| `0:88` Fantasia | 70 | 61 | 40 … 150 | -6.0 dB |
| `0:91` Space Voice | 8 | 7 | 120 | -4.5 dB |
| `0:93` Metal Pad | 14 | 14 | 70 … 75 | -2.2 dB |
| `0:94` Halo Pad | 11 | 6 | 150 | -6.0 dB |
| `0:96` Ice Rain | 11 | 7 | 50 | -1.0 dB |
| `0:97` Soundtrack | 8 | 8 | 110 | -4.0 dB |
| `0:98` Crystal | 4 | 3 | 180 | -7.5 dB |
| `0:99` Atmosphere | 15 | 15 | 70 … 73 | -2.1 dB |
| `0:101` Goblin | 10 | 10 | 40 … 190 | -8.0 dB |
| `0:103` Star Theme | 13 | 13 | 50 … 70 | -2.0 dB |
| `0:109` Bagpipes | 7 | 3 | 50 | -1.0 dB |
| `0:110` Fiddle | 145 | 84 | 130 | -5.0 dB |
| `0:111` Shenai | 8 | 1 | 31 | -0.1 dB |
| `0:122` Seashore | 2 | 2 | 33 … 45 | -0.8 dB |
| `1:98` Synth Mallet | 4 | 3 | 180 | -7.5 dB |
| `2:127` Lasergun | 3 | 2 | 180 | -7.5 dB |
| `3:122` Howling Winds | 2 | 2 | 79 … 190 | -8.0 dB |
| `8:5` Chorused FM EP | 174 | 114 | 40 … 90 | -3.0 dB |
| `8:25` 12-String Guitar | 135 | 2 | 30 | +0.0 dB |
| `8:26` Hawaiian Guitar | 1 | 1 | 78 | -2.4 dB |
| `8:30` Feedback Guitar | 25 | 4 | 180 | -7.5 dB |
| `8:31` Guitar Feedback | 2 | 2 | 180 | -7.5 dB |
| `8:38` Synth Bass 3 | 8 | 8 | 120 | -4.5 dB |
| `8:39` Synth Bass 4 | 11 | 2 | 50 | -1.0 dB |
| `8:63` Synth Brass 4 | 2 | 2 | 13 | +0.8 dB |
| `8:81` Doctor Solo | 9 | 8 | 42 | -0.6 dB |
| `8:125` Starship | 2 | 1 | 128 | -4.9 dB |
| `9:125` Burst Noise | 2 | 1 | 180 | -7.5 dB |
| `11:0` Piano & Str.-Fade | 320 | 52 | 10 … 40 | -0.5 dB |
| `11:1` Piano & Str.-Sus | 320 | 52 | 10 … 40 | -0.5 dB |
| `11:4` Tine & FM EPs | 153 | 57 | 40 … 90 | -3.0 dB |
| `11:5` Piano & FM EP | 293 | 109 | 10 … 90 | -3.0 dB |
| `11:78` Whistlin' | 3 | 1 | 190 | -8.0 dB |
| `11:81` Sawtooth Stab | 48 | 24 | 52 | -1.1 dB |
| `11:87` Doctor's Solo | 9 | 8 | 42 | -0.6 dB |
| `11:88` Harpsi Pad | 11 | 5 | 40 | -0.5 dB |
| `11:89` Solar Wind | 3 | 2 | 142 … 190 | -8.0 dB |
| `11:96` Mystery Pad | 1 | 1 | 142 | -5.6 dB |
| `11:98` Synth Chime | 3 | 2 | 190 | -8.0 dB |
| `11:119` Cymbal Crash | 14 | 4 | 10 … 40 | -0.5 dB |
| `12:0` Bell Piano | 236 | 52 | 10 … 40 | -0.5 dB |
| `12:38` Mean Saw Bass | 24 | 16 | 50 | -1.0 dB |
| `12:81` Saw Lead 2 | 48 | 24 | 52 | -1.1 dB |
| `12:89` Solar Wind 2 | 3 | 2 | 142 … 190 | -8.0 dB |
| `12:127` Shooting Star | 1 | 1 | 960 | -46.5 dB |
| `16:25` Mandolin | 10 | 5 | 119 | -4.5 dB |
| `120:0` Standard Drums | 172 | 11 | 10 … 190 | -8.0 dB |
| `120:1` Standard 2 Drums | 168 | 11 | 10 … 190 | -8.0 dB |
| `120:8` Room Drums | 209 | 12 | 10 … 190 | -8.0 dB |
| `120:16` Power Drums | 193 | 11 | 10 … 190 | -8.0 dB |
| `120:24` Electronic Drums | 137 | 10 | 10 … 190 | -8.0 dB |
| `120:25` 808/909 Drums | 98 | 5 | 37 … 190 | -8.0 dB |
| `120:26` Dance Drums | 150 | 12 | 10 … 190 | -8.0 dB |
| `120:32` Jazz Drums | 180 | 17 | 10 … 190 | -8.0 dB |
| `120:40` Brush Drums | 186 | 13 | 10 … 190 | -8.0 dB |
| `120:48` Orchestral Perc. | 129 | 6 | 10 … 190 | -8.0 dB |
| `120:56` SFX Kit | 73 | 7 | 33 … 190 | -8.0 dB |
| `128:0` Standard | 172 | 11 | 10 … 190 | -8.0 dB |
| `128:1` Standard 2 | 168 | 11 | 10 … 190 | -8.0 dB |
| `128:8` Room | 209 | 12 | 10 … 190 | -8.0 dB |
| `128:16` Power | 193 | 11 | 10 … 190 | -8.0 dB |
| `128:24` Electronic | 137 | 10 | 10 … 190 | -8.0 dB |
| `128:25` 808/909 | 98 | 5 | 37 … 190 | -8.0 dB |
| `128:26` Dance | 150 | 12 | 10 … 190 | -8.0 dB |
| `128:32` Jazz | 180 | 17 | 10 … 190 | -8.0 dB |
| `128:40` Brush | 186 | 13 | 10 … 190 | -8.0 dB |
| `128:48` Orchestral | 129 | 6 | 10 … 190 | -8.0 dB |
| `128:56` SFX | 73 | 7 | 33 … 190 | -8.0 dB |

Salida completa: `python3 scripts/read-sf2-modulators.py <GeneralUser_GS.sf3> --filter-q`.
