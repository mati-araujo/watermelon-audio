---
title: "NoisyPad → watermelon-audio · v2.19.1 adoptada y medida (I-2 de REQ-041)"
type: reference
status: current
created: 2026-09-16
---

# Respuesta de NoisyPad al aviso de v2.19.1

**Recibida el 2026-09-16 a la noche.** Contesta `respuesta-noisypad-2026-09-16-v2.19.1.md` (enviado el
16/09). Adoptaron 2.19.1 (un PR de toml, sin cambios de app) y traen **la tabla que I-2 del criterio de
muerte de REQ-041 pedía**: siete sondas + las dos de Q alto, 0/0 y 1/1, 2.19.0 contra 2.19.1, con nuestra
ventana. Texto íntegro, como llegó (copia sin editar en
`specs/referencias/inbox-noisypad-2026-09-16-carta-v2.19.1-acuse-I-2.md`).

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo verificado contra el host está al
> final, separado a propósito. Resumen: **REQ-041 S1 evaluado y VIVO, N = 8** con una sonda de Q alto.
> Las cuatro filas que se apartan de la *fórmula* de la nota de bump coinciden con el host **rendido**
> (máx |Δ_device − Δ_host| = 0,41 dB): la fórmula sólo vale en la banda de paso, y la nota de bump lo
> decía mal. Warm Pad en 2.19.1 **es** FluidSynth al 0,00 dB (oráculo con font mínimo). Shooting Star
> no cuenta: su "antes" saturaba (+10,6 dBFS en host). **Y un hallazgo que no es de REQ-041**: el Grand
> suave está 1,7 dB por debajo de FluidSynth porque tsf recorta la atenuación neta negativa ANTES de
> sumar los moduladores; 570 regiones en 39 presets ⇒ MINI-031.

---

# NoisyPad → watermelon-audio · v2.19.1 adoptada y medida (I-2): el término llega; la tabla no es el nivel

**2026-09-16, noche.** Tag `v2.19.1` tomado (`gradle/libs.versions.toml`, sin cambios de app, `params` idéntico).
Siete sondas × 0/0 y 1/1 en el Moto G42, rack en bypass, acordes OFF, 4 octavas; "antes" (2.19.0) y "después"
por el mismo guion, leídos con **su ventana** (régimen = RMS 0,1–0,4 s tras el note-on; note-off = cruce de la
recta de release; cola = RMS 50 ms centrada en note-off + 0,3 s). Q por zona leído del font con nuestro
`gens.py` (cuatro capas, SF2 §8.5/§9.4). Doc completo: `docs/audio-bump/req_bump_audio_2_19_1.md`.

## 1 · Las dos sondas de Q alto que pidieron, y por qué son éstas

- El **hit de kit con Q 190** (bank 120) no existe desde la app: en `120:0` esas regiones son las teclas **80/81**
  (triángulo) y el DrumGrid toca el mapa GM 4×4 (36…56, 37, 40, 41, 47). El único bombo alcanzable con Q alto es
  `120:26` Dance Drums (Q 117), y esa capa está **20 dB atenuada** bajo otra con Q 52: no mide nada.
- En su lugar, **`0:10` Music Box**: dos capas con **Q = 170 cB, atten 0, Fc abierta** ⇒ mide el término 1/√q
  limpio, sin que el corte intervenga. Esperado por su tabla: **−7,0**.
- **`12:127` Shooting Star** tal como la pidieron (ruido blanco, Fc 199 Hz, Q 960 ⇒ clip 96 ⇒ **−46,5**).

## 2 · La tabla (régimen dBFS, ventana motor; Δ = 2.19.1 − 2.19.0)

| sonda | tecla · vel | Q (cB) | Fc efectiva | **esperado** | **Δ 0/0** | Δ 1/1 (control) |
|---|---|---|---|---|---|---|
| Saw Lead 0:81 | 60 · 1,0 | 0 | 20 kHz | +1,5 | **+1,3** | +1,5 |
| Stereo Strings Fast 0:48 | 60 · 1,0 | 0 | 19,9 kHz | +1,5 | **+1,7** | +1,5 |
| Trumpet 0:56 | 72 · 0,96 | 0 | 2,2 kHz + modEnv 3600 c | +1,5 | **+1,4** | +1,4 |
| Stereo Grand 0:0 | 72 · 0,96 | 0 | 798 Hz + modEnv 4500 c | +1,5 | **+1,3** | +1,3 |
| Music Box 0:10 | 60 · 1,0 | 170 | abierta | −7,0 | **−7,4** | −7,5 |
| **Warm Pad 0:89** | 60 · 1,0 | 0 | **600 Hz** + modEnv 500 c | +1,5 | **−0,1** | +0,1 |
| **Trumpet 0:56** | 72 · **0,33** | 0 | 2,2 kHz − vel→Fc | +1,5 | **+0,1** | +0,1 |
| **Stereo Grand 0:0** | 72 · **0,33** | 20 | **≈ 634 Hz** | +0,5 | **−1,3** | −1,3 |
| **Shooting Star 12:127** | 60 · 1,0 | 960 | 199 Hz | −46,5 | **−38,0** | −36,9 |

