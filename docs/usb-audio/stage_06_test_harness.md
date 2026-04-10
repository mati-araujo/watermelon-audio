# Etapa 6 — Test harness profesional

**Estado:** propuesta — no iniciada.
**Dependencias:** stages 1–5 mergeados (la API está consolidada, los backends son estables).
**Duración estimada:** 4–6 días.
**Severidad de los gaps que cubre:** 1× menor (test runner actual) + capacidad de regression ≥ 25 devices.

---

## 1. Objetivo

Transformar `UsbAudioTestRunner` de un orquestador de stats polling en un verdadero laboratorio de validación de devices USB audio. Hoy sus tests son:

- **Playback**: asume tono pre-generado, solo mira stats.
- **Capture**: input levels placeholder hardcoded.
- **Loopback**: delega a playback en full-duplex — no mide round-trip real.
- **Stress**: playback prolongado.
- **Diagnostic**: playback a 44.1 y 48.

Al terminar esta etapa, el test harness debe generar sus propios tonos/impulses, medir latencia de loopback con precisión de µs mediante correlation, hacer tone sweep con FFT para respuesta en frecuencia, calcular THD y SNR, y ejecutar una device matrix automatizable desde CI (en devices con ADB) o manual (smoke test humano con ≥ 25 devices).

---

## 2. Test primitives nativas

El harness necesita fuentes de señal y analizadores en el lado C++, invocables desde Kotlin vía JNI simple. Se añaden como nuevos módulos en `audio/src/main/cpp/test_harness/`.

### 2.1 Generadores de señal

```cpp
// audio/src/main/cpp/test_harness/SignalGenerator.h
namespace watermelon_audio::test_harness {

class SignalGenerator {
public:
    virtual ~SignalGenerator() = default;
    virtual void fill(float* buffer, int numFrames, int numChannels) = 0;
    virtual void reset() = 0;
};

class SineGenerator : public SignalGenerator {
public:
    SineGenerator(float freqHz, float amplitude, int sampleRateHz);
    void fill(float* buffer, int numFrames, int numChannels) override;
    void reset() override;
};

class ImpulseGenerator : public SignalGenerator {
public:
    // Produces a single 1.0 peak at the configured frame, zeros elsewhere.
    // Intended for loopback latency measurement via correlation.
    ImpulseGenerator(int impulseFrame);
    void fill(float* buffer, int numFrames, int numChannels) override;
    void reset() override;
    int emitFrame() const { return mImpulseFrame; }
private:
    int mImpulseFrame;
    int mFramesEmitted = 0;
    bool mEmitted = false;
};

class LogSweepGenerator : public SignalGenerator {
public:
    // Logarithmic sine sweep from freqStart to freqEnd over `durationSec`.
    // Useful for impulse response extraction via deconvolution.
    LogSweepGenerator(float freqStartHz, float freqEndHz, float durationSec,
                       float amplitude, int sampleRateHz);
    void fill(float* buffer, int numFrames, int numChannels) override;
    void reset() override;
};

class WhiteNoiseGenerator : public SignalGenerator {
public:
    WhiteNoiseGenerator(float amplitude, uint64_t seed);
    void fill(float* buffer, int numFrames, int numChannels) override;
    void reset() override;
};

}  // namespace
```

### 2.2 Analizadores

```cpp
// audio/src/main/cpp/test_harness/LoopbackLatencyMeter.h
namespace watermelon_audio::test_harness {

class LoopbackLatencyMeter {
public:
    LoopbackLatencyMeter(int expectedImpulseFrameInOutput, int sampleRateHz);

    // Feed the captured input buffer. Returns the detected latency in frames
    // if the impulse has been detected, or nullopt if still searching.
    std::optional<int> feedCapturedFrames(const float* inputBuffer, int numFrames);

    float detectedLatencyMs() const;
    float correlationConfidence() const;  // 0..1
    int   peakFrame() const;
    void  reset();

private:
    int mExpectedOutputFrame;
    int mSampleRate;
    int mFramesSinceStart = 0;
    std::vector<float> mInputHistory;  // circular buffer, ~500ms
    int mHistoryWrite = 0;
    std::optional<int> mDetectedLatencyFrames;
    float mPeakAbs = 0.0f;
    int mPeakFrameRelative = -1;
    static constexpr float DETECTION_THRESHOLD = 0.1f;  // |sample| > 0.1 marks the impulse
};

}  // namespace
```

