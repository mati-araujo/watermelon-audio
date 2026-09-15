# Nota de bump para NoisyPad — los ocho destinos de note-on, y el velocity → brillo que estaba inerte

**Fecha**: 2026-09-15 · **Cambio**: MINI-027 (nace de REQ-039 S3, D6) · **Release**: la siguiente,
`feat` + `fix` · **Font medido**: `GeneralUser_GS.sf3` (GeneralUser GS 1.471, el que shipea
NoisyPad).

Esta nota dice **qué cambia de sonido y en qué presets**, medido sobre el archivo (el lector de
`pdta`, no el motor) y sobre las regiones que tsf construye de él (el test
`GeneralUserRegionsWhereTheVelocityToFilterWasInertAreCounted`, que imprime los números y los
declara). Si algo de acá no coincide con lo que se oye en el dispositivo, eso es un hallazgo y se
quiere saber.

## Lo que cambia, en dos frases

1. **El `fix`, que es el que más se oye**: el velocity → corte del filtro que 2.17.0 (REQ-039 S2)
   empezó a aplicar —el default #2 del spec y las curvas propias del archivo— estaba **inerte en
   toda región con envolvente o LFO al filtro**: el render lo pisaba en el primer bloque. Sobre
   GeneralUser son **3380 de 12311 regiones, en 111 presets** (166 zonas de instrumento en 24
   instrumentos). Ahí, desde este cambio, **una nota suave suena más oscura, como lo afinó el
   autor**: Stereo Grand con −3800 c a velocity 0 (−1900 a velocity 64), los bronces, las strings,
   los pads, los kits. A velocity 127 (el XY) no cambia nada: todas las curvas son decrecientes y
   valen 0 ahí.

2. **El `feat`**: los ocho destinos de modulador de fuente de nota que S2 no aplicaba se evalúan
   al disparar: las tres duraciones de la envolvente de volumen, el ataque de la de modulación,
   cuánta envolvente entra al filtro, el Q, el offset de arranque del sample y el paneo. En
   GeneralUser son **24 moduladores efectivos en 13 presets**, y lo que se oye es sobre todo en
   los **bronces**: *toque suave = ataque lento* (`0:56 Trumpet` y `0:59 Muted Trumpet`: +3000 tc
   a velocity 0 ⇒ el ataque dura ×5,7; `0:57 Trombone` +4000 ⇒ ×10; `0:61 Brass Section` hasta
   +14918 con curva cóncava ⇒ ×5500 a velocity 0 pero ×2,9 a velocity 64) y menos envolvente al
   filtro en la nota suave.

## Dónde, medido

### El `fix` — velocity → corte, por primera vez activo en estas regiones (111 presets)

Lo que se lista es el amount del modulador en cents a velocity 0 (lineal: a velocity 64 aplica la
mitad; a 127, cero). `−2400` es el default #2 del spec donde el archivo no lo borra; los demás son
la curva propia del autor. Un preset puede tener regiones de las dos clases y regiones donde nada
cambia (las que no tienen envolvente ni LFO al filtro ya sonaban bien desde 2.17.0/2.17.2).

