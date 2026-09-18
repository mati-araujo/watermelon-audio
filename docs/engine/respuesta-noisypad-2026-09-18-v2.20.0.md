---
title: "Aviso a NoisyPad — v2.20.0: pitch y envolvente por pista para el video (REQ-043, WV-3.2 / WV-3.1)"
type: reference
status: current
created: 2026-09-18
---

# Aviso a NoisyPad — 2026-09-18 · v2.20.0

**watermelon-audio → NoisyPad.** Cierra su carta del 2026-09-16 (WV-3, la capa de voz) y las tres ❓ que
contestaron el 17/09. Es un **minor** (`feat(kmp)`): dos lecturas nuevas en `ILooperBridge`, sin cambiar
nada de lo que ya usan. Lleva **cuatro deltas** respecto de lo acordado por carta (§4) y **un pedido con
número** (§6): la primera toma de voz real decide si esto vive.

> Redactado el 2026-09-18 **antes del tag**, sobre el PR de REQ-043 S2. **SIN ENVIAR**: se manda
> cuando `v2.20.0` esté verificada en el registro (4/4 coordenadas), con el run de Publish anotado acá.

## 1 · Lo que cambia, en dos firmas

```kotlin
// ILooperBridge (commonMain) — Android e iOS
fun looperAnalyzePitch(trackIndex: Int, hopMs: Double = 10.0): PitchSeries
fun looperGetLevelEnvelope(trackIndex: Int, binsPerSecond: Double = 100.0): LevelEnvelope

class PitchSeries(val hopFrames: Int, val frames: IntArray, val hz: FloatArray, val confidence: FloatArray)
class LevelEnvelope(val firstFrame: Int, val hopFrames: Int, val rms: FloatArray)
```

Las dos corren en el thread del llamador (UI/IO), read-only sobre una copia consistente del buffer, y son
**deterministas byte a byte** (mismo buffer ⇒ misma serie, corra o no el afinador). Una pista de 30 s con
hop 10 ms cuesta ~1 s en host sin optimizar (0,035× tiempo real en Debug); la envolvente, 0,001×. Ninguna
toca el thread de audio.

## 2 · El contrato, por escrito (R-API-60 / R-API-61, sobre R-API-59)

| | `looperAnalyzePitch` (WV-3.2) | `looperGetLevelEnvelope` (WV-3.1) |
|---|---|---|
| **eje** | frames absolutos del buffer, el mismo de `looperDetectOnsets` | ídem |
| **origen** | **`loopStart + W/2`** = `loopStart + 960` a 48 k (el `frame` apunta al **centro** de la ventana de 40 ms) | **`loopStart` exacto** (`firstFrame`) |
| **último punto** | `≤ loopEnd − W/2`; ventanas enteras, sin relleno, sin envolver | `bins = floor((loopEnd − loopStart) / hopFrames)`: la cola < 1 hop **no tiene bin** |
| **hop** | `hopFrames = round(hopMs · sr / 1000)`, redondeado **una vez**, **exacto** (sin mínimo) | `hopFrames = round(sr / binsPerSecond)`, ídem; **ventana = hop**, sin solapar |
| **valor** | `hz` en Hz; `confidence` = claridad NSDF del pico, 0..1 | RMS **crudo, lineal `[0, 1]`**, mono `(L+R)/2`; sin normalizar, sin dB, sin suavizar |
| **sin dato** | `hz == 0f && confidence == 0f` **exactos**, nunca interpolado, sin histéresis | (un bin de silencio es RMS pequeño o 0, es dato) |
| **rango** | nominal 60–1200 Hz, **efectivo 59,7–1200** (§4.5); por debajo sale 0/0, no su armónico | — |
| **tamaño** | `size == 0` ⇒ **no hay dato** (pista inactiva / sin contenido / región < W / hop inválido). Con dato, **el largo es el número real de puntos** — el bridge dimensiona por la cota `(loopEnd − loopStart)/hop + 1` y recorta al retorno: **nunca trunca, nunca rellena** | ídem |
| **`hopFrames` sin dato** | viene igual (si el hop era válido): sirve para dimensionar | ídem; `firstFrame` vale 0 y no dice nada |
| **`speed ≠ 1`** | la serie no cambia (como acordamos: ustedes escalan `audioFrame × speed`) | ídem |
| **`stretchTrack`** | invalida: re-analizar | ídem |

Lo que ustedes dijeron que harían y sigue valiendo: umbral de dibujo sobre `confidence` del lado del
renderer; normalización de la envolvente al pico, piso 0,01; `layerFor` hace el módulo por la región.

## 3 · Lo medido en S1 (fixture sintético `glide-voz.wav`, 4,5 s a 48 k)

Glide 110 → 440 Hz lineal en octavas (2,0 s) + 0,5 s de cero exacto + 220 Hz (1,5 s) + 65 Hz (0,5 s);
f0 + 6 armónicos a −6 dB/oct + vibrato 5 Hz ±15 c; −20 dBFS. Oráculo del generador, nunca del motor.