**Algoritmo de detección.** Simple y robusto: se sabe en qué frame exacto se emitió el impulso. Se busca en el input el primer frame donde `|sample| > 0.1` (umbral configurable). La latencia = `inputFrameAbs - outputFrameAbs`. La correlación es el cociente entre el peak detectado y el segundo peak (debe ser > 3:1 para alta confianza).

Para mayor robustez en presencia de ruido: cross-correlation (FFT-based o naive) entre el buffer de output conocido (golden) y el buffer de input capturado. Fuera del scope de esta primer versión; se deja como extension point.

```cpp
// audio/src/main/cpp/test_harness/FrequencyAnalyzer.h
namespace watermelon_audio::test_harness {

class FrequencyAnalyzer {
public:
    // Window sizes: 1024, 2048, 4096. Uses a simple radix-2 FFT.
    FrequencyAnalyzer(int fftSize, int sampleRateHz);

    // Feed captured frames. Results accumulate until computeStats() is called.
    void feed(const float* input, int numFrames);

    struct Stats {
        float fundamentalHz;     // strongest bin
        float fundamentalAmpDb;
        float thdPercent;        // total harmonic distortion (2nd..10th harmonic)
        float snrDb;              // signal to noise ratio
        float sinadDb;            // signal to noise and distortion
        float noiseFloorDb;
        std::vector<float> binsDb;  // full spectrum in dB (size = fftSize/2+1)
    };
    Stats computeStats();
    void  reset();

private:
    int mFftSize;
    int mSampleRate;
    std::vector<float> mWindow;       // Hann window
    std::vector<float> mAccumulator;  // accumulated frames for next FFT
    int mAccumCount = 0;
    std::vector<std::complex<float>> mFftOutput;
    std::vector<float> mAveragedPower;
    int mAveragedCount = 0;

    void applyFft(const float* timeDomain, std::complex<float>* freqDomain);
};

}  // namespace
```

**FFT interna.** Implementación simple radix-2 en C++ (no depende de KissFFT u otra). Para tamaños 1024/2048/4096 es rápida suficiente y no añade dependencia. Dejar TODO para migrar a KissFFT si se necesita mayor tamaño.

### 2.3 JNI bindings nuevos

```cpp
// audio/src/main/cpp/jni/jni_test_harness.cpp
extern "C" {

JNIEXPORT jlong JNICALL nativeCreateSineGenerator(freqHz, amp, sampleRate);
JNIEXPORT jlong JNICALL nativeCreateImpulseGenerator(impulseFrame);
JNIEXPORT jlong JNICALL nativeCreateLogSweep(freqStart, freqEnd, durationSec, amp, sampleRate);
JNIEXPORT void  JNICALL nativeDestroyGenerator(jlong handle);

JNIEXPORT jlong JNICALL nativeCreateLoopbackMeter(expectedFrame, sampleRate);
JNIEXPORT jint  JNICALL nativeFeedLoopbackMeter(jlong handle, jfloatArray frames);
JNIEXPORT jfloat JNICALL nativeGetLoopbackLatencyMs(jlong handle);

JNIEXPORT jlong JNICALL nativeCreateFrequencyAnalyzer(fftSize, sampleRate);
JNIEXPORT void  JNICALL nativeFeedAnalyzer(jlong handle, jfloatArray samples);
JNIEXPORT jfloatArray JNICALL nativeComputeAnalyzerStats(jlong handle);

// Set the currently active generator for the audio callback to consume
JNIEXPORT void  JNICALL nativeSetTestSource(jlong generatorHandle);

}  // extern "C"
```

### 2.4 Integración con el audio callback

Añadir en `IAudioCallback` un modo test: cuando hay un generator registrado (via `nativeSetTestSource`), el output se llena con `generator->fill()` en lugar del synth del engine. El input se enruta al meter/analyzer registrado. El rest del DSP pipeline permanece intacto.

Esto permite que los tests corran **dentro del engine real** (no en un stream separado), validando el path completo.

---

## 3. Tests refactorizados

### 3.1 `LOOPBACK_LATENCY` — medición real

