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
        static int checkCount = 0;
        if (++checkCount < 500) return;
        checkCount = 0;

        auto xrunResult = oboeStream->getXRunCount();
        if (xrunResult) {
            int32_t count = xrunResult.value();
            if (count > mLastXRunCount) {
                wma::logMessage(wma::LogLevel::WARN, "OboeAdapter",
                    "XRUN DETECTED! Count: %d (was %d)", count, mLastXRunCount);
                mLastXRunCount = count;
            }
        }
    }

    AudioEngine* mEngine;
    int32_t mLastXRunCount = 0;
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

    // Flush denormals to zero to prevent CPU performance degradation (10-100x slowdown)
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

        // Push preferred sample rate to BackendManager BEFORE starting so
        // the device negotiates to it during start().
        int preferredRate = mPreferredSampleRate.load(std::memory_order_acquire);
        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "[START] entry: preferredRate=%d useBackendMgr=1 fadeTimeMs=%d",
            preferredRate, fadeTimeMs);
        if (preferredRate > 0) {
            manager.setSampleRate(preferredRate);
        }

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
        const int expectedRate = (preferredRate > 0) ? preferredRate : 48000;
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

    // Use preferred sample rate if set (important for monitoring to match input stream)
    int preferredRate = mPreferredSampleRate.load(std::memory_order_acquire);
    if (preferredRate > 0) {
        builder.setSampleRate(preferredRate);
        LOGI("Using preferred sample rate: %d Hz", preferredRate);
    } else {
        LOGI("Using auto sample rate selection");
    }

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

void AudioEngine::applyEffectsAndOutput(float* output, int32_t numFrames) {
    // DC block + effects
    mOutputStage.dcBlock(mOutputStage.getTempBuffer(), numFrames);
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
    // BEFORE the looper mixes its playback into `output`. The looper's
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

    // ---- MASTER VOLUME (whole mix, no fade) ----
    // Applied AFTER the looper mix so master volume scales the combined
    // synth + FX + loops bus uniformly.
    const float masterVol = mMasterVolume.load(std::memory_order_acquire);
    if (masterVol != 1.0f) {
        simd::applyStereoGain(output, numFrames, masterVol);
    }

    // Output stage protection
    mOutputStage.processOutput(output, numFrames);
}

void AudioEngine::feedVocoderModulator(InputNode* inputNode, int32_t numFrames, bool hasInputMonitoring) {
    const int32_t totalSamples = numFrames * 2;
    if (hasInputMonitoring && mMonitoringBuffer.size() >= static_cast<size_t>(totalSamples)) {
        int framesRead = inputNode->getMonitoringSamples(mMonitoringBuffer.data(), numFrames);
        if (framesRead > 0) {
            // Convert stereo mic input to mono for vocoder
            for (int i = 0; i < framesRead; ++i) {
                mMonitoringBuffer[i] = (mMonitoringBuffer[i * 2] + mMonitoringBuffer[i * 2 + 1]) * 0.5f;
            }
            mEffectChain.setVocoderModulatorBuffer(mMonitoringBuffer.data(), framesRead);
        }
    }
}

