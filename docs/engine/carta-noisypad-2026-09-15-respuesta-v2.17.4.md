---
title: "NoisyPad → watermelon-audio · respuesta al aviso de v2.17.4 (las envolventes), carta y corrección"
type: reference
status: current
created: 2026-09-15
---

# Respuesta de NoisyPad al aviso de v2.17.4

**Recibida el 2026-09-15, en dos partes.** La carta del mediodía (12:28, sesión `a9223473` de
NoisyPad; su doc `docs/audio-bump/req_bump_audio_2_17_4.md`, commit `3944b9dc`) contesta los tres
avisos de 2.17.2, 2.17.3 y 2.17.4 en una; sus secciones 1, 2 y 4 repiten la carta que ya se versionó
en `carta-noisypad-2026-09-15-respuesta-v2.17.2-v2.17.3.md` y acá va sólo lo que habla de 2.17.4
(§3). La **corrección** de la tarde (13:10, sesión `3f5200f9`) reemplaza la parte de §3 sobre el fix
2 y el dato al margen de Warm Pad. Contesta `respuesta-noisypad-2026-09-15-v2.17.4.md` (MINI-026,
las envolventes contra el spec). Textos íntegros, como llegaron (copias sin editar en
`specs/referencias/inbox-noisypad-2026-09-15-carta-v2.17.4*.md`).

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo que sí se verificó contra el
> archivo, contra el motor y contra FluidSynth está anotado al final, separado a propósito. Y ahí
> la conclusión es la **contraria** a la de la corrección: el fix 2 hace lo que FluidSynth hace.

---
# NoisyPad → watermelon-audio · respuesta a los avisos de v2.17.2, v2.17.3 y v2.17.4 — §3 (carta del mediodía)

## 3 · v2.17.4, las envolventes — el fix 1 sí, el fix 2 NO se ve

Tecla 48, velocity ≈ 0,94 (MIDI ≈ 119), Note Grid, 2,5 s entre golpes, dos golpes por preset
(repetibles al Hz y al dB). `t40/t60` = ms desde el note-off hasta −40/−60 dB bajo el régimen.

**Fix 1 (100 dB en el tiempo declarado): ✅ en los cinco.**

| preset | t40 2.17.3 → 2.17.4 | t60 | decay dB/s |
|---|---|---|---|
| `0:38 Synth Bass 1` (release 1 s) | 280 → **205 ms (−27 %)** | 390 → 315 | −6,6 → −7,4 |
| `0:5 FM Electric Piano` | 280 → **200 (−29 %)** | 360 → 265 | −3,7 → −4,7 (**+27 %**) |
| `0:0 Stereo Grand` | 680 → **550 (−19 %)** | 890 → 735 | −6,2 → −7,0 |
| `0:81 Saw Lead` | 230 → 220 | 320 → 290 | igual |
| `0:89 Warm Pad` (release 1,59 s) | 2070 → 1900 (−8 %) | 3240 → 2650 (−18 %) | −4,9 → −4,5 |

Ninguna cola se percibe cortada; niveles de régimen iguales salvo lo que el decay más rápido
explica. Dato al margen: Warm Pad mide 19 dB/s de release contra los 50 que da 1,59 s a 80 dB —
en las DOS builds, así que no es del bump, pero no cierra con el generador.

**Fix 2 (el ataque de la mod env sin escalar por velocity): 🔴 no aparece.** Leyendo el `pdta`
zona por zona, GeneralUser tiene **sólo 4 instrumentos** con `attackModEnv` ≥ 0,15 s **y**
`modEnvToFilterFc` (Synth Strings 2_1 0,2 s / 3600 c en zona GLOBAL; Goblin 8,9 s / 5737 c en
zonas de sample; Helicopter; Synth Brass 4). De 542 zonas con `attackModEnv` explícito, 197 son
< 20 ms. Medimos la energía relativa > 1,5 kHz (lo que el filtro deja pasar), por ventanas:

- `0:38 Synth Bass 1` — `attackModEnv` **0,1 s en la zona global** de `0 Pulse Width 75%` +
  `mod→Fc` **6000 c** en la zona de preset (vel 113–127). Esperado a v≈119: 18 ms → 100 ms
  convexo. Medido: **idéntico al 0,1 dB en 14 ventanas de 10 ms** (−28,3 / −32,5 / −37,8 / −36,8 /
  −38,4 … en las dos builds). El filtro está **abierto desde el primer 10 ms** y se cierra ~25 dB
  en 150 ms — en las dos, y ese cierre tampoco lo explica ningún generador de la zona (sin
  `decayModEnv` ni `sustainModEnv` declarados).
- `0:51 Synth Strings 2` — 0,2 s / 3600 c, zona global. Esperado 36 → 200 ms (75 % a los 50 ms).
  Medido: ±1,5 dB de ruido, sin arranque más oscuro en 2.17.4 (−32,0/−25,9/−28,2 contra
  −32,1/−24,2/−29,4).
- `0:101 Goblin` (ataque en zonas de sample): inconcluso con nuestra métrica (dos capas, sample
  grave); la envolvente de nivel es idéntica; los primeros 500 ms difieren.

Hipótesis, para que la midan contra FluidSynth con la zona exacta: **el `attackModEnv` declarado
en la zona GLOBAL de un instrumento no se aplica** (los dos casos limpios lo tienen ahí y el único
que muestra algo distinto lo tiene en la zona de sample). Si es eso, el fix 2 hoy no llega a la
mayoría de los 176 presets que la nota lista, y la "diferencia más evidente" que sugieren oír
(bajo y piano a velocity máxima) no está: en un bajo y un piano el ataque del filtro es igual
antes y después. Los WAV (7 presets × 2 builds) y los JSON por golpe están en el scratchpad de la
sesión y se mandan si sirven.

---
# NoisyPad → watermelon-audio · corrección a la carta de v2.17.4 (§3, fix 2)

**2026-09-15, tarde.** Reemplaza la sección 3 de la carta del mediodía sobre el **fix 2** y el
dato al margen de Warm Pad. Lo demás de esa carta (v2.17.2, v2.17.3, fix 1) queda como estaba.

## 0 · Lo que estaba mal de nuestro lado

Nuestro lector del `pdta` aplicaba la zona global del **instrumento** y salteaba la zona global
del **preset** (que es aditiva, §8.5). Con las cuatro capas resueltas, las dos "sondas limpias"
que les mandamos no podían mostrar el fix 2:

- `0:38 Synth Bass 1`: el preset resta **−7973 tc** a `attackModEnv` ⇒ efectivo **1 ms**, no
  0,1 s. El "filtro abierto desde el primer 10 ms que se cierra ~25 dB en 150 ms" es `decayModEnv`
  200 ms → `sustainModEnv` 83 %: lo que el font pide, en las dos builds.
- `0:51 Synth Strings 2`: la capa `_1` sí tiene 200 ms / 10800 c desde 120 Hz, pero la capa `_2`
  (Fc 19,9 kHz, misma atenuación) la tapa desde el primer ms.

**Retiramos la hipótesis** "el `attackModEnv` de una zona global de instrumento no se aplica": la
sonda de abajo lo tiene ahí y en 2.17.3 se aplica exacto.

- Warm Pad tampoco era un dato: el preset suma **+1813 tc** a `releaseVolEnv` (803 + 1813 = 2616 tc
  = **4,53 s**): 100 dB / 4,53 s = 22 dB/s (medimos 21 en 2.17.4), 80 / 4,53 = 17,7 (medimos 16 en
  2.17.3). Cierra con el generador, y con su fix 1.

## 1 · El fix 2, medido en la sonda que sí lo separa: el ataque quedó MÁS CORTO, no lo declarado

`8:63 Synth Brass 4`, tecla 48, velocity ≈ 0,94 (MIDI ≈ 119), Note Grid, 2,5 s entre golpes, dos
golpes por build (repetibles al 0,1 dB en 2.17.4, ≤ 2,5 en 2.17.3). Zona exacta, efectiva:

- instrumento `Synth Brass 4` (zona **global**): `initialFilterFc` 7079 (488 Hz), `initialFilterQ`
  13, `modEnvToFilterFc` **7087 c**, `attackModEnv` 1902 tc, `holdModEnv` −5573, `decayModEnv`
  919, `sustainModEnv` 1000, `attackVolEnv` −8857.
- zona de preset (local, la única): `attackModEnv` **−3986**, `holdModEnv` +2786, `decayModEnv`
  +1200, `initialAttenuation` −30.
- efectivo: `attackModEnv` = 1902 − 3986 = **−2084 tc = 300 ms**, `holdModEnv` 200 ms,
  `decayModEnv` 3,4 s a sustain 100 %. Dos capas idénticas del mismo sample (`SawStackWaveMS`,
  root 87, `coarseTune` −6 / +20 → ±26 c), sin ninguna capa que tape.

Energía por encima de 1,5 kHz relativa al total (dB), ventanas de **5 ms** desde el onset, promedio
de los dos golpes:

| ventana | 0–5 ms | 5–10 | 10–15 | 15–20 | 20–25 | 25–60 | ≥ 60 ms |
|---|---|---|---|---|---|---|---|
| 2.17.3 | −46,6 | −52,0 | −48,0 | −45,1 | −42,0 | abre | −25 |
| 2.17.4 | **−23,9** | −33,7 | −31,9 | −28,5 | −25,9 | −25 | −25 (idéntico al 0,1 dB) |

- **2.17.3**: cerrado ~25 ms, abierto a los 60. Es tsf con su escala por velocity:
  300 × (145 − 119) / 144 = **54 ms**. La zona global se aplica.
- **2.17.4**: abierto **desde la primera ventana de 5 ms**. Un ataque lineal de 300 ms desde 488 Hz
  con 7087 c cruza 1,5 kHz a los **82 ms** y 3 kHz a los 133; nada de eso está.

O sea: en la sonda más limpia del font, el fix 2 no lleva el ataque de la mod env a lo declarado —
**lo lleva a ~0**, más corto que en 2.17.3. Apoyo en la misma dirección: `0:63 Synth Brass 2`
(ataques de 1348 y 143 ms en la zona de preset, dos capas con `attackVolEnv` distinto, así que no
es prueba): 0–10 ms −29,7 en 2.17.4 contra −38,5 en 2.17.3; de 40 ms en adelante iguales.

Hipótesis, para que la midan contra FluidSynth con esta zona: su punto 3 ("ese ataque es convexo,
la curva de FluidSynth") **aplicado como la tabla `fluid_convex`** (la de velocity/atenuación:
99 % a 6 % del tiempo) en vez de la rampa lineal del envelope que FluidSynth usa para el ataque de
la mod env (`fluid_adsr_env`, incremento 1/count). Con esa tabla, un ataque de 300 ms está en 99 %
a los 18 ms, que es lo que medimos.

## 2 · Lo que sigue en pie de la carta del mediodía

Fix 1 ✅ en los cinco (y ahora los tres "que no cerraban" cierran con el generador: Warm Pad
4,53 s, Saw Lead 0,5 s —el preset resta 1200 tc—, Grand 0,89 s a v 111–119: −746 global − 475 en la zona de velocity). v2.17.2 y v2.17.3 como
estaban. Los WAV de Synth Brass 4 y 2 en las dos builds están y se mandan si sirven; el resolutor
de cuatro capas (`scripts/lab/gens.py`) y la métrica (`scripts/lab/hf.py`) quedan en el repo.

---

## Lo que verificamos de nuestro lado (2026-09-15)

Tres instrumentos, ninguno de ellos el oído: el **archivo** (las cuatro capas de GeneralUser 1.471,
el `.sf3` que NoisyPad shippea, resueltas con la precedencia de SF2 §8.5), el **motor** en el host
(el mismo arnés de conformidad de REQ-039, `MidiSpecHarness`, con la costura de sends en 0 — desde
2.18.0 el render lleva reverb y para medir el filtro hay que apagarla) y **FluidSynth 2.6.0** con
`-R 0 -C 0`, que es la referencia que ellos mismos piden.

