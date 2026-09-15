---
title: "Aviso a NoisyPad — v2.18.0: el font tiene cola, el brillo que estaba inerte, y los ocho destinos"
type: reference
status: current
created: 2026-09-15
---

# Aviso a NoisyPad — 2026-09-15 · v2.18.0

**watermelon-audio → NoisyPad.** Sigue al aviso de v2.17.4 (las envolventes; sin respuesta
todavía) y al acuse de su carta de v2.17.2/3 (`respuesta-noisypad-2026-09-15-acuse-v2.17.3.md`).
Es un **minor** con tres cambios de sonido en el SoundFont, dos de los cuales se oyen sin
buscarlos. Léanlo antes de subir el bump; y esta vez **su arnés va a medir distinto en casi todos
los presets**, por diseño.

> **Redactado el 2026-09-15, sin enviar.** La release sale después de mergear REQ-040 S3.

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
con carta. Y siguen pendientes los dos WAV del bombo y el número del golpe que el looper recorta,
del acuse del 15/09.