| banco:programa | preset | velocity → corte (cents a velocity 0) |
|---|---|---|
| `0:0` | Stereo Grand | -3800 |
| `0:1` | Bright Grand | -3800 |
| `0:3` | Honky-Tonk | -3800 |
| `0:4` | Tine Electric Piano | -2400 |
| `0:20` | Reed Organ | -3600 |
| `0:21` | Accordian | -3600 |
| `0:22` | Harmonica | -3600 |
| `0:23` | Bandoneon | -3600 |
| `0:33` | Finger Bass | -3600 |
| `0:35` | Fretless Bass | -3600 |
| `0:37` | Slap Bass 2 | -2400 |
| `0:41` | Viola | -3600 |
| `0:42` | Cello | -2400 |
| `0:43` | Double Bass | -3600 |
| `0:44` | Stereo Strings Trem | -2400 |
| `0:48` | Stereo Strings Fast | -2400 |
| `0:49` | Stereo Strings Slow | -2400 |
| `0:50` | Synth Strings 1 | -2400 |
| `0:51` | Synth Strings 2 | -3600 |
| `0:56` | Trumpet | -2480 |
| `0:57` | Trombone | -3480 |
| `0:58` | Tuba | -3600 |
| `0:59` | Muted Trumpet | -2480 |
| `0:60` | French Horns | -3600 |
| `0:61` | Brass Section | -5300 / -4200 |
| `0:62` | Synth Brass 1 | -3600 |
| `0:63` | Synth Brass 2 | -3600 |
| `0:67` | Baritone Sax | -3600 |
| `0:75` | Pan Flute | -6000 |
| `0:76` | Bottle Blow | -3600 |
| `0:77` | Shakuhachi | -2600 |
| `0:79` | Ocarina | -3600 |
| `0:82` | Synth Calliope | -3600 |
| `0:83` | Chiffer Lead | -3500 / -2600 |
| `0:84` | Charang | -3600 |
| `0:85` | Solo Vox | -8000 |
| `0:88` | Fantasia | -2000 |
| `0:89` | Warm Pad | -2400 |
| `0:90` | Polysynth | -3600 |
| `0:91` | Space Voice | -3600 |
| `0:92` | Bowed Glass | -3600 |
| `0:93` | Metal Pad | -3600 |
| `0:94` | Halo Pad | -2400 |
| `0:95` | Sweep Pad | -3600 |
| `0:96` | Ice Rain | -3600 |
| `0:98` | Crystal | -3600 |
| `0:99` | Atmosphere | -3600 |
| `0:100` | Brightness | -3600 / -3306 |
| `0:102` | Echo Drops | -3600 |
| `0:103` | Star Theme | -3600 |
| `0:105` | Banjo | -6400 |
| `0:107` | Koto | -6400 / -5500 |
| `0:111` | Shenai | -3600 |
| `0:122` | Seashore | -2800 |
| `1:44` | Mono Strings Trem | -2400 |
| `1:48` | Mono Strings Fast | -9400 |
| `1:49` | Mono Strings Slow | -9400 |
| `1:56` | Trumpet 2 | -3600 |
| `1:57` | Trombone 2 | -3000 |
| `1:61` | Brass Section Mono | -4600 |
| `1:98` | Synth Mallet | -2400 |
| `2:122` | Thunder | -7000 |
| `3:122` | Howling Winds | -2400 |
| `7:125` | Jet Plane | -3600 |
| `8:4` | Chorused Tine EP | -2400 |
| `8:28` | Funk Guitar | -6000 / -3600 |
| `8:30` | Feedback Guitar | -3600 |
| `8:31` | Guitar Feedback | -3600 |
| `8:48` | Orchestra Pad | -4600 / -2400 |
| `8:50` | Synth Strings 3 | -2400 |
| `8:61` | Brass Section 2 | -4600 / -3600 |
| `8:81` | Doctor Solo | -3600 |
| `8:107` | Taisho Koto | -5500 / -5400 / -2800 |
| `11:0` | Piano & Str.-Fade | -3800 / -2400 |
| `11:1` | Piano & Str.-Sus | -3800 / -2400 |
| `11:4` | Tine & FM EPs | -2400 |
| `11:5` | Piano & FM EP | -3800 |
| `11:49` | Stereo Strings Velo | -2400 |
| `11:61` | Brass Section 3 | -4600 / -3600 |
| `11:87` | Doctor's Solo | -3600 |
| `11:88` | Harpsi Pad | -3600 / -2204 |
| `11:89` | Solar Wind | -2755 |
| `11:100` | Bright Saw Stack | -3000 |
| `11:119` | Cymbal Crash | -4800 |
| `12:0` | Bell Piano | -3800 |
| `12:4` | Bell Tine EP | -2400 |
| `12:27` | Clean Guitar 2 | -2800 |
| `12:48` | Full Orchestra | -9400 / -7000 / -3600 / -2400 |
| `12:49` | Mono Strings Velo | -9400 |
| `12:80` | Square Lead 2 | -3600 |
| `13:81` | Saw Lead 3 | -3600 |
| `120:0` | Standard Drums | -4800 / -3200 |
| `120:1` | Standard 2 Drums | -4800 / -3600 / -3200 |
| `120:8` | Room Drums | -4800 / -3200 |
| `120:16` | Power Drums | -4800 / -3600 / -3200 |
| `120:24` | Electronic Drums | -4800 / -3200 |
| `120:26` | Dance Drums | -4800 / -3200 |
| `120:32` | Jazz Drums | -13500 / -4800 / -3200 |
| `120:40` | Brush Drums | -6337 / -4800 / -3200 |
| `120:48` | Orchestral Perc. | -4800 / -3200 |
| `120:56` | SFX Kit | -7000 / -3600 / -2800 / -2400 |
| `128:0` | Standard | -4800 / -3200 |
| `128:1` | Standard 2 | -4800 / -3600 / -3200 |
| `128:8` | Room | -4800 / -3200 |
| `128:16` | Power | -4800 / -3600 / -3200 |
| `128:24` | Electronic | -4800 / -3200 |
| `128:26` | Dance | -4800 / -3200 |
| `128:32` | Jazz | -13500 / -4800 / -3200 |
| `128:40` | Brush | -6337 / -4800 / -3200 |
| `128:48` | Orchestral | -4800 / -3200 |
| `128:56` | SFX | -7000 / -3600 / -2800 / -2400 |


