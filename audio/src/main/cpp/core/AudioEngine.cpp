#include "AudioEngine.h"

// Oboe is Android-only. Everywhere else the engine runs exclusively through
// BackendManager/IAudioBackend — which is the path iOS uses with
// CoreAudioBackend (WA-2.4). The direct-Oboe path below is legacy: it predates
// IAudioBackend and is kept because it is what Android ships today.
//
// AudioEngine.h needs no guard: it already forward-declares oboe::AudioStream
// and hides the callback adapter behind an opaque unique_ptr<void>, so the
// header is Oboe-free by construction.
#if defined(__ANDROID__)
#define WMA_HAS_OBOE 1
#include <oboe/Oboe.h>
#else
#define WMA_HAS_OBOE 0
#endif
#include "../backends/BackendManager.h"
#include "../nodes/InputNode.h"
#include "../dsp/SIMDUtils.h"
#include "../platform/Logger.h"
#include "../platform/Platform.h"
#include "../voice/TouchTriggerSource.h"
#include <thread>
#include <chrono>

#if WMA_HAS_OBOE
// ========== OBOE CALLBACK ADAPTER (Phase 0B) ==========
// Bridges the legacy direct-Oboe path to AudioEngine's IAudioCallback interface.
// This adapter owns the oboe::AudioStreamCallback inheritance so AudioEngine doesn't need to.
class OboeCallbackAdapter : public oboe::AudioStreamCallback {
public:
    explicit OboeCallbackAdapter(AudioEngine* engine) : mEngine(engine) {}

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* oboeStream,
                                          void* audioData,
                                          int32_t numFrames) override {
        if (!mEngine) return oboe::DataCallbackResult::Stop;

        // XRun monitoring (only available with direct Oboe stream)
        monitorXRuns(oboeStream);

        // Delegate to the engine's unified processing via IAudioCallback
        auto result = mEngine->onAudioReady(
            static_cast<float*>(audioData), nullptr, numFrames);

        return (result == watermelon_audio::IAudioCallback::Result::CONTINUE)
            ? oboe::DataCallbackResult::Continue
            : oboe::DataCallbackResult::Stop;
    }

    void onErrorBeforeClose(oboe::AudioStream* stream, oboe::Result error) override {
        wma::logMessage(wma::LogLevel::ERROR, "OboeAdapter",
            "Stream error before close: %s (%d)", oboe::convertToText(error), static_cast<int>(error));
        if (mEngine) {
            mEngine->onBackendError(watermelon_audio::BackendError::FATAL);
        }
    }

    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
        wma::logMessage(wma::LogLevel::ERROR, "OboeAdapter",
            "Stream error after close: %s (%d)", oboe::convertToText(error), static_cast<int>(error));
        if (mEngine) {
            if (error == oboe::Result::ErrorDisconnected) {
                mEngine->onBackendError(watermelon_audio::BackendError::DEVICE_DISCONNECTED);
            } else {
                mEngine->onBackendError(watermelon_audio::BackendError::FATAL);
            }
        }
    }

private:
    void monitorXRuns(oboe::AudioStream* oboeStream) {
        if (!oboeStream) return;
        // WD-1.5 — era un `static int`, o sea GLOBAL DE PROCESO: dos instancias
        // del motor compartian el contador y el muestreo dependia de cuantos
        // motores existieran. Ahora es del adapter, que es lo que mide.
        if (++mXRunCheckCount < 500) return;
        mXRunCheckCount = 0;

        // WD-1.1 — era un logMessage directo desde el thread de audio, y en el
        // peor caso posible: se dispara justo cuando el motor NO llega a tiempo,
        // asi que el log agregaba un syscall encima de un underrun. El conteo
        // ahora se publica al motor, que ya lo expone al thread de control.
        auto xrunResult = oboeStream->getXRunCount();
        if (xrunResult) {
            int32_t count = xrunResult.value();
            if (count > mLastXRunCount) {
                mLastXRunCount = count;
                if (mEngine) mEngine->publishXRunCount(count);
            }
        }
    }

    AudioEngine* mEngine;
    int32_t mLastXRunCount = 0;
    int mXRunCheckCount = 0;
};

// Custom deleter for opaque OboeCallbackAdapter pointer
void AudioEngine::OboeAdapterDeleter::operator()(void* p) const {
    delete static_cast<OboeCallbackAdapter*>(p);
}
#else
// Without Oboe nothing ever populates mOboeAdapter, but the deleter is
// declared in the header and must still link.
void AudioEngine::OboeAdapterDeleter::operator()(void*) const {}
#endif  // WMA_HAS_OBOE

#define LOG_TAG "AudioEngine"

// ========== LOGGING CONFIGURATION ==========
// In release builds, disable verbose logging in audio callback to prevent glitches
// Error logging is always enabled
#ifdef NDEBUG
    // Release build: Only errors, no info/warning in hot path
    #define LOGI(...) ((void)0)
    #define LOGW(...) ((void)0)
    #define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)
    // Callback-specific logging disabled in release
    #define LOGI_CALLBACK(...) ((void)0)
    static constexpr bool AUDIO_DIAG_ENABLED = false;
#else
    // Debug build: Full logging
    #define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
    #define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
    #define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)
    // Callback logging only in debug (periodic, low frequency)
    #define LOGI_CALLBACK(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
    static constexpr bool AUDIO_DIAG_ENABLED = true;
#endif

// ========== AUDIO_DIAG: Diagnostic logging for engine lifecycle ==========
// Filter with: adb logcat -s AUDIO_DIAG
#define AUDIO_DIAG_TAG "AUDIO_DIAG"
#define AUDIO_DIAG(...) do { \
    if (AUDIO_DIAG_ENABLED) wma::logMessage(wma::LogLevel::INFO, AUDIO_DIAG_TAG, __VA_ARGS__); \
} while(0)

AudioEngine::AudioEngine() {
    // Wire looper event dispatcher into AudioLooper BEFORE starting the
    // dispatcher worker thread, so the first audio callback can already push.
    mAudioLooper.setEventDispatcher(&mLooperEventDispatcher);
    mLooperEventDispatcher.start();

    // IMPROVED (Fase 2.2.3): Manejo robusto de memoria insuficiente
    try {
        // Intentar pre-alocar buffers con tamaño completo
        mOutputStage.resizeTempBuffer(8192);  // 4096 frames * 2 channels

        // Waveform capture initialized via WaveformCapture default constructor (1024 samples)

        mUsingReducedBuffers = false;

        LOGI("AudioEngine: Full-size buffers allocated successfully (waveform double-buffered)");

    } catch (const std::bad_alloc& e) {
        LOGE("Failed to allocate full-size buffers: %s", e.what());

        // FALLBACK: Intentar con buffers más pequeños
        try {
            mOutputStage.resizeTempBuffer(0);

            mOutputStage.resizeTempBuffer(4096);  // Reducir a la mitad (2048 frames * 2 channels)
            mWaveformCapture.resize(512);
            mUsingReducedBuffers = true;

            LOGW("Using reduced buffer sizes due to memory constraints (4096 samples, waveform double-buffered)");

        } catch (const std::bad_alloc& e2) {
            LOGE("FATAL: Cannot allocate even reduced buffers: %s", e2.what());
            mInitializationFailed.store(true, std::memory_order_release);
            return;  // No continuar con la inicialización
        }
    }

    // Solo inicializar osciladores si la allocación de memoria tuvo éxito
    if (!mInitializationFailed.load(std::memory_order_acquire)) {
        try {
            // Oscillators and modulators now owned by OscillatorBank (Phase 1E)
            LOGI("OscillatorBank ready: %d oscillators, modulators included",
                 mOscBank.getOscillatorCount());

            // ========== SYNTH ENGINES (Phase 6, Phase 1E — owned by SynthEngineDispatcher) ==========
            // mEngineDispatcher is constructed as a member, engines are already allocated
            LOGI("SynthEngineDispatcher ready (engines allocated in member constructor)");

            // Pre-alocar buffer para monitoring (Full-Duplex)
            try {
                mMonitoringBuffer.resize(8192);  // 4096 frames * 2 channels
                // Mono, so half: one sample per frame of the buffer above.
                mVocoderMonoBuffer.resize(4096);
                LOGI("Monitoring buffer allocated successfully");
            } catch (const std::bad_alloc& e) {
                LOGE("Failed to allocate monitoring buffer: %s", e.what());
                // No es fatal, monitoring simplemente no estará disponible
            }

            // ========== MIXER NODE INTEGRATION (Phase 3.1) ==========
            try {
                mMixerNode = std::make_unique<MixerNode>();

                // Pre-allocate AudioBuffers (will be resized in start() with actual sample rate)
                mOscillatorBuffer.setSize(2, 4096);
                mInputBuffer.setSize(2, 4096);
                mMixerOutputBuffer.setSize(2, 4096);

                LOGI("MixerNode and AudioBuffers allocated successfully");
            } catch (const std::bad_alloc& e) {
                LOGE("Failed to allocate MixerNode: %s", e.what());
                mMixerNode.reset();
                // Not fatal - will fall back to inline mixing
            }

            // ========== OSCILLATOR NODE (Phase 3.2) ==========
            try {
                mOscillatorNode = std::make_unique<OscillatorNode>();
                // Register synth engines with OscillatorNode (Phase 6)
                // Engine instances are owned by SynthEngineDispatcher
                auto registerIfPresent = [&](EngineTypeId id) {
                    SynthEngine* eng = mEngineDispatcher.getEngine(static_cast<int>(id));
                    if (eng) mOscillatorNode->registerEngine(static_cast<int>(id), eng);
                };
                registerIfPresent(EngineTypeId::KARPLUS_STRONG);
                registerIfPresent(EngineTypeId::FM_SYNTH);
                registerIfPresent(EngineTypeId::SUPERSAW);
                registerIfPresent(EngineTypeId::WAVETABLE);
                registerIfPresent(EngineTypeId::GRANULAR);
                registerIfPresent(EngineTypeId::SOUNDFONT);
                LOGI("OscillatorNode allocated successfully");
            } catch (const std::bad_alloc& e) {
                LOGE("Failed to allocate OscillatorNode: %s", e.what());
                mOscillatorNode.reset();
                // Not fatal - will use legacy oscillators
            }

            // ========== EFFECT CHAIN NODE (Phase 3.3) ==========
            try {
                mEffectChainNode = std::make_unique<EffectChainNode>();

                // Pre-allocate effect output buffer (will be resized in start())
                mEffectOutputBuffer.setSize(2, 4096);

                LOGI("EffectChainNode and effect buffer allocated successfully");
            } catch (const std::bad_alloc& e) {
                LOGE("Failed to allocate EffectChainNode: %s", e.what());
                mEffectChainNode.reset();
                // Not fatal - will use legacy mEffectChain
            }

            // ========== VOICE SYSTEM (Phase 2 - Polyphonic Voices) ==========
            try {
                mVoiceManager = std::make_unique<voice::VoiceManager>();

                // Register TouchTriggerSource
                auto touchSource = std::make_unique<voice::TouchTriggerSource>();
                mVoiceManager->registerSource(std::move(touchSource));

                LOGI("VoiceManager allocated with TouchTriggerSource");
            } catch (const std::bad_alloc& e) {
                LOGE("Failed to allocate VoiceManager: %s", e.what());
                mVoiceManager.reset();
                // Not fatal - will use legacy dual touch
            }

            LOGI("AudioEngine constructed successfully with %d oscillators",
                 mOscBank.getOscillatorCount());

        } catch (const std::bad_alloc& e) {
            LOGE("FATAL: Cannot allocate components: %s", e.what());
            mInitializationFailed.store(true, std::memory_order_release);
        }
    } else {
        LOGE("AudioEngine initialization failed - memory allocation error");
    }
}

AudioEngine::~AudioEngine() {
    // Reclaim the deferred-stop worker FIRST, before touching mFadeCtrl: it runs
    // stop()->mFadeCtrl.cancel(), so joining it here is what keeps that off the
    // destructor's own cancel() below (the data race TSan caught) and stops it
    // outliving `this`.
    mStopFadeCancel.store(true, std::memory_order_release);
    if (mStopFadeThread && mStopFadeThread->joinable()) {
        mStopFadeThread->join();
    }

    // RAII: Cancel pending fade thread and ensure stream is closed
    mFadeCtrl.cancel();
    stop();

    // Stop the looper dispatcher AFTER the audio stream is closed so no
    // RT thread can still be pushing while we drain & join the worker.
    mAudioLooper.setEventDispatcher(nullptr);
    mLooperEventDispatcher.setSink(nullptr);
    mLooperEventDispatcher.stop();

    // IMPROVED (Fase 2.1.4): Limpiar recovery thread si existe
    if (mRecoveryThread && mRecoveryThread->joinable()) {
        mRecoveryThread->join();
    }

    // WD-1.3 — despublicar el InputNode antes de que se destruya con el objeto.
    //
    // stop() ya cerro el stream, asi que no deberia quedar ningun callback; esto
    // es el cinturon del tirante. Sin el, mInputNodeRt queda apuntando a un
    // objeto que el destructor de mInputNode esta por liberar, y basta un
    // callback tardio de un backend que no respetara su propio stop() para
    // convertirlo en un UAF.
    mInputNodeRt.store(nullptr, std::memory_order_release);
    waitForCallbackDrain(std::chrono::milliseconds(250));

    LOGI("AudioEngine destroyed");
}

void AudioEngine::rollbackFailedStart() {
    // Un start que ya pasó por Running y falló DESPUÉS.
    //
    // start() transiciona a Running *antes* de arrancar el backend, y eso es
    // deliberado: es el fix PHASE 7.1 contra la carrera en la que el thread DSP
    // empieza a llamar onAudioReady() con el estado todavía en Starting y
    // devuelve silencio. No hay que deshacerlo.
    //
    // Lo que estaba mal era el rollback. Hacía `transitionToState(Stopped)`
    // directo, y la tabla de transiciones sólo admite Running -> Stopping. La
    // transición se descartaba con un `Invalid state transition: 2 -> 0` en el
    // log y **el motor quedaba en Running sin stream**: `getEngineState()`
    // devolvía Running, y el siguiente start() veía "ya corre" y no hacía nada,
    // así que un fallo transitorio dejaba el motor inservible hasta reiniciar
    // el proceso.
    //
    // Pasar por Stopping no es un rodeo burocrático: es el mismo camino que usa
    // stop(), y deja el estado siguiendo a la realidad en vez de al revés.
    transitionToState(EngineState::Stopping);
    transitionToState(EngineState::Stopped);

    // El fade se arrancó antes de tocar el backend, para que los primeros
    // callbacks vieran una rampa válida. Si el arranque no prosperó, esa rampa
    // no la va a consumir nadie: dejarla viva hace que isFading() informe una
    // transición en curso sobre un motor detenido.
    //
    // Hacen falta las dos llamadas y no alcanza con cancel(). `cancel()` mata el
    // worker del stop-fade y setea su flag, pero **no toca
    // mFadeRemainingFrames**, que es justo lo que lee isFading(). El reset de
    // los contadores se hace con un fade de largo cero — es el mismo idiom que
    // ya usa la ruta de reset del motor, no una invención de acá.
    mFadeCtrl.cancel();
    mFadeCtrl.startFade(0.0f, 0.0f, 48000, 0);
}

