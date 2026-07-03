#include "SynthEngineDispatcher.h"
#include "../voice/VoiceManager.h"
#include "../voice/VoicePool.h"
#include "../voice/Voice.h"
#include "../platform/Logger.h"

#define LOG_TAG "SynthEngineDispatcher"

#ifdef NDEBUG
    #define LOGI(...) ((void)0)
    #define LOGW(...) ((void)0)
    #define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)
#else
    #define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
    #define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
    #define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)
#endif

SynthEngineDispatcher::SynthEngineDispatcher() {
    try {
        mEngineBuffer.resize(8192);  // 4096 frames * 2 channels

        // Karplus-Strong (Phase 6A)
        mKarplusStrong = std::make_unique<KarplusStrongEngine>();
        mKarplusStrong->prepare(48000, 4096);

        // FM Synth (Phase 6B)
        mFMEngine = std::make_unique<FMEngine>();
        mFMEngine->prepare(48000, 4096);

        // Supersaw (Phase 6E)
        mSupersawEngine = std::make_unique<SupersawEngine>();
        mSupersawEngine->prepare(48000, 4096);

        // Wavetable (Phase 6C)
        mWavetableEngine = std::make_unique<WavetableEngine>();
        mWavetableEngine->prepare(48000, 4096);

        // Granular (Phase 6D)
        mGranularEngine = std::make_unique<GranularEngine>();
        mGranularEngine->prepare(48000, 4096);

        // Secondary instances for dual touch (touch 2)
        mKarplusStrong2 = std::make_unique<KarplusStrongEngine>();
        mKarplusStrong2->prepare(48000, 4096);
        mFMEngine2 = std::make_unique<FMEngine>();
        mFMEngine2->prepare(48000, 4096);
        mSupersawEngine2 = std::make_unique<SupersawEngine>();
        mSupersawEngine2->prepare(48000, 4096);
        mWavetableEngine2 = std::make_unique<WavetableEngine>();
        mWavetableEngine2->prepare(48000, 4096);
        mGranularEngine2 = std::make_unique<GranularEngine>();
        mGranularEngine2->prepare(48000, 4096);

        // Voice engine pools (one per voice per engine type)
        for (int i = 0; i < MAX_VOICE_ENGINES; ++i) {
            mKSPool[i] = std::make_unique<KarplusStrongEngine>();
            mKSPool[i]->prepare(48000, 4096);
            mFMPool[i] = std::make_unique<FMEngine>();
            mFMPool[i]->prepare(48000, 4096);
            mSupersawPool[i] = std::make_unique<SupersawEngine>();
            mSupersawPool[i]->prepare(48000, 4096);
            mWavetablePool[i] = std::make_unique<WavetableEngine>();
            mWavetablePool[i]->prepare(48000, 4096);
            mGranularPool[i] = std::make_unique<GranularEngine>();
            mGranularPool[i]->prepare(48000, 4096);
        }

        // SoundFont engine (Phase 8) — shared manager + engine instances
        mSoundFontManager = std::make_unique<SoundFontManager>();
        mSoundFontEngine = std::make_unique<SoundFontEngine>();
        mSoundFontEngine->setSoundFontManager(mSoundFontManager.get());
        mSoundFontEngine->prepare(48000, 4096);

        LOGI("SynthEngineDispatcher: primary + dual touch + voice pool (%d per type) + SoundFont allocated",
             MAX_VOICE_ENGINES);
    } catch (const std::bad_alloc& e) {
        LOGE("Failed to allocate synth engines: %s", e.what());
    }
}