Los que **no** están en esta lista no cambian con el `fix`: `0:81 Saw Lead` y `0:80 Square Lead`
(sin filtro dinámico: lo que midieron en 2.17.2 ya era lo correcto), `0:6 Harpsichord`,
`0:12 Marimba`, las guitarras del banco 0, `0:38 Synth Bass 1`, `0:116 Taiko`…

### El `feat` — los 24 moduladores, por preset

| instrumento (ámbito) | destinos | amount | presets | qué se oye |
|---|---|---|---|---|
| `Trumpet 3`, `Trombone 3`, `Muted Trumpet 3` (global) | attackVolEnv · attackModEnv · initialFilterQ · modEnvToFilterFc | 3000/4000/3000 tc · 10000/5000/10000 tc · 8 cB · −2000 c | `0:56 Trumpet`, `0:57 Trombone`, `0:59 Muted Trumpet` | la nota suave ataca lento (×5,7 / ×10 / ×5,7 a velocity 0) y con menos barrido de filtro; el barrido del mod env también entra más lento |
| `Brass Section` (zona de preset, dos capas) | attackVolEnv | 14918 / 10061 tc, cóncava | `0:61 Brass Section` | ataque de la sección hasta segundos a velocity baja |
| — (global de preset) | modEnvToFilterFc | −8000 c | `12:38 Mean Saw Bass` | el barrido de filtro se achica con la velocity |
| `Standard Kick 3` (tecla 36, velocity 99–127) | decayVolEnv · modEnvToFilterFc | −3986 tc · +8000 c cóncava creciente | `120:0`, `128:0 Standard` | el kick FUERTE: decay más corto y filtro que abre |
| `Room Snare 2` (global) | decayVolEnv · releaseVolEnv | −3986 tc | `120:8/16`, `128:8/16` (Room, Power) | el snare suave se apaga más lento que el fuerte |
| — (global de preset) | keynum → pan | 250 | `0:6 Harpsichord`, `8:6 Coupled Harpsichord` | los graves a la izquierda, los agudos a la derecha (±25 %) |
| `Bagpipes` (3 zonas) | startAddrsOffset | 3000 muestras | `0:109 Bagpipes` | la nota suave arranca 3000 muestras adentro del sample (sin el chiff) |

Quedan **6 moduladores declarados y no aplicados**, los seis con amount 0 (`velocity →
startAddrsCoarseOffset` en `Bagpipes`): inertes por construcción; se cuentan, no se implementan.

## Cómo verificarlo del lado de NoisyPad

- **El `fix`**: `0:0 Stereo Grand` en el Note Grid, la misma tecla golpeada suave (velocity ≈ 0,35)
  y fuerte (≈ 0,96): el centroide del golpe suave **baja** respecto de 2.17.4 (a velocity 44 el
  corte cae 3800 × (1 − 44/127) = 2480 c en las regiones con envolvente al filtro); el fuerte y el
  XY no se mueven (± 3 %). Nivel y pitch iguales.
- **El `feat`**: `0:56 Trumpet` en el Note Grid, golpe suave: el ataque se estira (a velocity 44 el
  `attackVolEnv` suma 3000 × 83/127 = 1961 tc ⇒ ×3,1). Se mide como en la carta de 2.17.3: el
  instante en que el nivel llega a −3 dB del pico.
- Lo que **no** cambia: `0:81 Saw Lead`, `0:89 Warm Pad` en el XY, y todo a velocity 1,0.

## La pregunta activa (I-3)

**¿Algún preset suena distinto de lo que esperan y no está en las dos listas? ¿Y alguno de las
listas suena PEOR que antes — una nota suave demasiado oscura, un ataque de bronce demasiado
lento?** Va con nombre, tecla y velocity, y es un disparador nuestro: se mide contra FluidSynth
2.6.0 sobre un font mínimo con los generadores y el modulador exactos de esa zona.