bool AudioEngine::transitionToState(EngineState newState) {
    EngineState currentState = mState.load(std::memory_order_acquire);

    // Validar transiciones permitidas
    bool validTransition = false;
    switch (currentState) {
        case EngineState::Stopped:
            validTransition = (newState == EngineState::Starting);
            break;
        case EngineState::Starting:
            validTransition = (newState == EngineState::Running || newState == EngineState::Stopped);
            break;
        case EngineState::Running:
            validTransition = (newState == EngineState::Stopping);
            break;
        case EngineState::Stopping:
            validTransition = (newState == EngineState::Stopped);
            break;
    }

    if (!validTransition) {
        LOGE("Invalid state transition: %d -> %d", static_cast<int>(currentState), static_cast<int>(newState));
        return false;
    }

    // Intentar hacer la transición atómicamente
    if (mState.compare_exchange_strong(currentState, newState, std::memory_order_acq_rel)) {
        LOGI("State transition: %d -> %d", static_cast<int>(currentState), static_cast<int>(newState));
        // IMPROVED: Incrementar versión al cambiar estado (Fase 2.1.3)
        incrementStateVersion();
        return true;
    }

    return false;
}

bool AudioEngine::start(int fadeTimeMs) {
    std::lock_guard<std::mutex> lock(mStateMutex);

    // WD-1.2 — esto NO configura el thread de audio, y ahora el comentario lo
    // dice. Es el diagnostico de arranque: deja una linea de log con la rama de
    // ISA que se compilo, para que un build no pueda perder de vista con que
    // implementacion quedo. El flush que SI le sirve al thread RT esta al
    // principio de onAudioReady().
    wma::platform::flushDenormals();

    // IMPROVED (Fase 2.2.3): Verificar que la inicialización fue exitosa
    if (mInitializationFailed.load(std::memory_order_acquire)) {
        LOGE("Cannot start: initialization failed (memory allocation error)");
        return false;
    }

    // Verificar estado actual
    EngineState currentState = mState.load(std::memory_order_acquire);
    if (currentState != EngineState::Stopped) {
        LOGE("Cannot start: engine is not in Stopped state (current: %d)", static_cast<int>(currentState));
        return false;
    }

    // Transicionar a Starting
    if (!transitionToState(EngineState::Starting)) {
        LOGE("Failed to transition to Starting state");
        return false;
    }

    // ========== BACKEND MANAGER PATH (USB Audio Phase 1) ==========
    // When BackendManager is enabled, delegate stream lifecycle to it
    if (mUseBackendManager.load(std::memory_order_acquire)) {
        LOGI("Starting via BackendManager...");

        auto& manager = watermelon_audio::BackendManager::getInstance();

        // Ensure callback is set
        manager.setCallback(this);

        // ====================================================================
        // Nobody may have chosen a backend, and without one start() can only
        // fail. BackendManager builds the platform's system backend in its
        // constructor but never selects it: selectBackend() is called only from
        // wma_select_backend() and AudioEngineImpl.setAudioBackend(), both at
        // the consumer's request. Nothing in the public API requires that call,
        // and AudioEngine.start() (Kotlin) does not make it.
        //
        // On Android this never showed because mUseBackendManager is false
        // there — the direct Oboe path does not come through here at all. Off
        // Android the flag defaults to true and this IS the only way to open a
        // stream, so the engine could never start: "BackendManager: No backend
        // selected". Found by the WA-5.5 harness on the first tap, with the ten
        // gate commands green; the host suite missed it because CApiFixture
        // calls wma_select_backend(1) by hand.
        //
        // A default is the right shape rather than making every consumer pick:
        // asking for a stream without naming a backend can only mean "the one
        // this platform has". An explicit choice still wins — the check is for
        // NONE, so this never overrides a caller who did decide.
        //
        // It goes here and NOT inside BackendManager::start(): that method holds
        // mMutex, selectBackend() takes the same non-recursive mutex, and the
        // two together deadlock.
        // ====================================================================
        if (manager.getCurrentType() == watermelon_audio::BackendType::NONE) {
            LOGI("No backend selected — defaulting to the system backend");
            // OBOE is the system-backend slot in this enum on every platform:
            // resolveBackendForSplit() maps it to mSystemBackend, which is
            // CoreAudioBackend on iOS and the fake in the host suite. The name
            // is Android's history, not a claim about which backend this is.
            if (!manager.selectBackend(watermelon_audio::BackendType::OBOE)) {
                LOGE("Cannot start: no system backend available on this platform");
                transitionToState(EngineState::Stopped);
                return false;
            }
        }

        // MINI-007 — aca habia un `manager.setSampleRate(preferido)` para que el
        // device negociara a un rate pedido. El unico escritor de ese campo era
        // un setter que NINGUNA superficie publica alcanzaba (cero `wma_*`, cero
        // `JNIEXPORT`), asi que en un telefono el valor era siempre 0 y esta rama
        // no se tomo nunca. El rate lo negocia el backend.
        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "[START] entry: useBackendMgr=1 fadeTimeMs=%d", fadeTimeMs);

        // ====================================================================
        // CRITICAL: configure components BEFORE starting the backend.
        //
        // The Oboe path (lines 590+) prepares all components — OutputStage,
        // OscillatorBank, EffectChain, voice manager, audio graph nodes —
        // and only THEN calls mStream->requestStart(). That ordering means
        // the very first audio callback fires into a fully-initialised state.
        //
        // Stage 1's USB path inverted this: manager.start() spun up the DSP
        // thread, then configureComponentsWithSampleRate() ran on the main
        // thread. The DSP thread's first onAudioReady() callback (which can
        // fire within ~1 ms of manager.start() returning) would hit the
        // shared component state mid-configuration. On a cold launch the
        // components were still in constructor defaults (wrong sample rate,
        // unprepared filter coefficients) → first ~ms of output garbled
        // and that garbled audio got captured into the USB ring buffer.
        // Subsequent stop/start cycles inherited valid state from the
        // previous configureComponents call so the bug appeared to "go away"
        // after one mode switch.
        //
        // Fix: prepare everything with the preferred sample rate now. If
        // the device coerces to a different rate during manager.start(),
        // re-run configureComponentsWithSampleRate() afterwards (the call
        // is idempotent for matching rates).
        // ====================================================================
        // MINI-007: con el rate preferido borrado, esta expresion resolvia
        // SIEMPRE a la rama del literal — no hay rate que pedir antes de que el
        // device negocie.
        //
        // Se deja el literal A LA VISTA en vez de esconderlo detras de
        // `currentSampleRate()`: consultarlo aca leeria el backend, que en un
        // test con el manager ya corriendo contesta OTRO rate, y eso seria un
        // cambio de comportamiento colado en un MINI que promete cero. El camino
        // que lo corrige ya existe y es el `configureComponentsWithSampleRate()`
        // de despues de `manager.start()` — que es exactamente la doctrina del
        // baseline de `check-literal-rate.py`: "el arreglo no es cambiar el
        // numero, es que exista un camino que lo corrija".
        //
        // 🔴 Ese lint NO lo reporta (medido: sigue en 18 llamadas, ninguna nueva)
        // porque persigue LLAMADAS que preparan un subsistema con un literal, y
        // esto es la inicializacion de una local. Queda dicho para que nadie lo
        // lea como "ya esta declarado en algun lado".
        //
        // 🔴 `test_rate_reconfiguration.cpp` DEPENDE de este valor: es el que
        // hace que el fake, negociando 44100, produzca coercion. Esa dependencia
        // NO se deja escrita en un comentario —se probo que un comentario no la
        // sostiene: con este rate en 44100 los dos tests de coercion quedan
        // verdes sin ejercer nada, y la suite entera tambien— sino en el
        // `static_assert` que ese archivo hace contra la constante.
        const int expectedRate = kPreNegotiationSampleRate;
        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "[START] pre-configure components: expectedRate=%d", expectedRate);
        configureComponentsWithSampleRate(expectedRate);

        // Pre-start the fade envelope so initial callbacks see a valid
        // (0 → 1) ramp instead of whatever the previous stop() left in
        // mFadeCtrl (typically 0 → 0).
        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "[START] pre-fade: 0.0 -> 1.0 over %dms @ %dHz",
            fadeTimeMs, expectedRate);
        mFadeCtrl.startFade(0.0f, 1.0f, expectedRate, fadeTimeMs);
        mFadeCtrl.setPaused(false);

        // FIX PHASE 7.1: Transition to Running BEFORE starting the backend
        // This prevents the race condition where the DSP thread starts calling
        // onAudioReady() while state is still Starting, causing silence.
        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "[START] transition -> Running");
        if (!transitionToState(EngineState::Running)) {
            LOGE("Failed to transition to Running state");
            wma::logMessage(wma::LogLevel::ERROR, "WMA_AUDIT",
                "[START] transition -> Running FAILED");
            transitionToState(EngineState::Stopped);
            return false;
        }

        // NOW start the backend. Callbacks fire into already-prepared
        // components and the engine state is Running.
        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "[START] calling manager.start()...");
        watermelon_audio::BackendResult result = manager.start();
        if (result != watermelon_audio::BackendResult::OK) {
            LOGE("Failed to start via BackendManager: %s",
                 watermelon_audio::backendResultToString(result));
            wma::logMessage(wma::LogLevel::ERROR, "WMA_AUDIT",
                "[START] manager.start() FAILED: %s",
                watermelon_audio::backendResultToString(result));
            rollbackFailedStart();
            return false;
        }
        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "[START] manager.start() -> OK");

        // Defensive: verify the actual rate the device negotiated. If it
        // differs from our pre-configuration assumption, re-prepare
        // components with the corrected rate. In practice the three test
        // DACs (GHW UAC1, UGREEN CM720 UAC2, C-Media UC02 UAC1) all honor
        // the requested rate exactly, so this fallback is rarely hit.
        watermelon_audio::StreamInfo info = manager.getStreamInfo();
        int actualRate = info.sampleRate;

        LOGI("=== BACKEND MANAGER STREAM STARTED ===");
        LOGI("  Backend type: %s", watermelon_audio::backendTypeToString(info.backendType));
        LOGI("  Sample rate: %d Hz (expected %d)", actualRate, expectedRate);
        LOGI("  Channel count: %d", info.channelCount);
        LOGI("  Buffer size: %d frames", info.framesPerBuffer);
        LOGI("  Output latency: %.1f ms", info.outputLatencyMs);
        LOGI("======================================");

        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "[START] getStreamInfo: actualRate=%d channels=%d frames=%d latencyMs=%.1f",
            actualRate, info.channelCount, info.framesPerBuffer, info.outputLatencyMs);

        if (actualRate > 0 && actualRate != expectedRate) {
            LOGW("Device coerced sample rate %d -> %d, re-configuring components",
                 expectedRate, actualRate);
            wma::logMessage(wma::LogLevel::WARN, "WMA_AUDIT",
                "[START] reconfigure needed: YES (expected=%d actual=%d)",
                expectedRate, actualRate);
            configureComponentsWithSampleRate(actualRate);
            mFadeCtrl.startFade(0.0f, 1.0f, actualRate, fadeTimeMs);
            mFadeCtrl.setPaused(false);
        } else {
            wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
                "[START] reconfigure needed: NO (actualRate=%d matches expectedRate)",
                actualRate);
        }

        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "START_USB_FADE: sampleRate=%d, fadeTimeMs=%d",
            actualRate > 0 ? actualRate : expectedRate, fadeTimeMs);

        LOGI("AudioEngine started via BackendManager successfully");
        return true;
    }

    // ========== LEGACY OBOE PATH ==========
    // Direct Oboe stream creation (original code)
#if !WMA_HAS_OBOE
    // Reaching here off Android means the caller never enabled BackendManager,
    // and there is no other way to open a stream. Callers on those platforms
    // must call setUseBackendManager(true) (the constructor defaults it to true
    // where Oboe is unavailable, so this is a misuse, not a normal path).
    LOGE("Cannot start: no Oboe on this platform and BackendManager is disabled");
    transitionToState(EngineState::Stopped);
    return false;
