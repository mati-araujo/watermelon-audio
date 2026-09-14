# Nota de bump para NoisyPad — la afinación fina y el coarseTune salen del keytrack

**Fecha**: 2026-09-14 · **Cambio**: MINI-025 (nace del spec-test, REQ-039 S3) · **Release**:
`v2.17.3` (es un `fix`) · **Font medido**: `GeneralUser_GS.sf3` (GeneralUser GS 1.471, el que shipea
NoisyPad).

Esta nota dice **qué cambia de sonido y en qué presets**, medido sobre el archivo con
`scripts/read-sf2-modulators.py --pitch` (lee el `pdta`, no el motor: puede contradecirlo). Si algo
de acá no coincide con lo que se oye en el dispositivo, eso es un hallazgo y se quiere saber.

## Lo que cambia, en una frase

Hasta `v2.17.2` el motor aplicaba los tres offsets de pitch de un SoundFont —`coarseTune`
(semitonos), `fineTune` (cents) y la corrección de afinación del sample— **adentro** del keytrack
(`scaleTuning`): en una zona con `scaleTuning` 50 entraban a la mitad, con 40 al 40 %, y con 0
desaparecían. El spec SF2 (§8.1.2) y FluidSynth los aplican **afuera**, como offsets absolutos:

    pitch = raíz + (tecla − raíz)·scaleTuning/100 + coarse + (fine + corrección)/100

Desde `v2.17.3` el motor hace lo mismo. Medido contra el SoundFont-Spec-Test: la prueba #8
(`scaleTune/rootKey`) pasa de 23 cents de error a 0,00 hop a hop contra FluidSynth 2.6.0; y sobre
fonts mínimos, FluidSynth da lo mismo con scaleTuning 0, 50 y 100 (−22,47 c para `fine −23`;
+200,6 c para `coarse +2`), que es lo que el motor da ahora.

Lo que **no** cambia: nada de nivel, ni las curvas de velocity, ni la expresión. Y **ningún preset
melódico del banco 0** (programas 0–111): GeneralUser sólo usa `scaleTuning ≠ 100` en percusión y
efectos.

## En qué presets, medido

**570 zonas (preset × instrumento) en 67 presets**, todas de percusión o efectos: las **once
baterías** en sus dos bancos, 120 y 128 (22 presets, 404 de las 570 zonas), y los programas GM
**115–127** de los bancos 0–12 (45 presets, 166 zonas: Wood Block, Taiko, Melodic Tom, Fret Noise,
Seashore, Birds, Telephone, Helicopter, Applause, Gun Shot, Concert Bass Drum, 808 Tom…).

**Δ es cuánto se mueve el pitch con el cambio, en cents (negativo = ahora suena más grave que en
2.17.2).** Es `(100·coarse + fine + corr) · (1 − scaleTuning/100)`, con la zona de preset sumando
a la de instrumento (SF2 §8.5). El caso típico es un tambor con `coarse` negativo y `scaleTuning`
40/50 que en 2.17.2 sonaba **más agudo** de como lo afinó el autor — Concert Bass Drum 2 a la
tecla 36: 780 cents (más de una quinta) arriba; el 808 Tom, 361 arriba; Wood Block, 1344 arriba.
La magnitud máxima es 2450 c (`12:127 Shooting Star`, coarse +49 con scaleTuning 50: ahora suena
dos octavas MÁS agudo).