watermelon_audio::IAudioCallback::Result AudioEngine::handleNotRunning(
    float* output, int32_t numFrames, InputNode* inputNode) {
    const int32_t totalSamples = numFrames * 2;
    // Silence the synth output but allow monitoring
    std::fill(output, output + totalSamples, 0.0f);

    static int notRunningCheckCount = 0;
    if (++notRunningCheckCount >= 200) {
        LOGI_CALLBACK("ENGINE (not running): inputNode=%p, monEnabled=%d, bufSize=%zu, totalSamples=%d",
             inputNode,
             inputNode ? inputNode->isMonitoringEnabled() : false,
             mMonitoringBuffer.size(),
             totalSamples);
        notRunningCheckCount = 0;
    }

    if (inputNode && inputNode->isMonitoringEnabled()) {
        if (mMonitoringBuffer.size() >= static_cast<size_t>(totalSamples)) {
            int framesRead = inputNode->getMonitoringSamples(mMonitoringBuffer.data(), numFrames);

            static int notRunningMixCount = 0;
            if (++notRunningMixCount >= 100) {
                float maxSample = 0.0f;
                for (int i = 0; i < std::min(framesRead * 2, 100); ++i) {
                    if (std::abs(mMonitoringBuffer[i]) > maxSample) {
                        maxSample = std::abs(mMonitoringBuffer[i]);
                    }
                }
                LOGI_CALLBACK("ENGINE MONITOR (not running): framesRead=%d, maxSample=%.4f, numFrames=%d",
                     framesRead, maxSample, numFrames);
                notRunningMixCount = 0;
            }

            if (framesRead > 0) {
                int32_t samplesToMix = framesRead * 2;
                for (int32_t i = 0; i < samplesToMix && i < totalSamples; ++i) {
                    output[i] = mMonitoringBuffer[i];
                }
                for (int32_t i = samplesToMix; i < totalSamples; ++i) {
                    output[i] = 0.0f;
                }
            }
        }
    }
    return watermelon_audio::IAudioCallback::Result::CONTINUE;
}

void AudioEngine::renderInputFx(float* output, int32_t numFrames, InputNode* inputNode) {
    const int32_t totalSamples = numFrames * 2;

    // DIAGNOSTIC: Oboe INPUT_FX path
    static int oboeInputFxLogCount = 0;
    if (++oboeInputFxLogCount >= 300) {
        bool inputRunning = inputNode ? inputNode->isInputStreamRunning() : false;
        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "OBOE_INPUT_FX: inputNode=%p, monEnabled=%d, inputStreamRunning=%d",
            inputNode, inputNode ? inputNode->isMonitoringEnabled() : 0, inputRunning);
        oboeInputFxLogCount = 0;
    }

    // 1. Read input samples into temp buffer
    if (mOutputStage.getTempBufferSize() >= static_cast<size_t>(totalSamples)) {
        int framesRead = inputNode->getMonitoringSamples(mOutputStage.getTempBuffer(), numFrames);

        // DIAGNOSTIC: Log frames read
        static int framesReadLogCount = 0;
        if (++framesReadLogCount >= 300) {
            float monPeak = 0.0f;
            int samples = std::min(framesRead * 2, 64);
            for (int i = 0; i < samples; ++i) {
                float abs = std::abs(mOutputStage.getTempBuffer()[i]);
                if (abs > monPeak) monPeak = abs;
            }
            wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
                "OBOE_INPUT_FX_READ: framesRead=%d/%d, monPeak=%.5f",
                framesRead, numFrames, monPeak);
            framesReadLogCount = 0;
        }

        if (framesRead > 0) {
            // Zero-pad if fewer frames than requested
            if (framesRead < numFrames) {
                int32_t samplesRead = framesRead * 2;
                std::fill_n(mOutputStage.getTempBuffer() + samplesRead,
                            totalSamples - samplesRead, 0.0f);
            }

            // DC block + effects + fade + output
            applyEffectsAndOutput(output, numFrames);
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
    applyEffectsAndOutput(output, numFrames);
}

void AudioEngine::renderVoiceSystem(float* output, int32_t numFrames) {
    // 1. Process voice system (generates polyphonic output)
    mVoiceManager->process(mOutputStage.getTempBuffer(), numFrames);

    // 2. Apply modulator if exists
    mOscBank.applyModulation(mOutputStage.getTempBuffer(), numFrames);

    // 3. DC block + effects + fade + output
    applyEffectsAndOutput(output, numFrames);
}

void AudioEngine::renderSingleTouch(float* output, int32_t numFrames,
                                     int cachedEngineType, size_t cachedOscIndex,
                                     bool cachedHasActiveModulator, size_t cachedModIndex,
                                     InputNode* inputNode) {
    const int32_t totalSamples = numFrames * 2;
    bool oscillatorEnabled = mOscillatorEnabled.load(std::memory_order_acquire);
    bool hasInputMonitoring = inputNode && inputNode->isMonitoringEnabled();

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
        feedVocoderModulator(inputNode, numFrames, hasInputMonitoring);

        // 4-6. DC block + effects + fade + output
        applyEffectsAndOutput(output, numFrames);

    } else {
        std::fill_n(output, totalSamples, 0.0f);
    }
}