#else

    // Construir y abrir stream
    // CRITICAL FIX: Use Shared mode to allow automatic device routing (headphones, etc.)
    // Exclusive mode locks to the device at creation time and doesn't switch
    // Create Oboe callback adapter (bridges to IAudioCallback)
    if (!mOboeAdapter) {
        mOboeAdapter.reset(new OboeCallbackAdapter(this));
    }
    auto* adapter = static_cast<OboeCallbackAdapter*>(mOboeAdapter.get());

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)  // Changed from Exclusive for proper routing
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setCallback(adapter)
            ->setErrorCallback(adapter);

    // MINI-007 — aca habia un `builder.setSampleRate(preferido)`. Sin escritor
    // alcanzable, la rama era inalcanzable en device: el camino real siempre fue
    // la seleccion automatica.
    LOGI("Using auto sample rate selection");

    oboe::Result result = builder.openStream(mStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open stream: %s", oboe::convertToText(result));
        mStream.reset();
        transitionToState(EngineState::Stopped);
        return false;
    }

    // ========== CRITICAL: CONFIGURE BUFFER SIZE FOR GLITCH-FREE AUDIO ==========
    // Set buffer size to 2x frames per burst (double buffering) to prevent underruns
    // This is crucial for devices with problematic audio hardware
    int framesPerBurst = mStream->getFramesPerBurst();
    int targetBufferSize = framesPerBurst * 2;  // Double buffering minimum

    // On some devices, we may need even larger buffers
    // If burst is very small (<128), use at least 256 frames
    if (targetBufferSize < 256) {
        targetBufferSize = 256;
    }

    oboe::Result bufferResult = mStream->setBufferSizeInFrames(targetBufferSize);
    int actualBufferSize = mStream->getBufferSizeInFrames();

    if (bufferResult != oboe::Result::OK) {
        LOGW("Could not set buffer size to %d: %s (actual: %d)",
             targetBufferSize, oboe::convertToText(bufferResult), actualBufferSize);
    } else {
        LOGI("Buffer size configured: requested=%d, actual=%d frames",
             targetBufferSize, actualBufferSize);
    }

    // Reset XRun counter for monitoring
    mLastXRunCount = 0;

    // Configurar SampleRate en TODOS los osciladores
    int sampleRate = mStream->getSampleRate();

    // DEBUG: Log stream info for routing diagnostics
    LOGI("=== OUTPUT STREAM OPENED ===");
    LOGI("  Sample rate: %d Hz", sampleRate);
    LOGI("  Channel count: %d", mStream->getChannelCount());
    LOGI("  Sharing mode: %s", mStream->getSharingMode() == oboe::SharingMode::Shared ? "Shared" : "Exclusive");
    LOGI("  Performance mode: %s",
         mStream->getPerformanceMode() == oboe::PerformanceMode::LowLatency ? "LowLatency" :
         mStream->getPerformanceMode() == oboe::PerformanceMode::PowerSaving ? "PowerSaving" : "None");
    LOGI("  Device ID: %d", mStream->getDeviceId());
    LOGI("  Frames per burst: %d", framesPerBurst);
    LOGI("  Buffer size: %d frames (capacity: %d)", actualBufferSize, mStream->getBufferCapacityInFrames());
    LOGI("  Estimated latency: %.1f ms", (float)actualBufferSize / sampleRate * 1000.0f);
    LOGI("==============================");

    // Configure oscillators and modulators (Phase 1E — delegated to OscillatorBank)
    mOscBank.prepare(sampleRate);

    // Configurar sample rate en la cadena de efectos
    mEffectChain.setSampleRate(sampleRate);

    // ========== SYNTH ENGINE PREPARATION (Phase 6, Phase 1E — delegated to SynthEngineDispatcher) ==========
    {
#if WMA_HAS_OBOE
        int framesPerBurst = mStream ? mStream->getFramesPerBurst() : 256;
#else
        // No Oboe: the backend owns the burst size. 256 is the same fallback
        // the Oboe path uses when the stream is not open yet.
        const int framesPerBurst = 256;
#endif
        int maxBlock = std::max(framesPerBurst * 4, 4096);
        mEngineDispatcher.prepare(sampleRate, maxBlock);
        // Assign engines to voices
        mEngineDispatcher.updateVoiceEngines(mVoiceManager.get());
        LOGI("Synth engines prepared via dispatcher: sr=%d, maxBlock=%d", sampleRate, maxBlock);
    }

    // ========== MIXER NODE PREPARATION (Phase 3.1) ==========
    if (mMixerNode) {
        int framesPerBurst = mStream->getFramesPerBurst();
        int maxBlockSize = std::max(framesPerBurst * 4, 4096);  // Allow for larger blocks

        mMixerNode->prepare(sampleRate, maxBlockSize);

        // Resize AudioBuffers to match actual block size
        mOscillatorBuffer.setSize(2, maxBlockSize);
        mInputBuffer.setSize(2, maxBlockSize);
        mMixerOutputBuffer.setSize(2, maxBlockSize);

        // Connect AudioBuffers to MixerNode inputs
        mMixerNode->setInputBuffer(MixerNode::INPUT_OSCILLATOR, &mOscillatorBuffer);
        mMixerNode->setInputBuffer(MixerNode::INPUT_EXTERNAL, &mInputBuffer);

        LOGI("MixerNode prepared with sampleRate=%d, maxBlockSize=%d", sampleRate, maxBlockSize);
    }

    // ========== OSCILLATOR NODE PREPARATION (Phase 3.2) ==========
    if (mOscillatorNode) {
        int framesPerBurst = mStream->getFramesPerBurst();
        int maxBlockSize = std::max(framesPerBurst * 4, 4096);

        mOscillatorNode->prepare(sampleRate, maxBlockSize);
        LOGI("OscillatorNode prepared with sampleRate=%d, maxBlockSize=%d", sampleRate, maxBlockSize);
    }

    // ========== EFFECT CHAIN NODE PREPARATION (Phase 3.3) ==========
    if (mEffectChainNode) {
        int framesPerBurst = mStream->getFramesPerBurst();
        int maxBlockSize = std::max(framesPerBurst * 4, 4096);

        mEffectChainNode->prepare(sampleRate, maxBlockSize);

        // Resize effect output buffer to match actual block size
        mEffectOutputBuffer.setSize(2, maxBlockSize);

        LOGI("EffectChainNode prepared with sampleRate=%d, maxBlockSize=%d", sampleRate, maxBlockSize);
    }

    // ========== VOICE MANAGER PREPARATION (Phase 2 - Polyphonic Voices) ==========
    if (mVoiceManager) {
        int framesPerBurst = mStream->getFramesPerBurst();
        int maxBlockSize = std::max(framesPerBurst * 4, 4096);

        mVoiceManager->prepare(sampleRate, maxBlockSize);
        LOGI("VoiceManager prepared with sampleRate=%d, maxBlockSize=%d", sampleRate, maxBlockSize);
    }

    // ========== CHORD HARMONY PREPARATION (Phase 9C) ==========
    {
        int framesPerBurst = mStream->getFramesPerBurst();
        int maxBlockSize = std::max(framesPerBurst * 4, 4096);
        mChordHarmony.prepare(sampleRate, maxBlockSize);
        LOGI("ChordHarmony prepared with sampleRate=%d, maxBlockSize=%d", sampleRate, maxBlockSize);
    }

    // Reset DSP state to prevent transients from previous session
    mOutputStage.prepare(sampleRate, 0);
    LOGI("DSP state reset and configured at %dHz sample rate", sampleRate);

    // Configure fade-in using the provided fade time (prevents clicks on start)
    mFadeCtrl.startFade(0.0f, 1.0f, sampleRate, fadeTimeMs);
    mFadeCtrl.setPaused(false);

    // FIX: Transition to Running BEFORE starting the stream to prevent
    // the race condition where the callback fires with state=Starting (silence)
    // and then suddenly switches to Running (full audio = click)
    if (!transitionToState(EngineState::Running)) {
        LOGE("Failed to transition to Running state");
        mStream->close();
        mStream.reset();
        transitionToState(EngineState::Stopped);
        return false;
    }

    // Iniciar stream (callback will see Running state immediately)
    result = mStream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start stream: %s", oboe::convertToText(result));
        mStream->close();
        mStream.reset();
        transitionToState(EngineState::Stopped);
        return false;
    }

    LOGI("AudioEngine started successfully");
    AUDIO_DIAG("ENGINE START: state=Running, sampleRate=%d, bufferSize=%d", sampleRate, actualBufferSize);
    return true;
#endif  // WMA_HAS_OBOE
}

void AudioEngine::stop() {
    std::unique_lock<std::mutex> lock(mStateMutex);

    // WD-2.1 — un motor offline no tiene backend que cerrar. Sale por el camino
    // corto: transicion de estado y nada mas. Sin esto se le pediria a
    // BackendManager que pare algo que nunca arranco.
    if (mOfflineMode.load(std::memory_order_acquire)) {
        EngineState offlineState = mState.load(std::memory_order_acquire);
        if (offlineState == EngineState::Stopped || offlineState == EngineState::Stopping) {
            return;
        }
        transitionToState(EngineState::Stopping);
        mFadeCtrl.cancel();
        mOfflineMode.store(false, std::memory_order_release);
        mOfflineMaxBlockFrames.store(0, std::memory_order_release);
        transitionToState(EngineState::Stopped);
        LOGI("Motor offline detenido");
        return;
    }

    // Verificar estado actual
    EngineState currentState = mState.load(std::memory_order_acquire);
    if (currentState == EngineState::Stopped || currentState == EngineState::Stopping) {
        LOGI("Engine already stopped or stopping");
        return;
    }

    // Transicionar a Stopping
    if (!transitionToState(EngineState::Stopping)) {
        LOGE("Failed to transition to Stopping state");
        return;
    }

    // ========== BACKEND MANAGER PATH (USB Audio Phase 1) ==========
    if (mUseBackendManager.load(std::memory_order_acquire)) {
        LOGI("Stopping via BackendManager...");

        auto& manager = watermelon_audio::BackendManager::getInstance();
        manager.stop();

        // Wait for any remaining callbacks
        auto timeout = std::chrono::milliseconds(500);
        mStopCondition.wait_for(lock, timeout, [this] {
            return mActiveCallbacks.load(std::memory_order_acquire) == 0;
        });

        // FIX: Clear buffers and reset DSP on stop via BackendManager too
        mOutputStage.clearTempBuffer();
        mWaveformCapture.clear();
        mOutputStage.reset();
        // Reset oscillators via OscillatorBank (Phase 1E)
        mOscBank.setAllPrimaryParams(440.0f, 0.0f);
        for (int i = 0; i < mOscBank.getOscillatorCount(); ++i) {
            auto* osc = mOscBank.getPrimaryOscillator(i);
            if (osc) osc->resetPhase();
            auto* osc2 = mOscBank.getSecondaryOscillator(i);
            if (osc2) { osc2->resetPhase(); osc2->setParameters(440.0f, 0.0f); }
        }
        if (mVoiceManager) { mVoiceManager->reset(); }
        mFadeCtrl.cancel();
        mFadeCtrl.startFade(0.0f, 0.0f, 48000, 0);  // Reset to zero
        mFadeCtrl.setPaused(false);

        transitionToState(EngineState::Stopped);
        LOGI("AudioEngine stopped via BackendManager");
        AUDIO_DIAG("ENGINE STOP (BackendManager): state=Stopped, buffers cleared");
        return;
    }

    // ========== LEGACY OBOE PATH ==========
    // Detener el stream (esto previene nuevos callbacks)
#if WMA_HAS_OBOE
    if (mStream) {
        LOGI("Stopping audio stream...");
        mStream->stop();
    }
#endif

    // CRÍTICO: Esperar a que todos los callbacks activos terminen
    // Timeout de 1 segundo para evitar deadlocks
    auto timeout = std::chrono::milliseconds(1000);
    bool callbacksFinished = mStopCondition.wait_for(lock, timeout, [this] {
        return mActiveCallbacks.load(std::memory_order_acquire) == 0;
    });

    if (!callbacksFinished) {
        LOGE("WARNING: Timeout waiting for audio callbacks to finish (active: %d)",
             mActiveCallbacks.load());
    } else {
        LOGI("All audio callbacks finished");
    }

    // Cerrar el stream
#if WMA_HAS_OBOE
    if (mStream) {
        mStream->close();
        mStream.reset();
        LOGI("Audio stream closed");
    }
#endif

    // FIX: Clear all internal buffers to prevent dirty buffer clicks on next start
    mOutputStage.clearTempBuffer();
    mWaveformCapture.clear();
    mDualTouch.clearBuffers();
    if (!mMonitoringBuffer.empty()) std::fill(mMonitoringBuffer.begin(), mMonitoringBuffer.end(), 0.0f);

    // Reset DSP state to prevent transients on restart
    mOutputStage.reset();

    // Reset oscillator phases and amplitudes to prevent clicks on restart (Phase 1E)
    mOscBank.setAllPrimaryParams(440.0f, 0.0f);
    for (int i = 0; i < mOscBank.getOscillatorCount(); ++i) {
        auto* osc = mOscBank.getPrimaryOscillator(i);
        if (osc) osc->resetPhase();
        auto* osc2 = mOscBank.getSecondaryOscillator(i);
        if (osc2) { osc2->resetPhase(); osc2->setParameters(440.0f, 0.0f); }
    }

    // Reset voice system to clear any lingering voices
    if (mVoiceManager) {
        mVoiceManager->reset();
    }

    // Reset fade state
    mFadeCtrl.cancel();
    mFadeCtrl.startFade(0.0f, 0.0f, 48000, 0);  // Reset to zero
    mFadeCtrl.setPaused(false);

    LOGI("Buffers and DSP state cleared");

    // Transicionar a Stopped
    transitionToState(EngineState::Stopped);
    LOGI("AudioEngine stopped successfully");
    AUDIO_DIAG("ENGINE STOP: state=Stopped, buffers cleared");
}

void AudioEngine::updateXY(float x, float y) {
    // Lock-free: solo usa atómicos en los osciladores
    float freq = 50.0f + (x * 1950.0f);
    float amp = y;

    // When arp is active, don't set oscillator params from UI thread.
    // The arp controls freq/amp from the audio callback (avoids amplitude race P0-4).
    // Only feed base frequency to arp sequencer.
    if (mArpSequencer.isEnabled()) {
        mArpSequencer.setBaseFrequency(freq);
        mEffectChain.setVocoderCarrierFrequency(freq);
        return;
    }

    // Normal path: update all oscillators
    mOscBank.setAllPrimaryParams(freq, amp);

    // Sync vocoder carrier frequency with XY pad
    mEffectChain.setVocoderCarrierFrequency(freq);
}

void AudioEngine::setFrequencyRange(float minHz, float maxHz) {
    if (mOscillatorNode) {
        mOscillatorNode->setFrequencyRange(minHz, maxHz);
    }
}

void AudioEngine::setFrequencyAndAmplitude(float frequency, float amplitude) {
    // Lock-free: solo usa atómicos en los osciladores
    // Este método establece frecuencias directas, sin conversión XY
    // Esto permite que la cuantización de escalas musicales desde Kotlin funcione correctamente

    mOscBank.setAllPrimaryParams(frequency, amplitude);

    // Sync vocoder carrier frequency with oscillator
    mEffectChain.setVocoderCarrierFrequency(frequency);
}

// ========== VOICE FILTER (Phase 6) ==========

void AudioEngine::setVoiceFilterEnabled(bool enabled) {
    if (mVoiceManager) {
        auto* pool = mVoiceManager->getVoicePool();
        if (pool) {
            for (int i = 0; i < pool->getTotalVoiceCount(); ++i) {
                auto* voice = pool->getVoice(i);
                if (voice) voice->setFilterEnabled(enabled);
            }
        }
    }
}

void AudioEngine::setVoiceFilterCutoff(float hz) {
    if (mVoiceManager) {
        auto* pool = mVoiceManager->getVoicePool();
        if (pool) {
            for (int i = 0; i < pool->getTotalVoiceCount(); ++i) {
                auto* voice = pool->getVoice(i);
                if (voice) voice->setFilterCutoff(hz);
            }
        }
    }
}

void AudioEngine::setVoiceFilterResonance(float q) {
    if (mVoiceManager) {
        auto* pool = mVoiceManager->getVoicePool();
        if (pool) {
            for (int i = 0; i < pool->getTotalVoiceCount(); ++i) {
                auto* voice = pool->getVoice(i);
                if (voice) voice->setFilterResonance(q);
            }
        }
    }
}

void AudioEngine::setVoiceFilterMode(int mode) {
    if (mVoiceManager) {
        auto* pool = mVoiceManager->getVoicePool();
        if (pool) {
            for (int i = 0; i < pool->getTotalVoiceCount(); ++i) {
                auto* voice = pool->getVoice(i);
                if (voice) voice->setFilterMode(mode);
            }
        }
    }
}

// ========== SYNTH ENGINE SYSTEM (Phase 6, Phase 1E — delegated to SynthEngineDispatcher) ==========

void AudioEngine::setEngineType(int engineType) {
    mEngineDispatcher.setEngineType(engineType);

    // Propagate to OscillatorNode (graph path)
    if (mOscillatorNode) {
        mOscillatorNode->setEngineType(engineType);
    }

    // Propagate to voice system (Phase 6)
    mEngineDispatcher.updateVoiceEngines(mVoiceManager.get());

    incrementStateVersion();
}

void AudioEngine::setEngineParameter(int paramId, float value) {
    mEngineDispatcher.setEngineParameter(paramId, value);

    // Also propagate to OscillatorNode engines
    if (mOscillatorNode) {
        mOscillatorNode->setEngineParameter(paramId, value);
    }
}

// ========== SOUNDFONT ENGINE (Phase 8, Phase 1E — delegated to SynthEngineDispatcher) ==========

