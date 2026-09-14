---
title: "Respuesta a NoisyPad — v2.17.1 (la atenuación declarada), su hallazgo #3 medido, y #4"
type: reference
status: current
created: 2026-09-14
---

# Respuesta a NoisyPad — 2026-09-14 · v2.17.1, #3 y #4

**watermelon-audio → NoisyPad.** Contesta su respuesta del 12/09 al aviso de v2.17.0
(`carta-noisypad-2026-09-12-respuesta-v2.17.0.md`) y avisa **v2.17.1**.

> **Redactada el 2026-09-14, sin enviar todavía.**

Cuatro cosas, en orden de lo que les cambia: el bump, su hallazgo #3 (tenían razón, y era
nuestro), lo que ese hallazgo destapó al medirlo, y #4.

## 1 · v2.17.1 — la atenuación declarada entra como fue programada

Es un `fix` (MINI-024) y cambia el nivel de **235 de los 269** presets de GeneralUser. La nota de
bump entera, por preset, está en `nota-de-bump-2026-09-12-initial-attenuation.md`; lo que hay que
saber para escuchar:

- Hasta 2.17.0 el generador `initialAttenuation` del SoundFont entraba a **0,1 dB por dB
  declarado**; el spec-quirk que el SoundFont-Spec-Test pide emular y que FluidSynth aplica es
  **0,4**. Una zona programada 10 dB abajo suena ahora 4 dB abajo, no 1.
- **Presets que bajan enteros** (todas sus zonas declaran atenuación): `96 Ice Rain` −16,5 dB,
  `123 Birds` −12, `81 Saw Lead` −11,1, `80 Square Lead` −10, `6 Harpsichord` −7,5, `38 Synth
  Bass 1` −6,9, `62 Synth Brass 1` −6,3, `16 Tonewheel Organ` −6,0… (32 del banco 0 con ≥ 6 dB).
- **Presets que cambian por dentro** (sólo algunas zonas): `42 Cello`, `40 Violin`, `110 Fiddle`,
  `5 FM Electric Piano`, `30 Distortion Guitar`, `47 Timpani`… — una capa que estaba 5 dB abajo pasa
  a estar los 20 que declara. Éstos no suenan "más bajos": suenan **distintos**. Los pianos están
  acá (`Stereo Grand` 984 de 1056 zonas).
- **Lo que no cambia**: los 34 presets sin atenuación declarada (`56 Trumpet`, `57 Trombone`,
  `68 Oboe`, `26/27/28/29` guitarras…), y los dos controles de nivel de ustedes (expresión por toque
  y global), que viven aguas abajo.
- **No compensen por preset a mano** (medimos su árbol el 12/09: no hay tablas de ganancia por
  preset, bien). Si la ganancia de salida está calibrada a oído contra 2.17.0, va a quedar baja.

Su #5 —*"nada con nombre, no medimos presets con atenuación declarada"*— lo leímos como *sin
oportunidad*, no como verde. Con 2.17.1 la oportunidad es cualquiera de los de arriba.

## 2 · Su #3: tenían razón, y el defecto era de nuestra nota, no del motor

`0:81 Saw Lead` **no** tiene la velocity anulada. Lo medimos en el archivo y en el motor, con el
mismo note-on que cruza producción (`channelNoteOnWithModulators`), tecla 60, ventana estable de
0,5 a 0,84 s:

| velocity | RMS, motor 2.17.x | banda < 700 Hz | su medición | tsf pelado (2.16.4) |
|---|---|---|---|---|
| 124 | 0 (ref) | 0 | 0 | 0 |
| 76 | **−7,06 dB** | −7,08 | −6,9 | −4,25 |
| 38 | **−17,08 dB** | −17,09 | −16,9 | −10,27 |

Coincide con lo suyo a **0,2 dB**, y la banda de la fundamental cae exactamente lo mismo que el
total: es **atenuación**, no filtro. La curva es la del default #1 a **800 cB cóncava** — y está en
el archivo, en la **zona global del preset**:

```
pzone 1255 [GLOBAL]  pmod: velocity (cóncava, decreciente) -> initialAttenuation amt=800
  INST 117 Saw Decline, zona global:
                     imod: velocity (cóncava, decreciente) -> initialAttenuation amt=0
```

El instrumento pone el default en 0 (lo que la nota leyó como "anulado"); el preset lo vuelve a
declarar, y por SF2 §9.5 los moduladores de preset se **suman** a los de instrumento. El motor lo
resuelve así desde 2.17.0 (los cuatro ámbitos, `SoundFontModulatorTable::resolve()`). **El
instrumento con que escribimos la nota, no**: `read-sf2-modulators.py --presets` recorría sólo
`imod`. Un lector de un solo ámbito describe otro archivo.

De las hipótesis (las dos suyas y la del filtro, que agregamos por su centroide): no son capas por velocity (`Saw Lead` tiene 8 zonas, todas por `keyRange`,
ninguna por `velRange`), no es el filtro (la fundamental cae igual que el total), es la tercera —el
modulador de zona de preset que la nota no miraba.

