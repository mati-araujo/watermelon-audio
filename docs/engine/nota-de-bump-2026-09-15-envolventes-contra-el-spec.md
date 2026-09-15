# Nota de bump para NoisyPad — las envolventes de tsf contra el spec

**Fecha**: 2026-09-15 · **Cambio**: MINI-026 (nace del spec-test, REQ-039 S3) · **Release**: la
siguiente `fix` · **Font medido**: `GeneralUser_GS.sf3` (GeneralUser GS 1.471, el que shipea
NoisyPad).

Esta nota dice **qué cambia de sonido y en qué presets**, medido sobre el archivo con
`scripts/read-sf2-modulators.py --generators` y un recorrido preset → instrumento (lee el `pdta`,
no el motor). Si algo de acá no coincide con lo que se oye en el dispositivo, eso es un hallazgo y
se quiere saber.

## Lo que cambia, en cuatro frases

Son cuatro diferencias entre tsf y SF2 §8.1.2, las cuatro en las **envolventes**. Todas se
midieron primero contra FluidSynth 2.6.0 sobre el SoundFont-Spec-Test y después contra la fórmula
del spec sobre fonts mínimos generados.

1. **Decay y release de volumen recorren 100 dB en el tiempo declarado**, no 80. tsf traía una
   constante de LinuxSampler (`−9.226`) que hacía que un `decayVolEnv`/`releaseVolEnv` de 1 s
   tardara 1 s en bajar 80,13 dB; el spec dice 100 dB (#36: *"if the sustain level were −100dB,
   the Volume Envelope Decay Time would be the time spent in decay phase"*; #38: *"until 100dB
   attenuation were reached"*). **Toda pendiente de decay y de release es ahora un 25 % más
   rápida en dB/s**: una release de 1 s que antes llegaba a −60 dB (inaudible) a los 0,75 s
   llega a los 0,60; un decay hasta un sustain de −12 dB que tardaba 150 ms tarda 120.
   FluidSynth usa 96 dB (su tope interno), así que contra él el motor queda un 4 % más rápido —
   declarado en el trinquete del spec-test, no es un residuo abierto.

2. **El ataque de la envolvente de modulación no depende de la velocity.** tsf lo escalaba por
   `(145 − velocity)/144` —un SFZ-ismo—: a velocity 127 un `attackModEnv` de 100 ms duraba
   12,5 ms. El spec (#26) no tiene tal cosa. **Los barridos de filtro y de pitch de las notas
   fuertes duran ahora lo que el font declara**: hasta 8× más largos a velocity máxima, igual que
   a velocity baja.

3. **El ataque de la envolvente de modulación es convexo**, como pide el spec (#26, §9.1.7), con
   la curva de FluidSynth (`1 + (40/96)·log10(t/T)`): a la mitad del ataque va al 88 %, no al
   50 %. Los barridos de filtro **abren más rápido al principio** y llegan al final en el mismo
   tiempo.

4. **La release de la envolvente de modulación recorre el 100 % en el tiempo declarado** (#30).
   tsf la hacía desde el nivel actual en el tiempo entero: desde un sustain del 50 % tardaba el
   doble. Los filtros/pitch **vuelven a su reposo más rápido tras el note-off** en los presets
   con sustain de modulación parcial.

Lo que **no** cambia: el sustain de volumen (tsf ya lo ponía en centibeles exactos: 120 cB =
−12,0 dB; FluidSynth lo redondea a −11,5), los niveles, las curvas de velocity, la afinación, ni
el LFO. **Sobre #5 A del spec-test** (LFO de volumen a velocity 127) el motor no cambia: la
referencia estaba saturada a 0 dBFS y FluidSynth amplifica igual que tsf.

## En qué presets, medido

| fix | generador | zonas | presets (de 269) |
|---|---|---|---|
| 1 | `releaseVolEnv` | 1672 | **259** |
| 1 | `decayVolEnv` | 1275 | **207** |
| 2, 3, 4 | `modEnvToFilterFc` | 1102 | **176** — 80 de los 112 melódicos del banco 0 |
| 2, 3, 4 | `modEnvToPitch` | 69 | 30 |
| 2 | `attackModEnv` declarado | 542 | (el resto tiene ataque instantáneo: el fix 2 no los toca) |

O sea: **el fix 1 toca prácticamente todo preset**, de forma sutil (colas un 20 % más cortas en lo
audible). Los fixes 2–4 tocan los 176 presets con envolvente de filtro, y ahí el cambio es
**audible en las notas fuertes**: pianos (Stereo Grand, Bright Grand, Tine/FM Electric Piano),
órganos, guitarras (Steel, Jazz, Muted, Distortion), todos los bajos (Acoustic, Finger, Pick,
Fretless, Slap 1/2, Synth Bass 1/2), cuerdas, voces y bronces. Antes, a velocity alta el filtro
abría casi instantáneamente; ahora abre en el tiempo que el autor del font programó.

## Cómo se verificó

- Spec-test (`SfSpecConformance`, contra FluidSynth 2.6.0): la prueba #2 (mod env → pitch) pasa de
  **600 c a 9,5 c** de diferencia en el peor hop; #3 (keynum → decay) de 9,6 dB a 2,6 (los +4 %
  por construcción); la release de #1 mide 100 dB/s exactos.
- Siete tests propios sobre fonts mínimos contra la fórmula del spec
  (`test_soundfont_envelopes.cpp`), con un mutante por fix que vuelve al comportamiento anterior y
  se pone rojo.
- Nada del snapshot, de la C API ni de Kotlin cambia.

## Lo que se pide

1. Si un preset con envolvente de filtro **"cambió demasiado"** en el dispositivo, nombre del
   preset, tecla y velocity: se mide contra FluidSynth sobre un font mínimo con los generadores
   exactos de esa zona, como en las notas anteriores.
2. Si una cola de release se percibe **cortada**, lo mismo: la pendiente nueva es la del spec y se
   puede mostrar el número.