bool AudioEngine::loadSoundFont(const void* data, int size) {
    const int sampleRate = currentSampleRate();
    return mEngineDispatcher.loadSoundFont(data, size, sampleRate);
}

bool AudioEngine::loadSoundFontFromPath(const char* path) {
    const int sampleRate = currentSampleRate();
    return mEngineDispatcher.loadSoundFontFromPath(path, sampleRate);
}

bool AudioEngine::loadSoundFontFromFd(int fd, int64_t offset, int64_t length) {
    const int sampleRate = currentSampleRate();
    return mEngineDispatcher.loadSoundFontFromFd(fd, offset, length, sampleRate);
}

void AudioEngine::unloadSoundFont() {
    mEngineDispatcher.unloadSoundFont();
}

void AudioEngine::setSoundFontPreset(int presetIndex) {
    mEngineDispatcher.setSoundFontPreset(presetIndex);
    incrementStateVersion();
}

int AudioEngine::getSoundFontPresetCount() const {
    return mEngineDispatcher.getSoundFontPresetCount();
}

const char* AudioEngine::getSoundFontPresetName(int presetIndex) const {
    return mEngineDispatcher.getSoundFontPresetName(presetIndex);
}

bool AudioEngine::getSoundFontPresetKeyRange(int presetIndex, int& outMinKey, int& outMaxKey) const {
    return mEngineDispatcher.getSoundFontPresetKeyRange(presetIndex, outMinKey, outMaxKey);
}

bool AudioEngine::getSoundFontPresetBankProgram(int presetIndex, int& outBank, int& outProgram) const {
    return mEngineDispatcher.getSoundFontPresetBankProgram(presetIndex, outBank, outProgram);
}

bool AudioEngine::isSoundFontLoaded() const {
    return mEngineDispatcher.isSoundFontLoaded();
}

void AudioEngine::sfNoteOn(int touchId, int midiNote, float velocity) {
    if (touchId == 0
        && mArpSequencer.isEnabled()
        && mEngineDispatcher.getEngineType() == 6 /* SOUNDFONT */) {
        float freq = 440.0f * std::pow(2.0f, (midiNote - 69) / 12.0f);
        mArpSequencer.setBaseFrequency(freq);
        mArpSequencer.setTouchActive(true);
        return;
    }
    mEngineDispatcher.sfNoteOn(touchId, midiNote, velocity);
}

void AudioEngine::sfNoteOff(int touchId) {
    if (touchId == 0
        && mArpSequencer.isEnabled()
        && mEngineDispatcher.getEngineType() == 6 /* SOUNDFONT */) {
        mArpSequencer.setTouchActive(false);
        return;
    }
    mEngineDispatcher.sfNoteOff(touchId);
}

void AudioEngine::sfNoteOffAll() {
    mEngineDispatcher.sfNoteOffAll();
}

void AudioEngine::sfNoteOffAllExcept(int keepTouchId) {
    mEngineDispatcher.sfNoteOffAllExcept(keepTouchId);
}

void AudioEngine::sfSetTouchExpression(int touchId, float expression) {
    mEngineDispatcher.sfSetTouchExpression(touchId, expression);
}

void AudioEngine::setOscillatorType(int typeId) {
    mOscBank.setOscillatorType(typeId);

    // Propagate oscillator type to Voice System (Phase 2)
    if (typeId >= 0 && typeId < mOscBank.getOscillatorCount()) {
        if (mVoiceManager) {
            voice::TouchTriggerSource* touchSource = mVoiceManager->getTouchSource();
            if (touchSource) {
                touchSource->setOscillatorType(typeId);
                LOGI("Propagated oscillator type %d to VoiceSystem", typeId);
            }
        }

        // FIX P1.2: Notify StateSynchronizer of state change
        incrementStateVersion();
    }
}

void AudioEngine::setModulatorType(int typeId) {
    mOscBank.setModulatorType(typeId);

    // FIX P1.2: Notify StateSynchronizer of state change
    incrementStateVersion();
}

void AudioEngine::setModulatorParameter(int paramId, float value) {
    mOscBank.setModulatorParameter(paramId, value);
    incrementStateVersion();
}

// setDualTouchMixMode is now an inline delegate in AudioEngine.h (Phase 1E)

void AudioEngine::setSecondaryOscillatorType(int typeId) {
    if (typeId < 0 || typeId >= mOscBank.getOscillatorCount()) {
        LOGE("Invalid secondary oscillator type: %d", typeId);
        return;
    }

    mDualTouch.setSecondaryOscillatorType(typeId);
    incrementStateVersion();
}

// mixDualTouchSignals moved to DualTouchManager::mixSignals (Phase 1E)

int AudioEngine::getWaveformSamples(float* buffer, int size) {
    return mWaveformCapture.read(buffer, size);
}

// ========== RENDER SUB-METHODS (Step 8 decomposition) ==========

void AudioEngine::applyEffectsAndLooper(float* output, int32_t numFrames) {
    // DC block
    mOutputStage.dcBlock(mOutputStage.getTempBuffer(), numFrames);

    // ---- PISTAS RUTEADAS A LA CADENA (REQ-007) ----
    // Las pistas marcadas se suman a la ENTRADA de la cadena, no a su salida. Es
    // el único punto del bloque donde se las mezcla: `AudioLooper::process()`, más
    // abajo, atiende el conjunto complementario, así que cada pista se mezcla
    // exactamente una vez por bloque.
    //
    // Sin ninguna pista marcada —el caso por defecto y el que corre hoy para todo
    // el mundo— `mixFxTracks` no toca el buffer y esto cuesta una comparación por
    // pista, sin una sola pasada extra sobre las muestras.
    mAudioLooper.mixFxTracks(mOutputStage.getTempBuffer(), numFrames);

    // Effects
    mEffectChain.process(mOutputStage.getTempBuffer(), output, numFrames);

    // ---- PRE-ROLL CAPTURE ----
    // Stash the post-FX signal into the pre-roll ring BEFORE the looper consumes it.
    // The looper uses snapshots of this ring to seed new recordings with audio
    // captured before the user pressed REC, eliminating reaction-time gaps.
    mPreRollRing.write(output, numFrames);

    // ---- TRANSPORT TICK ----
    // Snapshot Transport's play position BEFORE the tick so we pass the
    // block-start frame to the looper (not the post-tick frame).
    const int64_t playFrameAtBlockStart = mTransport.getPlayFrame();
    // Emits any scheduled metronome clicks for this audio block. RT-safe; runs
    // before the looper so the click is mixed by AudioLooper::process.
    mTransport.tick(numFrames, mAudioLooper);

    // ---- FADE (applied to synth + FX only — NOT to loops) ----
    // The pause/scene-change fade mutes the instrument signal but must let
    // existing loops keep playing through the transition. We apply the fade
    // BEFORE the looper mixes its playback into `output`.
    //
    // REQ-007 — la excepción, y es deliberada: una pista MARCADA ya se sumó
    // arriba, a la entrada de la cadena, así que para ella el fade SÍ aplica.
    // Es la contrapartida de entrar al bus del instrumento (AC-007.5): deja de
    // valer el invariante de este comentario, que sigue rigiendo para todas las
    // demás. The looper's
    // recording tap reads `output` here, so an in-progress recording would
    // capture the fade-out + transition + fade-in; callers that care about
    // clean takes must abort recording before triggering the fade (handled
    // on the NoisyPad side via the scene-change orchestrator).
    float fadeStart, fadeEnd;
    mFadeCtrl.processFadeBlock(numFrames, fadeStart, fadeEnd);
    if (mFadeCtrl.isPaused()) { fadeStart = 0.0f; fadeEnd = 0.0f; }

    // ---- SYNTH VOLUME ----
    // El nivel del instrumento comparte posición y semántica con el fade
    // (synth + FX, sin loops), así que viaja DENTRO del mismo ramp en vez de
    // pedir una segunda pasada sobre el buffer: cuesta dos multiplicaciones por
    // bloque en lugar de 2*numFrames.
    //
    // El ramp va del valor del bloque anterior al actual, que es lo que evita el
    // zipper noise cuando el usuario arrastra el control. Sin eso, un salto de
    // 1.0 a 0.2 entre bloques es un escalón en la forma de onda.
    const float synthVol = mSynthVolume.load(std::memory_order_acquire);
    fadeStart *= mSynthVolumePrev;
    fadeEnd *= synthVol;
    mSynthVolumePrev = synthVol;

    simd::applyStereoGainRamp(output, numFrames, fadeStart, fadeEnd);

    // ---- LOOPER TAP + PLAYBACK MIX ----
    // Captures the faded synth/FX signal for recording; then mixes the loop
    // playback into `output`. Loop playback is intentionally NOT scaled by
    // the engine fade so loops remain audible during scene changes.
    mAudioLooper.process(output, numFrames, playFrameAtBlockStart);

    // The master volume and the output protection chain used to live here, and
    // that is exactly what made the master miss the monitored input: this
    // function runs per render path, while the input is summed once, afterwards.
    // Both moved to applyMasterAndProtectOutput(), which processAudioBlock()
    // calls at the tail of the block. See ticket 2 in §16 of
    // docs/kmp/kmp_requirements.md.
    //
    // Note what did NOT move: the looper's recording tap above still runs
    // upstream of the master, so the master is not baked into takes — it never
    // was, whatever the ticket said.
}

void AudioEngine::captureMonitoringBlock(InputNode* inputNode, int32_t numFrames,
                                         bool hasInputMonitoring) {
    mMonitoringFramesRead = 0;

    const int32_t totalSamples = numFrames * 2;
    if (!hasInputMonitoring || inputNode == nullptr) return;
    if (mMonitoringBuffer.size() < static_cast<size_t>(totalSamples)) return;

    mMonitoringFramesRead = inputNode->getMonitoringSamples(mMonitoringBuffer.data(), numFrames);

    // WD-1.1 — esto era un LOGI/LOGW periodico desde el thread de audio.
    //
    // El log del mismatch de sample rate NO era ruido: el smoke de device lo
    // leia de logcat para distinguir "el mic glitchea" de "el mic no esta
    // conectado". Por eso se reemplaza en vez de borrarse — y el flag es mejor
    // que el log, porque no depende de un build de debug ni de que el texto no
    // cambie. Son dos stores relajados, sin formateo y sin syscall.
    const int outputSampleRate = currentSampleRate();
    const int inputSampleRate = inputNode->getCaptureSampleRate();
    mLastInputSampleRate.store(inputSampleRate, std::memory_order_relaxed);
    mSampleRateMismatch.store(
        inputSampleRate > 0 && outputSampleRate > 0 && inputSampleRate != outputSampleRate,
        std::memory_order_relaxed);
}

void AudioEngine::feedVocoderModulator() {
    if (mMonitoringFramesRead <= 0) return;

    const size_t needed = static_cast<size_t>(mMonitoringFramesRead);
    if (mVocoderMonoBuffer.size() < needed) return;

    // Downmix into a buffer of its own. Writing the mono signal back over
    // mMonitoringBuffer — which is what this used to do — corrupted the stereo
    // frames that the MIX sum reads out of the same buffer.
    for (size_t i = 0; i < needed; ++i) {
        mVocoderMonoBuffer[i] =
            (mMonitoringBuffer[i * 2] + mMonitoringBuffer[i * 2 + 1]) * 0.5f;
    }
    mEffectChain.setVocoderModulatorBuffer(mVocoderMonoBuffer.data(), mMonitoringFramesRead);
}

watermelon_audio::IAudioCallback::Result AudioEngine::handleNotRunning(
    float* output, int32_t numFrames) {
    const int32_t totalSamples = numFrames * 2;
    // Silence the synth output but allow monitoring
    std::fill(output, output + totalSamples, 0.0f);

    // WD-1.1 — dos LOGI_CALLBACK periodicos borrados. El contador dice lo mismo
    // que decian ("el callback esta corriendo con el motor parado") sin costo.
    mNotRunningBlocks.bump();

    if (mMonitoringFramesRead > 0) {
        const int32_t samplesToMix = std::min(mMonitoringFramesRead * 2, totalSamples);
        for (int32_t i = 0; i < samplesToMix; ++i) {
            output[i] = mMonitoringBuffer[i];
        }
        for (int32_t i = samplesToMix; i < totalSamples; ++i) {
            output[i] = 0.0f;
        }

        // A stopped engine is still an output path, and it used to be the only
        // one that handed the microphone to the device raw: no master, and no
        // protection at all, with the default +12 dB of input gain in front of
        // it. The master applies here for the same reason it applies below —
        // it is the level of everything that leaves.
        //
        // Lightweight and not the full chain on purpose: the lookahead limiter
        // is only sized by OutputStage::prepare(), which runs on start(), and
        // this path is reachable before the engine has ever been started.
        const float masterVol = mMasterVolume.load(std::memory_order_acquire);
        if (masterVol != 1.0f) {
            simd::applyStereoGain(output, numFrames, masterVol);
        }
        mOutputStage.processOutputLightweight(output, numFrames);
    }
    return watermelon_audio::IAudioCallback::Result::CONTINUE;
}

void AudioEngine::renderInputFx(float* output, int32_t numFrames, InputNode* inputNode) {
    const int32_t totalSamples = numFrames * 2;

    // WD-1.1 — aca vivian DOS `wma::logMessage` directos mas, que la auditoria
    // del 2026-08-13 no habia encontrado: los hallo el lint. Mismo patron que
    // los bloques de onAudioReady —llamada directa, saltea los macros, sobrevive
    // a NDEBUG— y en un path que corre por bloque en modo INPUT_FX de Oboe.

    // 1. Read input samples into temp buffer
    if (mOutputStage.getTempBufferSize() >= static_cast<size_t>(totalSamples)) {
        int framesRead = inputNode->getMonitoringSamples(mOutputStage.getTempBuffer(), numFrames);

        if (framesRead > 0) {
            // Zero-pad if fewer frames than requested
            if (framesRead < numFrames) {
                int32_t samplesRead = framesRead * 2;
                std::fill_n(mOutputStage.getTempBuffer() + samplesRead,
                            totalSamples - samplesRead, 0.0f);
            }

            // DC block + effects + fade + output
            applyEffectsAndLooper(output, numFrames);
        } else {
            std::fill_n(output, totalSamples, 0.0f);
        }
    } else {
        std::fill_n(output, totalSamples, 0.0f);
    }
}

void AudioEngine::renderSoundFont(float* output, int32_t numFrames) {
    auto* sfEngine = mEngineDispatcher.getSoundFontEngine();
    if (!sfEngine) {
        std::fill_n(output, numFrames * 2, 0.0f);
        return;
    }

    if (mArpSequencer.isEnabled()) {
        float bpm = mBpm.load(std::memory_order_relaxed);
        auto arpOut = mArpSequencer.process(numFrames, bpm);

        const bool gateOn = arpOut.frequency > 0.0f && arpOut.amplitude > 0.001f;
        if (gateOn) {
            const int midiNote = SoundFontEngine::frequencyToMidi(arpOut.frequency);
            if (arpOut.trigger && mArpSfPrevMidiNote >= 0 && mArpSfPrevMidiNote != midiNote) {
                sfEngine->noteOff(0);
            }
            sfEngine->noteOn(0, midiNote, arpOut.amplitude);
            mArpSfPrevMidiNote = midiNote;
        } else {
            if (mArpSfPrevMidiNote >= 0) {
                sfEngine->noteOff(0);
                mArpSfPrevMidiNote = -1;
            }
        }
    }

    sfEngine->render(mOutputStage.getTempBuffer(), numFrames);
    applyEffectsAndLooper(output, numFrames);
}

