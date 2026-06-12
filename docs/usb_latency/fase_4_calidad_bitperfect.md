# Fase 4 — Calidad e integridad: modo bit-perfect

**Objetivo:** que la cadena float → wire PCM sea transparente por defecto y que las no-linealidades (soft clip, dither) sean decisiones explícitas del consumidor, no defaults ocultos.

**Hallazgos cubiertos:** Q1, Q2, Q3, Q5 (+ simetría de escalas).
**Depende de:** nadie — puede ejecutarse en paralelo a cualquier fase.
**Estimación:** 0.5–1 día.

---

## 4.1 — Soft clip opt-in (Q1)

`AudioFormatConverter.h:248-257`: `softClip()` con umbral 0.95 se aplica **siempre** en todas las rutas `floatTo*` → cualquier sample > 0.95 se altera. Eso contradice "transporte limpio" y mete `tanh` en el RT path.

**Cambio:**
- Default `mSoftClipThreshold = 1.0f` y short-circuit: `if (mSoftClipThreshold >= 1.0f) return hardClamp(sample);` — la rama tanh desaparece del hot path por defecto.
- `setSoftClipThreshold()` ya existe; documentar que < 1.0 implica procesamiento no transparente.
- Plumbing: `TransferConfig.softClipThreshold` (default 1.0) → `mFormatConverter` en `UsbTransferManager::configure()`. Exponer a Kotlin solo si NoisyPad lo pide (no es necesario para el caso guitarra: el motor ya limita).

## 4.2 — Redondeo correcto (Q2)

Todas las rutas usan `static_cast<int32_t>(sample * scale)` (truncamiento hacia cero → sesgo + distorsión correlacionada de bajo nivel).

**Cambio:** redondeo al más cercano en todas las conversiones float→PCM:

```cpp
int32_t value = static_cast<int32_t>(std::lrintf(sample * scale));
```

`lrintf` usa el modo de redondeo actual (round-to-nearest-even por defecto en ARM64/NEON) y compila a `fcvtns` — más barato que el cast con branch. Verificar en los 5 paths: `floatToS16`, `floatToS24_3LE`, `floatToS24_4LE`, `floatToS32`.

## 4.3 — Simetría de escalas

Hoy: escala `32767.0f` (asimétrica: −1.0 → −32767, desperdicia un código) y clamp previo a [-1, 1].

**Cambio (estándar de la industria):** escalar por `2^(n-1)` y clampear el entero al rango del tipo:

```cpp
// S16:
constexpr float scale = 32768.0f;
int32_t v = (int32_t)std::lrintf(sample * scale);
output[i] = (int16_t)std::clamp(v, -32768, 32767);
// S24: scale 8388608.0f, clamp [-8388608, 8388607]
// S32: scale 2147483648.0f (double para el producto), clamp int64 → int32
```

Esto hace además que el round-trip con los decoders existentes (`s16ToFloat` divide por 32768) sea **exactamente inverso** para todos los valores representables — condición de bit-perfect verificable por test.

Nota S32: `sample * 2147483648.0f` con sample=1.0 desborda int32 tras lrintf → hacer el producto en `double` y clampear en `int64_t` antes del cast.

## 4.4 — Dither configurable por API (Q3)

- `setDitheringEnabled()` existe; falta el plumbing: `TransferConfig.ditherEnabled` (default **true** para S16, ignorado en ≥24 bits — ya es así porque solo `floatToS16` lo consulta) → JNI `nativeSetUsbDither(jboolean)` → `IAudioNativeBridge.setUsbDither(...)`.
- Modo "bit-perfect estricto" = `dither off + softClip 1.0`: exponer como un solo flag de conveniencia `setUsbBitPerfect(true)` que setea ambos.
- El TPDF actual (±0.5 LSB, xorshift) es correcto; sin cambios de algoritmo. Un detalle: `TpdfDither::get(16)` escala `1/(1<<16)` que en dominio [-1,1] equivale a ±0.5 LSB de S16 — correcto; dejar comentario explicándolo porque no es obvio.

## 4.5 — Gain mono→estéreo configurable (Q5)

`LibusbBackend.cpp:1730`: `monoGain = 0.707f` fijo altera el nivel de entrada de instrumento sin necesidad (duplicar mono a L/R no clipea).

**Cambio:** `std::atomic<float> mMonoInputGain{1.0f}` en `LibusbBackend` + setter (`setMonoInputGain`, rango [0, 1]); default 1.0 (cambio de comportamiento: documentar en CHANGELOG — el nivel de entrada mono sube +3 dB respecto de versiones previas). JNI/Kotlin solo si NoisyPad quiere exponerlo.

## 4.6 — Tests

Suite nueva host-side `usb/tests/test_format_converter.cpp` (el converter no depende de libusb):

1. **Identidad bit-perfect**: para cada formato, recorrer todos los valores S16 (65536) / muestreo denso de S24: `pcm → float → pcm` reproduce los bytes exactos con dither off + softClip 1.0.
2. **Redondeo**: valores a medio LSB van al par más cercano (round-half-even), sin sesgo: media del error de cuantización sobre ruido uniforme < 1e-6 FS.
3. **Clamp**: +1.0 → 32767/8388607/INT32_MAX; −1.0 → −32768/−8388608/INT32_MIN; ±1.5 saturado sin wrap.
4. **Dither**: con dither on, la media del error sigue ~0 y la desviación ≈ LSB/√6 + TPDF esperado; determinismo con seed fija.
5. **S24_4LE**: justificación MSB (`<<8`) verificada contra el decoder (`>>8`) — round-trip exacto.

## Criterios de aceptación
- Tests de 4.6 verdes.
- Reproducción de un archivo de referencia −0.1 dBFS por el DAC: captura por loopback (Fase 5 hardware) sin diferencias audibles ni recorte en el pico (antes, el soft clip alteraba todo el rango > 0.95).
- Sin regresión de CPU en `floatToS16/S24` (lrintf ≤ cast en ARM64; verificar con benchmark simple si hay dudas).

## Riesgos
- El cambio de monoGain y de escala (32767→32768) altera niveles/bits respecto de versiones publicadas: coordinar versionado semántico con NoisyPad (es el único consumidor).
