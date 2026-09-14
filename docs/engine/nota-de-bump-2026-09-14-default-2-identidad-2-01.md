# Nota de bump para NoisyPad — el borrado del default #2 con identidad SF 2.01 se honra

**Fecha**: 2026-09-14 · **Cambio**: MINI-028 (nace del #3 de su carta del 12/09) · **Release**: la
siguiente `patch` desde `v2.17.1` (es un `fix`) · **Font medido**: `GeneralUser_GS.sf3` (GeneralUser
GS 1.471, el que shipea NoisyPad).

Esta nota dice **qué cambia de sonido y en qué presets**, medido sobre el archivo con
`scripts/read-sf2-modulators.py --presets` y con el note-on de producción. Si algo de acá no coincide
con lo que se oye en el dispositivo, eso es un hallazgo y se quiere saber.

## Lo que cambia, en una frase

Hasta `v2.17.1` el motor aplicaba en **todos** los presets de GeneralUser un filtro por velocity de
**−2400 cents** (el default #2 del spec SF 2.04) que el archivo **borra** — pero lo borra con la
identidad del spec **2.01** (`amtSrc` velocity/switch), y el motor sólo reconocía la 2.04. Desde este
cambio las dos identidades nombran el mismo default: un font que lo borra con cualquiera no lo
recibe; un font que calla, sí (#14 A y #14 C del spec-test siguen el spec). Es lo que ustedes
midieron como centroide 1123 → 880 Hz en `Saw Lead` con la velocity, y lo que pidieron: **el font
como lo afina su autor, FluidSynth como referencia** (que no implementa ese default).

Lo que **no** cambia: el nivel (la curva de velocity → atenuación de 2.17.0, la atenuación declarada
de 2.17.1), la expresión por toque y la global. Está medido: `Saw Lead` a velocity 38 sigue en
−17,05 dB respecto de 124.

## En qué presets, medido

GeneralUser 1.471 borra el default #2 en **1422 zonas de instrumento**, y algún borrado alcanza a
**los 269 presets**. Lo que se oye es **más brillo a baja velocity** — la nota suave deja de
oscurecerse 2400 cents de más (a velocity 0; proporcionalmente menos a velocities altas: a 64 son
−1200 cents de más, a 100 son −510). Tres formas, por preset:

| qué | presets | qué se oye |
|---|---|---|
| **el timbre deja de moverse con la velocity** (el archivo no declara ninguna curva propia) | **140** de 269 · 66 del banco 0 | una nota suave y una fuerte tienen el **mismo brillo**, como en 2.16.4 y como en FluidSynth: `0:6 Harpsichord`, `0:12 Marimba`, `0:38 Synth Bass 1`, `0:80 Square Lead`, `0:87 Bass & Lead`, `0:5 FM Electric Piano`, `0:30 Distortion Guitar`, `0:32 Acoustic Bass`, `0:46 Orchestral Harp`, `0:68 Oboe`, `0:109 Bagpipes`, `0:116 Taiko`… |
| **queda sólo la curva del archivo** | **46** | el filtro sigue cerrándose con la velocity, **2400 cents menos** que hasta 2.17.1: `0:81 Saw Lead` (−4400 → **−2000**), `0:62 Synth Brass 1` y `0:73 Flute` (−6000 → −3600), `0:65 Alto Sax` (−6000 → −3600), `0:50 Synth Strings 1` (−4800 → −2400) |
| **mezcla por región** (algunas zonas con curva propia, otras sin) | **83** | el brillo cambia distinto según la tecla o la capa: los tres pianos (`0:0 Stereo Grand` −6200/−2400 → **−3800/0**: las capas fuertes conservan su curva, las demás dejan de oscurecerse), `0:40 Violin` y `0:110 Fiddle` (−3600/−2400 → −3600/0), `0:60 French Horns`, `0:42 Cello`, `0:20 Reed Organ` |

Los que **no** cambian (declaran su propia curva con la identidad 2.04, así que ya reemplazaban al
default): `0:89 Warm Pad` (−2400 propio), `0:44/48/49 Stereo Strings`, `0:56 Trumpet` (−2480),
`0:57 Trombone` (−3480).

**Medido con el note-on de producción**, `Saw Lead` tecla 72, velocity 124 → 38: centroide **1640 →
1195 Hz** hasta 2.17.1; **1638 → 1546** desde este cambio — muestra a muestra igual a lo que el
archivo declara (`SoundFontDefaultTwoIdentity.GeneralUserSawLeadSoundsLikeWhatTheFileDeclares`).

## Lo que esto significa para NoisyPad

- **Nada que compensar.** Es la referencia que pidieron (FluidSynth) en el tramo del brillo.
- En el **grid** con velocity del golpe, una nota suave suena **más brillante** que en 2.17.x — en
  140 presets, igual de brillante que una fuerte. En el **XY** (velocity 1,0 + expresión) no cambia
  nada: a velocity 127 el default valía 0.
- Si a baja velocity un preset suena ahora **demasiado brillante** con nombre y número, eso es un
  dato para el falsador de MINI-028 (§ *si falla*), no una queja de gusto: dispara medir contra
  FluidSynth ese preset.

## Cómo se midió

- `scripts/read-sf2-modulators.py GeneralUser_GS.sf3 --presets`: columna `default #2` (qué identidad
  usa el archivo para borrarlo y qué hace el motor) y **Z** = zonas con un 2.01 de amount ≠ 0 (hoy 0:
  la mitad "un 2.01 con amount propio reemplaza" de la regla es inobservable en este font).
- Contra el spec-test: `#14 D borrado 2.01` pasa de **S** (seguía el spec 2.04 contra FluidSynth) a
  **F** (plana en los dos, 0,02 contra 0,01 dB); `#14 B` (−7200 declarado **y** borrado 2.01 en la
  misma zona) sigue en 15,70 contra 15,68 — el borrado no pisa la curva declarada; `#14 A/C` siguen
  el spec.
- Sobre fonts propios (`test_soundfont_default_two_identity.cpp`): borrar con 2.01 es muestra a
  muestra igual que borrar con 2.04; sin borrar, el default sigue; un 2.01 con −1200 y switch se
  evalúa en lugar del default (a velocity 100 = borrado; a 38 = −1200 con identidad 2.04).

## Una pregunta, la misma

**¿Algún preset suena distinto de lo que esperan —nivel, afinación, envolvente o brillo— y no está
en esta lista?** Con nombre y número.