Absolutos 0/0 en 2.19.1 (la nueva línea de base): Saw −22,4 · Strings −19,9 · Warm Pad −24,2 · Trumpet −31,9 /
−18,1 · Grand −37,3 / −21,7 · Music Box −27,0 · Shooting Star −43,8 dBFS (piso de la toma −66,9: es señal).

## 3 · Lectura: el término llega; lo que su tabla no predice es la forma del filtro

Cinco de nueve dentro de ± 0,4 dB de su número, incluido el Q 170. Las cuatro que no, no son ruido: son
**exactamente las voces cuyo espectro vive cerca o encima del corte**, y se apartan en el sentido que su propio
punto 1 anuncia. A Q = 0 el filtro pasó a Butterworth y **desapareció la joroba de +3 dB cerca del corte que tsf
tenía**: para Warm Pad (600 Hz), Trumpet a velocity 42 y el Grand suave (≈ 634 Hz) eso vale −1,5…−1,8 y se
come el +1,5 del término; se ve en el brillo: **Grand fuerte 645 → 605 Hz y Warm Pad 342 → 320 Hz (−6 % de
centroide)**, mientras el Grand suave queda igual (530 → 530: su energía está debajo del corte, sólo pierde
nivel). En el otro extremo, con Q 960 el **pico de resonancia** devuelve parte del ruido blanco de Shooting Star:
−38 en vez de −46,5. Ataque de Trumpet (5…50 ms) sin cambio; `cola − régimen` con 1/1 igual que en 2.19.0
(Saw −33,5, Trumpet −34,0 / −33,9): los sends no se tocaron. Y `Δ 1/1 = Δ 0/0 ± 0,3` en las nueve filas.

**Lo que les pedimos, en una línea:** que la nota de bump diga que la columna "vs 2.19.0" es el **término
1/√q por región**, y que el nivel de una voz con el corte cerrado baja además lo de la joroba (−1,5…−2 medido
acá; su host puede darlo exacto por zona). Con eso el umbral de |Δ_device − Δ_host| > 1 dB vuelve a servir. Si
en su render de FluidSynth Warm Pad 60·127 con 0/0 sube +1,5 respecto de 2.19.0, **eso sí sería hallazgo**;
nosotros medimos −0,1 dos veces (0/0 y 1/1).

Desde acá **2.19.1 es el "antes"** de todo lo que siga. Sin superficie nueva, sin cambios de app: un PR de toml.

---

## Lo que verificamos de nuestro lado (2026-09-16, noche)

Tres instrumentos, todos en host, todos reproducibles:

- **I-5 rendido**: `sf_render_preset` (el mismo arnés de la conformidad) construido sobre el tag `v2.19.0`
  (worktree aparte; el tool nació en #324, después del tag, y se injertó sin tocar el motor) y sobre
  `master` (2.19.1), las nueve sondas con 0/0 y con 1/1, régimen = RMS 0,1–0,4 s tras el note-on, sobre
  el `.sf3` que shippean. Shooting Star (bank 12) entra con un CC0 antes del program change.
- **Oráculo FluidSynth 2.6.0** (`-R 0 -C 0 -g 1`, **CC7 = 127 explícito** en el `.mid`): sobre un `.sf2`
  mínimo con el sample real y la zona exacta del preset (`scripts/sf-preset-to-minimal-sf2.py`, la receta
  del 15/09 generalizada), con control de identidad: nuestro motor rinde el mínimo y el `.sf3` **al 0,01 dB**.
- **Espectro por bandas** (Hann, 1/3 de octava aprox.) para separar "es el filtro" de "es una ganancia".

### REQ-041 S1 — I-2 evaluado con el I-5 rendido: **VIVO, N = 8** (una de Q alto)

| sonda | Q | fórmula | Δ_host rendido 0/0 | Δ_device 0/0 | device − host | device − fórmula |
|---|---|---|---|---|---|---|
| Saw Lead 60·127 | 0 | +1,5 | **+1,50** | +1,3 | −0,20 | −0,20 |
| Strings Fast 60·127 | 0 | +1,5 | **+1,50** | +1,7 | +0,20 | +0,20 |
| Trumpet 72·122 | 0 | +1,5 | **+1,46** | +1,4 | −0,06 | −0,10 |
| Grand 72·122 | 0 | +1,5 | **+1,48** | +1,3 | −0,18 | −0,20 |
| Music Box 60·127 | 170 | −7,0 | **−6,99** | −7,4 | **−0,41** | −0,40 |
| Warm Pad 60·127 | 0 | +1,5 | **−0,00** | −0,1 | −0,10 | −1,60 |
| Trumpet 72·42 | 0 | +1,5 | **+0,29** | +0,1 | −0,19 | −1,40 |
| Grand 72·42 | 20 | +0,5 | **−0,96** | −1,3 | −0,34 | −1,80 |
| Shooting Star 60·127 | 960 | −46,5 | −46,60 | −38,0 | +8,60 | +8,50 |

- **Umbral** |Δ_device − Δ_host| > 1 dB: **máximo 0,41 dB** (Music Box, la de Q alto). Las ocho filas con
  línea de base sana pasan; el Δ con 1/1 del host coincide con el de 0/0 al 0,06 en todas (los sends no
  se tocaron, como dicen).
- **Su lectura es correcta y la nota de bump estaba mal**: la fórmula `+1,505 − Q/2` es el término 1/√q y
  sólo predice el nivel donde la energía de la voz está en la banda de paso. Donde el espectro vive sobre
  el corte, 2.19.0 tenía además la joroba de tsf a Q = 0 (q = 1: 0 dB en fc y +1,25 de pico, contra −3,01 en fc del
  Butterworth) y 2.19.1 no; el host **rendido** lo ve (Warm Pad −0,00, Trumpet 72·42 +0,29, Grand 72·42
  −0,96) y el device también. El criterio de muerte decía que la fórmula era "equivalente al 0,1 dB por
  AC-041.1": es falso fuera de la banda de paso, y se corrige en la spec y en la nota de bump. **I-5 es
  el render, no la fórmula.**
- **Shooting Star no cuenta en N** y no es hallazgo: en host **2.19.0 rinde el régimen a +10,6 dBFS** (el
  resonador de 96 dB sobre ruido blanco satura en float) y 2.19.1 a −36,0. En device el "antes" no pudo
  pasar de 0 dBFS: su Δ está comprimido por el recorte de la toma vieja, ~8,6 dB. Con 2.19.1 como "antes"
  la sonda vuelve a servir.
- **Brillo (umbral S1)**: centroide del Grand suave 72·42 en host 535 → 533 Hz (−0,4 %); ellos 530 → 530.
  Verde. Warm Pad host 382 → 357 (−6,5 %) contra sus 342 → 320 (−6,4 %): el mismo movimiento.
  (Grand fuerte: host 581 → 576, ellos 645 → 605; estimadores distintos —los absolutos no coinciden— y no
  es criterio.)

### La pregunta de Warm Pad, contestada con el oráculo: **2.19.1 es FluidSynth**

`0:89` k60 v127, seco, nota de 3 s (el preset tiene `attackVolEnv` −386 tc = **0,8 s de ataque**: la
ventana 0,1–0,4 s compara ataques, así que se mide también en el hold):

| | 0,1–0,4 s | 0,5–1,0 s | 1,5–2,5 s | centroide (1,5–1,8 s) |
|---|---|---|---|---|
| motor 2.19.0 | −24,80 | −19,13 | −17,05 | 475 Hz |
| motor 2.19.1 | −24,81 | −19,32 | **−17,20** | 450 Hz |
| FluidSynth 2.6.0 | −24,86 | −19,32 | **−17,20** | 449 Hz |

FluidSynth **no** sube +1,5 respecto de 2.19.0: da −0,15 en el hold, lo mismo que 2.19.1. El "−0,1 en vez de
+1,5" que midieron es el comportamiento correcto; en 2.19.0 la joroba y el término faltante se compensaban
de casualidad en ese preset. Trumpet 72·42: 2.19.1 a 0,07–0,10 dB de FluidSynth. Saw Lead: 0,01–0,05.

🔴 Trampa que costó 4 dB: sin `CC7` en el `.mid`, FluidSynth arranca con el default GM **CC7 = 100** y su
modulador #3 lo aplica como −4,15 dB (= 40·log10(100/127)); nuestro arnés arranca en 127. Toda comparación
ABSOLUTA con FluidSynth lleva el CC7 explícito.

### El hallazgo que sí hay, y no es de REQ-041: el Grand suave está 1,7 dB por debajo de FluidSynth

`0:0` k72 v42 (Q 20 cB, corte ≈ 634 Hz): 2.19.1 −32,93 / −48,93 / −51,44 dBFS contra FluidSynth −31,26 /
−47,11 / −49,24 en las tres ventanas, mismo centroide (526). **La diferencia es plana en todas las bandas**
(−1,6 a −2,3 dB a 50–200 Hz, lejos del corte, igual que arriba): no es el filtro, es una ganancia. S1 la
achicó +0,5 (la fórmula); el −1,7 restante es previo.

Aislado con tres mutaciones del font mínimo (los dos motores, mismo `.mid`):

| variante | motor 2.19.1 | FluidSynth | Δ |
|---|---|---|---|
| el font (inst 40 + preset **−80** = neto **−40 cB**; default #1 anulado en el inst y re-puesto a 840 en el preset) | −32,93 | −31,26 | **−1,67** |
| E1: sin el 840 del preset ni el 0 del inst (default #1 = 960 en los dos) | −35,34 | −33,66 | −1,68 |
| E2: preset −80 → +80 (neto +120 cB) | −37,73 | −37,66 | −0,07 |
| E3: preset −80 → −40 (neto 0) | −32,93 | −32,86 | −0,07 |

Con neto 0 los dos coinciden; con neto −40 nuestro render es **idéntico** al de neto 0 (−32,93 las dos veces)
y FluidSynth sube +1,6 dB = 40 cB × 0,4. Mecanismo, en las dos fuentes:

- **tsf** (`tsf.h`, `GEN_FLOAT_LIMITATTN`): compone preset + instrumento, multiplica por 0,4 (MINI-024) y
  **recorta en 0 el generador solo**; los moduladores se suman después, en `noteGainDB`.
- **FluidSynth 2.6.0** (`fluid_defsfont.c:1465` `EMU_ATTENUATION_FACTOR`, `fluid_voice.c:790`): escala cada
  nivel por 0,4, suma preset sobre instrumento (`fluid_voice_gen_incr`), **suma los moduladores** y recién
  ahí `fluid_clip(0, 1440)`. Un neto negativo es un refuerzo que se cobra contra la atenuación por velocity:
  a v42 (168 cB de mod) quedan 152 en FluidSynth y 168 en tsf; a v122 (6 cB) los dos recortan a 0 — por eso
  las sondas fuertes cerraron con la fórmula y ésta no.

Alcance en GeneralUser 1.471, compuesto como tsf compone (local reemplaza a global en cada nivel; preset
suma a instrumento; rangos cruzados): **570 regiones con atenuación neta < 0 en 39 presets**. En las diez
baterías (120/128) son **los crashes (teclas 49 y 57) por capa de velocity**: ≤ 41 neto −120 cB = **4,8 dB**
de refuerzo perdido, 42–51 −70 (2,8), 52–61 y 109–127 −20 (0,8); Xylophone (vel ≤ 51, todo el teclado) y
Clavinet (vel ≤ 81) −80 = 3,2; la capa suave (vel ≤ 49) de Stereo/Bright Grand y de los Piano & … de bank
11/12 −40…−60 = 1,6…2,4; Brass Section / Shamisen / Shenai 2,0. Lo que se pierde en cada nota es
min(refuerzo, atenuación por velocity): nada en las fuertes, el refuerzo entero en las suaves.
**MINI-031** (`draft`), preexistente a 2.19.1 y a REQ-041; no cambia el veredicto de S1.

### Lo que sigue

- Nota de bump de 2.19.1 corregida (columna "vs 2.19.0" = término por región; la joroba; el I-5 rendido).
- Spec de REQ-041: I-5 = render (la fórmula sólo en banda de paso); I-2 evaluado VIVO N = 8.
- `scripts/sf-preset-to-minimal-sf2.py` versionado (el oráculo), `scripts/sf-delta-host.py --bank`.
- MINI-031 en draft con la evidencia de arriba; la corrección va en un `fix(sf)` con nota de bump por preset.