void SynthEngineDispatcher::prepare(int sampleRate, int maxBlockSize) {
    // Primary engines
    if (mKarplusStrong) mKarplusStrong->prepare(sampleRate, maxBlockSize);
    if (mFMEngine) mFMEngine->prepare(sampleRate, maxBlockSize);
    if (mSupersawEngine) mSupersawEngine->prepare(sampleRate, maxBlockSize);
    if (mWavetableEngine) mWavetableEngine->prepare(sampleRate, maxBlockSize);
    if (mGranularEngine) mGranularEngine->prepare(sampleRate, maxBlockSize);

    // Secondary instances (dual touch)
    if (mKarplusStrong2) mKarplusStrong2->prepare(sampleRate, maxBlockSize);
    if (mFMEngine2) mFMEngine2->prepare(sampleRate, maxBlockSize);
    if (mSupersawEngine2) mSupersawEngine2->prepare(sampleRate, maxBlockSize);
    if (mWavetableEngine2) mWavetableEngine2->prepare(sampleRate, maxBlockSize);
    if (mGranularEngine2) mGranularEngine2->prepare(sampleRate, maxBlockSize);

    // SoundFont engine
    if (mSoundFontEngine) mSoundFontEngine->prepare(sampleRate, maxBlockSize);

    // Voice pool engines
    for (int i = 0; i < MAX_VOICE_ENGINES; ++i) {
        if (mKSPool[i]) mKSPool[i]->prepare(sampleRate, maxBlockSize);
        if (mFMPool[i]) mFMPool[i]->prepare(sampleRate, maxBlockSize);
        if (mSupersawPool[i]) mSupersawPool[i]->prepare(sampleRate, maxBlockSize);
        if (mWavetablePool[i]) mWavetablePool[i]->prepare(sampleRate, maxBlockSize);
        if (mGranularPool[i]) mGranularPool[i]->prepare(sampleRate, maxBlockSize);
    }

    LOGI("All engines prepared: sr=%d, maxBlock=%d", sampleRate, maxBlockSize);
}

void SynthEngineDispatcher::reset() {
    // Reset primary engines
    if (mKarplusStrong) mKarplusStrong->reset();
    if (mFMEngine) mFMEngine->reset();
    if (mSupersawEngine) mSupersawEngine->reset();
    if (mWavetableEngine) mWavetableEngine->reset();
    if (mGranularEngine) mGranularEngine->reset();

    // Reset secondary engines
    if (mKarplusStrong2) mKarplusStrong2->reset();
    if (mFMEngine2) mFMEngine2->reset();
    if (mSupersawEngine2) mSupersawEngine2->reset();
    if (mWavetableEngine2) mWavetableEngine2->reset();
    if (mGranularEngine2) mGranularEngine2->reset();

    // Reset SoundFont
    if (mSoundFontEngine) mSoundFontEngine->noteOffAll();

    // Reset voice pools
    for (int i = 0; i < MAX_VOICE_ENGINES; ++i) {
        if (mKSPool[i]) mKSPool[i]->reset();
        if (mFMPool[i]) mFMPool[i]->reset();
        if (mSupersawPool[i]) mSupersawPool[i]->reset();
        if (mWavetablePool[i]) mWavetablePool[i]->reset();
        if (mGranularPool[i]) mGranularPool[i]->reset();
    }

    // Reset crossfade state
    mPrevEngineType = mCurrentEngineType.load(std::memory_order_acquire);
    mEngineCrossfadeGain = 1.0f;
    mEngineCrossfadeRemaining = 0;
}

void SynthEngineDispatcher::setEngineType(int engineType) {
    if (engineType >= 0 && engineType <= static_cast<int>(EngineTypeId::SOUNDFONT)) {
        mCurrentEngineType.store(engineType, std::memory_order_release);
        LOGI("Engine type set to %d", engineType);
    } else {
        LOGE("Invalid engine type: %d", engineType);
    }
}