void AudioEngine::renderDualTouch(float* output, int32_t numFrames,
                                   const TouchState& dualTouchState,
                                   int cachedEngineType, size_t cachedOscIndex,
                                   bool cachedHasActiveModulator, size_t cachedModIndex,
                                   InputNode* inputNode) {
    const int32_t totalSamples = numFrames * 2;
    bool oscillatorEnabled = mOscillatorEnabled.load(std::memory_order_acquire);
    bool hasInputMonitoring = inputNode && inputNode->isMonitoringEnabled();

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
        feedVocoderModulator(inputNode, numFrames, hasInputMonitoring);

        // DC block + effects + fade + output
        applyEffectsAndOutput(output, numFrames);

    } else {
        std::fill_n(output, totalSamples, 0.0f);
    }
}

void AudioEngine::handleMixMonitoring(float* output, int32_t numFrames,
                                       InputNode* inputNode,
                                       bool oscillatorEnabled, bool hasInputMonitoring) {
    const int32_t totalSamples = numFrames * 2;

    // DEBUG: Log monitoring status periodically with sample rate info
    static int monitorCheckCount = 0;
    if (++monitorCheckCount >= 500) {
        int outputSampleRate = currentSampleRate();
        int inputSampleRate = inputNode ? inputNode->getStreamSampleRate() : 0;
        LOGI("ENGINE MONITOR CHECK: inputNode=%p, monitoringEnabled=%d, oscEnabled=%d, outputSR=%d, inputSR=%d, mixerNode=%p",
             inputNode,
             hasInputMonitoring,
             oscillatorEnabled,
             outputSampleRate,
             inputSampleRate,
             mMixerNode.get());

        if (inputSampleRate > 0 && outputSampleRate > 0 && inputSampleRate != outputSampleRate) {
            LOGW("SAMPLE RATE MISMATCH! Input=%d, Output=%d - THIS CAUSES GLITCHES!",
                 inputSampleRate, outputSampleRate);
        }
        monitorCheckCount = 0;
    }

    // DIAGNOSTIC: Log MIX mode conditions periodically
    static int mixConditionLogCount = 0;
    if (++mixConditionLogCount >= 500) {
        LOGI("MIX CHECK: oscEnabled=%d, hasInputMonitoring=%d, inputNode=%p, mixerNode=%p",
             oscillatorEnabled, hasInputMonitoring, inputNode, mMixerNode.get());
        mixConditionLogCount = 0;
    }

    // Only mix input in MIX mode (oscillator enabled + input monitoring enabled)
    if (oscillatorEnabled && hasInputMonitoring) {
        if (mMixerNode && mMonitoringBuffer.size() >= static_cast<size_t>(totalSamples)) {
            // 1. Copy oscillator output to mOscillatorBuffer (non-interleaved)
            mOscillatorBuffer.copyFromInterleaved(output, numFrames);

            // 2. Read input samples and convert to non-interleaved
            int framesRead = inputNode->getMonitoringSamples(mMonitoringBuffer.data(), numFrames);

            mInputBuffer.clear();
            if (framesRead > 0) {
                mInputBuffer.copyFromInterleaved(mMonitoringBuffer.data(), framesRead);
            }

            // 3. Process through MixerNode
            AudioBuffer dummyInput;
            mMixerNode->process(dummyInput, numFrames);

            // 4. Copy MixerNode output back to interleaved outputData
            mMixerNode->getOutputBuffer().copyToInterleaved(output, numFrames);

            // 5. Output protection
            mOutputStage.processOutputNoClip(output, numFrames);

            // DEBUG: Log mix periodically
            static int mixerMixCount = 0;
            if (++mixerMixCount >= 100) {
                LOGI("MIX MODE (MixerNode): framesRead=%d, busLevel=%.2f, inputLevel=%.2f",
                     framesRead,
                     mMixerNode->getInputLevel(MixerNode::INPUT_OSCILLATOR),
                     mMixerNode->getInputLevel(MixerNode::INPUT_EXTERNAL));
                mixerMixCount = 0;
            }
        } else {
            // Fallback to simple mixing if MixerNode not available
            if (mMonitoringBuffer.size() >= static_cast<size_t>(totalSamples)) {
                int framesRead = inputNode->getMonitoringSamples(mMonitoringBuffer.data(), numFrames);

                if (framesRead > 0) {
                    simd::addStereoBuffers(output, output, mMonitoringBuffer.data(),
                                           framesRead, true);
                    simd::hardLimitStereo(output, framesRead);
                }
            }
        }
    }
}

