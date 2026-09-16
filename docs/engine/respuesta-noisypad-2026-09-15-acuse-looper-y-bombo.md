---
title: "Respuesta a NoisyPad — el looper no recorta (el umbral es suyo) y el bombo mide +600 c exactos"
type: reference
status: current
created: 2026-09-15
---

# Respuesta a NoisyPad — 2026-09-15 · el número del looper y los WAV del bombo

**watermelon-audio → NoisyPad.** Contesta su carta de la noche
(`carta-noisypad-2026-09-15-respuesta-acuse-v2.18.0.md`). No avisa versión: la próxima es **2.19.0**
con REQ-042, que ya está ratificado y entra en implementación.

> **Redactada el 2026-09-15; enviada por chat el 2026-09-16**, junto con el aviso de v2.19.0 (por eso §3 ya
> dice el desenlace del escalón).

## 1 · El looper: no hay MINI — el umbral es la decisión, y es de ustedes

Gracias por el número: con él se pudo ir al lugar exacto. El motor **no detecta onsets al grabar**:
`startRecording` graba desde el bloque en que se llama (o desde el trigger armado), y el pre-roll sólo
agrega lo anterior. Lo que corre la región es **su auto-loop de la primera vuelta libre**:
`LooperRecordingCoordinator.finalizeAutoLoop` llama `trimTrack` y después `findContentBounds(trackIndex)`
con el default de **su propia interfaz**, `thresholdRatio = 0.03` (`ILooperController.kt:129`), y usa el
`firstOnset` que eso devuelve como principio de la región. `wma_looper_find_content_bounds` hace lo que
su KDoc dice: la primera muestra por encima del 3 % del **pico** de la pista (≈ −30 dB bajo el pico).

- Saw Lead: −30 dB del pico se cruza a 2–6 ms del onset ⇒ su "muestra 0 a −1,2 dB del régimen". ✓
- Warm Pad: a 32–42 ms ⇒ su "−20,8 dB rel". ✓ (Su "−20 dB del nivel a 200 ms" es el mismo umbral
  contra otra referencia.)
- No depende de `Record`: es un análisis sobre la pista terminada. ✓

Su propio comentario en ese archivo ya lo nombra (*"recorta la toma a donde el detector de onsets crea
que está el contenido"*). Dos salidas, las dos de su lado: **(a)** para fuentes internas ya tienen el
frame del primer note-on (`onTakeStarted` + `getPlayFrame()` y sus eventos de toque): el `firstOnset`
puede ser ese frame y no el cruce del umbral — es la salida que respeta el ataque de un pad entero;
**(b)** bajar `thresholdRatio` para pads (0,003 = −50 dB) lleva el corte de Warm Pad a ~10 ms, a cambio
de que una cola sucia también cuente como contenido. Lo que sí es nuestro: el KDoc de esa función no
dice cuánto se pierde con un ataque lento; se completa en el próximo cambio que toque la cabecera.

## 2 · El bombo: +600 c exactos, y el "2 % corto" era el estimador

Las dos capas de `8:116` (Taiko −780 y Timpani −810) tienen `scaleTuning` 40: 15 teclas = **+600 c** en
cualquier build. Sobre sus dos WAV, correlación del espectro en log-f a 1 c/bin y pico parabólico del
fundamental, por ventanas desde el onset:

| ventana | correlación log-f | r | pico 36 → 51 |
|---|---|---|---|
| 0–0,10 s | +613 c | 0,86 | 38,6 → 50,6 Hz = +467 |
| 0,10–0,40 | +604 | 0,81 | 36,5 → 51,2 = +588 |
| 0,20–0,60 | +593 | 0,89 | 36,2 → 50,9 = +591 |
| **0–0,80, la nota entera** | **+600** | **0,91** | 36,3 → 51,0 = +590 |

Sus 39,2 c/tecla (588 en 15) son lo que da el **pico** en cualquier ventana: un bombo de dos capas con
glide no tiene un pico estable, y sobre ventanas cortas la correlación a 5 c/bin también se acorta (la
de 0–100 ms se pasa a +613). La nota entera cierra en 600 con la mejor correlación. No era el motor, no
era el archivo: era la ventana. **Cerrado**; los dos WAV hicieron su trabajo.

## 3 · Lo que viene

REQ-042 quedó así, ratificado: `wma_sf_set_ambience(engine, reverb, chorus)` + `wma_sf_get_ambience_reverb`
/ `_chorus`; en Kotlin `sfSetAmbience` / `sfGetAmbienceReverb` / `sfGetAmbienceChorus`; 0..1 lineal
sobre la amplitud del send (0,5 = −6 dB, 0,1 = −20 dB); default 1/1; **del instrumento** (sobrevive a
`reset()`, cambio y descarga de font); el escalón 0 → 1 en caliente se midió antes de decidir si llevaba
rampa — se midió y **llevó rampa** (el detalle va en el aviso de 2.19.0). Sale como **2.19.0** con nota de
bump. Nada pendiente de ustedes.