void SynthEngineDispatcher::setEngineParameter(int paramId, float value) {
    int engineType = mCurrentEngineType.load(std::memory_order_acquire);

    // Primary engine
    SynthEngine* engine = getEngine(engineType);
    if (engine) {
        engine->setParameter(paramId, value);
    }

    // Secondary (dual touch) engine
    SynthEngine* engine2 = getDualTouchEngine(engineType);
    if (engine2) {
        engine2->setParameter(paramId, value);
    }

    // Voice pool engines
    for (int i = 0; i < MAX_VOICE_ENGINES; ++i) {
        SynthEngine* voiceEngine = getVoiceEngine(engineType, i);
        if (voiceEngine) {
            voiceEngine->setParameter(paramId, value);
        }
    }
}

SynthEngine* SynthEngineDispatcher::getEngine(int engineType) const {
    switch (engineType) {
        case static_cast<int>(EngineTypeId::KARPLUS_STRONG):
            return mKarplusStrong.get();
        case static_cast<int>(EngineTypeId::FM_SYNTH):
            return mFMEngine.get();
        case static_cast<int>(EngineTypeId::SUPERSAW):
            return mSupersawEngine.get();
        case static_cast<int>(EngineTypeId::WAVETABLE):
            return mWavetableEngine.get();
        case static_cast<int>(EngineTypeId::GRANULAR):
            return mGranularEngine.get();
        case static_cast<int>(EngineTypeId::SOUNDFONT):
            return mSoundFontEngine.get();
        default:
            return nullptr;
    }
}

SynthEngine* SynthEngineDispatcher::getDualTouchEngine(int engineType) const {
    switch (engineType) {
        case static_cast<int>(EngineTypeId::KARPLUS_STRONG):
            return mKarplusStrong2.get();
        case static_cast<int>(EngineTypeId::FM_SYNTH):
            return mFMEngine2.get();
        case static_cast<int>(EngineTypeId::SUPERSAW):
            return mSupersawEngine2.get();
        case static_cast<int>(EngineTypeId::WAVETABLE):
            return mWavetableEngine2.get();
        case static_cast<int>(EngineTypeId::GRANULAR):
            return mGranularEngine2.get();
        // SOUNDFONT: no dual-touch engine — bypasses dual-touch in onAudioReady
        default:
            return nullptr;
    }
}

SynthEngine* SynthEngineDispatcher::getVoiceEngine(int engineType, int voiceIndex) const {
    if (voiceIndex < 0 || voiceIndex >= MAX_VOICE_ENGINES) return nullptr;
    switch (engineType) {
        case static_cast<int>(EngineTypeId::KARPLUS_STRONG):
            return mKSPool[voiceIndex].get();
        case static_cast<int>(EngineTypeId::FM_SYNTH):
            return mFMPool[voiceIndex].get();
        case static_cast<int>(EngineTypeId::SUPERSAW):
            return mSupersawPool[voiceIndex].get();
        case static_cast<int>(EngineTypeId::WAVETABLE):
            return mWavetablePool[voiceIndex].get();
        case static_cast<int>(EngineTypeId::GRANULAR):
            return mGranularPool[voiceIndex].get();
        // SOUNDFONT: no voice pool — bypasses VoicePool in onAudioReady
        default:
            return nullptr; // CLASSIC -> Voice uses its own AudioSource oscillators
    }
}

int SynthEngineDispatcher::detectCrossfadeAndGetType() {
    const int cachedEngineType = mCurrentEngineType.load(std::memory_order_acquire);

    if (cachedEngineType != mPrevEngineType) {
        mEngineCrossfadeRemaining = ENGINE_CROSSFADE_SAMPLES;
        mEngineCrossfadeGain = 0.0f; // Start from silence, ramp up
        mPrevEngineType = cachedEngineType;
    }

    return cachedEngineType;
}

void SynthEngineDispatcher::applyCrossfade(float* buffer, int numFrames) {
    if (mEngineCrossfadeRemaining <= 0) return;

    const float rampStep = 1.0f / static_cast<float>(ENGINE_CROSSFADE_SAMPLES);
    for (int i = 0; i < numFrames && mEngineCrossfadeRemaining > 0; ++i) {
        mEngineCrossfadeGain += rampStep;
        if (mEngineCrossfadeGain > 1.0f) mEngineCrossfadeGain = 1.0f;
        buffer[i * 2] *= mEngineCrossfadeGain;
        buffer[i * 2 + 1] *= mEngineCrossfadeGain;
        mEngineCrossfadeRemaining--;
    }
}

