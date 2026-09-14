# Nota de bump para NoisyPad — la atenuación declarada entra como fue programada

**Fecha**: 2026-09-12 · **Cambio**: MINI-024 · **Release**: la siguiente `patch` desde `v2.17.0`
(es un `fix`) · **Font medido**: `GeneralUser_GS.sf3` (GeneralUser GS 1.471, el que shipea NoisyPad).

Esta nota dice **qué cambia de sonido y en qué presets**, medido sobre el archivo con
`scripts/read-sf2-modulators.py --attenuation` — no sobre la intuición. Si algo de acá no
coincide con lo que se oye en el dispositivo, eso es un hallazgo y se quiere saber.

## Lo que cambia, en una frase

Hasta `v2.17.0` el renderizador aplicaba el generador `initialAttenuation` del SoundFont a **0,1 dB
por dB declarado**. El spec-quirk que el SoundFont-Spec-Test pide emular *"for compatibility with
existing SoundFonts"* —y que FluidSynth y el hardware aplican— es **0,4**. Desde este cambio, una
zona programada 10 dB abajo suena **4 dB** abajo, no 1. Es un factor **cuatro veces** mayor sobre
todo lo que el font declara atenuado.

Lo que **no** cambia: los moduladores a `initialAttenuation` (la curva de velocity del default #1 y
las del archivo, los de 2.17.0) entran en dB **sin** el factor, como el spec manda y como estaban.
Está medido: la escalera de velocity de 127 → 111 sigue en 2,34 dB, exactamente el spec.

## En qué presets, medido

Sobre los **269 presets** de GeneralUser: **235 declaran** `initialAttenuation` en alguna zona. El
extra de atenuación en la zona más atenuada de cada uno, al pasar de 0,1 a 0,4 dB/dB:

| extra de atenuación | presets |
|---|---|
| menos de 1 dB | 11 |
| 1 a 3 dB | 35 |
| 3 a 6 dB | 72 |
| 6 a 12 dB | 73 |
| **12 dB o más** | **44** |

Del banco 0 (los 128 melódicos), **113** están afectados. Y hay **dos efectos distintos**, que
conviene distinguir al escuchar:

### 1 · Presets que bajan enteros

Cuando **todas** las zonas del preset declaran atenuación, el preset completo baja al menos lo que
declara su zona menos atenuada. Los 32 del banco 0 con ≥ 6 dB, los más grandes:

| preset | baja al menos |
|---|---|
| `96 Ice Rain` | **−16,5 dB** |
| `123 Birds` | −12,0 |
| `81 Saw Lead` | −11,1 |
| `80 Square Lead` | −10,0 |
| `6 Harpsichord` | −7,5 |
| `38 Synth Bass 1` · `51 Synth Strings 2` | −6,9 |
| `62 Synth Brass 1` | −6,3 |
| `16 Tonewheel Organ` | −6,0 |
| `99 Atmosphere` · `18 Rock Organ` | −5,0 |
| `4 Tine Electric Piano` · `102 Echo Drops` | −4,5 |

Es lo que su autor programó: esos presets eran **más fuertes de lo que debían** respecto del resto
del font, y el balance **entre** presets estaba mal.

### 2 · Presets que cambian por dentro

Cuando sólo **algunas** zonas declaran atenuación —segundas capas, samples de relleno, crossfades
por velocity— lo que cambia es el balance **adentro** del preset: una capa programada 20 dB abajo
estaba 5 dB abajo, y ahora está donde va. Los 12 del banco 0 con ≥ 12 dB en su zona más atenuada:

`42 Cello` (93 de 129 zonas, hasta +22 dB), `30 Distortion Guitar` (+21), `76 Bottle Blow` (+21),
`5 FM Electric Piano` (193 de 193, +21), `110 Fiddle` (+20), `40 Violin` (+20), `96 Ice Rain` (+19),
`77 Shakuhachi` (+17), `101 Goblin` (+17), `47 Timpani` (+15), `4 Tine Electric Piano` (+12),
`123 Birds` (+12).