void AudioEngine::renderVoiceSystem(float* output, int32_t numFrames) {
    // 1. Process voice system (generates polyphonic output)
    mVoiceManager->process(mOutputStage.getTempBuffer(), numFrames);

    // 2. Apply modulator if exists
    mOscBank.applyModulation(mOutputStage.getTempBuffer(), numFrames);

    // 3. DC block + effects + fade + output
    applyEffectsAndLooper(output, numFrames);
}

void AudioEngine::renderSingleTouch(float* output, int32_t numFrames,
                                     int cachedEngineType, size_t cachedOscIndex,
                                     bool cachedHasActiveModulator, size_t cachedModIndex) {
    const int32_t totalSamples = numFrames * 2;
    bool oscillatorEnabled = mOscillatorEnabled.load(std::memory_order_acquire);

    if (oscillatorEnabled) {
        // ========== ARPEGGIATOR (Phase 7) ==========
        float renderFreq, renderAmp;
        bool arpActive = mArpSequencer.isEnabled();
        ArpSequencer::ArpOutput arpOut{};

        if (arpActive) {
            float bpm = mBpm.load(std::memory_order_relaxed);
            arpOut = mArpSequencer.process(numFrames, bpm);
            renderFreq = arpOut.frequency;
            renderAmp = arpOut.amplitude;
        } else {
            auto* osc0 = mOscBank.getPrimaryOscillator(0);
            renderFreq = osc0 ? osc0->getFrequency() : 440.0f;
            renderAmp = osc0 ? osc0->getAmplitude() : 0.0f;
        }

        // 1. Render oscillator / engine
        SynthEngine* activeEngine = (cachedEngineType > 0) ? mEngineDispatcher.getEngine(cachedEngineType) : nullptr;

        if (activeEngine) {
            activeEngine->process(mOutputStage.getTempBuffer(), numFrames, renderFreq,
                                  arpActive ? renderAmp : renderAmp);
        } else if (mOscBank.getPrimaryOscillator(static_cast<int>(cachedOscIndex))) {
            if (arpActive) {
                mOscBank.getPrimaryOscillator(static_cast<int>(cachedOscIndex))->setParameters(renderFreq, renderAmp);
            }
            mOscBank.getPrimaryOscillator(static_cast<int>(cachedOscIndex))->render(mOutputStage.getTempBuffer(), numFrames);
        } else {
            std::fill_n(mOutputStage.getTempBuffer(), totalSamples, 0.0f);
        }

        // 1c. Apply arp gate envelope per-sample
        if (arpActive) {
            float envStart = arpOut.gateEnvStart;
            float envEnd = arpOut.gateEnvEnd;
            if (envStart != 1.0f || envEnd != 1.0f) {
                float envStep = (numFrames > 1) ? (envEnd - envStart) / static_cast<float>(numFrames - 1) : 0.0f;
                float env = envStart;
                for (int32_t i = 0; i < numFrames; ++i) {
                    mOutputStage.getTempBuffer()[i * 2] *= env;
                    mOutputStage.getTempBuffer()[i * 2 + 1] *= env;
                    env += envStep;
                }
            }
        }

        // 1b. Apply engine crossfade if switching
        mEngineDispatcher.applyCrossfade(mOutputStage.getTempBuffer(), numFrames);

        // 1d. Mix chord harmony voices (Phase 9C)
        mChordHarmony.renderInto(mOutputStage.getTempBuffer(), numFrames);

        // 2. Apply modulator
        mOscBank.applyModulation(mOutputStage.getTempBuffer(), numFrames);

        // 3.5. Pass mic buffer to vocoder as modulator
        feedVocoderModulator();

        // 4-6. DC block + effects + fade + output
        applyEffectsAndLooper(output, numFrames);

    } else {
        std::fill_n(output, totalSamples, 0.0f);
    }
}

void AudioEngine::renderDualTouch(float* output, int32_t numFrames,
                                   const TouchState& dualTouchState,
                                   int cachedEngineType, size_t cachedOscIndex,
                                   bool cachedHasActiveModulator, size_t cachedModIndex) {
    const int32_t totalSamples = numFrames * 2;
    bool oscillatorEnabled = mOscillatorEnabled.load(std::memory_order_acquire);

    // Get engine pointers for non-classic dispatch
    SynthEngine* engine1 = (cachedEngineType > 0) ? mEngineDispatcher.getEngine(cachedEngineType) : nullptr;
    SynthEngine* engine2 = (cachedEngineType > 0) ? mEngineDispatcher.getDualTouchEngine(cachedEngineType) : nullptr;

    bool hasValidSource = oscillatorEnabled && (
        engine1 != nullptr ||
        (mOscBank.getPrimaryOscillator(static_cast<int>(cachedOscIndex)) != nullptr &&
         mOscBank.getSecondaryOscillator(static_cast<int>(cachedOscIndex)) != nullptr)
    );

    if (hasValidSource) {
        const float amp1 = dualTouchState.amp1;
        const float amp2 = dualTouchState.amp2;
        const float freq1 = dualTouchState.freq1;
        const float freq2 = dualTouchState.freq2;

        const bool touch1Active = amp1 > 0.001f;
        const bool touch2Active = amp2 > 0.001f;

        if (touch1Active && touch2Active) {
            if (engine1 && engine2) {
                engine1->process(mDualTouch.getTouch1Buffer(), numFrames, freq1, amp1);
                engine2->process(mDualTouch.getTouch2Buffer(), numFrames, freq2, amp2);
            } else {
                mOscBank.getPrimaryOscillator(static_cast<int>(cachedOscIndex))->setParameters(freq1, amp1);
                mOscBank.getPrimaryOscillator(static_cast<int>(cachedOscIndex))->render(mDualTouch.getTouch1Buffer(), numFrames);
                mOscBank.getSecondaryOscillator(static_cast<int>(cachedOscIndex))->setParameters(freq2, amp2);
                mOscBank.getSecondaryOscillator(static_cast<int>(cachedOscIndex))->render(mDualTouch.getTouch2Buffer(), numFrames);
            }
            mDualTouch.mixSignals(
                mDualTouch.getTouch1Buffer(),
                mDualTouch.getTouch2Buffer(),
                mOutputStage.getTempBuffer(),
                numFrames,
                dualTouchState
            );
        } else if (touch1Active) {
            if (engine1) {
                engine1->process(mOutputStage.getTempBuffer(), numFrames, freq1, amp1);
            } else {
                mOscBank.getPrimaryOscillator(static_cast<int>(cachedOscIndex))->setParameters(freq1, amp1);
                mOscBank.getPrimaryOscillator(static_cast<int>(cachedOscIndex))->render(mOutputStage.getTempBuffer(), numFrames);
            }
        } else if (touch2Active) {
            if (engine2) {
                engine2->process(mOutputStage.getTempBuffer(), numFrames, freq2, amp2);
            } else {
                mOscBank.getSecondaryOscillator(static_cast<int>(cachedOscIndex))->setParameters(freq2, amp2);
                mOscBank.getSecondaryOscillator(static_cast<int>(cachedOscIndex))->render(mOutputStage.getTempBuffer(), numFrames);
            }
        } else {
            simd::clearBuffer(mOutputStage.getTempBuffer(), numFrames * 2);
        }

        // Apply engine crossfade if switching
        mEngineDispatcher.applyCrossfade(mOutputStage.getTempBuffer(), numFrames);

        // Apply modulator
        mOscBank.applyModulation(mOutputStage.getTempBuffer(), numFrames);

        // Pass mic buffer to vocoder as modulator
        feedVocoderModulator();

        // DC block + effects + fade + output
        applyEffectsAndLooper(output, numFrames);

    } else {
        std::fill_n(output, totalSamples, 0.0f);
    }
}

void AudioEngine::handleMixMonitoring(float* output, int32_t numFrames,
                                       bool oscillatorEnabled, bool hasInputMonitoring) {
    // WD-1.1 — LOGI periodico borrado.

    // Only mix input in MIX mode (oscillator enabled + input monitoring enabled)
    if (!oscillatorEnabled || !hasInputMonitoring) return;

    if (mMixerNode) {
        // 1. Copy the instrument bus to mOscillatorBuffer (non-interleaved).
        //    NOT the oscillator, despite the name: by this point `output` is the
        //    finished bus — synth + FX + loops.
        mOscillatorBuffer.copyFromInterleaved(output, numFrames);

        // 2. This block's monitored input, captured once by
        //    captureMonitoringBlock(), converted to non-interleaved.
        mInputBuffer.clear();
        if (mMonitoringFramesRead > 0) {
            mInputBuffer.copyFromInterleaved(mMonitoringBuffer.data(), mMonitoringFramesRead);
        }

        // 3. Process through MixerNode
        AudioBuffer dummyInput;
        mMixerNode->process(dummyInput, numFrames);

        // 4. Copy MixerNode output back to interleaved outputData
        mMixerNode->getOutputBuffer().copyToInterleaved(output, numFrames);

        // No output protection here, and that is the fix, not an omission. This
        // used to run processOutputNoClip() on top of the processOutput() that
        // applyEffectsAndLooper had already run on the same block: the same
        // stateful lookahead limiter advanced twice per block, the soft clipper
        // ran twice, and the meters were updated twice — the second time over a
        // reading the first had already taken of the PRE-mix bus. The protection
        // now runs once, at the tail, in applyMasterAndProtectOutput().

        // WD-1.1 — LOGI periodico borrado.
    } else if (mMonitoringFramesRead > 0) {
        // Fallback to simple mixing if MixerNode could not be allocated. The
        // hard limit that used to be here went the same way as the chain above:
        // the tail protects this sum too.
        simd::addStereoBuffers(output, output, mMonitoringBuffer.data(),
                               mMonitoringFramesRead, true);
    }
}

void AudioEngine::applyMasterAndProtectOutput(float* output, int32_t numFrames) {
    // ---- MASTER VOLUME ----
    // The level of everything that leaves: instrument, effects, loops AND the
    // monitored input, which is why this runs here and not inside
    // applyEffectsAndLooper. "The instrument" is a separate control with its own
    // position in the chain — see setSynthVolume().
    //
    // Upstream of this, and deliberately: the looper's recording tap. The master
    // is a monitoring level, not part of the take.
    const float masterVol = mMasterVolume.load(std::memory_order_acquire);
    if (masterVol != 1.0f) {
        simd::applyStereoGain(output, numFrames, masterVol);
    }

    // ---- OUTPUT PROTECTION ----
    // Exactly once per block, on the signal that actually leaves. OutputStage's
    // own contract says every path converges here last; MIX used to break that
    // by running the chain a second time over a buffer this one had already
    // metered.
    mOutputStage.processOutput(output, numFrames);
}

// ========== DECOMPOSED processAudioBlock ==========

watermelon_audio::IAudioCallback::Result AudioEngine::processAudioBlock(
    float* audioData, int32_t numFrames) {
    // WD-1.3 — el CallbackGuard se movio a onAudioReady().
    //
    // Aca cubria solo esta funcion, y el fast-path de USB de onAudioReady
    // retorna ANTES de llegar: esos bloques no contaban como callback en vuelo,
    // asi que ni el drenaje de stop() ni el del retiro del InputNode los veian.
    // Subirlo al punto de entrada cubre las dos ramas y cualquier otra futura.

    try {
        // WD-1.3 — un load atomico. Sin lock, sin refcount, sin posibilidad de
        // que el destructor del nodo corra en este thread.
        InputNode* inputNode = mInputNodeRt.load(std::memory_order_acquire);

        // WD-1.1 — el LOGI_CALLBACK periodico de estado/xruns lo reemplaza el
        // contador de bloques: el estado y el conteo de xruns ya son
        // consultables desde el thread de control.
        EngineState state = mState.load(std::memory_order_acquire);
        mCallbackBlocks.bump();

        float* outputData = audioData;
        const int32_t totalSamples = numFrames * 2;

        // This block's monitored input, read ONCE. Both consumers — the vocoder
        // modulator and the MIX sum — take their samples from the snapshot; the
        // stopped path below does too.
        const bool monitoringEnabled = inputNode && inputNode->isMonitoringEnabled();
        captureMonitoringBlock(inputNode, numFrames, monitoringEnabled);

        // Not-Running: silence + monitoring
        if (state != EngineState::Running) {
            return handleNotRunning(outputData, numFrames);
        }

        // Buffer validation
        if (totalSamples > static_cast<int32_t>(mOutputStage.getTempBufferSize())) {
            // WD-1.1 — era un LOGE, y es justo el caso peor para loguear: si
            // esto dispara, dispara en TODOS los bloques hasta que alguien
            // reconfigure. El contador lo hace visible sin realimentar el
            // problema con un syscall por callback.
            mBufferOverflowBlocks.bump();
            std::fill(outputData, outputData + totalSamples, 0.0f);
            return watermelon_audio::IAudioCallback::Result::CONTINUE;
        }

        // Mode detection: cache state once per callback
        const auto dualTouchState = mDualTouch.snapshot();
        bool oscillatorEnabled = mOscillatorEnabled.load(std::memory_order_acquire);
        const bool hasInputMonitoring = monitoringEnabled;
        const size_t cachedOscIndex = static_cast<size_t>(mOscBank.getOscillatorType());
        const size_t cachedModIndex = static_cast<size_t>(mOscBank.getModulatorType());
        const bool cachedHasActiveModulator = mOscBank.hasActiveModulator();
        const int cachedEngineType = mEngineDispatcher.detectCrossfadeAndGetType();

        // Render per mode
        if (!oscillatorEnabled && hasInputMonitoring) {
            renderInputFx(outputData, numFrames, inputNode);

        } else if (cachedEngineType == static_cast<int>(EngineTypeId::SOUNDFONT)
                   && mEngineDispatcher.getSoundFontEngine() && oscillatorEnabled) {
            renderSoundFont(outputData, numFrames);

        } else if (mUseVoiceSystem.load(std::memory_order_acquire) && mVoiceManager
                   && oscillatorEnabled && !mArpSequencer.isEnabled()) {
            renderVoiceSystem(outputData, numFrames);

        } else if (!dualTouchState.active) {
            renderSingleTouch(outputData, numFrames, cachedEngineType, cachedOscIndex,
                              cachedHasActiveModulator, cachedModIndex);

        } else {
            renderDualTouch(outputData, numFrames, dualTouchState, cachedEngineType,
                            cachedOscIndex, cachedHasActiveModulator, cachedModIndex);
        }

        // MIX mode monitoring (post-render)
        handleMixMonitoring(outputData, numFrames, oscillatorEnabled, hasInputMonitoring);

        // Master volume + output protection, once, over the finished mix.
        applyMasterAndProtectOutput(outputData, numFrames);

        // Waveform capture: the final output, exactly what the device gets.
        // The looper's tap is elsewhere on purpose — it sits inside
        // applyEffectsAndLooper, upstream of the master, so a take is not
        // scaled by the monitoring level.
        mWaveformCapture.write(outputData, numFrames);

        mCallbackErrorCount.store(0, std::memory_order_relaxed);
        return watermelon_audio::IAudioCallback::Result::CONTINUE;

    // WD-1.1 — los cuatro LOGE de estos handlers borrados.
    //
    // Loguear desde el handler de una excepcion en el thread de audio es el
    // peor momento posible: si el motor esta tirando excepciones por bloque, el
    // log las convierte en una tormenta de syscalls encima de un motor que ya
    // esta fallando. El contador conserva el hecho, y `mCallbackErrorCount`
    // —que ya existia— conserva la cuenta que decide el STOP.
    } catch (const std::exception&) {
        mCallbackExceptions.bump();
        std::fill(audioData, audioData + numFrames * 2, 0.0f);
        int errorCount = mCallbackErrorCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (errorCount > 100) {
            return watermelon_audio::IAudioCallback::Result::STOP;
        }
        return watermelon_audio::IAudioCallback::Result::CONTINUE;

    } catch (...) {
        mCallbackExceptions.bump();
        std::fill(audioData, audioData + numFrames * 2, 0.0f);
        int errorCount = mCallbackErrorCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (errorCount > 100) {
            return watermelon_audio::IAudioCallback::Result::STOP;
        }
        return watermelon_audio::IAudioCallback::Result::CONTINUE;
    }
}

