# Fase 5 — Test de round-trip en dispositivo real (loopback físico)

**Objetivo:** medición automatizada y confiable de la latencia round-trip **real** (analógica, end-to-end) usando un DAC USB full-duplex con la salida conectada a la entrada por un cable miniplug↔miniplug. La librería provee toda la lógica (estímulo, captura, análisis, estado, resultado); **NoisyPad implementa solo la UI** consumiendo la API Kotlin.

Es además la herramienta de verificación de las Fases 1–3 (criterios de aceptación) y la fuente del `measuredRoundTripMs` que persiste la Fase 2.3.

**Depende de:** Fase 0 (L7, para el desglose software vs analógico). Puede desarrollarse en paralelo a la Fase 1.
**Estimación:** 3–4 días (núcleo C++ 2, plumbing JNI/Kotlin 1, validación 1).

---

## 5.1 — Estado actual (qué se reusa y qué se descarta)

| Activo | Estado | Decisión |
|---|---|---|
| `utils/LatencyBenchmark.h` → `RoundTripDetector` | Detección por umbral (0.1) de un impulso único; sensible a nivel/ruido; sin estadística | **Descartar** como detector; sirve de referencia de estructura RT-safe |
| `jni/jni_benchmark.cpp:129-164` (`nativeStartRoundTripTest` etc.) | Scaffolding muerto: el estado nunca avanza de 1, nada en el camino de audio lo alimenta; ligado al engine Oboe | **Reemplazar** (mantener nombres viejos como deprecated para no romper `LatencyAnalyzer.kt`) |
| `api/latency/LatencyAnalyzer.kt` (`runRoundTripTestFlow`) | Flow de polling sobre el scaffolding muerto | Migrar al backend nuevo |
| `internal/usb/UsbAudioTestRunner.kt:373` (`runLoopbackTest`) | Stub que devuelve SKIPPED | Implementar sobre la API nueva |
| `domain/usb/UsbTestResult.kt` (`UsbTestType.LOOPBACK`) | Tipos commonMain ya definidos | Extender con los campos del resultado |

---

## 5.2 — Diseño del medidor (`usb/RoundTripMeasurer.{h,cpp}`)

### Principio

Separación estricta en dos planos:
- **Plano RT** (dentro del audio callback): genera el estímulo en `outputData` y copia `inputData` a un buffer de captura preasignado, con contador de samples absoluto. Cero alloc, cero locks, cero análisis.
- **Plano de análisis** (worker thread): correlación cruzada y estadística cuando la captura termina.

### Integración con el backend — callback swap

El medidor **es un `IAudioCallback`**. Para medir la configuración viva (perfil, jitter budget convergido, rings en régimen) se instala en caliente:

```cpp
// LibusbBackend: mCallback pasa de IAudioCallback* a
std::atomic<IAudioCallback*> mCallback;   // el DSP loop ya lo lee por iteración
```

- `LibusbBackend::swapCallback(IAudioCallback* next) → IAudioCallback* prev` (nuevo, thread-safe, válido con el stream corriendo). El DSP loop carga el puntero una vez por iteración (`memory_order_acquire`) — el swap es glitchless por construcción (el test emite silencio salvo los bursts).
- Secuencia del test: requiere `FULL_DUPLEX` activo → `swapCallback(measurer)` → ciclo de medición → `swapCallback(original)`. Si el stream no está en duplex, error `REQUIRES_FULL_DUPLEX` (la UI ofrece reiniciar en duplex).
- Backend-agnóstico: al implementarse contra `IAudioCallback`, el mismo medidor sirve para el camino Oboe en el futuro (fuera de alcance de esta fase, pero no introducir dependencias de LibusbBackend en el measurer).

### Estímulo

**Chirp lineal enventanado**, no impulso:
- 10 ms (480 samples @48 kHz), barrido 500 Hz → 6 kHz, ventana Hann, amplitud configurable (default 0.25 ≈ −12 dBFS).
- Razones: energía ~50× la de un click al mismo pico (robusto al ruido y a niveles bajos de loopback), pico de autocorrelación agudo y único (resolución sub-ms garantizada), espectro dentro de la banda plana de cualquier códec (sobrevive AC coupling y anti-alias de DACs baratos, donde un impulso de 1 sample se degrada).
- Precomputado en `start()` (fuera del RT path) para el sample rate del stream activo.

### Máquina de estados (plano RT, atomics)

```
IDLE → CALIBRATING → MEASURING(burst k de N) → ANALYZING → COMPLETE | ERROR
```

**CALIBRATING (≈500 ms):** emite tono de 1 kHz a la amplitud configurada; mide RMS y pico de entrada.
- RMS entrada < −60 dBFS → `ERROR_NO_SIGNAL` ("cable no conectado o volumen a cero").
- Pico > −1 dBFS → reducir amplitud ×0.5 y reintentar (máx 2); si persiste → `ERROR_CLIPPING`.
- Resultado: gain estimado del loop (informativo) y validación del setup antes de gastar tiempo en bursts.

