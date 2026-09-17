---
title: "Respuesta a NoisyPad — WV-3: la capa de voz del video (orden y contrato, cláusula por cláusula)"
type: reference
status: current
created: 2026-09-16
---

# Respuesta a NoisyPad — 2026-09-16 · WV-3, la voz del video

**watermelon-audio → NoisyPad.** Contesta su carta *"WV-3: la capa de voz del video, y lo que ya consumimos"*
(versionada en `carta-noisypad-2026-09-16-wv3-la-capa-de-voz.md`). Segunda de tres, separada como pidieron.

> **Redactado el 2026-09-16 a la noche. SIN ENVIAR**: lo envía el humano por chat y se marca acá. Las
> decisiones de abajo se tomaron el 16/09 (grilling) y viven en la spec de REQ-043 (`draft`) y MINI-030
> (`review`); lo que sigue abierto está marcado como pregunta.

## 1 · Orden

| pedido | respuesta |
|---|---|
| WV-3.2 en el mismo bump que WL-5.1, aunque WL-5.2 venga después | **Sí, y antes de lo que pedían.** El detector de WL-5.1 ya existe: es el MPM del afinador (`dsp/McLeodPitch`, REQ-001) — {freqHz, confidence}, ventana y hop configurables. Lo que falta es el recorrido por hop sobre el buffer de una pista, la C API y el bridge. Eso es **REQ-043 = WV-3.2 + WV-3.1 juntos** (mismo eje, mismo fixture), un solo bump. WL-5.2 (autotune) queda para después, como dicen. |
| "primero la voz" (su §4 de WL-4.1) | **Acordado**: REQ-043 va antes que las etapas 2 y 3 de REQ-041 (rampa e interpolación) y antes que WL-4.1. |
| la nota sobre `getTrackWaveform` (ceros ≡ ausente) | **Es un defecto del bridge y sale primero, suelto: MINI-030** (patch con nota, `fix(kmp)`). Ver §2. |

## 2 · Contrato, cláusula por cláusula

| # | pedido | respuesta | dónde queda |
|---|---|---|---|
| 0 | "no analizado" ≠ "en silencio": tamaño 0 / `null` para "no hay" | **`FloatArray(0)` cuando el motor no escribió ningún bin** (pista inactiva / sin contenido), Android e iOS. Con N > 0 bins sigue devolviendo `numBins` con relleno de silencio, como hoy (sus llamadores de UI no reescalan). Un consumidor que indexa `[0]` sin mirar el tamaño se rompe — por eso viaja como bump con nota, aunque la firma no cambie. Ustedes no: entran por `hasAudio`. | MINI-030 (`review`), R-API-59 en `ILooperBridge`: *"0 elementos = no hay dato; N con ceros al final = dato con silencio"*, y vale para toda lectura de análisis por pista que se agregue. |
| 1 | `frame` en frames del **buffer**, no de la región | **Sí**: el eje de `getTrackWaveform` / `detectOnsets`. | REQ-043, R-API-60 |
| 2 | `freqHz = 0` o `confidence = 0` donde no hay pitch, nunca interpolado | **Los dos a 0, exactamente**, cuando la compuerta de ausencia (REQ-014) o el soporte espectral (REQ-031) dicen que no — los mismos veredictos que el afinador publica como NO_SIGNAL / NO_LOCK. **Nunca un punto interpolado.** `confidence` es **la claridad NSDF del MPM, tal cual**, no una binaria ni un número inventado: el umbral de dibujo es suyo, y la cinta se corta donde ustedes digan. | REQ-043, R-API-60 + R-MOT-45 |
| 3 | `hopMs` exacto, o devuelto | **Devuelto en frames**: el motor toma su `hopMs`, lo redondea a frames enteros UNA vez y devuelve el hop real; la serie lleva `frame` absoluto por punto, así que nada se acumula. ❓ **Pregunta**: ¿prefieren fijar `hopMs` ustedes (esto), o que el motor fije el hop del detector y ustedes decimen? La primera encaja con el eje del buffer. | REQ-043 (pregunta abierta 1) |
| 4 | Determinista: misma serie byte a byte | **Sí, y es AC**: sin estado compartido con el afinador en vivo (ring propio, como `analyzeBuffer`), offline en el thread de UI/IO sobre una lectura consistente del buffer (la disciplina de `detectOnsets`). | REQ-043 AC de determinismo |
| 5 | WV-3.1: RMS decimado que respete la región; región vs buffer explícito en nombre o doc | **RMS** (no pico como la waveform de hoy), respeta la región, y el eje va en el doc. ❓ **Confirmen RMS**: la carta lo pide, la waveform actual es pico; queremos que quede escrito por ustedes. | REQ-043, R-API-61 |
| 6 | WV-3.3 puede esperar | Fuera de REQ-043. | — |
| 7 | `speed ≠ 1` (no lo preguntaron; lo preguntamos nosotros) | El eje es el buffer: el análisis **no cambia** con la velocidad de reproducción; la conversión a tiempo de reproducción es suya (como hoy el cursor en `frames × speed`). ❓ **¿Confirman que es lo que esperan?** | REQ-043 (pregunta abierta 2) |

## 3 · Cómo se verifica (el mismo AC de los dos lados)

| observable | su AC | nuestro AC (host, gate) | umbral |
|---|---|---|---|
| pitch sobre el glide 110 → 440 Hz en 2 s + 0,5 s silencio + 220 Hz | error < 1 % en el glide; `confidence` ≈ 0 en el silencio, cero puntos interpolados; serie idéntica en dos análisis | el mismo WAV, **generado por nosotros** y versionado como fixture; importado por el mismo camino (`importTrack`) | **el umbral se declara DESPUÉS de correr el fixture** (la regla de este repo): hoy "< 1 %" es su número y lo tomamos como piso, no como techo; **cualquier punto con `freqHz > 0` dentro del silencio es hallazgo** (ese sí, desde ya) |
| envolvente sobre el WAV de 8 golpes | 8 máximos locales en los frames de `detectOnsets` (± 1 bin) | ídem, con `detectOnsets` como oráculo cruzado | ± 1 bin, como lo escribieron; ❓ **pásennos `Download/audiograma-prueba.wav`** (4 s, pico −23 dBFS) o lo reconstruimos y se los mandamos para que midan sobre el mismo |
| determinismo | dos análisis seguidos, byte a byte | dos análisis, byte a byte, y con el afinador en vivo corriendo al mismo tiempo | 0 diferencias |
| en device | Moto G42, `cmd video` con material real, tabla | — (I-2 del criterio de muerte de REQ-043) | el que enuncie el criterio |

## 4 · Lo que no cambia

Ni renderers ni UI, como dicen: `VoiceRibbonRenderer` es suyo. El afinador no se toca: el recorrido por hop es
nuevo; el detector y las compuertas se reusan tal cual.

**Tres respuestas suyas nos destraban los AC**: hop (fijado por ustedes y devuelto en frames, ¿sí?), RMS en
WV-3.1 (¿sí?), `speed ≠ 1` sin efecto sobre la serie (¿sí?). Y el WAV de 8 golpes.
