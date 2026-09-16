---
title: "Aviso a NoisyPad — v2.19.0: la perilla de la ambiencia del font (y los dos cierres pendientes)"
type: reference
status: current
created: 2026-09-15
---

# Aviso a NoisyPad — 2026-09-15 · v2.19.0

**watermelon-audio → NoisyPad.** Sigue al acuse del looper y el bombo (`…-acuse-looper-y-bombo.md`,
que ya tienen). Es un **minor** con **una superficie nueva** —la perilla que pidieron con carta— y
**ningún cambio de sonido por defecto**.

> **Redactado el 2026-09-15; enviado por chat el 2026-09-16**, tras verificar `v2.19.0` en el registro
> (4/4 coordenadas, control 9.9.9 → 404). Su respuesta es **I-1** del criterio de muerte de REQ-042.
> **Contestado el 16/09 a la tarde** (`carta-noisypad-2026-09-16-respuesta-v2.19.0.md`): adoptaron 2.19.0,
> AC-1..AC-7 con número, la perilla deja el seco (I-1 de REQ-042 evaluado, N = 3, vivo), el pop no se oye
> con instrumento de banda alta, y la tabla de colas con tecla/velocity para REQ-040 — que dio I-1 rojo
> contra el host (F-2: se piden los WAV crudos antes de tocar nada).

## 1 · Lo que hay

`sfSetAmbience(reverb: Float, chorus: Float)`, `sfGetAmbienceReverb()`, `sfGetAmbienceChorus()` en
`ISoundFontBridge` (C API `wma_sf_set_ambience` + dos getters; Android por JNI, iOS por cinterop).
Por instancia, sin CC. **0..1 lineal sobre la amplitud del send** (0,5 = −6 dB, 0,1 = −20 dB), default
**1/1** (= FluidSynth = 2.18.0). Escala **todos** los sends —generador y default #8/#9 con CC91/CC93 en
reset— **antes** de las unidades: con 0/0 el render es, muestra a muestra, el font seco. **Es un ajuste
del instrumento, no del font**: sobrevive a `reset()`, al cambio y la descarga de font; se fija una vez
y queda. Fuera de rango satura; `NaN` deja ese bus como estaba y avisa por el registro. Thread de
control (no RT-safe por ese aviso). Lo que NO trae: parámetros de las unidades, CC91/CC93 en vuelo,
apagado del DSP en 0/0 (la compuerta de 2 s ya lo hace). Nota completa:
`nota-de-bump-2026-09-15-la-perilla-de-la-ambiencia.md`.

## 2 · Lo medido

0/0 ≡ seco muestra a muestra (con control positivo); 1/1 ≡ 2.18.0 byte a byte; linealidad a 3·10⁻⁸;
el **escalón 0/0 → 1/1 en caliente** medido antes de decidir rampa, contra el transitorio del propio
note-on en 110 / 220 / 440 / 873 Hz: sin rampa, conmutar era 10,3 / 6,8 / 10,2 / 1,2× más brusco que
tocar la nota ⇒ **rampa de 5 ms** (tiempo fijo, por muestra, sólo en el cambio en caliente: un set
antes de arrancar aplica de una, así que 0/0 es el seco desde la primera muestra); con rampa 0,94 /
0,94 / 0,95 / 0,90×. Los getters devuelven el objetivo, no el valor en tránsito. Persistencia
afirmada por `reset()` + swap + `prepare()`. Spec-test 18 F · 11 S · 15 R; arnés JNI 126 → 129 de 318.

## 3 · Para su toggle y su arnés

Un toggle "ambiencia del font" = `sfSetAmbience(1f, 1f)` / `(0f, 0f)`, aplicado una vez al arrancar
desde lo que persistan; los getters para afirmar el estado antes de medir. Con 0/0, **su línea de base
seca de 2.19.0 contra 2.17.4** tiene que dar niveles y colas iguales en todo salvo lo que MINI-027
cambió (el corte modulado por velocity en 111 presets).

## 4 · La pregunta

**¿El A/B en 0/0 cierra contra 2.17.4 en sus sondas (Saw Lead, Strings, Warm Pad, Grand, Trumpet)?**
Concretamente: a +0,3 s del note-off, con 0/0, ¿algún preset sin filtro modulado queda más de 3 dB
arriba de 2.17.4? Es nuestro criterio de muerte de la perilla, y el instrumento es su `tanda-fx.sh`:
si no lo corren, el criterio queda sin evaluar (no verde). Y si conmutar en caliente **se oye** (un pop
al tocar el toggle con una nota sonando), ese es el dato que nuestro número no vio.

## 5 · Nada más pendiente

Los dos cierres anteriores (el looper es su `findContentBounds(0,03)`; el bombo mide +600 c exactos)
ya están en el acuse anterior. De ustedes, nada salvo el A/B cuando suban.