// ========== FUNCIONES DE FADE Y VOLUMEN ==========

void AudioEngine::setMasterVolume(float volume) {
    float clampedVolume = std::max(0.0f, std::min(1.0f, volume));
    mMasterVolume.store(clampedVolume, std::memory_order_release);
    LOGI("Master volume set to: %.2f", clampedVolume);
}

void AudioEngine::setSynthVolume(float volume) {
    // Sin log: a diferencia del master, se espera que esto lo arrastre un slider
    // y loguear por cada valor inundaría logcat. El estado se lee con
    // getSynthVolume().
    float clampedVolume = std::max(0.0f, std::min(1.0f, volume));
    mSynthVolume.store(clampedVolume, std::memory_order_release);
    incrementStateVersion();
}

float AudioEngine::calculateCurrentVolume() {
    float masterVol = mMasterVolume.load(std::memory_order_acquire);
    float fadeVol = mFadeCtrl.getCurrentFadeVolume();
    return masterVol * fadeVol;
}

// processFadeBlock() moved to FadeController (Phase 1E)

bool AudioEngine::startWithFade(int fadeTimeMs) {
    // Delegate to start() with the fade time — fade is configured BEFORE stream starts,
    // eliminating the race condition where callbacks process stale fade state
    mFadeCtrl.cancel();
    LOGI("Starting with fade in: %d ms", fadeTimeMs);
    AUDIO_DIAG("ENGINE START_WITH_FADE: fadeMs=%d", fadeTimeMs);
    return start(fadeTimeMs);
}

void AudioEngine::stopWithFade(int fadeTimeMs) {
    mFadeCtrl.cancel();

    // Fade out, then stop. Previously this was gated on `if (mStream)`, so the
    // BackendManager path (mStream always null) fell through to a bare stop()
    // and cut the audio dead — an audible click. currentSampleRate() resolves
    // on both paths, so the fade now happens regardless of backend.
    if (fadeTimeMs > 0) {
        mFadeCtrl.startFade(1.0f, 0.0f, currentSampleRate(), fadeTimeMs);

        LOGI("Stopping with fade out: %d ms", fadeTimeMs);
        AUDIO_DIAG("ENGINE STOP_WITH_FADE: fadeMs=%d", fadeTimeMs);

        // Delayed stop after the fade completes. Owned (see mStopFadeThread) so
        // the destructor can reclaim it; a detached thread could outlive the
        // engine and call stop() on freed memory. A prior worker, if any, is
        // superseded: cancel and join it before starting the next.
        if (mStopFadeThread && mStopFadeThread->joinable()) {
            mStopFadeCancel.store(true, std::memory_order_release);
            mStopFadeThread->join();
        }
        mStopFadeCancel.store(false, std::memory_order_release);
        mStopFadeThread = std::make_unique<std::thread>([this, fadeTimeMs]() {
            // Chunked sleep so teardown can reclaim the thread promptly instead
            // of blocking for the whole fade.
            const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(fadeTimeMs + 50);
            while (!mStopFadeCancel.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    stop();  // idempotent
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            // Cancelled: the engine is tearing down and owns the stop() itself.
        });
    } else {
        stop();
    }
}

void AudioEngine::pauseWithFade(int fadeTimeMs) {
    mFadeCtrl.cancel();

    bool useBackend = mUseBackendManager.load(std::memory_order_acquire);
    wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
        "PAUSE_WITH_FADE: fadeTimeMs=%d, mStream=%p, useBackendMgr=%d",
        fadeTimeMs, mStream.get(), useBackend);

    // The old shape was `if (mStream) fade; else if (useBackend) pause abruptly`,
    // because mStream is always null on the BackendManager path. With
    // currentSampleRate() the fade resolves on both paths, so USB/CoreAudio get
    // the same fade Oboe always got instead of an abrupt pause.
    if (fadeTimeMs > 0) {
        mFadeCtrl.fadeOutAndPause(currentSampleRate(), fadeTimeMs);

        LOGI("Pausing with fade out: %d ms", fadeTimeMs);
        AUDIO_DIAG("ENGINE PAUSE_WITH_FADE: fadeMs=%d", fadeTimeMs);
    } else {
        mFadeCtrl.setPaused(true);
    }
}

void AudioEngine::resumeWithFade(int fadeTimeMs) {
    // FIX: Cancel any pending pause fade thread to prevent it from
    // setting paused=true after we clear it
    mFadeCtrl.cancel();

    bool useBackend = mUseBackendManager.load(std::memory_order_acquire);
    wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
        "RESUME_WITH_FADE: fadeTimeMs=%d, mStream=%p, useBackendMgr=%d",
        fadeTimeMs, mStream.get(), useBackend);

    // Same story as pauseWithFade: the BackendManager path used to skip the
    // fade and snap straight back to full volume.
    if (fadeTimeMs > 0) {
        mFadeCtrl.resumeWithFade(currentSampleRate(), fadeTimeMs);

        LOGI("Resuming with fade in: %d ms", fadeTimeMs);
        AUDIO_DIAG("ENGINE RESUME_WITH_FADE: fadeMs=%d", fadeTimeMs);
    } else {
        // Instant restore to full volume — avoids stuck-at-zero silence.
        mFadeCtrl.setPaused(false);
        mFadeCtrl.startFade(1.0f, 1.0f, currentSampleRate(), 0);
    }
}

// cancelPendingFade() moved to FadeController::cancel() (Phase 1E)

// ========== MANEJO DE ERRORES DE STREAM (FASE 2.1.4) ==========

// NOTE: Legacy onErrorBeforeClose/onErrorAfterClose removed in Phase 0B.
// Error handling is now unified in onBackendError() above.
// For the legacy Oboe path, OboeCallbackAdapter translates Oboe errors
// to BackendError and calls onBackendError().

// ========== COMPONENT CONFIGURATION HELPER ==========

// ========== STREAM INFO (moved from header in Phase 0B) ==========

int AudioEngine::currentSampleRate() const {
    int32_t sampleRate = 0;
    int32_t bufferSize = 0;
    double latencyMillis = 0.0;
    if (getStreamInfo(sampleRate, bufferSize, latencyMillis) && sampleRate > 0) {
        return sampleRate;
    }

    // El rate del render offline (REQ-015). Sin backend al que preguntarle es lo
    // unico que sabe a que rate corre el motor, y su UNICO escritor es
    // `startOffline()`. Fuera de un render offline vale 0 y se cae a 48000, que
    // es exactamente lo que pasaba en device antes de MINI-007.
    const int offline = mOfflineSampleRate.load(std::memory_order_acquire);
    if (offline > 0) {
        return offline;
    }
    return 48000;
}

bool AudioEngine::getStreamInfo(int32_t& sampleRate, int32_t& bufferSize, double& latencyMillis) const {
    // Try BackendManager first (works for both USB and Oboe-via-backend paths)
    if (mUseBackendManager.load(std::memory_order_acquire)) {
        auto& manager = watermelon_audio::BackendManager::getInstance();
        if (manager.isRunning()) {
            auto info = manager.getStreamInfo();
            sampleRate = info.sampleRate;
            bufferSize = info.framesPerBuffer;
            latencyMillis = info.outputLatencyMs;
            return true;
        }
    }

    // Legacy Oboe path. Off Android mStream is never populated, so this is the
    // only branch and it reports "no stream" — which is correct: without
    // BackendManager running there is nothing to describe.
    if (!mStream) {
        sampleRate = -1;
        bufferSize = -1;
        latencyMillis = -1.0;
        return false;
    }

#if WMA_HAS_OBOE
    sampleRate = mStream->getSampleRate();
    bufferSize = mStream->getFramesPerBurst();

    auto latency = mStream->calculateLatencyMillis();
    if (latency) {
        latencyMillis = latency.value();
    } else {
        latencyMillis = -1.0;
    }
    return true;
#else
    return false;
#endif
}

oboe::AudioStream* AudioEngine::getOutputStream() const {
    return mStream.get();
}


// ========== MOTOR SIN DEVICE (WD-2.1) ==========

bool AudioEngine::startOffline(int sampleRate, int maxBlockFrames) {
    std::lock_guard<std::mutex> lock(mStateMutex);

    // Mismo flush que hace start(): deja el diagnostico de que rama de ISA se
    // compilo. El que le sirve al render lo hace renderBlock() en su thread.
    wma::platform::flushDenormals();

    if (mInitializationFailed.load(std::memory_order_acquire)) {
        LOGE("startOffline: la inicializacion habia fallado");
        return false;
    }
    if (sampleRate <= 0) {
        LOGE("startOffline: sampleRate invalido (%d)", sampleRate);
        return false;
    }
    // El tope no es arbitrario: EffectChain aloca sus scratch a 8192 samples
    // (4096 frames estereo) en el constructor, y pasarse activa su guarda de
    // overflow — que rellena de SILENCIO y sigue. Un render que devuelve
    // silencio sin decir por que es la peor forma de fallar, asi que se rechaza
    // aca, donde se puede explicar.
    constexpr int kMaxSupportedBlockFrames = 4096;
    if (maxBlockFrames <= 0 || maxBlockFrames > kMaxSupportedBlockFrames) {
        LOGE("startOffline: maxBlockFrames %d fuera de rango (1..%d)",
             maxBlockFrames, kMaxSupportedBlockFrames);
        return false;
    }

    EngineState currentState = mState.load(std::memory_order_acquire);
    if (currentState != EngineState::Stopped) {
        LOGE("startOffline: el motor no esta detenido (estado %d)",
             static_cast<int>(currentState));
        return false;
    }
    if (!transitionToState(EngineState::Starting)) {
        return false;
    }

    // currentSampleRate() consulta al backend y, si no hay, cae a este campo.
    // Publicarlo aca es lo que hace que TODO el motor vea el rate correcto sin
    // tocar una sola linea mas: no hay backend al que preguntarle.
    //
    // MINI-007: este es el UNICO escritor del campo. Antes lo compartia con un
    // setter publico que ningun consumidor podia alcanzar, y por eso el campo se
    // llamaba "preferido" — un nombre que prometia una capacidad inexistente.
    mOfflineSampleRate.store(sampleRate, std::memory_order_release);
    mOfflineMaxBlockFrames.store(maxBlockFrames, std::memory_order_release);
    mOfflineMode.store(true, std::memory_order_release);

    configureComponentsWithSampleRate(sampleRate, maxBlockFrames);

    // Fade de largo cero: un render offline tiene que ser determinista desde el
    // primer sample. Una rampa de arranque haria que el bloque 0 no sea
    // comparable con el bloque 0 de la corrida siguiente.
    mFadeCtrl.startFade(0.0f, 1.0f, sampleRate, 0);
    mFadeCtrl.setPaused(false);

    if (!transitionToState(EngineState::Running)) {
        mOfflineMode.store(false, std::memory_order_release);
        transitionToState(EngineState::Stopped);
        return false;
    }

    LOGI("startOffline: %d Hz, bloques de hasta %d frames, sin device",
         sampleRate, maxBlockFrames);
    return true;
}

bool AudioEngine::renderBlock(float* output, const float* input, int frames) {
    if (output == nullptr || frames <= 0) {
        return false;
    }
    if (!mOfflineMode.load(std::memory_order_acquire)) {
        LOGE("renderBlock: el motor no arranco con startOffline()");
        return false;
    }
    if (frames > mOfflineMaxBlockFrames.load(std::memory_order_acquire)) {
        LOGE("renderBlock: %d frames excede el maximo declarado (%d)",
             frames, mOfflineMaxBlockFrames.load(std::memory_order_acquire));
        return false;
    }

    // El MISMO camino que recorre un callback real. No una ruta paralela: si
    // divergieran, un golden capturado aca dejaria de valer para el audio que
    // sale por el parlante, que es exactamente lo que este metodo existe para
    // garantizar.
    onAudioReady(output, input, frames);
    return true;
}

