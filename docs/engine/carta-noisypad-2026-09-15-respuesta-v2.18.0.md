---
title: "NoisyPad → watermelon-audio · respuesta al aviso de v2.18.0 (la cola, el brillo, el ataque) y el pedido de la perilla"
type: reference
status: current
created: 2026-09-15
---

# Respuesta de NoisyPad al aviso de v2.18.0

**Recibida el 2026-09-15 a la noche.** Contesta el envío único `respuesta-noisypad-2026-09-15-v2.18.0.md`
(§0 la carta de 2.17.4 cotejada, §1–§6 el aviso de v2.18.0). Retira el "hallazgo" del fix 2, verifica
los tres tambores de §5 con número, hace **dos preguntas** (el +2,5 dB del Grand suave; el ataque fuerte
de Trumpet corrido) y **pide la perilla con carta**, con tres razones de producto. Texto íntegro, como
llegó (copia sin editar en `specs/referencias/inbox-noisypad-2026-09-15-carta-v2.18.0.md`).

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo que sí se verificó contra el
> archivo y el motor está al final, separado a propósito. Las dos preguntas tienen respuesta medida:
> ninguna es un hallazgo del motor, y la primera deja un dato para REQ-041.

---
# NoisyPad → watermelon-audio · respuesta al aviso de v2.18.0 (y acuse de su §0)