| métrica (hop 10 ms, W 40 ms, 447 puntos) | valor |
|---|---|
| error `\|f_est − f_true\| / f_true`, **máx** | **0,255 %** (techo de la carta: 1 %) |
| error, **p95** | **0,113 %** |
| huecos en tramos tonales | **0** |
| saltos de octava | **0** |
| silencio: puntos con `hz ≠ 0` o `confidence ≠ 0` | **0 de 47** (exactos) |
| barrido sintético 60–1200 Hz (139 tonos) | **0 octavas**, peor error 0,065 % |
| ventana elegida | **40 ms** (con 30 ms el tramo de 65 Hz sale entero 0/0: `τmax ≤ W/2` no baja de 66,7 Hz) |
| rango efectivo | **59,7–1200 Hz** |
| costo, host arm64 Debug (−O0), 30 s a 48 k | pitch **0,035×** tiempo real (1049 ms); envolvente **0,0011×** (33 ms) |

Los umbrales del test quedaron como trinquete al medido × 1,5 (máx 0,383 %, p95 0,170 %) además del
techo de 1 %. Y los golden del afinador no se movieron: el MPM es el mismo, parametrizado.

## 4 · Los deltas respecto de lo acordado (léanlos antes de adoptar)

1. **No hay "mínimo por ventana".** Su carta del 17/09 aceptaba que *"si el MPM impone un mínimo por
   ventana, lo aplica y lo devuelve"*. **No existe**: un hop menor que la ventana es solapamiento, y
   `hopFrames` sale exacto (`round(hopMs · sr / 1000)`). Lo único que devuelve el motor es ese redondeo.
2. **Las dos series tienen origen DISTINTO.** Pitch arranca en `loopStart + W/2` (el `frame` es el centro
   de la ventana; sellarlo al inicio sesga +1,4 % a 1 oct/s, medido 2,17 % con el mutante); la envolvente
   arranca en `loopStart` exacto. **Alineen por `frame`, no por índice**: `pitch[k]` y `rms[k]` NO hablan
   del mismo instante.
3. **El fixture del glide suma un tramo de 65 Hz** (0,5 s al final) que no estaba en lo acordado: es el
   único punto que decide 40 vs 30 ms de ventana (§3). No les cambia nada; se los decimos porque el
   `.truth.txt` que puedan querer reusar lo trae.
4. **El AC de la envolvente se afirma por FLANCO ± 2 bins, no por máximo local**, y `detectOnsets` ve
   **7 de 8** golpes. Con bins de 10 ms los golpes de `audiograma-prueba.wav` son mesetas planas
   (±0,4 dB): "máximo local en onset ± 1" es rojo con la envolvente **correcta** (el flanco queda debajo
   del tercer bin). Se afirma *bin ≥ −35 dB con antecesor a dos bins < −45 dB* en onset **± 2 bins** (la
   tolerancia sale de `detectOnsets`: 3 × 256 frames, medido 416–620 frames antes del flanco) más el
   nivel absoluto de las mesetas (−28/−32/−30 ± 1 dB). Y `detectOnsets` es **ciego al golpe del frame 0**
   (el flujo de energía arranca en la ventana 1): si cruzan onsets contra envolvente, cuenten con eso.

Y uno que no es delta sino precisión: **la ventana efectiva del rango es 59,7–1200 Hz**, con margen en
lags (S1: sin ese margen, 1171/1175/1200 Hz leían una octava abajo con confianza 1,00).

## 5 · Lo que les cambia a ustedes

- `VisualComposition: VOICE -> null` tiene con qué dejar de serlo: `VoiceRibbonRenderer` recibe
  `PitchSeries` con `frame` absoluto; el umbral de `confidence` es de ustedes.
- `SampleWaveRenderer` / Modo Show: `LevelEnvelope` con `firstFrame`/`hopFrames`; normalizar al pico
  es de ustedes.
- Nada de lo existente se mueve: ni `looperGetTrackWaveform` (MINI-030 sigue igual), ni nivel, ni
  sends, ni las firmas que ya usan. En la C API sólo **suma** (`wma_looper_analyze_pitch`,
  `wma_looper_get_level_envelope`), sin tocar las 280 anteriores.

## 6 · Lo que pedimos: la primera toma de voz real (I-2), con su carta

REQ-043 se midió sobre un fixture sintético; **no hay voz real en nuestro repo y no se fabrica**. La
evaluación que decide si el REQ vive es la de ustedes, y está definida **antes** de que la corran:

> En la **primera toma de voz real** que midan con `cmd video` en el G42, sobre la serie que devolvemos
> (`hopMs = 10`), contar:
>
> **(a) saltos de octava por cada 10 s cantados**: pares consecutivos `(f[k], f[k+1])`, ambos `> 0`, con
> `f[k+1] / f[k]` en **(1,9; 2,1)**, en **(0,47; 0,53)**, en **(2,9; 3,1)** o en **(0,32; 0,35)** (×2, ÷2, ×3,
> ÷3). Techo: **1 por 10 s cantados**.
>
> **(b) % de puntos 0/0 dentro de tramos cantados**: `hz == 0 && confidence == 0` sobre los puntos cuyo
> `frame` cae en un **bin de la envolvente > −35 dB** (`looperGetLevelEnvelope` a 100 bins/s, mismo umbral
> que AC-043.5; `20·log10(rms)`). Techo: **20 %**.

Con los dos números y la duración cantada (segundos con bins > −35 dB) alcanza; si pueden, la toma
(WAV de la pista, `looperCaptureTrack` a float32) para reproducirlo acá. **Si cualquiera supera su techo,
REQ-043 está muerto y lo rehacemos**; si pasan dos releases nuestras sin la medición, queda "sin evaluar".
Un delta de contrato (re-derivar hop u origen del lado de ustedes) no mata nada: se arregla con un bump.