void AudioEngine::configureComponentsWithSampleRate(int sampleRate, int maxBlockSize) {
    // REQ-006.1 — QUIESCE. Nada de lo que sigue es seguro con el thread de audio
    // adentro.
    //
    // No es teorico y no es solo el dispatcher: esta funcion reasigna la
    // `DelayLine` de Karplus-Strong, hace `resize()` del buffer de Granular,
    // `setSize()` de cuatro buffers de nodo, y re-prepara looper, pre-roll,
    // VoiceManager y MixerNode. Medido el 2026-08-20 con TSan sobre el camino de
    // coercion de `start()`: `SynthEngineDispatcher::prepare` ->
    // `KarplusStrongEngine::prepare` contra `onAudioReady` ->
    // `renderSingleTouch` -> `KarplusStrongEngine::process`.
    //
    // Y el camino de coercion CORRE CON AUDIO: `start()` lo ejecuta despues de
    // `manager.start()` (`:568`). El comentario de `:480-495` ya habia declarado
    // el invariante —"configure components BEFORE starting the backend"— y ese
    // arreglo cubrio el pre-configure y dejo afuera justo esta rama.
    //
    // 250 ms: un bloque son ~2,7 ms. Es el mismo techo que usa el retiro del
    // InputNode, por la misma razon (ver `setInputNode`).
    ReconfigureQuiesce quiesce(*this, std::chrono::milliseconds(250));
    if (!quiesce.drained()) {
        // NO se prepara. Se conserva el rate viejo.
        //
        // Un motor afinado al rate anterior es un defecto audible y ACOTADO;
        // preparar sobre un callback vivo es un use-after-free. Es la misma
        // jerarquia que toma `setInputNode()`, que prefiere filtrar un nodo
        // antes que arriesgar un UAF.
        LOGE("configureComponentsWithSampleRate(%d): el thread de audio no cerro "
             "su bloque en 250ms — se conserva la configuracion anterior en vez "
             "de re-preparar por abajo de un callback vivo",
             sampleRate);
        return;
    }

    LOGI("Configuring audio components with sample rate: %d Hz", sampleRate);

    // Configure oscillators and modulators (Phase 1E — delegated to OscillatorBank)
    mOscBank.prepare(sampleRate);

    // Configure effect chain
    mEffectChain.setSampleRate(sampleRate);

    // Configure looper (click envelope is sample-rate aware; transport too).
    mAudioLooper.setSampleRate(sampleRate);
    {
        // Pre-size the looper mix buffer to the largest block the backend can
        // deliver so the audio thread never has to grow it on the fly (QW-4).
        // Same sizing policy as the other nodes above (framesPerBurst × 4,
        // min 4096).
#if WMA_HAS_OBOE
        int framesPerBurst = mStream ? mStream->getFramesPerBurst() : 256;
#else
        // No Oboe: the backend owns the burst size. 256 matches the fallback
        // the Oboe path uses before the stream is open.
        const int framesPerBurst = 256;
#endif
        int maxBlock = std::max(framesPerBurst * 4, 4096);
        mAudioLooper.prepareMixBuffer(maxBlock);
    }
    mTransport.setSampleRate(sampleRate);

    // Pre-roll ring: 1 second of post-FX output for seeding new recordings.
    // This is the maximum pre-roll duration; UI requests N ms <= 1000.
    mPreRollRing.prepare(sampleRate);

    // Configure arpeggiator
    mArpSequencer.prepare(sampleRate);
    LOGI("ArpSequencer prepared");

    // Configure synth engines (Phase 1E — via SynthEngineDispatcher)
    mEngineDispatcher.prepare(sampleRate, maxBlockSize);
    mEngineDispatcher.updateVoiceEngines(mVoiceManager.get());
    LOGI("SynthEngineDispatcher prepared");

    // Configure MixerNode
    if (mMixerNode) {
        mMixerNode->prepare(sampleRate, maxBlockSize);
        mOscillatorBuffer.setSize(2, maxBlockSize);
        mInputBuffer.setSize(2, maxBlockSize);
        mMixerOutputBuffer.setSize(2, maxBlockSize);
        mMixerNode->setInputBuffer(MixerNode::INPUT_OSCILLATOR, &mOscillatorBuffer);
        mMixerNode->setInputBuffer(MixerNode::INPUT_EXTERNAL, &mInputBuffer);
        LOGI("MixerNode prepared");
    }

    // Configure OscillatorNode
    if (mOscillatorNode) {
        mOscillatorNode->prepare(sampleRate, maxBlockSize);
        LOGI("OscillatorNode prepared");
    }

    // Configure EffectChainNode
    if (mEffectChainNode) {
        mEffectChainNode->prepare(sampleRate, maxBlockSize);
        mEffectOutputBuffer.setSize(2, maxBlockSize);
        LOGI("EffectChainNode prepared");
    }

    // Configure VoiceManager
    if (mVoiceManager) {
        mVoiceManager->prepare(sampleRate, maxBlockSize);
        LOGI("VoiceManager prepared");
    }

    // Configure ChordHarmony (Phase 9C)
    mChordHarmony.prepare(sampleRate, maxBlockSize);

    // Configure output stage (DC blocker, limiter, ditherer)
    mOutputStage.prepare(sampleRate, 0);
    LOGI("OutputStage configured at %dHz", sampleRate);
}

// ========== BACKEND MANAGER CONTROL (USB Audio Phase 1) ==========

void AudioEngine::setUseBackendManager(bool enabled) {
    EngineState currentState = mState.load(std::memory_order_acquire);
    if (currentState != EngineState::Stopped) {
        LOGE("Cannot change backend mode while engine is running");
        return;
    }

    mUseBackendManager.store(enabled, std::memory_order_release);
    LOGI("BackendManager mode: %s", enabled ? "enabled" : "disabled");

    if (enabled) {
        // Register this AudioEngine as the callback for BackendManager
        auto& manager = watermelon_audio::BackendManager::getInstance();
        manager.setCallback(this);

        // MINI-007 — aca habia un tercer `manager.setSampleRate(preferido)`, la
        // ultima de las tres ramas que solo el setter borrado podia encender.
    }
}

// ========== IAUDIOCALLBACK IMPLEMENTATION (Backend Abstraction) ==========
// FIX PHASE 7.2 & 7.3: Refactored callback for USB audio
// FIX PHASE 8: Direct USB INPUT_FX processing without double buffering

watermelon_audio::IAudioCallback::Result AudioEngine::onAudioReady(
    float* outputData,
    const float* inputData,
    int32_t numFrames) {

    // WD-1.2 — flush-to-zero, en ESTE thread.
    //
    // Es lo primero del callback y va sin guarda. FPCR/MXCSR son estado por
    // thread: hasta WD-1.2 los unicos call sites corrian en el thread del
    // llamador (AudioEngine::start, OboeBackend::start) y CoreAudioBackend no
    // llamaba a ninguno, asi que el thread RT corria con denormales habilitados
    // en las tres plataformas. Este es el unico punto por el que pasan TODOS
    // los backends —Oboe, Core Audio, libusb y Split entran todos por
    // IAudioCallback::onAudioReady— asi que ponerlo aca cubre tambien a
    // cualquier backend futuro sin que nadie se acuerde de agregarlo.
    //
    // Sin `thread_local`: ver la nota de Platform.h. La guarda costaba mas
    // riesgo (posible malloc de TLS en la primera llamada) que el read-modify-
    // write de un registro de control que ahorra.
    wma::platform::flushDenormalsRtSafe();

    // WD-1.3 — la barrera de callbacks, en el punto de entrada.
    //
    // Vivia adentro de processAudioBlock(), que el fast-path de USB de mas
    // abajo NO alcanza: esos bloques quedaban invisibles para el drenaje de
    // stop() y para el del retiro del InputNode. Aca cubre las dos ramas, y las
    // salidas tempranas las maneja el RAII.
    mActiveCallbacks.fetch_add(1, std::memory_order_acquire);
    struct CallbackGuard {
        std::atomic<int>& counter;
        // RT-SAFE-ALLOW: es una REFERENCIA a la condition variable, no una espera.
        // El destructor solo hace notify_all(), que no bloquea. La espera vive en
        // stop() y en waitForCallbackDrain(), del lado del thread de control.
        std::condition_variable& cv;
        // RT-SAFE-ALLOW: idem — parametro del constructor, no una espera.
        CallbackGuard(std::atomic<int>& c, std::condition_variable& condition)
            : counter(c), cv(condition) {}
        ~CallbackGuard() {
            int remaining = counter.fetch_sub(1, std::memory_order_release) - 1;
            if (remaining == 0) cv.notify_all();
        }
    } callbackGuard(mActiveCallbacks, mStopCondition);

    // REQ-006.1 — la compuerta del quiesce.
    //
    // Va ACA, en el punto de entrada, por la misma razon que la barrera de
    // WD-1.3 unas lineas arriba: el fast-path de USB de mas abajo NO pasa por
    // processAudioBlock(), y una compuerta puesta ahi dejaria descubierta justo
    // la rama que corre en el caso USB — que es donde el device coerce el rate.
    //
    // El bloque sale en SILENCIO y sin tocar engines ni buffers de nodo, que es
    // lo que el thread de control esta re-preparando en este instante. Dura unos
    // pocos bloques y solo en un cambio de sample rate.
    if (mEnginesReconfiguring.load(std::memory_order_acquire)) {
        if (outputData != nullptr) {
            std::memset(outputData, 0, static_cast<size_t>(numFrames) * 2 * sizeof(float));
        }
        return watermelon_audio::IAudioCallback::Result::CONTINUE;
    }

    // Service a pending EffectChain reset before doing ANY audio work
    // this block. The UI / JNI thread sets the flag via
    // requestResetEffectChain() when the audio context changes in a
    // way that would let stale state bleed through — primarily the
    // chaos_pad → input_fx transition, where a reverb tail cooked by
    // loud synth audio would otherwise leak into the first blocks of
    // mic processing. Compare-exchange acquires ownership atomically
    // so concurrent requests collapse into a single reset.
    bool expected = true;
    if (mResetEffectChainPending.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel)) {
        mEffectChain.reset();

        // The output stage carries audio across the transition too: its
        // lookahead limiter holds 5 ms. Resetting the chain but not this left
        // the first block of INPUT_FX with a tail of the pad — the same bleed
        // the chain reset exists to prevent, one stage further downstream.
        mOutputStage.reset();
    }

    // ========== DIRECT USB INPUT_FX MODE ==========
    // When USB backend provides input AND oscillator is disabled (INPUT_FX mode),
    // process input directly through effect chain without going through InputNode.
    // This eliminates ~100ms of latency from double buffering.
    bool oscillatorEnabled = mOscillatorEnabled.load(std::memory_order_acquire);
    bool isUsbInputFxMode = (inputData != nullptr) && !oscillatorEnabled && (numFrames > 0);

    // WD-1.1 — aca vivia un bloque `WMA_AUDIT` que cada 300 callbacks
    // formateaba once argumentos y hacia un syscall de logging, adentro de un
    // deadline de 2,7 ms. Salteaba los macros LOGI/LOGW —que si se compilan a
    // ((void)0) bajo NDEBUG— llamando a wma::logMessage directo, asi que
    // SOBREVIVIA A RELEASE. Lo que medía (peak de entrada, estado, fade) lo
    // cubren los contadores de WD-5.1 sin salir del thread.

    if (isUsbInputFxMode && outputData != nullptr) {
        mUsbDirectBlocks.bump();

        int totalSamples = numFrames * 2;  // Stereo

        // Ensure temp buffer is large enough
        if (mOutputStage.getTempBufferSize() < static_cast<size_t>(totalSamples)) {
            // Note: Resize should not happen in audio thread normally,
            // but this is a safety check
            return watermelon_audio::IAudioCallback::Result::CONTINUE;
        }

        // 1. Copy USB input to temp buffer for processing
        std::memcpy(mOutputStage.getTempBuffer(), inputData, totalSamples * sizeof(float));

        // 2. Apply DC blocking
        mOutputStage.dcBlock(mOutputStage.getTempBuffer(), numFrames);

        // 2b. Pistas ruteadas a la cadena (REQ-007). VA ACÁ, y no es decorativo:
        // este camino rápido tiene su propio `mAudioLooper.process()` más abajo,
        // y esa pasada SALTEA las pistas marcadas. Sin esta llamada, una pista
        // marcada no la mezclaría nadie y quedaría MUDA con USB INPUT_FX activo
        // — un modo entero donde el flag apagaría la pista en vez de rutearla.
        mAudioLooper.mixFxTracks(mOutputStage.getTempBuffer(), numFrames);

        // 3. Process through effect chain (INPUT → EFFECTS → OUTPUT)
        mEffectChain.process(mOutputStage.getTempBuffer(), outputData, numFrames);

        // 3b. Looper integration (post-FX, pre-master). Mirrors applyEffectsAndLooper.
        // Without this the USB direct INPUT_FX fast-path bypasses the looper entirely:
        // no recording, no playback of existing tracks, no transport advance.
        mPreRollRing.write(outputData, numFrames);
        const int64_t usbPlayFrameAtBlockStart = mTransport.getPlayFrame();
        mTransport.tick(numFrames, mAudioLooper);
        mAudioLooper.process(outputData, numFrames, usbPlayFrameAtBlockStart);

        // 4. Apply fade and master volume (block-based interpolation)
        float fadeStart, fadeEnd;
        mFadeCtrl.processFadeBlock(numFrames, fadeStart, fadeEnd);

        float masterVol = mMasterVolume.load(std::memory_order_acquire);
        if (mFadeCtrl.isPaused()) {
            masterVol = 0.0f;
        }

        // SIMD-optimized: Linear interpolation of fade across block
        float gainStart = fadeStart * masterVol;
        float gainEnd = fadeEnd * masterVol;
        simd::applyStereoGainRamp(outputData, numFrames, gainStart, gainEnd);

        // 5. Output stage protection (lightweight — no lookahead limiter for USB direct path
        //    to avoid stale delay buffer glitches and minimize latency)
        mOutputStage.processOutputLightweight(outputData, numFrames);

        // WD-1.1 — segundo bloque `WMA_AUDIT` borrado, mismo caso que el de
        // arriba: wma::logMessage directo, sobrevivia a NDEBUG.

        return watermelon_audio::IAudioCallback::Result::CONTINUE;
    }

    // ========== NON-INPUT_FX MODES (MIX, CHAOS_PAD, etc.) ==========
    // For modes that need InputNode (vocoder, MIX mode), feed USB input to InputNode
    if (inputData != nullptr && numFrames > 0 && oscillatorEnabled) {
        // WD-1.3 — un load atomico, igual que en processAudioBlock. El nodo no
        // puede desaparecer mientras estamos adentro: el CallbackGuard de arriba
        // mantiene el contador en 1, y setInputNode() no suelta la referencia
        // vieja hasta ver ese contador en 0.
        if (InputNode* inputNode = mInputNodeRt.load(std::memory_order_acquire)) {
            // Feed USB input samples to InputNode's ring buffer
            // This is needed for vocoder modulator and MIX mode
            inputNode->feedExternalInput(inputData, numFrames);
            mUsbFedBlocks.bump();
        }
    }

    // Process audio (main DSP path — shared by all backends)
    return processAudioBlock(outputData, numFrames);
}

void AudioEngine::onCaptureDiscontinuity(uint64_t framesQueuedAhead) noexcept {
    // WD-1.3 — el mismo load atomico que usa `onAudioReady`: el nodo no puede
    // desaparecer mientras el callback esta adentro.
    if (InputNode* inputNode = mInputNodeRt.load(std::memory_order_acquire)) {
        inputNode->reportCaptureDiscontinuity(framesQueuedAhead);
    }
}

void AudioEngine::onBackendError(watermelon_audio::BackendError error) {
    LOGE("Backend error: %s", watermelon_audio::backendErrorToString(error));

    mStreamError.store(true, std::memory_order_release);
    mLastStreamErrorCode.store(static_cast<int>(error), std::memory_order_release);
    incrementStateVersion();

    // Transition to stopped
    mState.store(EngineState::Stopped, std::memory_order_release);

    if (error == watermelon_audio::BackendError::DEVICE_DISCONNECTED) {
        if (mUseBackendManager.load(std::memory_order_acquire)) {
            LOGI("Device disconnected, BackendManager will handle recovery");
            // BackendManager handles fallback to Oboe
        } else {
            // Legacy path: automatic recovery via restart
            LOGI("Hardware disconnected, attempting automatic recovery...");

            if (mRecoveryThread && mRecoveryThread->joinable()) {
                mRecoveryThread->join();
            }

            mRecoveryThread = std::make_unique<std::thread>([this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                LOGI("Attempting to restart stream after hardware disconnection...");

                bool success = start();
                if (success) {
                    LOGI("Stream recovery successful!");
                    mStreamError.store(false, std::memory_order_release);
                    mLastStreamErrorCode.store(0, std::memory_order_release);
                } else {
                    LOGE("Stream recovery failed. User intervention required.");
                }
            });
        }
    } else if (error == watermelon_audio::BackendError::FATAL) {
        LOGE("Non-recoverable error. User intervention required.");
    }
}