**2026-09-15, noche.** Adoptado: NoisyPad está en **2.18.0** (PR #254). Todo medido en el Moto G42 con el
mismo código de app antes y después (sólo cambia la lib), mismo guion por `adb`, export del mix por el
canal de debug, rack en bypass. Reemplaza y cierra la carta de la tarde.

## 0 · Su §0: tienen razón en todo, y lo anotamos con su causa

- **Fix 2 = FluidSynth.** Retiramos el "hallazgo" de la tarde. Leímos `fluid_convex` mal (0,48 al 6 %,
  no 0,99) y medimos contra una rampa lineal imaginada en vez de contra la referencia. Su fila de
  FluidSynth con `hf.py` sobre la zona exacta es lo que le faltaba a nuestro arnés: desde ahora, antes de
  llamar defecto a una envolvente, pedimos o rendimos esa fila (`render-spec-reference.sh`).
- **La zona local pisa a la global dentro del nivel.** Grand 1,37 s (1018 − 475), y su t40 lo confirma en
  las dos builds. El resolutor (`scripts/lab/gens.py`) ya lo hacía bien; la carta lo sumó a mano.
- Los tres matices de 2.17.3 (bombo de dos capas, keytrack 2 % corto por la correlación sobre dos capas,
  Wood Block al revés): aceptados. El Wood Block ya se alcanza desde la app (#253: un kit abre en Drum
  Grid por default pero deja elegir Note Grid): **76 pico 825 Hz, 77 pico 618 Hz**, −24 dBFS los dos, en
  2.17.4.
- Les debemos dos cosas, que no van en esta carta: los dos WAV del bombo (36 y 51 en 2.17.1) y el número
  del golpe que el looper recorta. Lo que ya sabemos del segundo: en el export, el primer golpe aparece
  cortado en la muestra 0 (Warm Pad entra a −46 dB y llega al pico 0,5 s después, o sea ~0,3 s adentro de
  su ataque de 0,8 s) — el `loopStart` cae DESPUÉS del primer note-on. Falta medirlo con Record y el
  note-on con marca de tiempo; va en la próxima tanda.

## 1 · v2.18.0, lo que pidieron verificar en §5

**La cola (REQ-040).** XY, apoyada de 1 s y soltar, nivel del mix relativo al régimen:

| preset | tras el note-off | 2.17.4 | 2.18.0 |
|---|---|---|---|
| `0:81 Saw Lead` (release 0,5 s; 5 % + 6,3 % reverb, 20 % chorus) | +0,3 / +0,5 / +0,75 s | −64,0 / cero digital / cero | −35,9 / −51,8 / −71,3; cero a +1,0 s |
| t20 / t40 / t60 | | 110 / 210 / 300 ms | 150 / 370 / 670 ms |
| `0:48 Stereo Strings Fast` (14 % + 6,3 %) | +0,1 / +0,3 / +0,5 / +1,0 s | −7,6 / −24,6 / −36,8 / −81,7 | −4,4 / −21,2 / −32,8 / −74,0 (+3…4 dB) |
| t60 | | 820 | 900 |
| `0:89 Warm Pad` (14 % + 6,3 %) | +1,0 / +2,0 s | −20,1 / −44,6 | −21,5 / −44,3 — no separa: su release de 4,53 s tapa al cuarto |

Régimen igual (± 0,5 dB). Es lo que dicen: existe, dura < 1 s, es del cuarto (se apaga a cero exacto) y
es discreta (−36 dB a 0,3 s). Con el rack en bypass no se oye como "reverb": se oye como que la nota no
corta seco. Nada suena con demasiada cola ni con chorus donde no se espera, en lo que tocamos (Saw Lead,
Strings, Warm Pad, Grand, Trumpet, y el smoke de los 7 engines).

**El brillo (MINI-027 fix).** `0:0 Stereo Grand`, Note Grid, tecla 72:

| golpe | centroide 20–120 ms | 100–400 ms | nivel 0,1–0,4 s |
|---|---|---|---|
| suave (v ≈ 0,35) | 658 → 531 Hz (−19 %) | 594 → 531 | −38,8 → −36,3 (**+2,5 dB**) |
| fuerte (v ≈ 0,96) | 638 → 638 | 581 → 586 | −23,3 → −22,5 (+0,8) |

Trumpet suave 1340 → 1203 Hz (−10 %), fuerte 1347 → 1358. Nada "demasiado oscuro" a estas velocities.
**Pregunta 1**: el golpe suave del Grand sube +2,5 dB de nivel contra +0,8 el fuerte (Grand: reverb 7 %,
¿chorus?). No está en la nota. ¿Es el wet (que a igual send pesa más contra una nota más chica), o el
corte modulado moviendo el Q / la resonancia del filtro? Si es lo primero, la referencia `-R 1 -C 1` lo
tiene que mostrar igual.

**El ataque (MINI-027 feat).** `0:56 Trumpet`, dB relativos al régimen (0,3–0,4 s) por instante, RMS de
5 ms:

| | 5 ms | 10 | 15 | 20 | 30 | 50 |
|---|---|---|---|---|---|---|
| suave 2.17.4 | −5,6 | −0,7 | −0,7 | +0,9 | +0,3 | +1,6 |
| suave 2.18.0 | −9,2 | −6,4 | −3,0 | −1,6 | −0,2 | −1,1 |
| fuerte 2.17.4 | −5,6 | −0,6 | −0,7 | +0,9 | +0,4 | +1,6 |
| fuerte 2.18.0 | −6,2 | −2,4 | −1,2 | −0,2 | −0,4 | +0,7 |

El suave llega al régimen en 10 → 30 ms: ×3 (dicen ×3,1 a v 44 — cierra). No es "demasiado lento": es un
ataque de trompeta. **Pregunta 2**: el fuerte también se corre, 10 → ~17–20 ms, donde la nota dice ×1,07.
Nuestra lectura: la copia del chorus (12–20 ms de retardo) entra al régimen y corre la llegada en las dos
velocities, y con eso el "ataque" que mide un RMS del mix ya no es sólo `attackVolEnv`. Si es así, no es
un hallazgo; si el fuerte de Trumpet en `-R 1 -C 1` llega en 10 ms, sí lo es.

params diff vacío; smoke igual (BPM y preset heredados, la toma propia −14,8 → −15,3 dBFS).

## 2 · La perilla: sí, la pedimos

Tres razones de producto, no de gusto:

1. NoisyPad ya tiene reverb y chorus en el rack. Un usuario con Concert Hall encendido apila dos reverbs
   sin saberlo y sin poder apagar la de adentro.
2. A/B. Hoy no podemos oír ni medir el font seco contra el font con cola en el mismo device sin cambiar
   de lib.
3. El arnés. Para los bumps que vienen necesitamos la línea de base seca para separar los sends de todo
   lo demás; si no, cada tanda con SoundFont compara contra un cuarto.

**Pedido concreto**: un control por instancia, sin CC, `setSoundFontAmbience(reverb: Float, chorus:
Float)` en 0..1 con default 1 (= FluidSynth), que escale el send de todas las voces (generador + default
#8) antes de las unidades. Con eso NoisyPad lo expone como un solo toggle "ambiencia del font" en la hoja
del SoundFont, default encendido, y el arnés lo apaga para medir.

## 3 · Nada más pendiente de nuestro lado

Cero superficie en 2.18.0, nada que adoptar. Los arneses nuevos (`tanda-fx.sh`, `fx.py`, `gens.py` con las
cuatro capas) quedan en el repo. En la próxima tanda: los WAV del bombo y el número del looper.

---

## Lo que verificamos de nuestro lado (2026-09-15)

Contra el archivo (las cuatro capas de GeneralUser 1.471 resueltas con la precedencia de §8.5) y contra
el motor en el host: el mismo `.sf3`, `MidiSpecHarness`, tres renders por caso — **2.17.4**, **2.18.0
seco** (costura de sends en 0) y **2.18.0 con sends** — para separar lo que cambió el dry (MINI-027) de
lo que suma el wet (REQ-040). Tecla 72, velocity 44 y 122, nota de 1 s. (Scratchpad `3021329d`:
`wmarender`/`wmarender2174`, `lvl.py`.)

### Pregunta 1 — el +2,5 dB del Grand suave: 1,7 dB es el DRY (la resonancia sobre la fundamental), 0,6 el wet

`0:0 Stereo Grand` a la tecla 72 tiene **reverb 7 % y NADA de chorus** (a las dos velocities). Nivel
RMS en 0,1–0,4 s y centroide en 20–120 ms:

| golpe | 2.17.4 | 2.18.0 seco | 2.18.0 con sends | Δ dry | Δ wet | Δ total (ellos) |
|---|---|---|---|---|---|---|
| suave (v 44) | −33,1 dBFS · 667 Hz | −31,4 · 535 | −30,8 · 534 | **+1,7** | +0,6 | +2,3 (**+2,5**) |
| fuerte (v 122) | −19,3 · 629 | −19,3 · 629 | −18,7 · 626 | 0,0 | +0,6 | +0,6 (**+0,8**) |

- **El wet pesa lo mismo en las dos** (+0,6 dB): el send es proporcional a la voz (`gainMono × send`), así
  que "a igual send pesa más contra una nota más chica" no ocurre. Esa mitad de su pregunta queda cerrada
  por construcción y por medición.
- **La diferencia es el dry, y es la segunda mitad de su pregunta**: el corte modulado. En la zona de
  velocity 0–49 el Grand declara `initialFilterFc` 2660 Hz con **`initialFilterQ` 20 cB (2 dB)** y un
  velocity → Fc de **−3800 c** (el que MINI-027 destapó: estaba inerte). A v 44 el corte baja
  −3800 × 83/127 = −2483 c ⇒ **634 Hz**, justo encima de la fundamental de la tecla 72 (523 Hz): el
  centroide cae 667 → 535 (ellos 658 → 531 ✓) y **la resonancia de 2 dB queda montada sobre la
  fundamental**, que es lo que domina el RMS ⇒ +1,7 dB. A v 122 la zona es otra (120–127: Fc 798 Hz,
  Q 0) y no se mueve nada. En 2.17.4 el corte de la nota suave se quedaba en 2660 Hz (el defecto de
  MINI-027) y la resonancia no tocaba la fundamental.
- 🔴 **Lo que la referencia va a mostrar distinto, y es de REQ-041, no de esto**: FluidSynth 2.6.0
  define el Q como la altura del pico **sobre la respuesta sin resonancia** (Butterworth), no sobre DC:
  `q_lin = 10^((Q_dB − 3,01)/20)` (`fluid_iir_filter.c:62-90`, con la cita del spec que lo justifica).
  tsf usa `q_lin = 10^(Q_dB/20)`: **3 dB más de resonancia a cualquier Q**. Con Q = 2 dB, el pico de
  FluidSynth es ≈ +0,6 dB y el nuestro ≈ +2,7, así que la nota suave del Grand subiría en FluidSynth
  ~+0,6 dB donde nosotros +1,7. Uno o dos dB, sólo en las regiones con Q > 0 y el corte cerca de la
  fundamental, y es exactamente la fila #10 del spec-test (*Filter resonance*), que REQ-041 ya tiene
  declarada. Se anota ahí como primera evidencia con preset y número; no abre nada nuevo.

### Pregunta 2 — el fuerte de Trumpet NO se corrió: subió el régimen contra el que lo miden

`0:56 Trumpet` tiene **reverb 7 % y NADA de chorus**: la copia retrasada 12–20 ms que proponen no
existe en ese preset. Y sus moduladores a v 122 pesan (127 − 122)/127 = 3,9 %: `attackVolEnv` +118 tc
(×1,07, como dice la nota), `attackModEnv` +394, Fc −98 c, Q +0,3 cB. RMS de 5 ms relativo al régimen
(0,3–0,4 s), host:

| | 5 ms | 10 | 15 | 20 | 30 | 50 |
|---|---|---|---|---|---|---|
| fuerte 2.17.4 | −17,6 | −2,0 | −2,1 | −0,4 | −0,7 | +1,1 |
| fuerte 2.18.0 seco | −18,2 | −2,2 | −2,1 | −0,4 | −0,7 | +1,1 |
| fuerte 2.18.0 con sends | −19,0 | −3,0 | −3,0 | −1,2 | −1,6 | +0,4 |
| suave 2.17.4 | −17,6 | −2,0 | −2,1 | −0,4 | −0,7 | +1,1 |
| suave 2.18.0 seco | −27,9 | −10,9 | −8,3 | −3,7 | −1,2 | +1,1 |

- **El dry del fuerte es idéntico entre 2.17.4 y 2.18.0** (al 0,2 dB en los seis instantes). Con sends,
  las seis columnas bajan **−0,8 dB parejo**: el régimen de 0,3–0,4 s ya lleva el wet (+0,7 dB) y los
  primeros 25 ms no (el comb más corto tarda 1116 muestras). Es lo mismo que en su tabla: su fuerte
  2.18.0 está −0,6 / −1,8 / −0,5 / −1,1 / −0,8 / −0,9 debajo del de 2.17.4, un desplazamiento
  constante, no un ataque más largo. El "10 → 17–20 ms" es leer el cruce de −1 dB sobre una curva
  corrida 0,8 dB hacia abajo. **No es un hallazgo**, y la referencia con efectos va a hacer exactamente
  lo mismo (su régimen también lleva el wet).
- La regla para su arnés: el régimen de una medición de ataque se toma **antes de los 25 ms** o sobre
  el render seco — o se espera a la perilla.
- El suave sí cambia, y es lo que la nota anunció: en 2.17.4 el suave y el fuerte eran la MISMA curva
  (los moduladores de velocity estaban inertes), en 2.18.0 el suave tarda ×3.

### El resto

- **Wood Block, cotejado por la diferencia**: 825 → 618 Hz entre 76 y 77 son **−500 c**, y el archivo
  declara exactamente eso: la misma capa `Wood Block_1` (root 60, scale 50) con coarse −27 / fine +62
  en la 76 y −32 / +12 en la 77, o sea −1319 y −1594 sobre un keytrack de +800 / +850: (850 − 1594) −
  (800 − 1319) = **−500**. Antes de MINI-025 la 77 sonaba +50 c *arriba* de la 76. Es la confirmación
  del Wood Block que el aviso de 2.17.3 no había podido tener.
- **La cola**: consistente con lo que el aviso dijo (existe, < 1 s, del cuarto, cero exacto). Los t60 de
  Saw Lead 300 → 670 ms son la cola del cuarto reemplazando a la release, como avisamos en §2 del
  envío. Sin contradicción con el motor; no se re-midió.
- **El looper**: su hipótesis (*el `loopStart` cae después del primer note-on*; Warm Pad entra a −46 dB
  y llega al pico 0,5 s después) es medible y, si se confirma con Record y note-on con marca de tiempo,
  es un MINI nuestro. Queda esperando su número.
- **La perilla: pedida con carta, con tres razones de producto.** Es el REQ que el aviso ofrecía: la
  costura `SoundFontSendBus::setSendScale` ya escala los dos buses antes de las unidades; falta la
  superficie (C API, JNI, `ISoundFontBridge`, iOS) y el contrato (por instancia, sin CC, 0..1, default
  1, generador + default #8). Va como REQ propio, por amplificación.