```kotlin
// UsbAudioTestRunner.kt
suspend fun runLoopbackLatencyTest(config: UsbTestConfig): UsbTestResult {
    require(config.streamingMode == UsbStreamingMode.FULL_DUPLEX) {
        "Loopback test requires full-duplex mode"
    }

    val sampleRate = config.sampleRate
    val impulseFrame = 4800  // 100ms into the stream at 48kHz
    val generator = TestHarness.createImpulseGenerator(impulseFrame)
    val meter = TestHarness.createLoopbackLatencyMeter(impulseFrame, sampleRate)

    usbAudioManager.startStreaming(
        UsbStreamConfig(sampleRate, 2, 24, UsbStreamingMode.FULL_DUPLEX)
    ).getOrThrow()

    try {
        TestHarness.attachAsActiveSource(generator)
        TestHarness.attachAsActiveMeter(meter)

        // Run for ~1 second; the impulse emits at 100ms, input arrives later.
        delay(1000)

        val latencyMs = meter.detectedLatencyMs()
        val confidence = meter.correlationConfidence()

        return UsbTestResult(
            testType = UsbTestType.LOOPBACK,
            passed = latencyMs > 0 && latencyMs <= config.maxAllowedLatencyMs
                      && confidence >= 0.8f,
            measuredLatencyMs = latencyMs,
            confidence = confidence,
            // ... other fields from stats
        )
    } finally {
        TestHarness.detachAll()
        usbAudioManager.stopStreaming()
        generator.destroy()
        meter.destroy()
    }
}
```

Retorna la latencia real en ms con confianza de detección. El hw loopback físico (entrada conectada a salida con cable de interface audio) es requerido — esto es normal en pro audio testing.

### 3.2 `TONE_SWEEP_FR` — respuesta en frecuencia

```kotlin
suspend fun runToneSweepTest(config: UsbTestConfig): UsbTestResult {
    val generator = TestHarness.createLogSweep(
        freqStartHz = 20f,
        freqEndHz = 20000f,
        durationSec = 10f,
        amplitude = 0.5f,
        sampleRate = config.sampleRate,
    )
    val analyzer = TestHarness.createFrequencyAnalyzer(
        fftSize = 4096,
        sampleRate = config.sampleRate,
    )

    usbAudioManager.startStreaming(
        UsbStreamConfig(config.sampleRate, 2, 24, UsbStreamingMode.FULL_DUPLEX)
    ).getOrThrow()

    try {
        TestHarness.attachAsActiveSource(generator)
        TestHarness.attachAsActiveMeter(analyzer)
        delay(11_000)  // sweep + settle

        val stats = analyzer.computeStats()
        val frNonflatness = calculateFrNonflatness(stats.binsDb, 20f..20000f)
        val passed = frNonflatness < 3.0  // within 3dB is acceptable

        return UsbTestResult(
            testType = UsbTestType.FREQUENCY_RESPONSE,
            passed = passed,
            frNonflatnessDb = frNonflatness,
            // ... full spectrum can be attached as blob for UI plot
        )
    } finally {
        TestHarness.detachAll()
        usbAudioManager.stopStreaming()
    }
}
```

### 3.3 `THD_SINAD` — calidad de señal

Emite una sinusoidal a 1 kHz, captura 3 segundos, corre el `FrequencyAnalyzer`, lee `stats.thdPercent` y `stats.sinadDb`. Pasa si THD < 0.1% y SINAD > 60 dB (típicos de devices pro).

### 3.4 `DRIFT_STABILITY` (ya en stage 3, aquí se refina)

Extender el test de stage 3 para correlacionar el drift con la ventana de observación: drift instantáneo vs drift acumulado en 60 segundos. Graficable.

### 3.5 `CAPTURE_LEVEL` real

Antes era un placeholder. Ahora:

```kotlin
suspend fun runCaptureLevelTest(config: UsbTestConfig): UsbTestResult {
    usbAudioManager.startStreaming(config.toStreamConfig()).getOrThrow()
    val analyzer = TestHarness.createFrequencyAnalyzer(2048, config.sampleRate)
    TestHarness.attachAsActiveMeter(analyzer)

    delay(config.durationMs)

    val stats = analyzer.computeStats()
    // Peak level and noise floor from spectrum
    val peakDb = stats.binsDb.max()
    val noiseFloorDb = stats.noiseFloorDb

    return UsbTestResult(
        testType = UsbTestType.CAPTURE_LEVEL,
        passed = peakDb > -80 && peakDb < 0,  // sensible input range
        inputPeakDb = peakDb,
        inputNoiseFloorDb = noiseFloorDb,
    )
}
```

Ya no necesita hardcoded placeholders.

---

## 4. Device matrix automation

### 4.1 Objetivo

Correr la suite completa en N devices y producir un reporte consolidado. Útil tanto para CI (parcial, con devices conectados a un host de test) como para QA manual (human-operated).

### 4.2 `DeviceMatrixRunner`

