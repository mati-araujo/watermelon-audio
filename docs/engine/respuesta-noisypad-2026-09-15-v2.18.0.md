---
title: "Aviso a NoisyPad — v2.18.0: el font tiene cola, el brillo que estaba inerte, y los ocho destinos (con la respuesta a su carta de 2.17.2/3/4)"
type: reference
status: current
created: 2026-09-15
---

# Aviso a NoisyPad — 2026-09-15 · v2.18.0, y la respuesta a su carta

**watermelon-audio → NoisyPad.** Un solo envío para dos cosas: la **respuesta a su carta del 15/09**
(2.17.2, 2.17.3, 2.17.4 y la corrección de la tarde; §0) y el **aviso de v2.18.0** (§1–§6), que ya
está publicada (4/4 coordenadas en el registro). Absorbe el acuse de 2.17.2/3 que se había redactado
aparte (`respuesta-noisypad-2026-09-15-acuse-v2.17.3.md`): no se manda por separado. Es un
**minor** con tres cambios de sonido en el SoundFont, dos de los cuales se oyen sin buscarlos.
Léanlo antes de subir el bump; y esta vez **su arnés va a medir distinto en casi todos los
presets**, por diseño.

> **Enviado el 2026-09-15. Contestado el 15/09 a la noche**:
> `carta-noisypad-2026-09-15-respuesta-v2.18.0.md` — adoptaron 2.18.0, retiraron el "hallazgo" del fix
> 2, verificaron la cola, el brillo y el ataque con número, hicieron dos preguntas (contestadas al final
> de esa carta: ninguna es un hallazgo; el +2,5 dB del Grand suave es la resonancia sobre la fundamental,
> con un dB de diferencia contra el spec que es de REQ-041) y **pidieron la perilla con carta**.

## 0 · Su carta, cotejada: fix 1 cierra, fix 2 es FluidSynth, y tres matices de 2.17.3

Cada número contra el archivo (las cuatro capas de GeneralUser 1.471), contra el motor en el host y,
donde hacía falta, contra FluidSynth 2.6.0. Detalle en `carta-noisypad-2026-09-15-respuesta-v2.17.4.md`
y `…-v2.17.2-v2.17.3.md`, al final de cada una.

**Fix 1 (100 dB) ✅, y bien medido.** Los t40 de 2.17.4 son 0,4 × la release efectiva en los cinco
(exactos en Synth Bass 1, FM EP y Stereo Grand), y los de 2.17.3, 40/80,13 ×. Bien por resolver las
cuatro capas: es lo que nos pasó a nosotros el 14/09 con el default #1. **Queda una regla más**: dentro
de un mismo nivel la zona local **reemplaza** a la global; sólo *entre* niveles se suma. Stereo Grand
no es 0,89 s (−746 − 475 suma las dos zonas del preset): es 1018 (zona del instrumento) − 475 (la local
del preset pisa la global) = 543 tc = **1,368 s** — y su propio t40 lo dice: 550 ms = 0,4 × 1,368 en
2.17.4, 680 = 40/80,13 × 1,368 en 2.17.3. Warm Pad 4,53 s y Saw Lead 0,5 s cierran como los escribieron.

**Fix 2: 🔴 su hipótesis es falsa, con número.** Rendimos `8:63 Synth Brass 4`, tecla 48, velocity
119, con **FluidSynth 2.6.0 `-R 0 -C 0` sobre la zona exacta** (FluidSynth no abre el `.sf3`; se
construyó un `.sf2` mínimo con el sample real y las cuatro capas tal cual, y nuestro motor rinde ese
`.sf2` y el `.sf3` idénticos al 0,1 dB) y aplicamos su métrica (`hf.py`: energía > 1,5 kHz relativa,
ventanas de 5 ms desde el onset):

| render | 0–5 | 5–10 | 10–15 | 15–20 | 20–25 | 25–60 | ≥ 60 |
|---|---|---|---|---|---|---|---|
| **FluidSynth 2.6.0** | **−20,6** | −22,6 | −19,4 | −22,4 | −24,0 | −16,3 | −19,8 |
| 2.17.4 | **−18,8** | −20,3 | −27,9 | −20,7 | −20,3 | −17,5 | −21,0 |
| 2.17.3 | −29,3 | −34,4 | −29,8 | −26,3 | −20,9 | −17,7 | −21,0 |
| control cerrado (`modEnvToFilterFc` = 0), FluidSynth | −34,4 | −43,1 | −40,1 | −43,1 | −50,2 | −38,0 | −41,2 |