void SynthEngineDispatcher::updateVoiceEngines(void* voiceManagerPtr) {
    if (!voiceManagerPtr) return;

    auto* vm = static_cast<voice::VoiceManager*>(voiceManagerPtr);
    int engineType = mCurrentEngineType.load(std::memory_order_acquire);
    auto* pool = vm->getVoicePool();
    if (!pool) return;

    int maxVoices = pool->getTotalVoiceCount();
    for (int i = 0; i < maxVoices && i < MAX_VOICE_ENGINES; ++i) {
        auto* voice = pool->getVoice(i);
        if (voice) {
            // nullptr for CLASSIC (voice uses its own oscillators)
            voice->setEngine(getVoiceEngine(engineType, i));
        }
    }
}

// ========== SOUNDFONT METHODS ==========

bool SynthEngineDispatcher::loadSoundFont(const void* data, int size, int sampleRate) {
    if (!mSoundFontManager) {
        LOGE("loadSoundFont: SoundFontManager not initialized");
        return false;
    }
    return mSoundFontManager->loadFromMemory(data, size, sampleRate);
}

bool SynthEngineDispatcher::loadSoundFontFromPath(const char* path, int sampleRate) {
    if (!mSoundFontManager) {
        LOGE("loadSoundFontFromPath: SoundFontManager not initialized");
        return false;
    }
    return mSoundFontManager->loadFromPath(path, sampleRate);
}

void SynthEngineDispatcher::unloadSoundFont() {
    if (mSoundFontManager) {
        mSoundFontManager->unload();
    }
}

void SynthEngineDispatcher::setSoundFontPreset(int presetIndex) {
    if (mSoundFontEngine) mSoundFontEngine->setPreset(presetIndex);
    LOGI("SoundFont preset set to %d", presetIndex);
}

int SynthEngineDispatcher::getSoundFontPresetCount() const {
    return mSoundFontManager ? mSoundFontManager->getPresetCount() : 0;
}

const char* SynthEngineDispatcher::getSoundFontPresetName(int presetIndex) const {
    return mSoundFontManager ? mSoundFontManager->getPresetName(presetIndex) : nullptr;
}

bool SynthEngineDispatcher::getSoundFontPresetKeyRange(int presetIndex, int& outMinKey, int& outMaxKey) const {
    if (!mSoundFontManager) return false;
    return mSoundFontManager->getPresetKeyRange(presetIndex, outMinKey, outMaxKey);
}

bool SynthEngineDispatcher::getSoundFontPresetBankProgram(int presetIndex, int& outBank, int& outProgram) const {
    if (!mSoundFontManager) return false;
    return mSoundFontManager->getPresetBankProgram(presetIndex, outBank, outProgram);
}

bool SynthEngineDispatcher::isSoundFontLoaded() const {
    return mSoundFontManager && mSoundFontManager->isLoaded();
}

void SynthEngineDispatcher::sfNoteOn(int touchId, int midiNote, float velocity) {
    if (mSoundFontEngine) {
        mSoundFontEngine->noteOn(touchId, midiNote, velocity);
    }
}

void SynthEngineDispatcher::sfNoteOff(int touchId) {
    if (mSoundFontEngine) {
        mSoundFontEngine->noteOff(touchId);
    }
}

void SynthEngineDispatcher::sfNoteOffAll() {
    if (mSoundFontEngine) {
        mSoundFontEngine->noteOffAll();
    }
}

void SynthEngineDispatcher::sfNoteOffAllExcept(int keepTouchId) {
    if (mSoundFontEngine) {
        mSoundFontEngine->noteOffAllExcept(keepTouchId);
    }
}