```kotlin
// androidMain/.../internal/usb/DeviceMatrixRunner.kt
class DeviceMatrixRunner(
    private val manager: IUsbAudioManager,
    private val testRunner: UsbAudioTestRunner,
) {
    data class Result(
        val deviceId: String,
        val deviceName: String,
        val vendorId: Int,
        val productId: Int,
        val timestamp: Long,
        val testResults: List<UsbTestResult>,
        val capabilitySnapshot: UsbCapabilitySnapshot?,
    ) {
        val passRate: Double get() = testResults.count { it.passed }.toDouble() / testResults.size
    }

    suspend fun runStandardSuite(timeoutMsPerTest: Long = 30_000L): Result {
        val device = manager.selectedDevice.value
            ?: error("No device selected")
        val snapshot = manager.currentCapabilitySnapshot.value
        val tests = listOf(
            UsbTestPresets.RATE_NEGOTIATION_SWEEP,
            UsbTestPresets.LOOPBACK_LATENCY,
            UsbTestPresets.THD_SINAD,
            UsbTestPresets.DRIFT_STABILITY,
            UsbTestPresets.CAPTURE_LEVEL,
        )
        val results = tests.map { config ->
            withTimeout(timeoutMsPerTest) { testRunner.runTest(config) }
        }
        return Result(
            deviceId = device.deviceId,
            deviceName = device.name,
            vendorId = device.vendorId,
            productId = device.productId,
            timestamp = System.currentTimeMillis(),
            testResults = results,
            capabilitySnapshot = snapshot,
        )
    }

    fun exportMatrixReport(results: List<Result>, format: Format): String
    enum class Format { HUMAN_READABLE, MARKDOWN_TABLE, CSV, JSON }
}
```

### 4.3 Reporte en Markdown

Ejemplo de output:

```markdown
# USB Audio Device Matrix — 2026-04-12

| Device | VID:PID | UAC | Rate | Loopback | THD | SINAD | Drift | Overall |
|---|---|---|---|---|---|---|---|---|
| Scarlett Solo 3rd Gen | 1235:8211 | 2.0 | ✓ all | 7.2ms | 0.04% | 92dB | 12ppm | ✅ 5/5 |
| C-Media UC02 | 0D8C:0014 | 1.0 | ✓ 44/48 | 18.1ms | 0.8% | 58dB | 85ppm | ⚠️ 4/5 |
| FiiO BTR5 | 2972:0047 | 1.0 | ✓ all | 12.4ms | 0.02% | 108dB | 5ppm | ✅ 5/5 |
...
```

Cada fila es un device; cada columna un test. Exportable a CSV para tracking entre versiones.

### 4.4 Regression detection

Un test de regresión diff: comparar el reporte actual con un baseline checkeado al repo (`audio/src/androidTest/resources/baselines/device_matrix.csv`). Cualquier degradación (pass → fail, latencia > 10% peor, SINAD > 2 dB peor) se loggea como warning.

---

## 5. Infraestructura `TestHarness` Kotlin

```kotlin
// commonMain/.../test_harness/TestHarness.kt (con expect/actual)
expect object TestHarness {
    fun createSineGenerator(freqHz: Float, amplitude: Float, sampleRate: Int): SignalGeneratorHandle
    fun createImpulseGenerator(impulseFrame: Int): SignalGeneratorHandle
    fun createLogSweep(startHz: Float, endHz: Float, durationSec: Float, amp: Float, sampleRate: Int): SignalGeneratorHandle
    fun createWhiteNoise(amplitude: Float, seed: Long): SignalGeneratorHandle

    fun createLoopbackLatencyMeter(expectedImpulseFrame: Int, sampleRate: Int): LoopbackMeterHandle
    fun createFrequencyAnalyzer(fftSize: Int, sampleRate: Int): FrequencyAnalyzerHandle

    fun attachAsActiveSource(generator: SignalGeneratorHandle)
    fun attachAsActiveMeter(analyzer: LoopbackMeterHandle)
    fun attachAsActiveMeter(analyzer: FrequencyAnalyzerHandle)
    fun detachAll()
}

// androidMain actual delegates to JNI
actual object TestHarness { ... }
```

`SignalGeneratorHandle` y siblings son wrappers Kotlin alrededor del `jlong` nativo, con `Closeable` para cleanup.

---

## 6. Criterios de aceptación

- [ ] `SignalGenerator` (Sine, Impulse, LogSweep, WhiteNoise) implementados en C++ con tests unitarios.
- [ ] `LoopbackLatencyMeter` detecta correctamente un impulso con confidence > 0.95 en un test sintético (input = output delayed).
- [ ] `FrequencyAnalyzer` calcula correctamente:
  - Fundamental de un tono 1 kHz puro (± 0.5 Hz).
  - THD < 0.001% para una sinusoidal limpia.
  - SNR > 100 dB para una sinusoidal a amplitud 0.5 sin ruido.