FluidSynth **está abierto desde la primera ventana de 5 ms, igual que 2.17.4** (con ventanas de 10 ms,
a menos de 2 dB en las cinco primeras; "cerrado" en esta métrica es −40, la fila de control). Y con un ataque de 3 s (donde una rampa lineal a los 60 ms sigue en 530 Hz, cerrado, −40)
FluidSynth está en **−22,3** y nosotros en −21,2: **el ataque de la mod env al filtro es convexo en
FluidSynth**, no lineal. Está en su código: `fluid_rvoice.c:369-370` computa
`modenv_val = fluid_convex(127 · modenv.val)` durante el ataque y la **misma** `modenv_val` entra al
pitch (l. 380) y al filtro (l. 447, `fluid_iir_filter_calc`). La rampa lineal de `fluid_adsr_env` es
la *entrada* de la curva. Lo que su hipótesis leyó mal es la tabla: `fluid_convex` al 6 % del tiempo
vale ≈ 0,48, no 0,99. Lo que sí es cierto: con 7087 c desde 488 Hz la convexa cruza 1,5 kHz a los
5,4 ms y 3,8 kHz a los 19 ms (la lineal, a los 82 y 150), y su métrica satura arriba de ~3 kHz, así
que los 280 ms restantes del ataque son invisibles para `hf.py` y casi para el oído. **Un ataque de
300 ms convexo suena a 20–50 ms; es lo que el spec pide (§9.1.7) y lo que el autor programó contra
FluidSynth.** 2.17.3 abría *más tarde* que la referencia porque escalaba por velocity y era lineal:
el SFZ-ismo. Sin MINI; los WAV de Synth Brass no hacen falta. Su elección de sonda es la correcta:
`8:63` con esta tabla y FluidSynth como fila de referencia.

**2.17.3, tres matices que el archivo agrega** (el acuse que no salió, en corto):

1. **El bombo de concierto es dos capas**: `Taiko Drum` (−780, sin atenuación) sobre `Timpani Hard`
   (−810, en cuatro capas de velocity 150/100/50/0 cB). El −780 exacto es el Taiko dominando; los
   −775/−745 son lo que una correlación sobre dos desplazamientos puede dar. No es del motor.
2. **Su keytrack "ya estaba bien" viene un 2 % corto, y es consistente**: diez de diez a 0,975–0,983
   (media 0,981), 39,2 c por tecla donde el archivo declara 40. No es el motor (host exacto al cent,
   fórmula cotejada con FluidSynth a 0,00 c); nuestra hipótesis es la correlación del espectro entero
   sobre dos capas con un `initialFilterFc` que no sigue a la tecla. **Se cierra con dos WAV**: la 36 y
   la 51 del bombo en 2.17.1. Los demás no hacen falta.
3. **El Wood Block va al revés**: 76 → −1319, 77 → −1594 (y cada tecla apila un `_2` a −75/−350). Sin
   consecuencia: no lo alcanzan desde el Drum Grid.

**Y un dato de su doc del bump que sí nos toca**: *"un golpe de sacrificio (el motor recorta el
primero ~0,1 s)"* al grabar en el looper. Si es reproducible con número —cuánto, y si se cuenta desde
`Record` o desde el primer note-on— es un disparador nuestro con nombre, no un detalle del arnés.

## 1 · Lo que cambia, en tres frases

1. **El font tiene cola (REQ-040).** Un SoundFont declara por zona cuánto de cada voz manda a una
   reverb y a un chorus, y espera que el sintetizador tenga esas dos unidades; tsf no las tenía,
   así que todo salía seco. Ahora el motor lleva **dos unidades fijas adentro del sintetizador**
   (un Freeverb clásico y un chorus de tres voces, con los parámetros por default de FluidSynth
   2.6.0) y cada voz les manda lo que el archivo dice, más el default del spec con CC91 en su
   reset de GM: **+6,3 % de reverb a toda voz**. El wet sale sumado en la salida del instrumento:
   pasa por su rack, por el fade y por la grabación del looper como el dry. **225 de 269 presets**
   declaran reverb (3–21 % típico) y **140** chorus (Saw Lead 20 %, órganos 22 %, pads 14 %).
2. **El velocity → brillo que 2.17.2 "arregló" estaba inerte en 3380 de 12 311 regiones, en 111
   presets (MINI-027, `fix`).** En toda región con envolvente o LFO al filtro, el render pisaba en
   el primer bloque lo que el note-on había escrito: Stereo Grand, los bronces, las strings, los
   pads, los kits. Desde esta release, **una nota suave suena más oscura en esos 111 presets, como
   lo afinó el autor** (Stereo Grand: −3800 c a velocity 0, −1900 a velocity 64). A velocity 1,0 —el
   XY— no cambia nada.
3. **Los ocho destinos de modulador que faltaban (MINI-027, `feat`)**: velocity → ataque/decay/
   release de volumen, ataque del mod env, envolvente al filtro, Q, offset del sample y pan por
   tecla. 24 moduladores en 13 presets, sobre todo los **bronces**: *toque suave = ataque lento*
   (`0:56 Trumpet` ×5,7 a velocity 0; `0:57 Trombone` ×10).

Las notas completas, con la tabla por preset: `nota-de-bump-2026-09-15-los-sends-de-reverb-y-chorus.md`
y `nota-de-bump-2026-09-15-los-ocho-destinos-de-note-on.md`.

