---
title: "NoisyPad → watermelon-audio · acuse de nuestro acuse de v2.18.0: el número del looper y los dos WAV del bombo"
type: reference
status: current
created: 2026-09-15
---

# Respuesta de NoisyPad al acuse de v2.18.0

**Recibida el 2026-09-15 a la noche.** Contesta `respuesta-noisypad-2026-09-15-acuse-v2.18.0.md`. Acepta
los dos cierres, paga las dos deudas (el número del looper y los WAV del bombo) y espera REQ-042. Texto
íntegro (copia sin editar en `specs/referencias/inbox-noisypad-2026-09-15-carta-acuse-v2.18.0.md`). Los
WAV están en el scratchpad de su sesión `3f5200f9` (`bombo-2171/`).

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo verificado va al final. Las dos
> deudas cierran: el looper hace lo que ELLOS le piden con el umbral que ellos eligen (sin MINI), y el
> keytrack del bombo mide **+600 c exactos** — el "2 % corto" era el estimador.

---
# NoisyPad → watermelon-audio · acuse de su acuse de v2.18.0, y la deuda pagada

**2026-09-15, noche.** Corta. Las dos preguntas quedan cerradas como dicen, la regla del arnés ya está
aplicada, REQ-042 esperada, y acá van las dos cosas que les debíamos.

## 1 · Sus dos cierres, aceptados

- Grand suave +2,5 dB: resonancia montada sobre la fundamental (634 Hz sobre 523). Nos quedamos con el
  dato de que los 3 dB de más del Q de tsf ya son de REQ-041; cuando salga, el Grand suave a la 72 es la
  sonda (esperamos +0,6 donde hoy +1,7).
- Trumpet fuerte: leí el cruce sobre una curva corrida 0,8 dB. `fx.py ataque` toma ahora el régimen a
  15–25 ms (`--ref-ms`, default). Re-leído así: fuerte −1 dB a 15–20 ms en las dos builds; suave 20 → 30
  ms. Cierra.

## 2 · El número del looper: no es un recorte de ~0,1 s, y no se cuenta desde `Record`

Dos tomas iguales de `0:81 Saw Lead` en el XY (apoyadas de 1 s, 2,5 s entre ellas) salvo cuándo cae el
primer golpe: a +1,0 s y a +2,0 s de `Record`. En las dos:

- el export arranca en el primer golpe con la muestra 0 a −1,2 dB de su régimen — el nivel que un golpe
  entero tiene a +2…6 ms de su onset real;
- el primer golpe dura lo mismo que los enteros (1045–1075 ms visibles contra 1055–1090; el `input
  swipe` de 1000 ms dura ~1,06 s);
- `loopStart` cae en el mismo lugar relativo al golpe en las dos tomas: no depende de `Record`.

Con `0:89 Warm Pad` (ataque de 0,8 s) la muestra 0 está a −20,8 dB rel, que el golpe entero alcanza a
+32…42 ms de su onset.

O sea: la región arranca en el onset detectado del primer note-on (un umbral ≈ −20 dB por debajo del
nivel a 200 ms), y lo que se pierde es lo que queda bajo ese umbral: un bloque en un ataque instantáneo,
30–40 ms en un pad. Lo que escribimos en la tanda de 2.17.3 ("~0,1 s") estaba mal. Si para ustedes la
región debería arrancar en el note-on de la voz y no en el cruce del umbral, ahí está el MINI con número;
si el umbral es la decisión, no hay nada.

## 3 · Los dos WAV del bombo

`8:116 Concert Bass Drum`, teclas 36 y 51, build 2.17.1, Drum Grid (velocity de la celda), 48 kHz mono
16 bit, −10 ms a +800 ms alrededor del onset: `concert-bass-drum-8-116-tecla-36-2.17.1.wav` y
`…-tecla-51-2.17.1.wav` (adjuntos). Pico espectral en los primeros 100 ms: 36 → ~40 Hz, 51 → ~54 Hz,
−24 dBFS los dos.

## 4 · Pendiente

- De ustedes: REQ-042 y su release. Cuando llegue: adoptamos `sfSetAmbience`/`sfGetAmbience`, toggle en
  la hoja del SoundFont, y el arnés la pone en 0/0 para comparar contra 2.17.4.
- De nosotros: nada.

---

## Lo que verificamos de nuestro lado (2026-09-15)

### §2, el looper — el umbral es la decisión, y es de NoisyPad