// ========== DECOMPOSED processAudioBlock ==========

watermelon_audio::IAudioCallback::Result AudioEngine::processAudioBlock(
    float* audioData, int32_t numFrames) {
    // RAII callback guard
    mActiveCallbacks.fetch_add(1, std::memory_order_acquire);
    struct CallbackGuard {
        std::atomic<int>& counter;
        std::condition_variable& cv;
        CallbackGuard(std::atomic<int>& c, std::condition_variable& condition)
            : counter(c), cv(condition) {}
        ~CallbackGuard() {
            int remaining = counter.fetch_sub(1, std::memory_order_release) - 1;
            if (remaining == 0) cv.notify_all();
        }
    } guard(mActiveCallbacks, mStopCondition);

    try {
        // RT-safe InputNode access (try_lock, never blocks)
        std::shared_ptr<InputNode> inputNodePtr;
        if (mInputNodeMutex.try_lock()) {
            inputNodePtr = mInputNode;
            mInputNodeMutex.unlock();
        }
        InputNode* inputNode = inputNodePtr.get();

        // State check + periodic logging
        EngineState state = mState.load(std::memory_order_acquire);
        static int xrunCheckCount = 0;
        if (++xrunCheckCount >= 500) {
            xrunCheckCount = 0;
            LOGI_CALLBACK("ENGINE CALLBACK: state=%d, xruns=%d",
                 static_cast<int>(state), mLastXRunCount.load(std::memory_order_relaxed));
        }

        float* outputData = audioData;
        const int32_t totalSamples = numFrames * 2;

        // Not-Running: silence + monitoring
        if (state != EngineState::Running) {
            return handleNotRunning(outputData, numFrames, inputNode);
        }

        // Buffer validation
        if (totalSamples > static_cast<int32_t>(mOutputStage.getTempBufferSize())) {
            LOGE("Buffer overflow prevented: %d samples requested, %zu available",
                 totalSamples, mOutputStage.getTempBufferSize());
            std::fill(outputData, outputData + totalSamples, 0.0f);
            return watermelon_audio::IAudioCallback::Result::CONTINUE;
        }

        // Mode detection: cache state once per callback
        const auto dualTouchState = mDualTouch.snapshot();
        bool oscillatorEnabled = mOscillatorEnabled.load(std::memory_order_acquire);
        bool hasInputMonitoring = inputNode && inputNode->isMonitoringEnabled();
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
                              cachedHasActiveModulator, cachedModIndex, inputNode);

        } else {
            renderDualTouch(outputData, numFrames, dualTouchState, cachedEngineType,
                            cachedOscIndex, cachedHasActiveModulator, cachedModIndex, inputNode);
        }

        // MIX mode monitoring (post-render)
        handleMixMonitoring(outputData, numFrames, inputNode, oscillatorEnabled, hasInputMonitoring);

        // Waveform capture (final post-master output for visualization).
        // Looper tap moved INTO applyEffectsAndOutput (post-FX, pre-master-vol)
        // so it captures the dry instrument signal independent of master volume.
        mWaveformCapture.write(outputData, numFrames);

        mCallbackErrorCount.store(0, std::memory_order_relaxed);
        return watermelon_audio::IAudioCallback::Result::CONTINUE;

    } catch (const std::exception& e) {
        LOGE("CRITICAL: Exception in audio callback: %s", e.what());
        std::fill(audioData, audioData + numFrames * 2, 0.0f);
        int errorCount = mCallbackErrorCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (errorCount > 100) {
            LOGE("CRITICAL: Too many callback errors (%d), requesting stop", errorCount);
            return watermelon_audio::IAudioCallback::Result::STOP;
        }
        return watermelon_audio::IAudioCallback::Result::CONTINUE;

    } catch (...) {
        LOGE("CRITICAL: Unknown exception in audio callback");
        std::fill(audioData, audioData + numFrames * 2, 0.0f);
        int errorCount = mCallbackErrorCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (errorCount > 100) {
            LOGE("CRITICAL: Too many callback errors (%d), requesting stop", errorCount);
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

    const int preferred = mPreferredSampleRate.load(std::memory_order_acquire);
    if (preferred > 0) {
        return preferred;
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

void AudioEngine::configureComponentsWithSampleRate(int sampleRate) {
    LOGI("Configuring audio components with sample rate: %d Hz", sampleRate);

    // Default max block size (used when we don't have framesPerBurst)
    int maxBlockSize = 4096;

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

        // Configure BackendManager with current preferred settings
        int preferredRate = mPreferredSampleRate.load(std::memory_order_acquire);
        if (preferredRate > 0) {
            manager.setSampleRate(preferredRate);
        }
    }
}

// ========== IAUDIOCALLBACK IMPLEMENTATION (Backend Abstraction) ==========
// FIX PHASE 7.2 & 7.3: Refactored callback for USB audio
// FIX PHASE 8: Direct USB INPUT_FX processing without double buffering

watermelon_audio::IAudioCallback::Result AudioEngine::onAudioReady(
    float* outputData,
    const float* inputData,
    int32_t numFrames) {

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
    }

    // ========== DIRECT USB INPUT_FX MODE ==========
    // When USB backend provides input AND oscillator is disabled (INPUT_FX mode),
    // process input directly through effect chain without going through InputNode.
    // This eliminates ~100ms of latency from double buffering.
    bool oscillatorEnabled = mOscillatorEnabled.load(std::memory_order_acquire);
    bool isUsbInputFxMode = (inputData != nullptr) && !oscillatorEnabled && (numFrames > 0);

    // ========== DIAGNOSTIC: Log USB callback state (periodic) ==========
    static int diagLogCount = 0;
    if (++diagLogCount >= 300) {
        float fadeVol = mFadeCtrl.getCurrentFadeVolume();
        float masterVol = mMasterVolume.load(std::memory_order_acquire);
        bool paused = mFadeCtrl.isPaused();
        EngineState state = mState.load(std::memory_order_acquire);

        // Compute input peak level
        float inputPeak = 0.0f;
        if (inputData != nullptr && numFrames > 0) {
            int samples = std::min(numFrames * 2, 64);
            for (int i = 0; i < samples; ++i) {
                float abs = std::abs(inputData[i]);
                if (abs > inputPeak) inputPeak = abs;
            }
        }

        wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
            "USB_CB: inputData=%p, oscEnabled=%d, isUsbInputFxMode=%d, "
            "outputData=%p, numFrames=%d, state=%d, paused=%d, "
            "fadeVol=%.3f, masterVol=%.3f, inputPeak=%.5f",
            inputData, oscillatorEnabled, isUsbInputFxMode,
            outputData, numFrames, static_cast<int>(state), paused,
            fadeVol, masterVol, inputPeak);
        diagLogCount = 0;
    }

    if (isUsbInputFxMode && outputData != nullptr) {
        // Debug logging (periodic)
        static int directUsbLogCount = 0;
        if (++directUsbLogCount >= 500) {
            LOGI("USB DIRECT INPUT_FX: Processing %d frames directly (no InputNode buffering)", numFrames);
            directUsbLogCount = 0;
        }

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

        // 3. Process through effect chain (INPUT → EFFECTS → OUTPUT)
        mEffectChain.process(mOutputStage.getTempBuffer(), outputData, numFrames);

        // 3b. Looper integration (post-FX, pre-master). Mirrors applyEffectsAndOutput.
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

        // ========== DIAGNOSTIC: Output level after processing ==========
        static int directOutLogCount = 0;
        if (++directOutLogCount >= 300) {
            float outPeak = 0.0f;
            int samples = std::min(numFrames * 2, 64);
            for (int i = 0; i < samples; ++i) {
                float abs = std::abs(outputData[i]);
                if (abs > outPeak) outPeak = abs;
            }
            wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
                "USB_DIRECT_OUT: gainStart=%.3f, gainEnd=%.3f, fadeStart=%.3f, "
                "fadeEnd=%.3f, masterVol=%.3f, outPeak=%.5f, effects=%d",
                gainStart, gainEnd, fadeStart, fadeEnd, masterVol, outPeak,
                static_cast<int>(mEffectChain.getNumEffects()));
            directOutLogCount = 0;
        }

        return watermelon_audio::IAudioCallback::Result::CONTINUE;
    }

    // ========== NON-INPUT_FX MODES (MIX, CHAOS_PAD, etc.) ==========
    // For modes that need InputNode (vocoder, MIX mode), feed USB input to InputNode
    if (inputData != nullptr && numFrames > 0 && oscillatorEnabled) {
        // Try to get InputNode for feeding USB input (vocoder modulator, MIX mode)
        std::shared_ptr<InputNode> inputNodePtr;
        if (mInputNodeMutex.try_lock()) {
            inputNodePtr = mInputNode;
            mInputNodeMutex.unlock();
        }

        if (inputNodePtr) {
            // Feed USB input samples to InputNode's ring buffer
            // This is needed for vocoder modulator and MIX mode
            inputNodePtr->feedExternalInput(inputData, numFrames);

            static int usbMixLogCount = 0;
            if (++usbMixLogCount >= 500) {
                LOGI("USB MIX/VOCODER: Fed %d frames to InputNode", numFrames);
                usbMixLogCount = 0;
            }
        }
    }

    // Process audio (main DSP path — shared by all backends)
    return processAudioBlock(outputData, numFrames);
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

    // Configure oscillators and modulators (Phase 1E — delegated to OscillatorBank)
    mOscBank.prepare(sampleRate);

    mEffectChain.setSampleRate(sampleRate);
    mOutputStage.prepare(sampleRate, 0);

    incrementStateVersion();
}