## 2 · Lo que van a medir distinto

- **Todas sus tomas con SoundFont llevan ahora cola**, aun con el rack en bypass: niveles y
  centroides se mueven en 225 presets respecto de 2.17.4. La referencia para separar el send del
  resto es FluidSynth `-R 1 -C 1` con `synth.reverb.room-size=0.2 damp=0 width=0.5 level=0.9` y
  `chorus.nr=3 level=2.0 speed=0.3 depth=8` (la receta está en `scripts/render-spec-reference.sh`).
- La cola **es del cuarto**: sigue ~1 s tras soltar todo y tras cambiar de preset; se corta con el
  `reset()` del engine, al cambiar de font y sin font. Con 2 s de silencio se apaga sola (cero
  exacto).
- Costo medido en host (−O0, bloque de 128 a 48 kHz, 8 voces): 17 µs por bloque, 0,65 % del tiempo
  real; dormida, el 5 % de eso.
- 🔴 **Sus tandas con cola ya no comparan contra 2.17.x**: la línea de base de `tanda.sh` /
  `tanda-env.sh` se toma de nuevo en 2.18.0, y hay dos efectos distintos que separar:
  - **La reverb** (225 presets + el 6,3 % del CC91 en todos): la primera reflexión llega a los
    **≥ 25 ms** (el comb más corto, 1116 muestras a 44,1 kHz, escalado a su rate) y es chica (−24 dB
    el default; −30…−14 dB el send del preset). No toca las ventanas de onset de `hf.py` hasta
    25 ms ni el pitch. **Sí mata los t40/t60 como medida de la release**: cuando el dry cae 40 dB lo
    que queda es el cuarto, y el t60 pasa a ser el de la cola.
  - **El chorus** (140 presets; `8:63 Synth Brass 4` manda 27,8 %, Saw Lead 20 %, órganos 22 %):
    es una copia de la misma voz **retrasada 12–20 ms** y modulada en pitch (±13 c a 0,3 Hz), a
    `level 2,0 × send`. Sube el **nivel de régimen** (en #18 A del spec-test: +0,4 / +1,1 / +2,8 dB
    a 33 / 66 / 100 % de send; depende de la fase dry/wet en la ventana), y puede ensanchar un pitch
    por correlación. La relación `hf.py` casi no se mueve (mismo espectro, retrasado).
  - Lo que mide envolventes de nivel se compara contra FluidSynth con `-R 1 -C 1` (la receta de
    arriba), o se espera a la perilla (§3).

## 3 · Nada que adoptar, y nada que apagar (todavía)

Cero superficie nueva entre 2.17.4 y 2.18.0 (`git diff` vacío sobre `watermelon_audio.h`,
`IAudioNativeBridge`, `ISoundFontBridge`). La ambiencia del font **no tiene perilla**: es el sonido
del font, como suena en FluidSynth. La costura interna para atenuarla o apagarla ya existe; la
perilla pública es **un REQ de un día si la piden con carta** — para A/B, o porque un usuario la
quiera apagar cuando ya tiene la reverb del rack.

## 4 · Qué tan igual a FluidSynth, con número

Sobre el SoundFont-Spec-Test contra su render con efectos: la escalera de **reverb** por
generador a **0,23 dB** por nota; la de **chorus** con **1,54 dB** de residuo declarado —la
referencia no es monótona (al 100 % el dry y el wet se cancelan en la ventana) y la nuestra sí;
es la estructura del chorus, que no es la de FluidSynth (un FDN modulado, LGPL). CC91/CC93 en
vuelo no se siguen (no hay superficie de CC). Las otras 17 pruebas F no se movieron.

## 5 · Cómo verificarlo

- **La cola**: `0:89 Warm Pad` o `0:48 Stereo Strings Fast` (14 % + 6,3 %), una apoyada corta en
  el XY y soltar: con 2.17.4 el sonido para en la release; con 2.18.0 queda ~1 s de cola.
- **El brillo**: `0:0 Stereo Grand` en el Note Grid, la misma tecla suave (≈ 0,35) y fuerte (≈ 0,96):
  el centroide del golpe suave **baja** respecto de 2.17.4; el fuerte y el XY no (± 3 %).
- **El ataque**: `0:56 Trumpet`, golpe suave: el ataque se estira (×3,1 a velocity 44).

## 6 · La pregunta

**¿Algún preset suena con demasiada cola o con chorus donde no lo esperaban? ¿Alguna nota suave
demasiado oscura, un ataque de bronce demasiado lento? ¿Y quieren la perilla?** Lo primero va con
nombre, tecla y velocity y se mide contra FluidSynth `-R 1 -C 1`; lo último es un día de trabajo,
con carta. Y de §0 quedan los dos WAV del bombo (36 y 51 en 2.17.1) y el número del golpe que el
looper recorta.