🔴 **Éstos son los que más van a sorprender**, porque no suenan "más bajos": suenan **distintos**.
`Cello` y `Violin` pierden una capa que antes se oía a la par; `FM Electric Piano` cambia de timbre
con la velocity de otra manera. No es un defecto nuevo: es el preset como fue programado, y hasta
hoy no se había oído así en este motor.

Fuera del banco 0, en la misma clase: `8:5 Chorused FM EP` (+21,6, 326/326 zonas), `12:0 Bell Piano`
(948 de 1020 zonas, +20,7), `11:5 Piano & FM EP` (1081/1153, +20,7), `12:4 Bell Tine EP`, `11:4 Tine
& FM EPs`, `13:88 Night Vision` (+21,3), `8:30 Feedback Guitar` (+21,0).

## Lo que esto significa para NoisyPad

- **El nivel global de la app baja** en la mayoría de los presets, entre 1 y 16 dB según cuál. Si
  hay una ganancia de salida calibrada "a oído" contra `v2.17.0`, va a quedar baja. La referencia
  que **no** cambia son los 34 presets sin atenuación declarada — en el banco 0: `56 Trumpet`,
  `57 Trombone`, `68 Oboe`, `26 Jazz Guitar`, `27 Clean Guitar`, `28 Muted Guitar`, `29 Overdrive
  Guitar`, `10 Music Box`, `50 Synth Strings 1`, `83 Chiffer Lead`, `97 Soundtrack`, `118 Synth
  Drum`, `119 Reverse Cymbal`, `121 Breath Noise`, `127 Gun Shot`. Los pianos **no** están ahí:
  `0:0 Stereo Grand` tiene 984 de 1056 zonas atenuadas (hasta +9,6 dB en sus capas) y `0:1 Bright
  Grand` +3,0 — cambian por dentro, como los del efecto 2.
- **No compensen por preset a mano.** Medimos su árbol el 12/09 y no hay tablas de ganancia por
  preset de SoundFont: bien. Si aparece una, es corregir dos veces lo que el font ya corrigió.
- **Tope**: el máximo declarable pasa de 14,4 dB a **57,6** (1440 cB). Ninguna zona de GeneralUser
  supera **720 cB** (28,8 dB reales, medido); no hay preset que quede en silencio.
- El control de nivel por toque (`sfSetTouchExpression`) y la expresión global no cambian: viven
  aguas abajo de esto.

## Lo que NO cambia todavía (medido, con dueño)

- **La afinación fina se pierde con `scaleTuning ≠ 100`** (MINI-025): hasta −30 c en 31 zonas de 14
  instrumentos, todos de percusión o efectos. Como sonaba.
- **Envolventes y LFO** de tsf contra el spec (MINI-026). Como sonaban.
- **30 moduladores** de velocity hacia envolventes/Q/offsets (MINI-027). Como sonaban.
- Sends de reverb/chorus (REQ-040) y superficie MIDI CC: fuera de alcance por diseño.

## Cómo se midió

- `scripts/read-sf2-modulators.py GeneralUser_GS.sf3 --attenuation`: por preset, zonas con
  atenuación, rango en cB (regla SF2 §8.5: zona de instrumento absoluta + zona de preset sumada,
  globales incluidas) y el extra en dB en la zona más atenuada. Imprime la tabla entera.
- Contra terceros: la prueba #11 del SoundFont-Spec-Test (`SfSpecConformance.TheTwentyTwoAgainstTheirOracles`,
  fila S): siete tonos con 0/5/…/30 dB declarados dan pasos de **−2,00 dB**, como FluidSynth 2.6.0
  y como el README (*"exactly 2 dB"*). Antes el motor daba −0,50.
- Sobre un font propio (`InitialAttenuationGenerator`): 100 cB bajan 4,0 dB y son **sólo** una
  ganancia — muestra a muestra la misma onda escalada; 1440 cB son 57,6 y más se clampea.

## Una pregunta, la misma que en 2.17.0

**¿Algún preset suena distinto de lo que esperan —nivel, afinación o envolvente— y no está en
esta lista?** Es lo único que convierte el silencio en respuesta, y es el instrumento con que
vigilamos la clase entera de este cambio (tsf resolviendo un generador a otra escala que el spec).