- [ ] JNI bindings operacionales y sin leaks en 100 iteraciones consecutivas de create/destroy.
- [ ] `UsbAudioTestRunner.runTest(LOOPBACK_LATENCY)` retorna latencia real medida en un device con loopback físico.
- [ ] `runTest(TONE_SWEEP_FR)` produce un espectro válido y un cálculo de non-flatness.
- [ ] `runTest(THD_SINAD)` mide THD < 0.1% y SINAD > 60 dB en un device pro (Scarlett Solo).
- [ ] `DeviceMatrixRunner.runStandardSuite()` ejecutable en un device conectado, genera un reporte completo.
- [ ] `exportMatrixReport(MARKDOWN_TABLE)` produce una tabla bien formateada.
- [ ] Baseline `device_matrix.csv` checkeado al repo con ≥ 5 devices testeados.
- [ ] Documentación en `docs/testing/usb-audio-matrix.md` explicando cómo reproducir la matrix localmente.

---

## 7. Riesgos específicos

1. **FFT interna vs KissFFT.** Implementación naive puede ser lenta para 4096. Medir antes de comprometer; si es > 5 ms por iteración en ARM64 mid-range, migrar a KissFFT.
2. **Loopback físico requerido.** El test de latencia real necesita cable TRS/XLR conectando output a input. Documentar claramente en el quickstart.
3. **Device matrix tiempo total.** 5 tests × 20 devices × ~30 seg por test = 50 minutos por ciclo completo. Acotar con paralelización imposible (un device a la vez) → aceptar el tiempo.
4. **THD medido es del conjunto host+device.** No se puede separar. Documentar que los números son "end-to-end".
5. **Baseline drift natural.** Los números no son determinísticos — tolerancias amplias en la regression detection (10%).

---

## 8. Checklist de commit

Diff aproximado:

- `audio/src/main/cpp/test_harness/SignalGenerator.{h,cpp}` **nuevo** ~250 líneas
- `audio/src/main/cpp/test_harness/LoopbackLatencyMeter.{h,cpp}` **nuevo** ~180 líneas
- `audio/src/main/cpp/test_harness/FrequencyAnalyzer.{h,cpp}` **nuevo** ~400 líneas (incluye FFT)
- `audio/src/main/cpp/jni/jni_test_harness.cpp` **nuevo** ~250 líneas
- `audio/src/main/cpp/CMakeLists.txt` +5 líneas
- `audio/src/commonMain/kotlin/.../test_harness/TestHarness.kt` **nuevo** ~120 líneas (expect)
- `audio/src/androidMain/kotlin/.../test_harness/TestHarness.android.kt` **nuevo** ~180 líneas (actual)
- `audio/src/androidMain/kotlin/.../test_harness/*Handle.kt` **nuevo** ~100 líneas
- `audio/src/commonMain/kotlin/.../domain/usb/UsbTestResult.kt` +80 líneas (nuevos campos)
- `audio/src/androidMain/kotlin/.../internal/usb/UsbAudioTestRunner.kt` +400 líneas (tests nuevos, runTest refactor)
- `audio/src/androidMain/kotlin/.../internal/usb/DeviceMatrixRunner.kt` **nuevo** ~220 líneas
- `audio/src/test/cpp/test_harness/SignalGenerator_test.cpp` **nuevo** ~180 líneas
- `audio/src/test/cpp/test_harness/LoopbackLatencyMeter_test.cpp` **nuevo** ~150 líneas
- `audio/src/test/cpp/test_harness/FrequencyAnalyzer_test.cpp` **nuevo** ~200 líneas
- `audio/src/androidTest/resources/baselines/device_matrix.csv` **nuevo**
- `docs/testing/usb-audio-matrix.md` **nuevo** ~300 líneas

Commit messages sugeridos:
1. `feat(test): signal generators (sine, impulse, sweep, noise)`
2. `feat(test): LoopbackLatencyMeter with impulse detection`
3. `feat(test): FrequencyAnalyzer with FFT and THD/SINAD`
4. `feat(test): JNI bindings for test harness primitives`
5. `feat(test): refactor UsbAudioTestRunner with real measurements`
6. `feat(test): DeviceMatrixRunner for automated device suites`
7. `test(regression): device matrix baseline with 5 pro devices`
8. `docs(test): usb audio matrix testing guide`

---

## 9. Siguiente etapa

[stage_07_android14_bitperfect.md](stage_07_android14_bitperfect.md) agrega el fast path Android 14+ con AAudio MMAP bit-perfect, manteniendo libusb como fallback.