**MEASURING:** N bursts (default 10), espaciados `burstIntervalMs` (default 300 ms — debe superar la latencia máxima esperable; con perfil SAFE ~70 ms sobra; configurable hasta 1000 para depurar).
- El callback mantiene `mSampleCounter` (frames absolutos desde el inicio del test).
- Al emitir el burst k: registra `emitSample[k] = mSampleCounter + offsetEnBloque`.
- La entrada se copia continuamente (mono: canal L) a un buffer circular preasignado de 4 s con indexación absoluta — el análisis necesita la ventana `[emitSample[k], emitSample[k] + searchWindow]` con `searchWindow = 250 ms`.

**ANALYZING (worker thread):** por burst:
1. Correlación cruzada normalizada de la ventana de búsqueda contra el template del chirp (directa O(W×L) ≈ 12000×480 ≈ 5.8 M MACs por burst — trivial fuera del RT path; no hace falta FFT).
2. `latencySamples[k] = argmax(corr) − emitSample[k]`.
3. Confianza: `peak / max(sidelobe fuera de ±2 ms del pico)`; burst válido si > 3.0 y pico > 4× el piso de ruido de la calibración.

**Agregación:**
- Sobre los bursts válidos: mediana, MAD, min, max. Outliers > 3×MAD se excluyen y se recalcula.
- Resultado `COMPLETE` si ≥ 7 de N bursts válidos; si no, `ERROR_UNRELIABLE` con el conteo.

### Resultado

```cpp
struct RoundTripResult {
    float medianMs, madMs, minMs, maxMs;
    int   validBursts, totalBursts;
    float confidence;            // mediana de confidencias
    // Desglose (requiere L7 de Fase 0):
    float softwareOutputMs;      // getOutputLatencyMs() promediado durante el test
    float softwareInputMs;
    float residualMs;            // median − (out + in) ≈ conversores + URB sched + analógico
    // Contexto:
    int sampleRate; int profile; int jitterBudgetMs;
};
```

El **residual** es el validador de honestidad del sistema: si `residualMs` no está en ~1–3 ms, o la medición o el reporte de software (L7) están mal — es un test del test.

### Presupuesto de memoria (preasignado en `start()`, mlock)
- Captura: 4 s mono float = 768 KB.
- Template: 480 floats. Correlación: buffer de salida 12 k floats.
- Total < 1 MB, liberado al terminar.

---

## 5.3 — Superficie JNI (en `jni/jni_audio_bridge.cpp`, regla del repo)

```c
// Config empaquetada en floats para evitar structs JNI:
// [0]=burstCount [1]=burstIntervalMs [2]=amplitude [3]=searchWindowMs
jboolean nativeUsbRoundTripStart(jfloatArray config);
// Poll (50–100 ms desde Kotlin):
// [0]=state [1]=progressPct [2]=currentBurst [3]=medianMs [4]=madMs
// [5]=confidence [6]=softwareOutMs [7]=softwareInMs [8]=validBursts
// [9]=errorCode
jfloatArray nativeUsbRoundTripPoll();
void nativeUsbRoundTripCancel();   // restaura el callback original SIEMPRE
```

- Mutex de categoría: `lifecycleMutex` (start/cancel mutan el callback del backend).
- `nativeUsbRoundTripStart` falla (false) si: backend USB no corriendo, no duplex, test ya activo.
- **Garantía de restauración**: el swap-back del callback ocurre en `cancel`, en `COMPLETE/ERROR`, y en el teardown del backend (si el stream muere mid-test, el measurer detecta `onBackendError`/stop y marca `ERROR_STREAM_LOST`; el JNI de cancel es idempotente).
- Los viejos `nativeStartRoundTripTest/GetRoundTripResult/CancelRoundTripTest` (`jni_benchmark.cpp:129-164`) quedan deprecated devolviendo el estado del medidor nuevo cuando el backend activo es USB (compat con `LatencyAnalyzer.kt`), o IDLE si no.

## 5.4 — API Kotlin

### commonMain (`domain/usb/` + `api/`)

```kotlin
enum class RoundTripTestState { IDLE, CALIBRATING, MEASURING, ANALYZING, COMPLETE, ERROR }

enum class RoundTripTestError {
    NONE, NO_SIGNAL, CLIPPING, UNRELIABLE, REQUIRES_FULL_DUPLEX, STREAM_LOST, TIMEOUT
}

data class RoundTripTestProgress(
    val state: RoundTripTestState,
    val progressPct: Float,          // 0..100 (calibración 0-10, bursts 10-90, análisis 90-100)
    val currentBurst: Int, val totalBursts: Int,
)

data class RoundTripTestResult(
    val medianMs: Float, val jitterMs: Float,   // jitter = MAD
    val minMs: Float, val maxMs: Float,
    val confidence: Float, val validBursts: Int, val totalBursts: Int,
    val softwareOutputMs: Float, val softwareInputMs: Float, val residualMs: Float,
    val sampleRate: Int, val profile: UsbLatencyProfile,
    val error: RoundTripTestError,
)

data class RoundTripTestConfig(
    val burstCount: Int = 10,
    val burstIntervalMs: Int = 300,
    val amplitude: Float = 0.25f,
)

interface IRoundTripLatencyTester {
    val progress: StateFlow<RoundTripTestProgress>
    suspend fun run(config: RoundTripTestConfig = RoundTripTestConfig()): RoundTripTestResult
    fun cancel()
}
```