// ========== DUAL TOUCH METHODS (Phase 1E — delegated to DualTouchManager) ==========

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

void AudioEngine::setInputNode(std::shared_ptr<InputNode> inputNode) {
    {
        std::lock_guard<std::mutex> lock(mInputNodeMutex);
        mInputNode = inputNode;
    }
    if (inputNode) {
        LOGI("InputNode connected to AudioEngine for monitoring (shared_ptr)");
    } else {
        LOGI("InputNode disconnected from AudioEngine");
    }
}

void AudioEngine::setPreferredSampleRate(int sampleRate) {
    mPreferredSampleRate.store(sampleRate, std::memory_order_release);
    LOGI("Preferred sample rate set to: %d Hz (0 = auto)", sampleRate);
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

    // DEBUG: Log on count transitions only
    static int lastCount = -1;
    if (count != lastCount) {
        LOGI("updateMultiTouch: count %d -> %d, voiceSystem=%d, oscEnabled=%d, activeVoices=%d",
             lastCount, count,
             mUseVoiceSystem.load(std::memory_order_acquire),
             mOscillatorEnabled.load(std::memory_order_acquire),
             mVoiceManager->getActiveVoiceCount());
        for (int i = 0; i < count; i++) {
            LOGI("  touch[%d]: active=%d, pointerId=%d, freq=%.1f, amp=%.3f, x=%.3f, y=%.3f",
                 i, touches[i].active, touches[i].pointerId,
                 touches[i].frequency, touches[i].amplitude,
                 touches[i].x, touches[i].y);
        }
        lastCount = count;
    }

    // Forward touch data to the touch source
    touchSource->updateTouches(touches, count);
}