| banco:prog | preset | zonas | instrumentos afectados | Δ (cents) |
|---|---|---|---|---|
| 0:115 | Wood Block | 19 | Wood Block_1 (-1344); Wood Block_2 (-100) | -1344 … -100 |
| 0:116 | Taiko Drum | 35 | Taiko Drum (-12) | -12 … -12 |
| 0:117 | Melodic Tom | 25 | Melodic Tom (-300, -150, +100, +300, +550) | -300 … +550 |
| 0:120 | Fret Noise | 1 | Fret Noise (+4) | +4 … +4 |
| 0:122 | Seashore | 2 | Seashore (-388, -88) | -388 … -88 |
| 0:123 | Birds | 2 | Birds (-70, +70) | -70 … +70 |
| 0:124 | Telephone 1 | 3 | Telephone 1 (-750, -696, -496) | -750 … -496 |
| 0:125 | Helicopter | 1 | Helicopter (-649) | -649 … -649 |
| 0:126 | Applause | 2 | Applause (-180, +180) | -180 … +180 |
| 0:127 | Gun Shot | 1 | Gun Shot (-700) | -700 … -700 |
| 1:120 | Cut Noise | 1 | Cut Noise (-350) | -350 … -350 |
| 1:121 | Fl. Key Click | 1 | Fl. Key Click (+550) | +550 … +550 |
| 1:122 | Rain | 2 | Rain (-50, +150) | -50 … +150 |
| 1:123 | Dog | 1 | Dog (-600) | -600 … -600 |
| 1:124 | Telephone 2 | 1 | Telephone 2 (-75) | -75 … -75 |
| 1:125 | Car-Engine | 2 | Car-Engine (-540, -420) | -540 … -420 |
| 1:126 | Laughing | 1 | Laughing (-400) | -400 … -400 |
| 1:127 | Machine Gun | 1 | Machine Gun (-600) | -600 … -600 |
| 2:120 | String Slap | 1 | String Slap (-300) | -300 … -300 |
| 2:122 | Thunder | 2 | Thunder (-1200, -1050) | -1200 … -1050 |
| 2:124 | Door Creaking | 1 | Door Creaking (-300) | -300 … -300 |
| 2:125 | Car-Stop | 1 | Car-Stop (-400) | -400 … -400 |
| 2:127 | Lasergun | 3 | Lasergun_1 (-948); Lasergun_2 (-216, -96) | -948 … -96 |
| 3:123 | Bird 2 | 1 | Bird 2 (-1010) | -1010 … -1010 |
| 3:124 | Door | 1 | Door (-350) | -350 … -350 |
| 3:125 | Car-Pass | 2 | Car-Pass (-302, -298) | -302 … -298 |
| 3:126 | Punch | 1 | Punch (+140) | +140 … +140 |
| 3:127 | Explosion | 2 | Explosion (-180, -135) | -180 … -135 |
| 4:122 | Stream | 2 | Stream (-50, +50) | -50 … +50 |
| 4:123 | Scratch | 1 | Scratch (-50) | -50 … -50 |
| 5:122 | Bubbles | 1 | Bubbles (-450) | -450 … -450 |
| 5:124 | Windchime | 1 | Windchimes (-540) | -540 … -540 |
| 5:126 | Footsteps | 1 | Footsteps (-450) | -450 … -450 |
| 6:125 | Train | 2 | Train (-300) | -300 … -300 |
| 8:116 | Concert Bass Drum | 10 | Concert Bass Drum 2 (-810, -780) | -810 … -780 |
| 8:117 | Melodic Tom 2 | 12 | Melodic Tom 2 (-75) | -75 … -75 |
| 8:118 | 808 Tom | 1 | TR-808 Toms (-362) | -362 … -362 |
| 8:125 | Starship | 2 | Starship (-950, +50) | -950 … +50 |
| 9:125 | Burst Noise | 2 | Burst Noise (-1650, -1000) | -1650 … -1000 |
| 11:119 | Cymbal Crash | 9 | Cymbal Crash (+200) | +200 … +200 |
| 11:121 | Filter Snap | 1 | Filter Snap (-600) | -600 … -600 |
| 11:127 | Interference | 2 | Interference (-600, +650) | -600 … +650 |
| 12:119 | Tambourine | 1 | Tambourine (-1) | -1 … -1 |
| 12:122 | White Noise Wave | 1 | White Noise Wave 2 (-650) | -650 … -650 |
| 12:127 | Shooting Star | 1 | White Noise Wave 2 (+2450) | +2450 … +2450 |
| 120:0 | Standard Drums | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 120:1 | Standard 2 Drums | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 120:8 | Room Drums | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 120:16 | Power Drums | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 120:24 | Electronic Drums | 21 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75); Synth Drum (+200, +250, +350, +450, +600, +700); Reverse Cymbal (+200) | -1594 … +700 |
| 120:25 | 808/909 Drums | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 120:26 | Dance Drums | 21 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75); Synth Drum (+200, +250, +350, +450, +600, +700); Reverse Cymbal (+200) | -1594 … +700 |
| 120:32 | Jazz Drums | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 120:40 | Brush Drums | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 120:48 | Orchestral Perc. | 16 | Applause (-700); Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75); Castanets (+510) | -1594 … +510 |
| 120:56 | SFX Kit | 46 | Fret Noise (+330); Laughing (-225); Scream (+175); Punch (+140, +280); Heart Beat (+80); Footsteps (-425); Applause (-135, +225); Door Creaking (-288); Door (-350); Windchimes (-593); Car-Engine (-600, -480); Car-Stop (-500); Car-Pass (-426, -424); Siren (-175); Train (-562); Helicopter (-899); Starship (-1225, -225); Gun Shot (-1000); Machine Gun (-925); Lasergun_1 (-1308); Lasergun_2 (-576, -456); Explosion (-315, -270); Dog (-1000); Horse Gallop (-240); Birds (-420, -280); Rain (-550, -300); Thunder (-1675, -1525); Seashore (-938, -638); Stream (-625, -525); Bubbles (-1050); Scratch (-75); Car-Crash (-150) | -1675 … +330 |
| 128:0 | Standard | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 128:1 | Standard 2 | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 128:8 | Room | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 128:16 | Power | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 128:24 | Electronic | 21 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75); Synth Drum (+200, +250, +350, +450, +600, +700); Reverse Cymbal (+200) | -1594 … +700 |
| 128:25 | 808/909 | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 128:26 | Dance | 21 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75); Synth Drum (+200, +250, +350, +450, +600, +700); Reverse Cymbal (+200) | -1594 … +700 |
| 128:32 | Jazz | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 128:40 | Brush | 14 | Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75) | -1594 … -75 |
| 128:48 | Orchestral | 16 | Applause (-700); Wood Block_1 (-1594, -1319); Wood Block_2 (-350, -75); Castanets (+510) | -1594 … +510 |
| 128:56 | SFX | 46 | Fret Noise (+330); Laughing (-225); Scream (+175); Punch (+140, +280); Heart Beat (+80); Footsteps (-425); Applause (-135, +225); Door Creaking (-288); Door (-350); Windchimes (-593); Car-Engine (-600, -480); Car-Stop (-500); Car-Pass (-426, -424); Siren (-175); Train (-562); Helicopter (-899); Starship (-1225, -225); Gun Shot (-1000); Machine Gun (-925); Lasergun_1 (-1308); Lasergun_2 (-576, -456); Explosion (-315, -270); Dog (-1000); Horse Gallop (-240); Birds (-420, -280); Rain (-550, -300); Thunder (-1675, -1525); Seashore (-938, -638); Stream (-625, -525); Bubbles (-1050); Scratch (-75); Car-Crash (-150) | -1675 … +330 |

presets: 67 · zonas: 570

## Cómo verificarlo del lado de NoisyPad

Un tambor con nombre y tecla alcanza: `8:116 Concert Bass Drum` a la tecla 36 tiene que sonar
**780 cents más grave** que en 2.17.2 (3740 c absolutos donde antes 4520). Si tienen FluidSynth a
mano sobre el mismo `.sf2`, la referencia es esa; si no, el WAV de 2.17.2 contra el de 2.17.3 con
un estimador de pitch cualquiera separa 780 c sin esfuerzo.

## La pregunta activa (I-3)

**¿Algún preset —de baterías o de efectos, que es donde cambia— suena distinto de lo que esperan y
no está en la tabla? ¿Y alguno de la tabla suena PEOR que antes?** Un "el 808 sonaba mejor agudo"
no es una queja de gusto para nosotros: es el disparador para medir ese preset contra FluidSynth, y
va con número.
