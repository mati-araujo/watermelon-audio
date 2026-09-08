---
title: "Respuesta a Tunio — el «sesgo por cuerda» que reportaron en agosto tiene explicación, y no es del motor"
type: reference
status: current
created: 2026-09-08
---

# El «sesgo por cuerda» de agosto tiene explicación — y no es del motor

**2026-09-08 · sobre su reporte de agosto (G3 ≈ −3 c, E4 ≈ +2,6 c en hardware) · watermelon-audio → Tunio**

Quedó sin dueño desde agosto. Lo abrimos como REQ-035 con la hipótesis de que el strobe se apartaba
del fundamental sobre cuerdas reales —nuestro propio corpus daba hasta +5,40 c en `ukelele_C4`— y
la medición la refutó entera. Tres cosas: qué medimos, qué era, y qué significa para cómo comparan.

---

## 1. Qué medimos

Sobre los 41 archivos del corpus (`corpus-v1`, GeneralUser GS 2.0.3 renderizado con FluidSynth, el
mismo banco que ustedes), con el instrumento declarado, leímos en la misma publicación de la que sale
la lectura fina **los cuatro parciales del strobe por separado**, el subconjunto que entró al ajuste
y **el objetivo real contra el que el strobe estaba midiendo**, y los comparamos con un oráculo por
parcial (Goertzel + Hann, otro método) en la misma ventana de tiempo.

## 2. Qué era

En los archivos con "error" los cuatro parciales coincidían entre sí (`ukelele_C4`: +5,48 / +5,33 /
+5,23 / +5,40) y el oráculo los ponía a 0,00. No era el ajuste, no era el sample, no era el rastreo
de fase: **el strobe no estaba midiendo contra la altura verdadera del archivo, sino contra la
cuerda del catálogo**. Con candidatos, el modo rápido reengancha el objetivo a la cuerda que
eligió —el nominal temperado, 261,626 Hz para C4— y los cents publicados son relativos a ella. El
`+5,40` era la desafinación del propio sample respecto del temperamento igual (ese C4 del SoundFont
está a +5,78 c del nominal), leída por un afinador que hace exactamente lo que tiene que hacer.

Convertida la lectura a Hz absolutos (`objetivo · 2^(cents/1200)`) y comparada con el oráculo:

| | antes (cents publicados vs oráculo absoluto) | ahora (Hz absolutos vs oráculo) |
|---|---|---|
| `ukelele_C4` | +5,40 | **−0,37** |
| `guitarra-nylon_E4` | +4,28 | **+0,03** |
| `guitarra-jazz_E4` | +2,99 | **−0,10** |
| `guitarra-jazz_G3` | −2,03 | **−0,05** |
| máximo sobre los 39 convergidos | 5,40 | **0,73** (`guitarra-limpia_G3`, por su glide de ataque) |

Los dos archivos que quedan afuera de 1 c no están convergidos y lo declaran con σ: `bajo-acustico_G2`
(−1,61, σ 0,91: un glide de ataque de −27,7 c que se estabiliza recién a 1,0 s) y
`guitarra-acero_A2` (−3,98, σ 4,88: su cuarto parcial REAL está a −16 c de la serie estirada, y el
ajuste se niega a converger — es REQ-027 haciendo su trabajo).

## 3. Qué significa para ustedes

- **Los cents del snapshot son relativos a la cuerda enganchada** (con candidatos) o al objetivo que
  ustedes pusieron (sin candidatos), y `targetHz` dice cuál es. Nunca a la altura "verdadera" del sonido. Si comparan la
  lectura contra una referencia absoluta —un analizador espectral, otro afinador en modo cromático,
  un oráculo sobre el WAV— van a ver exactamente la desafinación del sample respecto del
  temperamento, con el signo y la magnitud de su reporte de agosto: en nuestro corpus, `jazz_G3` está
  a −1,98 c del nominal y `jazz_E4` a +3,10; `nylon_G3` a +1,73 y `nylon_E4` a +4,25. Su "G3 ≈ −3,
  E4 ≈ +2,6" es la firma de eso, no de un sesgo del motor.
- Para medir el error del motor hay que llevar la lectura a Hz absolutos con `targetHz` (lo que la
  C API expone) y compararla con la referencia. Es lo que ahora hace nuestro barrido, y el presupuesto
  del corpus pasa de 6 c a **1 c** sobre toda lectura mostrada, con los dos archivos de arriba
  declarados con su mecanismo como trinquete (si mejoran, el test se pone rojo para sacarlos).
- **No cambió nada del motor** en este REQ. Lo que cambió es el instrumento de medición y el
  presupuesto que afirma.
- La nota de REQ-033 les daba `guitarra-limpia_E4` "convergida −0,66 c" y `limpia_G3` "−1,76 c":
  esos también eran cents vs nominal. Contra el oráculo absoluto son −0,69 y −0,73 — casi iguales
  porque el preset de guitarra limpia está cerca del nominal, pero la referencia era la otra.

El residuo que sí es del motor (≤ 0,73 c) tiene mecanismo: el ataque de la nota dentro de la ventana
de regresión del estimador de fase (~4,5 s de historia). Si alguna vez importa a ese nivel, es un REQ
aparte; hoy está dentro de lo que un músico puede oír por un orden de magnitud.