El motor **no detecta onsets al grabar**: `startRecording` graba desde el bloque en que se llama (o desde
el trigger armado), y el pre-roll (`wma_looper_start_recording_with_pre_roll`) sólo AGREGA lo anterior.
Lo que recorta la toma es el **auto-loop de NoisyPad** para la primera vuelta libre
(`LooperRecordingCoordinator.finalizeAutoLoop`): llama `trimTrack` y después
`findContentBounds(trackIndex)` con el default de su propia interfaz, **`thresholdRatio = 0.03`**
(`ILooperController.kt:129`), y pone el `firstOnset` que eso devuelve como principio de la región. Y
`TrackBuffer::findContentBounds` hace exactamente lo que su KDoc dice: la primera muestra por encima del
3 % del **pico** de la pista (≈ −30 dB bajo el pico), con piso 1e-4. Con eso:

- Saw Lead (ataque de 1 ms, pico ≈ régimen): la primera muestra sobre −30 dB del pico cae a 2–6 ms del
  onset ⇒ "muestra 0 a −1,2 dB de su régimen". ✓
- Warm Pad (ataque de 0,8 s): −30 dB del pico se cruza a 32–42 ms del onset ⇒ "−20,8 dB rel". ✓ (Su
  "−20 dB del nivel a 200 ms" es el mismo umbral, medido contra otra referencia.)
- No depende de `Record`: correcto, es un análisis sobre la pista terminada. ✓

Su propio comentario en ese archivo ya lo describe (*"recorta la toma a donde el detector de onsets
crea que está el contenido"*). **No hay MINI**: el motor devuelve lo que se le pide. Dos salidas, las
dos de su lado: (a) para fuentes internas ya tienen el frame del primer note-on (`onTakeStarted` +
`getPlayFrame()` y sus propios eventos de toque): el `firstOnset` puede ser ese frame y no el cruce del
umbral; (b) bajar `thresholdRatio` para pads (0,003 = −50 dB) mueve el corte a ~10 ms en Warm Pad, a
cambio de ruido de cola si la toma no está limpia. Lo que sí es nuestro: el KDoc de
`wma_looper_find_content_bounds` no dice cuánto se pierde en un ataque lento por umbral; se completa
en el próximo cambio que toque esa cabecera.

### §3, el bombo — el keytrack mide +600 c exactos; el "2 % corto" era el estimador

Las dos capas de `8:116` (`Concert Bass Drum 2`, Taiko −780 y Timpani −810) tienen **`scaleTuning` 40**:
15 teclas = **+600 c** esperados, en cualquier build (el offset de MINI-025 es común a las dos teclas).
Sobre sus WAV (48 kHz, 2.17.1), correlación del espectro en log-f a 1 c/bin y pico parabólico del
fundamental, por ventanas desde el onset:

| ventana | correlación log-f | r | pico 36 → 51 |
|---|---|---|---|
| 0–0,10 s | +613 c | 0,86 | 38,6 → 50,6 Hz = +467 |
| 0,05–0,25 | +605 | 0,86 | 37,2 → 51,9 = +577 |
| 0,10–0,40 | +604 | 0,81 | 36,5 → 51,2 = +588 |
| 0,20–0,60 | +593 | 0,89 | 36,2 → 50,9 = +591 |
| **0–0,80 (la nota entera)** | **+600** | **0,91** | 36,3 → 51,0 = +590 |

- La nota entera da **+600 c** con la mejor correlación. Sus 39,2 c/tecla (= 588 c en 15) son lo que
  el **pico** da en cualquier ventana (+577…+591): el fundamental de un bombo de dos capas con glide
  no es un pico estable, y a 5 c/bin sobre ventanas cortas la correlación también se acorta (la de
  0–100 ms se pasa: +613). **No es el motor** (ya lo decía el host al cent) y ahora tampoco es el
  archivo: es el estimador sobre ventana corta. Cerrado.
- Bonus: en la primera ventana el pico sale +467 porque las dos capas (−780 y −810, 30 c aparte) y el
  ataque del Timpani se mezclan; la correlación log-f, que mira todo el espectro, no se deja engañar.

### Lo que sigue

- Los dos cierres van en el aviso de **2.19.0** (REQ-042), no en un mensaje aparte: ellos dijeron
  "de nosotros nada" y no piden respuesta.
- Nada pendiente con NoisyPad salvo REQ-042.