(En commonMain la interfaz usa el patrón del repo: nada de `android.*`; el `StateFlow` viene de kotlinx-coroutines, ya permitido.)

### androidMain

- `RoundTripLatencyTesterImpl` (en `internal/usb/`): loop de polling (`delay(75)`) sobre `AudioNativeBridge.usbRoundTripPoll()`, mapea a los data classes, expone el `StateFlow`. Timeout global = `burstCount × interval + 5 s`.
- Wiring en `UsbAudioTestRunner.runLoopbackTest` (`:373`): pre-check de duplex (ya existe, `:82-90`) → delega en el tester → vuelca el resultado a `UsbTestResult` (extender los tipos de `UsbTestResult.kt` con los campos de round-trip).
- Persistencia: al `COMPLETE`, guardar `medianMs` en `UsbLatencyTuningRepository` (Fase 2.3) si existe; si la Fase 2 no está, omitir (acoplamiento opcional).

## 5.5 — Contrato de UI para NoisyPad (informativo)

La librería garantiza todo lo medible; la UI solo necesita:
1. Pantalla de preparación: instrucción gráfica "conectá OUT → IN con el cable miniplug" + botón Iniciar (habilitado si `isFullDuplex`).
2. Durante el test: `progress.collect` → barra + indicador por burst (`currentBurst/totalBursts`). El usuario debe escuchar N chirps cortos — advertir que es normal y a volumen moderado.
3. Resultado: `medianMs` grande, `± jitterMs`, semáforo de calidad (`confidence`, `validBursts`), desglose out/in/residual en vista expandible, botón "guardar/compartir".
4. Errores accionables: NO_SIGNAL → revisar cable/volumen; CLIPPING → bajar volumen del DAC; UNRELIABLE → ambiente con ruido eléctrico o cable defectuoso.

## 5.6 — Tests

**Host-side (sin hardware)** — el análisis es lógica pura:
- `test_roundtrip_analyzer.cpp`: señal sintética = template retardado D samples + ruido blanco a SNR {40, 20, 10 dB} + respuesta de canal simple (LPF 1er orden) → el analizador recupera D ± 1 sample en 40/20 dB y reporta no-confiable o correcto en 10 dB (nunca un valor falso con confianza alta).
- Outliers: 8 bursts a D, 2 a D+800 → mediana = D, los 2 excluidos.
- Calibración: silencio → NO_SIGNAL; señal a 0 dBFS → ruta de reducción de amplitud.
- Máquina de estados: secuencias de cancel en cada estado restauran y terminan limpio.

**En dispositivo (manual, luego CI física si existe):**
- Los 3 DACs × perfiles {SAFE, LOW_LATENCY}: 5 corridas consecutivas → desviación de `medianMs` entre corridas < 0.5 ms.
- Coherencia: `medianMs ≈ softwareOut + softwareIn + (1–3 ms)`.
- Cable desconectado → NO_SIGNAL en < 2 s, callback original restaurado, stream sigue sano.
- Test durante reproducción activa de NoisyPad: el swap silencia el programa durante el test y lo restaura sin glitch ni leak.

## Criterios de aceptación
1. Resultado repetible (< 0.5 ms de dispersión entre corridas) en los 3 DACs.
2. `residualMs` ∈ [0.5, 4] ms en todos — valida tanto la medición como L7.
3. Robustez: ninguna combinación de cancel/disconnect/error deja el backend sin su callback original o con el measurer colgado.
4. NoisyPad puede construir la UI completa sin tocar nada nativo (la API Kotlin de 5.4 es suficiente).

## Riesgos
- **DACs con mute/anti-pop en la salida**: los primeros ms tras el primer audio pueden estar atenuados → la calibración (tono de 500 ms antes del primer burst) lo absorbe; mantener el orden calibración→bursts.
- **Niveles de loopback muy dispares** (salida de auriculares → entrada de línea/mic): cubierto por la calibración con ajuste de amplitud; si la entrada tiene AGC de hardware, el chirp corto puede medirse con ganancia variable — la correlación normalizada es inmune al gain, solo afecta la confianza.
- **Swap de callback en caliente**: el cambio de `IAudioCallback*` a atomic toca el DSP loop (1 línea) — revisar que no haya otro lector del puntero fuera del loop (`LibusbBackend.cpp:1628,1847,1922`: todos dentro del loop o en error-path no-RT).