**Y no era sólo `Saw Lead`: era la fila entera.** Medido sobre los 26 "anulados" de la nota, **los
26** tienen la curva repuesta en la zona global del preset: 500 cB en los 8 órganos y los 2
clavecines (−4,4 / −10,7 dB a velocity 76 / 38 — casi lo mismo que la curva vieja de tsf), 840 en los
5 pianos, 800 en los otros 11 (`Synth Bass 1`, `Synth Brass 1`, `FM Electric Piano`, `Square Lead 2/3`,
`Saw Lead 3`…). **Ningún preset melódico de GeneralUser tiene la velocity anulada**; las únicas
regiones a 0 son zonas sueltas de los kits de percusión. La nota de 2.17.0 está corregida en su
lugar, con las filas originales tachadas y la marca del 14/09; la tabla correcta de velocity →
nivel, por curva efectiva: **232** presets a 800 cB, 11 a 500, 5 a 840, 2 a 700, 19 con mezcla.

Para ustedes cambia poco en la práctica —el XY ya va con velocity 1,0 y expresión— pero cambia lo
que les dijimos que iban a oír en el grid con un piano: la velocity **sí** mueve el nivel, 840 cB,
más el cambio de capa.

## 3 · Lo que su centroide destapó: el default #2 sigue vivo donde GeneralUser lo borra

Su otra observación —el centroide de `Saw Lead` bajando 1123 → 880 Hz con la velocity— también se
reproduce (tecla 72: 1640 → 1195 Hz), y **no** es lo que el archivo pide. Es una decisión nuestra
de REQ-039 con una consecuencia que no habíamos medido sobre el font real:

- El default #2 del spec (velocity → corte del filtro, −2400 cents) tiene **dos identidades** según
  la versión del spec: SF 2.01 lo define con una fuente secundaria (`amtSrc` velocity/switch); SF
  2.04, sin ella. El motor es 2.04 —y así está afirmado contra el spec-test, donde la fila
  `veloToFC-deleted2.01` está medida como *"no anula"*.
- GeneralUser **1.471** (el que shipean) borra el default #2 en **1422** zonas de instrumento con la
  identidad **2.01**, y sólo con ésa. En nuestro motor ese borrado no anula nada: el −2400 sigue vivo
  en los **269 presets** y se **suma** a lo que el preset declare (`Saw Lead`: −2000 del preset más
  −2400 del default = −4400 cents por velocity). FluidSynth no implementa el default #2 en absoluto,
  así que ahí `Saw Lead` sólo tiene su −2000; y GeneralUser **2.0.3** ya lo borra con las **dos**
  identidades — la intención del autor no es ambigua.
- Medido en `Saw Lead`, tecla 72, velocity 124 → 38: con el default vivo el centroide baja
  **1640 → 1195 Hz** (lo que ustedes vieron); con sólo lo que el archivo declara, **1638 → 1546**.
  El nivel no se mueve (−17,05 contra −17,06 dB). Es una diferencia de **timbre**, en toda nota
  suave, en todos los presets.

Es un hallazgo del motor y tiene la forma de un MINI: decidir si el borrado con identidad 2.01 se
honra (como hace el autor del font y como suena en FluidSynth, el synth contra el que GeneralUser
se afina) o si el motor sigue el spec 2.04 a la letra (lo que hoy hace, a sabiendas, en el
spec-test). No lo decidimos en esta carta. Lo que sí: **hasta que se decida, una nota suave suena
más oscura de lo que el font pide, en los 269.** Si en el dispositivo el brillo a baja velocity les
molesta en algún preset, ése es el dato que ordena la cola.

## 4 · Su #4: es exactamente el contrato, y el KDoc decía otra cosa

`SoundFontEngine.h:345` hace lo que describieron: un `NOTE_ON` con la **misma** nota sobre un toque
activo **no re-ataca y descarta la velocity** — la velocity es del ataque y no se puede cambiar
después (R-MOT-13, desde REQ-008). Lo que el KDoc de `sfNoteOn` decía, *"arranca o actualiza"*,
sugería lo contrario; ahora dice lo que el motor hace, en la interfaz, en Android y en la C API. Y
hay un detalle que su receta ya encontró sola y que ahora está escrito: ese mismo reenvío **sí
reinicia la expresión por toque a 1,0** aunque no ataque (R-MOT-14 corre para todo `NOTE_ON`). Su
"re-enviada en el mismo frame después de cada note-on" es la lectura correcta.

Está afirmado con un test nuevo (`ARepeatedNoteOnWithTheSameNoteKeepsTheAttackAndResetsExpression`):
tras un gesto a 0,2 y un reenvío de la misma nota con velocity 0,3, el audio queda **muestra a
muestra** igual al de la nota que nunca recibió ni gesto ni reenvío (peor diferencia 6·10⁻⁸). Los dos
mutantes —re-atacar con la misma nota, y saltear el reset— lo ponen rojo por 4·10³ y 3·10⁴ sobre el
umbral. No cambiamos el motor: su arrastre de 16 escalones sigue sin mover la ganancia, y la
expresión sigue siendo el único camino para el nivel de una nota viva.

## 5 · La pregunta activa

**¿Algún preset suena distinto de lo que esperan —nivel, afinación, envolvente o brillo— y no está
en la nota de 2.17.1 ni en el §3 de esta carta?** Su #3 es la prueba de que la pregunta rinde:
una medición con nombre de preset corrigió una fila entera de la nota y destapó otra cosa que
ninguna prueba nuestra veía. Lo mismo vale para MINI-025 (afinación fina en percusión y efectos)
y 026 (envolventes): un preset con nombre y un número.

Los WAV de su banco (`docs/audio-bump/req_bump_audio_2_17_0.md`) nos sirven tal cual si quieren
mandarlos: el método de medición que usamos acá (RMS y banda < 700 Hz sobre 16384 muestras con
Hann desde 0,5 s) reproduce el suyo a 0,2 dB.