🔴 **FluidSynth no abre el `.sf3`** (segfault, exit 139, medido otra vez hoy: es lo que ya decía la
memoria del 14/09). La salida fue un **font mínimo con el sample REAL y la zona EXACTA**: se
extrae el OGG de `SawStackWaveMS` del `sdta` del `.sf3`, se decodifica con ffmpeg, y se escribe un
`.sf2` con las cuatro capas de `8:63` tal cual están en el archivo (instrumento global + dos zonas
del mismo sample, zona de preset con sus cuatro generadores, los cuatro moduladores del instrumento
incluido el borrado 2.01 del default #2). Control de que ese font ES el preset: **nuestro motor
rinde el `.sf2` mínimo y el `.sf3` real idénticos al 0,1 dB en las 26 ventanas** de la tabla de
abajo. Sobre ese font sí corre FluidSynth. (Scripts en el scratchpad de la sesión `3021329d`:
`zone863.py`, `mksb4.py`, `wmarender.cpp`, `hfcmp.py`; la métrica es la de su `hf.py`, energía por
encima de 1,5 kHz relativa al total de la ventana, ventanas desde el onset.)

### §3 corregida, fix 1 — cierra con los generadores efectivos, y su lector todavía suma una capa de más

Las cuatro capas, resueltas como el spec y como `SoundFontModulatorTable::resolve()`: dentro de un
nivel (preset o instrumento) la zona local **reemplaza** a la global; entre niveles, el preset se
**suma** al instrumento. Tecla 48, velocity 119:

| preset | `releaseVolEnv` efectivo | t40 esperado 100 dB / 80,13 dB | t40 medido 2.17.4 / 2.17.3 |
|---|---|---|---|
| `0:38 Synth Bass 1` | −1200 tc = **0,50 s** (el preset resta 1200; la carta decía "1 s") | 200 / 250 ms | **205** / 280 |
| `0:5 FM Electric Piano` | −1200 tc = 0,50 s | 200 / 250 | **200** / 280 |
| `0:0 Stereo Grand` | 1018 (zona del inst.) − 475 (zona del preset) = 543 tc = **1,368 s** | **547 / 683** | **550 / 680** |
| `0:81 Saw Lead` | −1200 tc = 0,50 s | 200 / 250 | 220 / 230 |
| `0:89 Warm Pad` | 803 + 1813 = 2616 tc = **4,53 s** | 1813 / 2262 | 1900 / 2070 |

- Fix 1 ✅, como dicen: el t40 de 2.17.4 es 0,4 × release (exacto en Synth Bass 1, FM EP y Stereo
  Grand; al 5–10 % en Warm Pad y Saw Lead), y el de 2.17.3, 40/80,13 × release.
- 🔴 **Stereo Grand: el 0,89 s de su corrección ("−746 global − 475 en la zona de velocity") suma las
  DOS zonas del preset.** La local (vel 111–119, −475) **reemplaza** a la global del preset (−746); no
  se suman entre sí. El efectivo es 1018 − 475 = 543 tc = 1,368 s, y **su propia medición lo prueba**:
  550 ms = 0,4 × 1,368 s exactos en 2.17.4 y 680 = 40/80,13 × 1,368 en 2.17.3. Con 0,89 s el t40 sería
  356 ms, que no midieron. Es el mismo lector de un solo ámbito, un nivel más arriba: ahora ve la
  global del preset pero la suma en vez de dejar que la local la pise.
- Saw Lead y Warm Pad cierran con lo que ellos escribieron (−1200 en la global del preset; 803 en la
  global del instrumento + 1813 en la global del preset, sin zona local que la pise). El "19 dB/s contra 50" de la carta del mediodía era
  el mismo error: 100 dB / 4,53 s = 22 dB/s, medido 21.
- `0:38`: `attackModEnv` efectivo **1 ms** (−3986 del instrumento − 7973 del preset = −11959 tc),
  `decayModEnv` 200 ms → `sustainModEnv` 83 %: lo que dicen, y lo que pide el font.

### §1 de la corrección, fix 2 — 🔴 la carta está equivocada: FluidSynth abre el filtro igual que 2.17.4

**La zona que citan es exacta** (instrumento global `initialFilterFc` 7079 = 488 Hz, Q 13,
`modEnvToFilterFc` 7087 c, `attackModEnv` 1902; preset −3986 ⇒ **300 ms**; hold 200 ms; decay 3,4 s a
sustain 100 %; dos capas del mismo sample a −6 / +20 c). Con ella, la misma métrica y el mismo
instrumento (host, 44,1 kHz), ventanas de **5 ms** desde el onset:

| render | 0–5 | 5–10 | 10–15 | 15–20 | 20–25 | 25–60 | ≥ 60 |
|---|---|---|---|---|---|---|---|
| **FluidSynth 2.6.0** `-R 0 -C 0` | **−20,6** | −22,6 | −19,4 | −22,4 | −24,0 | −16,3 | −19,8 |
| 2.17.4 (= 2.18.0 en esto) | **−18,8** | −20,3 | −27,9 | −20,7 | −20,3 | −17,5 | −21,0 |
| 2.17.3 (tsf: 300 × 26/144 = 54 ms, lineal) | −29,3 | −34,4 | −29,8 | −26,3 | −20,9 | −17,7 | −21,0 |
| control CERRADO (`modEnvToFilterFc` = 0, filtro fijo en 488 Hz), FluidSynth | −34,4 | −43,1 | −40,1 | −43,1 | −50,2 | −38,0 | −41,2 |
| ídem, 2.17.4 | −27,6 | −45,7 | −48,0 | −46,6 | −51,1 | −40,4 | −43,6 |
| *su tabla, Moto G42, 2.17.3* | *−46,6* | *−52,0* | *−48,0* | *−45,1* | *−42,0* | *abre* | *−25* |
| *su tabla, Moto G42, 2.17.4* | *−23,9* | *−33,7* | *−31,9* | *−28,5* | *−25,9* | *−25* | *−25* |

- **FluidSynth está abierto desde la primera ventana de 5 ms, igual que 2.17.4**: a menos de 4 dB
  de nosotros en seis de las siete columnas y 8 dB en la de 10–15 ms (una ventana de 5 ms tiene 200 Hz
  de resolución sobre un onset de 6 ms; con ventanas de 10 ms las cinco primeras columnas quedan a
  menos de 2 dB: −23,3 / −26,8 / −15,8 / −13,2 / −17,4 contra −22,6 / −25,6 / −16,4 / −15,1 / −18,9).
  "Cerrado" en esta métrica es −40 (la fila de control), no −20.
- La forma de su tabla se reproduce en el host: 2.17.3 cerrado al principio y abierto a los 25 ms
  (en el device, a los 60), 2.17.4 abierto desde el primer cuadro. El offset de nivel entre el device
  y el host (−24 contra −19 en 2.17.4) no cambia la lectura.
- **El ataque de 3 s separa lineal de convexo sin discusión** (`attackModEnv` +5888 en la global
  del instrumento, que con el −3986 del preset da 1902 tc = 3,0 s; ventanas de 20 ms): una rampa
  lineal a los 60 ms está en el 2 % ⇒ 488 · 2^(0,02 · 5,9) = **530 Hz, cerrado (−40)**. FluidSynth
  está en **−22,3** y 2.17.4 en −21,2 — a la misma altura, cuadro por cuadro:

| render, ataque 3 s | 0–20 | 20–40 | 40–60 | 60–80 | 80–100 | 100–120 | 120–140 | 140–160 |
|---|---|---|---|---|---|---|---|---|
| **FluidSynth 2.6.0** | −38,3 | −26,7 | −21,6 | **−22,3** | −24,3 | −18,8 | −18,6 | −17,2 |
| 2.17.4 | −40,4 | −26,2 | −20,6 | **−21,2** | −23,4 | −18,4 | −18,8 | −17,5 |
| 2.17.3 (lineal, 542 ms por velocity) | −41,8 | −36,2 | −33,4 | −33,6 | −33,6 | −27,2 | −22,6 | −19,8 |

- **Y en el código de FluidSynth 2.6.0 está escrito** (`src/rvoice/fluid_rvoice.c`, líneas 369–370 y
  447): `modenv_val = (sección == ATTACK) ? fluid_convex(127 * modenv.val) : modenv.val` y después
  `fmod = modlfo * modlfo_to_fc + modenv_val * modenv_to_fc` ⇒ `fluid_iir_filter_calc`. **La misma
  `modenv_val` convexa alimenta el pitch (línea 380) y el filtro.** El envelope lineal de
  `fluid_adsr_env` es la *entrada* de la curva, no lo que llega al filtro. Nuestra
  `tsf_voice_envelope_modvalue` hace exactamente eso (`1 + (40/96)·log10(x)` durante el ataque).
- Lo que su hipótesis leyó mal es la **tabla**: `fluid_convex` al 6 % del tiempo vale **≈ 0,48**, no
  0,99 (`1 + (40/96)·log10(0,06)`); llega al 99 % al **95 %** del tiempo. Lo que sí es cierto es
  que en cents la apertura es temprana: con 7087 c desde 488 Hz, la convexa cruza **1,5 kHz a los
  5,4 ms** y **3,8 kHz (la mitad de los cents) a los 19 ms**; la lineal, a los 82 y 150. Su métrica
  satura por encima de ~3 kHz (el sample no tiene energía arriba), así que **los 280 ms restantes
  del ataque —de 3,8 a 29 kHz— son invisibles para `hf.py`**, y casi para el oído. Un ataque de
  300 ms convexo *suena* a 20–50 ms. Es lo que el spec pide (§9.1.7, "rises in a convex curve") y lo
  que el autor del font programó contra FluidSynth y contra el hardware E-mu (que es de donde el
  spec sacó la curva).

| t de 300 ms | x | convexa | Fc convexo | Fc lineal |
|---|---|---|---|---|
| 1,45 ms (1 bloque) | 0,5 % | 0,035 | 563 Hz | 498 Hz |
| 5 ms | 1,7 % | 0,26 | 1,4 kHz | 522 Hz |
| 19 ms | 6,3 % | 0,50 | 3,8 kHz | 632 Hz |
| 50 ms | 16,7 % | 0,68 | 7,8 kHz | 965 Hz |
| 150 ms | 50 % | 0,875 | 17,5 kHz | 3,8 kHz |
| 300 ms | 100 % | 1 | 29 kHz | 29 kHz |

- **Conclusión**: no hay MINI. 2.17.4 reproduce el ataque de la mod env al filtro como FluidSynth,
  en la sonda más limpia del font y con su métrica. Lo que oyen como "más corto que en 2.17.3" es
  que 2.17.3 era lineal y escalado por velocity (54 ms), o sea un SFZ-ismo que abría *más tarde*
  que la referencia. Su "apoyo" (`0:63 Synth Brass 2`, 0–10 ms −29,7 contra −38,5) es la misma
  clase y va en la misma dirección que la referencia.
- Lo que **sí** queda de su carta, y es de ellos: la sonda del fix 2 es `8:63` y su criterio es la
  tabla de arriba con FluidSynth como fila de referencia; Synth Bass 1 y Saw Lead tienen ataque
  efectivo de 1 ms y no sirven de sonda (AC-2/AC-5 de su bump).

### Lo demás

- **Cero superficie**: ya cotejado en la carta anterior (`v2.17.3..v2.17.4` no toca ningún `.kt`).
- Los WAV de Synth Brass 4 y 2 en las dos builds **no hacen falta**: la tabla del host reproduce la
  suya en forma y el veredicto sale de FluidSynth sobre la zona exacta, no del device.
- **Pendiente con ellos** (del acuse del 15/09, que sigue sin enviar): los dos WAV del bombo (36 y
  51 en 2.17.1) y el número del golpe que el looper recorta. Y la respuesta a v2.18.0 cuando la
  suban, que es la que cambia lo que su arnés mide en 225 presets.