void AudioEngine::onStreamConfigChanged(const watermelon_audio::StreamInfo& newInfo) {
    LOGI("Stream config changed: %dHz, %d channels, backend=%s",
         newInfo.sampleRate,
         newInfo.channelCount,
         watermelon_audio::backendTypeToString(newInfo.backendType));

    // Update sample rate in all components
    int sampleRate = newInfo.sampleRate;

    // REQ-006.2 — se DELEGA en vez de mantener una lista propia.
    //
    // Aca vivian tres lineas —`mOscBank.prepare`, `mEffectChain.setSampleRate`,
    // `mOutputStage.prepare`— y nada mas. `configureComponentsWithSampleRate()`
    // hace esas TRES Y OTRAS NUEVE: looper, transport, pre-roll, arpegiador, el
    // dispatcher de engines, mixer node, oscillator node, effect chain node,
    // voice manager y chord harmony.
    //
    // O sea esta no era una lista mas chica a proposito: era una lista que
    // DRIFTEO. El sintoma medible es el dispatcher —un cambio de rate no le
    // llegaba, asi que la cuerda de Karplus seguia dimensionada para el rate
    // viejo— pero agregar aca una linea para el dispatcher habria dejado las dos
    // listas drifteando con una entrada menos de diferencia.
    //
    // Delegar deja UN SOLO lugar que sabe que se re-prepara ante un cambio de
    // rate, que es la condicion para que el trinquete de REQ-006.3 signifique
    // algo. Y trae el quiesce de REQ-006.1 sin repetirlo.
    configureComponentsWithSampleRate(sampleRate);

    // REQ-001 S1 (1.16) — y al InputNode NO lo tocaba nadie. Su unico
    // `prepare()` en todo el arbol es el `prepare(48000, 4096)` literal de
    // `wmaEnsureInputNode`, asi que el camino de captura reportaba 48000
    // corriera el device a lo que corriera. Para el afinador eso escala todas
    // las frecuencias medidas.
    //
    // Se publica el rate y NADA MAS: llamar a `prepare()` aca haria `resize()`
    // de los rings del nodo con el thread de captura adentro. Ver la nota de
    // `InputNode::setCaptureSampleRate()`.
    // Solo si NADIE informo la config del stream de entrada. En un backend
    // partido el de entrada manda, y este `sampleRate` es el de SALIDA.
    if (!mHasInputStreamConfig.load(std::memory_order_acquire)) {
        mCaptureStreamSampleRate.store(sampleRate, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mInputNodeMutex);
            if (mInputNode) mInputNode->setCaptureSampleRate(sampleRate);
        }
        // REQ-012.4 — EL CABLEADO. Publicar el rate arregla la MEDICION de frecuencia
        // (REQ-001 S1); re-preparar arregla el DSP, que hasta acá se quedaba con los
        // coeficientes del rate viejo y los rings dimensionados para él.
        //
        // Son dos pasos y no uno a propósito: publicar es un store atómico que no
        // puede fallar, y el afinador lo necesita SIEMPRE. Re-preparar puede no
        // ocurrir —si no se confirma el drenaje no se toca nada— y sería un error
        // que un drenaje fallido dejara además el rate sin publicar.
        //
        // FUERA del lock: `reconfigureInputNodeForRate` toma `mInputNodeMutex`.
        // Y es seguro llamarlo desde acá — el contrato de `IAudioBackend` dice de
        // este hook: "NOT called from RT thread - safe to allocate/log".
        reconfigureInputNodeForRate(sampleRate);
    }

    incrementStateVersion();
}

// ========== DUAL TOUCH METHODS (Phase 1E — delegated to DualTouchManager) ==========

void AudioEngine::onInputStreamConfigChanged(const watermelon_audio::StreamInfo& newInfo) {
    LOGI("Input stream config changed: %dHz, %d channels",
         newInfo.sampleRate, newInfo.channelCount);

    mHasInputStreamConfig.store(true, std::memory_order_release);
    mCaptureStreamSampleRate.store(newInfo.sampleRate, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mInputNodeMutex);
        if (mInputNode) mInputNode->setCaptureSampleRate(newInfo.sampleRate);
    }
    // REQ-012.4 — el cableado, ver la nota gemela en `onStreamConfigChanged`. Este es
    // el camino que MANDA: en un backend partido los dos lados negocian rates
    // distintos y el de entrada es el que describe a la captura.
    reconfigureInputNodeForRate(newInfo.sampleRate);
    incrementStateVersion();
}

void AudioEngine::setDualTouchMode(bool enabled) {
    mDualTouch.setEnabled(enabled);

    if (enabled) {
        // [[maybe_unused]]: sólo lo lee el LOGI de abajo, que es ((void)0) con NDEBUG.
        [[maybe_unused]] const int oscIdx = mOscBank.getOscillatorType();
        LOGI("Dual touch using oscillator: %d", oscIdx);
    }

    incrementStateVersion();
}

// updateDualTouch is now an inline delegate in AudioEngine.h (Phase 1E)

// ========== INPUT NODE INTEGRATION (Full-Duplex Monitoring) ==========

// WD-1.3 — publicar y retirar el InputNode sin que el thread de audio toque un
// refcount ni, mucho menos, un destructor.
//
// El orden importa y es el mismo de siempre en publicación/reclamación:
//
//   1. Publicar el puntero nuevo. Desde acá ningún callback NUEVO puede ver el
//      viejo.
//   2. Esperar a que termine el que pueda estar adentro AHORA. La barrera ya
//      existía para stop(); acá se reusa.
//   3. Recién entonces soltar la referencia vieja — en ESTE thread.
//
// Saltearse el paso 2 es la versión "obvia" del arreglo y es la que sigue
// rompiendo: un callback que cargó el puntero antes del store lo sigue usando
// mientras el thread de control destruye el objeto.
AudioEngine::InputReconfigure AudioEngine::reconfigureInputNodeForRate(int sampleRate) {
    // Mismo techo y misma razon que el retiro de `setInputNode()`: un bloque son
    // ~2,7 ms, y un thread de audio que no cerro uno en cien bloques esta trabado.
    constexpr auto kTecho = std::chrono::milliseconds(250);

    // MINI-007 — se valida ACA y no se deja que el `false` del nodo lo represente.
    // Mapear un rate invalido a `SinDrenaje` seria mentir sobre la razon, y esa
    // mentira tiene consecuencias: `SinDrenaje` es el unico caso en que el llamador
    // NO puede caer a ningun camino alternativo.
    if (sampleRate <= 0) return InputReconfigure::RateInvalido;

    std::shared_ptr<InputNode> node;
    {
        std::lock_guard<std::mutex> lock(mInputNodeMutex);
        node = mInputNode;
    }
    // MINI-007 — se distingue de `SinDrenaje` a proposito: ver el KDoc del enum.
    if (!node) return InputReconfigure::SinNodoPublicado;

    // 1. Retirar del camino de SALIDA. Desde aca ningun callback nuevo lo toca.
    mInputNodeRt.store(nullptr, std::memory_order_release);

    // 2. Drenar al que ya estaba adentro.
    InputReconfigure resultado = InputReconfigure::SinDrenaje;
    if (waitForCallbackDrain(kTecho)) {
        // 3. Y recien ahora el otro escritor: el thread de captura. Si tampoco se
        //    puede confirmar ese, `reconfigureForRate` no toca nada y devuelve false.
        resultado = node->reconfigureForRate(sampleRate, kTecho)
                        ? InputReconfigure::Reconfigurado
                        : InputReconfigure::SinDrenaje;
    } else {
        LOGE("reconfigureInputNodeForRate: no se pudo drenar el callback de salida en "
             "%lldms — no se re-prepara", static_cast<long long>(kTecho.count()));
    }

    // 4. Republicar SIEMPRE, drenado o no. Dejarlo retirado silenciaria la entrada
    //    para siempre, que es peor que no haber re-preparado.
    mInputNodeRt.store(node.get(), std::memory_order_release);
    return resultado;
}

void AudioEngine::setInputNode(std::shared_ptr<InputNode> inputNode) {
    // 250 ms: un bloque son ~2,7 ms. Si el thread de audio no cerró uno en cien
    // bloques, no está lento — está trabado, y el audio ya se rompió.
    constexpr auto kRetireTimeout = std::chrono::milliseconds(250);

    std::shared_ptr<InputNode> previous;
    {
        std::lock_guard<std::mutex> lock(mInputNodeMutex);
        previous = std::move(mInputNode);
        mInputNode = std::move(inputNode);
        // Un nodo que se engancha DESPUES del cambio de config se perdio el
        // aviso, y nadie se lo repite: `onStreamConfigChanged` solo toca al que
        // estaba puesto en ese momento. Es el caso normal del afinador, que
        // engancha el suyo cuando el usuario lo abre. Se le dice aca, y solo si
        // no sabia — un nodo que ya tiene rate (USB, que lo recibe directo del
        // driver) sabe mas que el motor.
        const int known = mCaptureStreamSampleRate.load(std::memory_order_relaxed);
        // El rate del NODO gana si lo tiene: un nodo que ya sabe (USB, que lo recibe
        // del driver; o su propio stream de Oboe ya abierto) sabe mas que el motor.
        const int propio = mInputNode ? mInputNode->getCaptureSampleRate() : 0;
        const int rate = propio > 0 ? propio : known;
        if (mInputNode && rate > 0) {
            if (propio <= 0) mInputNode->setCaptureSampleRate(rate);
            // REQ-012.4 — y ademas se lo PREPARA para ese rate, no solo se le avisa.
            //
            // 🔴 SE RE-PREPARA AUNQUE EL NODO YA SUPIERA SU RATE, y esa es la parte
            // que faltaba: saber el rate y estar PREPARADO para el son cosas
            // distintas. Un nodo que abrio su propio stream de Oboe
            // (`startInputStream`) publica el rate negociado pero sigue con el DSP
            // del provisional; con la guarda vieja —re-preparar solo si el rate era
            // desconocido— ese caso quedaba afuera justo al entrar al grafo.
            // Un nodo recien construido trae el `prepare()` provisional de
            // `wmaEnsureInputNode`, asi que sin esto entraria al grafo con el DSP
            // configurado para un rate que nadie midio.
            //
            // Va ANTES de publicar en `mInputNodeRt`: hasta esa linea el nodo no es
            // alcanzable por el callback de salida, o sea que aca el unico escritor
            // posible es su propio thread de captura — y a ese lo drena
            // `reconfigureForRate`. Por eso alcanza el del nodo y no hace falta el
            // del motor, que ademas volveria a tomar este mismo mutex.
            mInputNode->reconfigureForRate(rate, kRetireTimeout);
        }
        // 1. Publicar. release: el objeto está completamente construido antes.
        mInputNodeRt.store(mInputNode.get(), std::memory_order_release);
    }

    if (previous) {
        // 2. Drenar.
        if (!waitForCallbackDrain(kRetireTimeout)) {
            LOGE("InputNode retire: no se pudo confirmar el drenaje de callbacks "
                 "en %lldms — se filtra el nodo en vez de arriesgar un UAF",
                 static_cast<long long>(kRetireTimeout.count()));
            std::lock_guard<std::mutex> lock(mInputNodeMutex);
            mUndrainedInputNodes.push_back(std::move(previous));
        }
    }

    // 3. `previous` se destruye acá, al salir del scope, en el thread de control.
    LOGI("InputNode %s", mInputNodeRt.load(std::memory_order_relaxed)
                             ? "connected to AudioEngine for monitoring"
                             : "disconnected from AudioEngine");
}

bool AudioEngine::waitForCallbackDrain(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mStateMutex);

    // Espera acotada en vez de un solo wait_for con predicado: el notify_all del
    // CallbackGuard se hace SIN el mutex tomado, así que un wakeup se puede
    // perder entre que el waiter evalúa el predicado y se duerme. Re-chequear
    // cada 5 ms lo cubre sin depender de que el notify llegue.
    while (mActiveCallbacks.load(std::memory_order_acquire) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        mStopCondition.wait_for(lock, std::chrono::milliseconds(5));
    }
    return true;
}

bool AudioEngine::spinForCallbackDrain(std::chrono::milliseconds timeout) {
    // REQ-006.1. Ver la nota del header: no puede tomar `mStateMutex` porque
    // `start()` ya lo tiene cuando llama a configureComponentsWithSampleRate().
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (mActiveCallbacks.load(std::memory_order_acquire) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        // Polling con deadline. 200 us es ~1/13 de un bloque de 2,7 ms: corto
        // para no alargar el silencio, largo para no quemar el core.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

AudioEngine::ReconfigureQuiesce::ReconfigureQuiesce(AudioEngine& engine,
                                                    std::chrono::milliseconds timeout)
    : mEngine(engine), mDrained(false) {
    // 1. Cerrar la compuerta. Desde aca ningun callback NUEVO toca los engines.
    mEngine.mEnginesReconfiguring.store(true, std::memory_order_release);
    // 2. Drenar el que pueda estar adentro AHORA.
    //
    // Hacen falta LOS DOS pasos. La compuerta sola deja adentro al callback que
    // ya la habia leido en false; el drenaje solo no impide que entre uno nuevo.
    // `setInputNode()` puede drenar sin compuerta porque publica un puntero
    // nuevo primero — aca los engines son LOS MISMOS OBJETOS, asi que no hay
    // nada que publicar.
    mDrained = mEngine.spinForCallbackDrain(timeout);
}

AudioEngine::ReconfigureQuiesce::~ReconfigureQuiesce() {
    mEngine.mEnginesReconfiguring.store(false, std::memory_order_release);
}

void AudioEngine::setOscillatorEnabled(bool enabled) {
    mOscillatorEnabled.store(enabled, std::memory_order_release);
    // FIX P1.2: Notify StateSynchronizer of state change
    incrementStateVersion();
    LOGI("Oscillator enabled: %s", enabled ? "true" : "false");
}

// ========== VOICE SYSTEM (Phase 2 - Polyphonic Voices) ==========

void AudioEngine::updateMultiTouch(const voice::TouchData* touches, int count) {
    if (!mVoiceManager) {
        LOGW("updateMultiTouch: VoiceManager not available");
        return;
    }

    voice::TouchTriggerSource* touchSource = mVoiceManager->getTouchSource();
    if (!touchSource) {
        LOGW("updateMultiTouch: TouchTriggerSource not registered");
        return;
    }

    // WD-1.5 — aca habia un `static int lastCount` gateando un bloque de LOGI.
    // No es RT (esto lo llama el thread de control), pero era global de proceso
    // igual, y el log recorria el array de touches en cada transicion de conteo.
    // El conteo de voces activas ya es consultable por getActiveVoiceCount().

    // Forward touch data to the touch source
    touchSource->updateTouches(touches, count);
}