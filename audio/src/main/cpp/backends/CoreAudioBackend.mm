/**
 * CoreAudioBackend.mm
 *
 * Objective-C++ implementation of the Apple audio backend (WA-2.4).
 *
 * Signal path (output):
 *
 *     engine DSP callback (interleaved stereo float)
 *        -> AVAudioSourceNode render block  [RT thread]
 *        -> mainMixerNode
 *        -> outputNode  (hardware)
 *
 * Signal path (capture, full duplex):
 *
 *     inputNode (hardware)
 *        -> AVAudioSinkNode receiver block  [capture RT thread]
 *        -> mInputRing (SPSC, lock-free)
 *        -> AVAudioSourceNode render block  [output RT thread]
 *        -> onAudioReady(output, input, frames)
 *
 * Both directions ride the SAME AVAudioEngine, so they share one clock domain
 * and the ring never has to absorb drift — only the phase offset between the two
 * callbacks, which is one buffer.
 *
 * The blocks are thin trampolines into C++: they capture a raw `this` pointer
 * and touch only atomics, pre-allocated scratch and the ring. No allocation, no
 * lock, no ObjC messaging, no logging on the RT path.
 */

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>

#include "CoreAudioBackend.h"
#include "../platform/Logger.h"

#include <algorithm>
#include <cstring>

#define LOG_TAG "CoreAudioBackend"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

#if TARGET_OS_OSX
// =============================================================================
// macOS (REQ-055 S3): dispositivo HAL, buffer negociado, latencia real y el
// diagnostico de `CoreAudioMacDiag.h`. Nada de esto compila para iOS: alli
// AVAudioSession negocia y no existe el HAL de dispositivos.
// =============================================================================
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <atomic>
#include <mutex>
#include "CoreAudioMacDiag.h"

namespace watermelon_audio {
namespace {

constexpr uint32_t kMacTouchCapacity = 8192;

// Estado del diagnostico. Un solo backend vivo por proceso (el motor global).
//
// Reglas de escritura, que son las que hacen que el RT sea lock-free y sin carreras:
// - todo lo `rt*` lo escribe SOLO el thread de render (noteRenderEnter/Exit);
// - `halOverloads` lo escribe el listener del HAL (fetch_add) y lo lee cualquiera;
// - el reset NO pisa contadores desde afuera: sube `resetRequested` y el RT pone
//   todo a cero en su proximo bloque y lo confirma en `resetApplied`;
// - `pendingTouch` lo escribe el thread que toca y lo consume el RT con exchange.
struct MacDiagState {
    std::atomic<uint64_t> rtBlocks{0};
    std::atomic<uint64_t> rtOverBudget{0};
    std::atomic<uint64_t> rtDiscontinuities{0};
    std::atomic<uint64_t> rtSampleTimeInvalid{0};
    std::atomic<uint64_t> rtMaxTicks{0};
    std::atomic<uint64_t> rtSumTicks{0};
    std::atomic<uint64_t> rtLastFrames{0};
    std::atomic<uint64_t> rtMinFrames{0};
    std::atomic<uint64_t> rtMaxFrames{0};
    std::atomic<double>   rtNextSampleTime{-1.0};
    std::atomic<uint32_t> rtTouchCount{0};
    WmaMacTouchSample     rtTouches[kMacTouchCapacity] = {};

    std::atomic<uint64_t> halOverloads{0};
    std::atomic<uint64_t> budgetNumTicks{0};   // ticks mach por frame * 2^20 (punto fijo)
    std::atomic<uint64_t> pendingTouch{0};
    // Toques estampados desde el ultimo reset (thread de control). Si es mayor que los
    // que registro el RT, hubo toques pisados antes de llegar a un bloque: el lector lo
    // tiene que ver, porque los pisados son justo los de latencia mas alta.
    std::atomic<uint64_t> touchMarks{0};
    std::atomic<uint32_t> resetRequested{0};
    std::atomic<uint32_t> resetApplied{0};

    // Datos del dispositivo: los escribe la apertura y los lee el getter, los dos en
    // threads de control. El mutex NUNCA lo toca el RT.
    std::mutex       deviceMutex;
    WmaMacDeviceInfo device = {};
    bool             deviceValid = false;
};

// Nunca destruido (`no_destroy`): el listener del HAL y el RT pueden correr hasta el
// final del proceso, y un estatico destruido en el exit seria un use-after-free. Es un
// objeto de namespace y no un `static` de funcion a proposito: sin guarda de
// inicializacion perezosa en el camino del RT.
[[clang::no_destroy]] MacDiagState gMacDiag;

MacDiagState& macDiag() { return gMacDiag; }

uint64_t macTicksToNs(uint64_t ticks) {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t i{};
        mach_timebase_info(&i);
        return i;
    }();
    return ticks * tb.numer / tb.denom;
}

uint64_t macNsToTicks(double ns) {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t i{};
        mach_timebase_info(&i);
        return i;
    }();
    return static_cast<uint64_t>(ns * tb.denom / tb.numer);
}

OSStatus macGetProp(AudioObjectID obj, AudioObjectPropertySelector sel,
                    AudioObjectPropertyScope scope, UInt32 size, void* out) {
    AudioObjectPropertyAddress a{sel, scope, kAudioObjectPropertyElementMain};
    UInt32 s = size;
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &s, out);
}

AudioObjectID macDefaultOutputDevice() {
    AudioObjectID dev = kAudioObjectUnknown;
    if (macGetProp(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                   kAudioObjectPropertyScopeGlobal, sizeof(dev), &dev) != noErr) {
        return kAudioObjectUnknown;
    }
    return dev;
}

UInt32 macFirstOutputStreamLatency(AudioObjectID dev) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                 kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr ||
        size < sizeof(AudioStreamID)) {
        return 0;
    }
    AudioStreamID streams[16] = {};
    size = std::min<UInt32>(size, sizeof(streams));
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, streams) != noErr) return 0;
    UInt32 lat = 0;
    macGetProp(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal,
               sizeof(lat), &lat);
    return lat;
}

// Lo despacha el HAL en su thread de notificaciones, NO en el de IO. Igual queda en
// un fetch_add: no hay nada que valga la pena hacer aca que no pueda hacer el lector.
OSStatus macOverloadListener(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*) {
    macDiag().halOverloads.fetch_add(1, std::memory_order_relaxed);
    return noErr;
}

const AudioObjectPropertyAddress kMacOverloadAddress{
    kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain};

// El device con listener registrado, para sacarlo en el close. Control thread only.
AudioObjectID gMacListenedDevice = kAudioObjectUnknown;

}  // namespace

// Las dos mitades RT del diagnostico. Son raices declaradas de
// scripts/check-rt-safety.py: el render block es un bloque de ObjC que el walker no ve,
// asi que lo que corre adentro y es nuevo vive en funciones con nombre.
struct CoreAudioMacDiag {
    static uint64_t noteRenderEnter(const AudioTimeStamp* ts, uint32_t frames);
    static void noteRenderExit(uint64_t entryTicks, uint32_t frames);
};

uint64_t CoreAudioMacDiag::noteRenderEnter(const AudioTimeStamp* ts, uint32_t frames) {
    MacDiagState& d = macDiag();
    const uint64_t entry = mach_absolute_time();

    const uint32_t wanted = d.resetRequested.load(std::memory_order_acquire);
    if (wanted != d.resetApplied.load(std::memory_order_relaxed)) {
        d.rtBlocks.store(0, std::memory_order_relaxed);
        d.rtOverBudget.store(0, std::memory_order_relaxed);
        d.rtDiscontinuities.store(0, std::memory_order_relaxed);
        d.rtSampleTimeInvalid.store(0, std::memory_order_relaxed);
        d.rtMaxTicks.store(0, std::memory_order_relaxed);
        d.rtSumTicks.store(0, std::memory_order_relaxed);
        d.rtMinFrames.store(0, std::memory_order_relaxed);
        d.rtMaxFrames.store(0, std::memory_order_relaxed);
        d.rtTouchCount.store(0, std::memory_order_relaxed);
        d.resetApplied.store(wanted, std::memory_order_release);
    }

    if (ts != nullptr && (ts->mFlags & kAudioTimeStampSampleTimeValid)) {
        const double expected = d.rtNextSampleTime.load(std::memory_order_relaxed);
        if (expected >= 0.0 && ts->mSampleTime != expected) {
            d.rtDiscontinuities.fetch_add(1, std::memory_order_relaxed);
        }
        d.rtNextSampleTime.store(ts->mSampleTime + frames, std::memory_order_relaxed);
    } else {
        d.rtSampleTimeInvalid.fetch_add(1, std::memory_order_relaxed);
    }

    const uint64_t touch = d.pendingTouch.exchange(0, std::memory_order_acq_rel);
    if (touch != 0) {
        const uint32_t n = d.rtTouchCount.load(std::memory_order_relaxed);
        if (n < kMacTouchCapacity) {
            WmaMacTouchSample& s = d.rtTouches[n];
            s.touchNs       = touch;   // en ticks; el getter convierte
            s.renderEntryNs = entry;
            s.bufferHostNs  = (ts != nullptr && (ts->mFlags & kAudioTimeStampHostTimeValid))
                                  ? ts->mHostTime : 0;
            s.frames        = frames;
            d.rtTouchCount.store(n + 1, std::memory_order_release);
        }
    }
    return entry;
}

void CoreAudioMacDiag::noteRenderExit(uint64_t entryTicks, uint32_t frames) {
    MacDiagState& d = macDiag();
    const uint64_t dt = mach_absolute_time() - entryTicks;
    d.rtBlocks.fetch_add(1, std::memory_order_relaxed);
    d.rtSumTicks.fetch_add(dt, std::memory_order_relaxed);
    if (dt > d.rtMaxTicks.load(std::memory_order_relaxed)) {
        d.rtMaxTicks.store(dt, std::memory_order_relaxed);
    }
    const uint64_t budget = (d.budgetNumTicks.load(std::memory_order_relaxed) * frames) >> 20;
    if (budget > 0 && dt > budget) {
        d.rtOverBudget.fetch_add(1, std::memory_order_relaxed);
    }
    d.rtLastFrames.store(frames, std::memory_order_relaxed);
    const uint64_t mn = d.rtMinFrames.load(std::memory_order_relaxed);
    if (mn == 0 || frames < mn) d.rtMinFrames.store(frames, std::memory_order_relaxed);
    if (frames > d.rtMaxFrames.load(std::memory_order_relaxed)) {
        d.rtMaxFrames.store(frames, std::memory_order_relaxed);
    }
}

}  // namespace watermelon_audio

extern "C" {

int wma_macos_diag_render_stats(WmaMacRenderStats* out) {
    if (out == nullptr) return 0;
    auto& d = watermelon_audio::macDiag();
    out->blocks            = d.rtBlocks.load(std::memory_order_relaxed);
    out->overBudget        = d.rtOverBudget.load(std::memory_order_relaxed);
    out->discontinuities   = d.rtDiscontinuities.load(std::memory_order_relaxed);
    out->sampleTimeInvalid = d.rtSampleTimeInvalid.load(std::memory_order_relaxed);
    out->halOverloads      = d.halOverloads.load(std::memory_order_relaxed);
    out->maxRenderNs       = watermelon_audio::macTicksToNs(d.rtMaxTicks.load(std::memory_order_relaxed));
    out->sumRenderNs       = watermelon_audio::macTicksToNs(d.rtSumTicks.load(std::memory_order_relaxed));
    out->lastFrames        = d.rtLastFrames.load(std::memory_order_relaxed);
    out->minFrames         = d.rtMinFrames.load(std::memory_order_relaxed);
    out->maxFrames         = d.rtMaxFrames.load(std::memory_order_relaxed);
    out->touchMarks        = d.touchMarks.load(std::memory_order_relaxed);
    return 1;
}

int wma_macos_diag_device_info(WmaMacDeviceInfo* out) {
    if (out == nullptr) return 0;
    auto& d = watermelon_audio::macDiag();
    std::lock_guard<std::mutex> lock(d.deviceMutex);
    if (!d.deviceValid) return 0;
    *out = d.device;
    return 1;
}

void wma_macos_diag_reset(void) {
    auto& d = watermelon_audio::macDiag();
    d.halOverloads.store(0, std::memory_order_relaxed);
    d.touchMarks.store(0, std::memory_order_relaxed);
    d.resetRequested.fetch_add(1, std::memory_order_acq_rel);
}

void wma_macos_diag_mark_touch(void) {
    auto& d = watermelon_audio::macDiag();
    d.touchMarks.fetch_add(1, std::memory_order_relaxed);
    d.pendingTouch.store(mach_absolute_time(), std::memory_order_release);
}

int wma_macos_diag_touch_samples(WmaMacTouchSample* out, int max) {
    auto& d = watermelon_audio::macDiag();
    if (d.resetApplied.load(std::memory_order_acquire) !=
        d.resetRequested.load(std::memory_order_acquire)) {
        return -1;
    }
    const uint32_t n = d.rtTouchCount.load(std::memory_order_acquire);
    int count = 0;
    for (uint32_t i = 0; i < n && count < max; ++i, ++count) {
        const WmaMacTouchSample& s = d.rtTouches[i];
        out[count].touchNs       = watermelon_audio::macTicksToNs(s.touchNs);
        out[count].renderEntryNs = watermelon_audio::macTicksToNs(s.renderEntryNs);
        out[count].bufferHostNs  = s.bufferHostNs ? watermelon_audio::macTicksToNs(s.bufferHostNs) : 0;
        out[count].frames        = s.frames;
    }
    return count;
}

uint64_t wma_macos_diag_now_ns(void) {
    return watermelon_audio::macTicksToNs(mach_absolute_time());
}

}  // extern "C"
#endif  // TARGET_OS_OSX

namespace watermelon_audio {

// =============================================================================
// PIMPL — holds the Objective-C engine objects. Under ARC these members are
// strong references; resetting mImpl (or nil-ing them in closeEngineLocked)
// releases the engine.
// =============================================================================
struct CoreAudioBackend::Impl {
    AVAudioEngine*     engine     = nil;
    AVAudioSourceNode* sourceNode = nil;
    AVAudioSinkNode*   sinkNode   = nil;
    AVAudioFormat*     format     = nil;
};

// =============================================================================
// Constructor / Destructor
// =============================================================================

CoreAudioBackend::CoreAudioBackend()
    : mImpl(std::make_unique<Impl>())
    // Placeholder capacity; openEngineLocked() resizes to one second at the
    // negotiated rate. Constructing at zero would make resize() the only thing
    // standing between a mis-sequenced start() and a division by zero.
    , mInputRing(48000 * 2) {
    LOGI("CoreAudioBackend created");
}

CoreAudioBackend::~CoreAudioBackend() {
    if (mIsRunning.load()) {
        stop();
    }
    LOGI("CoreAudioBackend destroyed");
}

// =============================================================================
// Configuration
// =============================================================================

void CoreAudioBackend::setCallback(IAudioCallback* callback) {
    mCallback.store(callback, std::memory_order_release);
}

void CoreAudioBackend::setSampleRate(int sampleRate) {
    mRequestedSampleRate = sampleRate;
}

void CoreAudioBackend::setBufferSize(int framesPerBuffer) {
    mRequestedBufferSize = framesPerBuffer;
}

void CoreAudioBackend::setFullDuplexEnabled(bool enable) {
    // Two effects, on purpose:
    //
    // 1. The request itself, applied on the next start() — same contract as
    //    OboeBackend, where this is also just a flag read at stream-open time.
    //    Whether to open a capture stream at all has to be decided before the
    //    session is configured: an app that never wants input should never ask
    //    the user for the microphone, and playAndRecord changes output routing.
    //
    // 2. Live gating of delivery when capture is ALREADY running. Mode changes
    //    call this while the engine streams (setFullDuplexEnabled(mode == 2)),
    //    and detaching the sink node at runtime would mean reconfiguring the
    //    audio session — an audible glitch on the output path. So capture keeps
    //    running and only the handoff to the callback flips.
    mFullDuplexRequested.store(enable, std::memory_order_release);
    mDeliverInput.store(enable, std::memory_order_release);

    if (enable && !mCaptureActive.load(std::memory_order_acquire) &&
        mIsRunning.load(std::memory_order_acquire)) {
        LOGW("Full duplex requested while running without a capture stream — "
             "takes effect on the next start()");
    }
}

float CoreAudioBackend::getInputLatencyMs() const {
    return mInputLatencyMs.load(std::memory_order_acquire);
}

// =============================================================================
// Lifecycle
// =============================================================================

BackendResult CoreAudioBackend::start() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (mIsRunning.load()) {
        LOGW("Already running");
        return BackendResult::ERROR_ALREADY_RUNNING;
    }
    if (mCallback.load(std::memory_order_acquire) == nullptr) {
        LOGE("No callback set");
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    // Drop the previous stream's facts BEFORE opening, never after:
    // openEngineLocked() publishes the negotiated ones on its way out, and
    // invalidating them afterwards throws away exactly what it just measured.
    // getStreamInfo() would then fall back to reporting the *request*, and
    // "requested" and "negotiated" disagreeing is the whole reason this cache
    // exists — it is what told BackendManager::requestCapture() that capture
    // was already live on a stream that had none, so the reopen never happened.
    {
        std::lock_guard<std::mutex> lock(mStreamInfoMutex);
        mStreamInfoValid.store(false);
        mCachedStreamInfo = StreamInfo{};
    }

    BackendResult result = openEngineLocked();
    if (result != BackendResult::OK) {
        closeEngineLocked();
        return result;
    }

    mStopping.store(false);
    mIsPaused.store(false);
    mIsRunning.store(true);

    LOGI("CoreAudioBackend started");
    return BackendResult::OK;
}

void CoreAudioBackend::stop() {
    std::unique_lock<std::mutex> lock(mStreamMutex);

    if (!mIsRunning.load()) {
        return;
    }

    LOGI("Stopping CoreAudioBackend...");

    // Signal the render block to stop producing and to wake us when it drains,
    // then stop the engine so no new callbacks are scheduled.
    mStopping.store(true);

    if (mImpl->engine != nil) {
        [mImpl->engine stop];
    }

    // Wait for any in-flight render callback to finish before tearing down.
    const auto timeout = std::chrono::milliseconds(1000);
    const bool finished = mStopCondition.wait_for(lock, timeout, [this] {
        return mActiveCallbacks.load() == 0;
    });
    if (!finished) {
        LOGW("Timeout waiting for render callbacks to drain");
    }

    closeEngineLocked();

    mIsRunning.store(false);
    mIsPaused.store(false);
    {
        std::lock_guard<std::mutex> lock(mStreamInfoMutex);
        mStreamInfoValid.store(false);
        mCachedStreamInfo = StreamInfo{};
    }

    LOGI("CoreAudioBackend stopped");
}

void CoreAudioBackend::pause() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (!mIsRunning.load() || mIsPaused.load()) {
        return;
    }

    // Flip the flag first so any callback already scheduled emits silence, then
    // pause the engine (keeps the graph resident for a fast resume()).
    mIsPaused.store(true);
    if (mImpl->engine != nil) {
        [mImpl->engine pause];
    }
    LOGI("CoreAudioBackend paused");
}

void CoreAudioBackend::resume() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (!mIsRunning.load() || !mIsPaused.load()) {
        return;
    }

    if (mImpl->engine != nil) {
        NSError* err = nil;
        if (![mImpl->engine startAndReturnError:&err]) {
            LOGE("Failed to resume engine: %s",
                 err ? err.localizedDescription.UTF8String : "unknown");
            return;
        }
    }
    mIsPaused.store(false);
    LOGI("CoreAudioBackend resumed");
}

// =============================================================================
// State Queries
// =============================================================================

bool CoreAudioBackend::isRunning() const {
    return mIsRunning.load() && !mIsPaused.load();
}

float CoreAudioBackend::getOutputLatencyMs() const {
    return mOutputLatencyMs.load(std::memory_order_acquire);
}

// Contrato de IAudioBackend: seguro contra un start()/stop() concurrente. El
// lock es un copy de struct y no envuelve ninguna llamada que pueda bloquear.
// Acá el cache ya se publicaba al abrir el stream —no perezosamente, como hacía
// Oboe— así que lo único que faltaba era sincronizarlo. Sin eso, el poller de la
// UI leía `mCachedStreamInfo` mientras la reapertura lo reescribía: es la carrera
// que el TSan del CI encontró el 2026-08-12, del lado del backend real.
StreamInfo CoreAudioBackend::getStreamInfo() const {
    {
        std::lock_guard<std::mutex> lock(mStreamInfoMutex);
        if (mStreamInfoValid.load()) {
            return mCachedStreamInfo;
        }
    }
    // Before start() (or after stop()) report the requested configuration.
    StreamInfo info;
    info.sampleRate      = mRequestedSampleRate > 0 ? mRequestedSampleRate : 48000;
    info.channelCount    = 2;
    // Sin stream abierto no hay modo que reportar (ver el comentario de start()).
    info.lowLatency      = StreamInfo::LowLatency::UNKNOWN;
    info.framesPerBuffer = mRequestedBufferSize > 0 ? mRequestedBufferSize : 256;
    info.format          = AudioFormat::FLOAT_32;
    info.outputLatencyMs = 0.0f;
    // Before start() this is the *request*, not a fact — whether a capture stream
    // actually opens depends on microphone access, which is only known at start().
    info.isFullDuplex    = mFullDuplexRequested.load(std::memory_order_acquire);
    info.backendType     = BackendType::COREAUDIO;
    info.deviceName      = "Core Audio";
    return info;
}

// =============================================================================
// Engine setup / teardown (called with mStreamMutex held)
// =============================================================================

BackendResult CoreAudioBackend::openEngineLocked() {
    // ---- 1. Negotiate the session (iOS only; macOS has no AVAudioSession) ----
    const bool wantCapture = mFullDuplexRequested.load(std::memory_order_acquire);

    double negotiatedSampleRate = mRequestedSampleRate > 0 ? mRequestedSampleRate : 48000;
    double ioBufferDuration     = 0.0;   // seconds
    float  sessionOutputLatency = 0.0f;  // seconds
    float  sessionInputLatency  = 0.0f;  // seconds

#if TARGET_OS_IOS
    {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* err = nil;

        // playAndRecord only when capture was actually asked for: it prompts the
        // user for the microphone and, without defaultToSpeaker, routes output to
        // the receiver instead of the speaker. An output-only app should pay
        // neither price.
        //
        // Note the overlap with AudioSessionManager (WA-3.4, Kotlin): the host app
        // may also configure the session. The options below mirror it so whichever
        // runs last leaves the same category — but the backend never *downgrades*
        // a recording session to playback, which is the case that would silently
        // kill capture.
        BOOL categoryOk;
        if (wantCapture) {
            categoryOk = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                  withOptions:(AVAudioSessionCategoryOptionDefaultToSpeaker |
                                               AVAudioSessionCategoryOptionAllowBluetoothA2DP)
                                        error:&err];
        } else {
            categoryOk = [session setCategory:AVAudioSessionCategoryPlayback error:&err];
        }
        if (!categoryOk) {
            LOGW("setCategory failed: %s",
                 err ? err.localizedDescription.UTF8String : "unknown");
            err = nil;
        }

        if (mRequestedSampleRate > 0) {
            [session setPreferredSampleRate:(double)mRequestedSampleRate error:&err];
            err = nil;
        }
        if (mRequestedBufferSize > 0 && negotiatedSampleRate > 0) {
            const double dur = (double)mRequestedBufferSize / negotiatedSampleRate;
            [session setPreferredIOBufferDuration:dur error:&err];
            err = nil;
        }

        if (![session setActive:YES error:&err]) {
            LOGE("Failed to activate audio session: %s",
                 err ? err.localizedDescription.UTF8String : "unknown");
            return BackendResult::ERROR_STREAM_FAILED;
        }

        // Read back the values the system actually negotiated — these can differ
        // from what we asked for.
        negotiatedSampleRate = session.sampleRate;
        ioBufferDuration     = session.IOBufferDuration;
        sessionOutputLatency = (float)session.outputLatency;
        sessionInputLatency  = (float)session.inputLatency;
    }
#endif // TARGET_OS_IOS

#if TARGET_OS_OSX
    // macOS no tiene AVAudioSession: se negocia contra el dispositivo del HAL que va a
    // usar el outputNode (el de salida por defecto). Hallazgos 1-3 de MINI-028:
    //  1. el motor abre a la tasa NOMINAL del dispositivo, no a 48 kHz fijos — si no,
    //     el mixer de AVAudioEngine remuestrea y los bloques dejan de ser del buffer
    //     pedido (278 frames en vez de 256 a 44,1 kHz). AudioEngine re-prepara su DSP
    //     con la tasa que publique el stream (AudioEngine.cpp, tras manager.start()).
    //     No se cambia la tasa del dispositivo: es del sistema y la comparten otras apps;
    //  2. la latencia se lee del dispositivo (abajo, paso 6b);
    //  3. el buffer lo fija el backend (paso 5c), acotado al rango del dispositivo.
    const AudioObjectID macDevice = macDefaultOutputDevice();
    UInt32 macBufferTarget = static_cast<UInt32>(mRequestedBufferSize > 0 ? mRequestedBufferSize : 256);
    AudioValueRange macBufferRange{0, 0};
    {
        Float64 nominal = 0;
        if (macDevice != kAudioObjectUnknown &&
            macGetProp(macDevice, kAudioDevicePropertyNominalSampleRate,
                       kAudioObjectPropertyScopeGlobal, sizeof(nominal), &nominal) == noErr &&
            nominal > 0) {
            if (mRequestedSampleRate > 0 && std::lround(nominal) != mRequestedSampleRate) {
                LOGI("macOS: el dispositivo corre a %.0f Hz (pedido %d): se abre a la del dispositivo",
                     nominal, mRequestedSampleRate);
            }
            negotiatedSampleRate = nominal;
        } else {
            LOGW("macOS: no pude leer la tasa nominal del dispositivo %u", (unsigned)macDevice);
        }
        if (macDevice != kAudioObjectUnknown &&
            macGetProp(macDevice, kAudioDevicePropertyBufferFrameSizeRange,
                       kAudioObjectPropertyScopeGlobal, sizeof(macBufferRange), &macBufferRange) == noErr &&
            macBufferRange.mMaximum > 0) {
            macBufferTarget = static_cast<UInt32>(std::clamp<double>(
                macBufferTarget, macBufferRange.mMinimum, macBufferRange.mMaximum));
        }
    }
#endif // TARGET_OS_OSX

    if (negotiatedSampleRate <= 0) {
        negotiatedSampleRate = 48000;
    }

    // Derive the frames-per-buffer actually in effect.
    int negotiatedFrames = mRequestedBufferSize > 0 ? mRequestedBufferSize : 256;
    if (ioBufferDuration > 0.0) {
        negotiatedFrames = (int)std::lround(ioBufferDuration * negotiatedSampleRate);
    }
    if (negotiatedFrames <= 0) {
        negotiatedFrames = 256;
    }

    // ---- 2. Pre-allocate the deinterleave scratch (never touched at RT) ----
    // Size generously so the render block's frameCount can never exceed it; the
    // block also clamps defensively.
    mMaxFrames = std::max({mRequestedBufferSize, negotiatedFrames, 4096});
    mScratchInterleaved.assign(static_cast<size_t>(mMaxFrames) * 2, 0.0f);

    // Capture buffers. One scratch per RT thread — the sink and render blocks run
    // on different CoreAudio threads and must never share a scratch. The ring is
    // one second of stereo, far more than the phase offset between the two
    // callbacks needs; the slack is what absorbs a late capture block instead of
    // turning it into a dropout.
    mCaptureScratch.assign(static_cast<size_t>(mMaxFrames) * 2, 0.0f);
    mInputScratch.assign(static_cast<size_t>(mMaxFrames) * 2, 0.0f);
    mInputRing.resize(static_cast<size_t>(std::lround(negotiatedSampleRate)) * 2);
    mCapturePrimed.store(false, std::memory_order_release);
    mCaptureActive.store(false, std::memory_order_release);

    // ---- 3. Build the stereo float format for the graph connection ----
    //
    // NO interleaveado, y se decidió con evidencia.
    //
    // La versión original pedía `interleaved:YES`, y el render block traía las
    // dos ramas —una por layout de ABL— con el comentario "the engine honored
    // our format" en la interleaveada. Qué rama toma el OS estaba anotado como
    // pregunta de WA-4.3, para contestar en device.
    //
    // La contestó el harness de WA-5.5 en el simulador, y la respuesta es que no
    // toma ninguna: `[engine connect:sourceNode to:mixer format:format]` con un
    // formato interleaveado **tira NSException con -10868** (format not
    // supported) antes de renderizar un solo bloque. Los nodos de AVAudioEngine
    // se conectan entre sí en float deinterleaveado.
    //
    // Con esto la conexión pasa y el render entra por la rama planar, que ya
    // estaba escrita y con su scratch pre-alocado.
    //
    // Lo que esto NO verifica: cómo SUENA. Que el grafo conecte y el stream abra
    // se comprobó en el simulador; la calidad por la rama planar necesita oídos
    // y, para latencia, un device (WA-4.3 / G2).
    AVAudioFormat* format =
        [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                         sampleRate:negotiatedSampleRate
                                           channels:2
                                        interleaved:NO];
    if (format == nil) {
        LOGE("Failed to build AVAudioFormat");
        return BackendResult::ERROR_INVALID_CONFIG;
    }
    mImpl->format = format;

    // ---- 4. The render block: a raw trampoline into the C++ mixer ----
    CoreAudioBackend* backend = this;  // raw capture, no ObjC retain

    AVAudioSourceNodeRenderBlock renderBlock =
        ^OSStatus(BOOL* isSilence,
                  const AudioTimeStamp* timestamp,
                  AVAudioFrameCount frameCount,
                  AudioBufferList* outputData) {

        backend->mActiveCallbacks.fetch_add(1, std::memory_order_acquire);
#if !TARGET_OS_OSX
        (void)timestamp;
#endif

        const UInt32 numBuffers = outputData->mNumberBuffers;
        IAudioCallback* cb = backend->mCallback.load(std::memory_order_acquire);

        // Paused or no callback: emit silence.
        if (cb == nullptr || backend->mIsPaused.load(std::memory_order_acquire)) {
            for (UInt32 b = 0; b < numBuffers; ++b) {
                std::memset(outputData->mBuffers[b].mData, 0,
                            outputData->mBuffers[b].mDataByteSize);
            }
            *isSilence = YES;
            if (backend->mActiveCallbacks.fetch_sub(1, std::memory_order_release) == 1 &&
                backend->mStopping.load(std::memory_order_acquire)) {
                backend->mStopCondition.notify_all();
            }
            return noErr;
        }

        int frames = static_cast<int>(frameCount);
        if (frames > backend->mMaxFrames) {
            frames = backend->mMaxFrames;  // defensive clamp; should never trip
        }
#if TARGET_OS_OSX
        // REQ-055 S3: atomics y el reloj mach, nada mas (CoreAudioMacDiag.h).
        const uint64_t macEntryTicks =
            CoreAudioMacDiag::noteRenderEnter(timestamp, static_cast<uint32_t>(frames));
#endif

        // ---- Pull one block of captured audio, if there is one ----
        // A null inputData means "no input this block", which AudioEngine reads
        // as "not in INPUT_FX mode". So this must be stable, not per-block noise:
        // the ring has to hold a full block once (primed) before delivery starts,
        // and from then on an underrun yields silence rather than a null.
        const float* inputData = nullptr;
        if (backend->mDeliverInput.load(std::memory_order_acquire) &&
            backend->mCaptureActive.load(std::memory_order_acquire)) {

            const size_t needed = static_cast<size_t>(frames) * 2;

            if (!backend->mCapturePrimed.load(std::memory_order_acquire)) {
                if (backend->mInputRing.availableToRead() >= needed) {
                    backend->mCapturePrimed.store(true, std::memory_order_release);
                }
            }

            if (backend->mCapturePrimed.load(std::memory_order_acquire)) {
                // read() fills with silence and returns false on underrun, which
                // is exactly the wanted behaviour — keep the mode, drop audio.
                //
                // REQ-009 S3 (3.3b): pero para el AFINADOR ese silencio no es
                // inofensivo. El stream queda DILATADO —se entrega un bloque de
                // ceros de mas y el audio de fuente sigue esperando su turno— y
                // eso corre la referencia de fase. S1 lo midio en 0,40 cents con
                // el motor diciendo CONVERGIDO.
                //
                // `framesQueuedAhead` = 0: lo detecta el propio callback de
                // salida y el hueco es ESTE bloque, aca mismo. El overrun es el
                // que necesita distancia, no este.
                if (!backend->mInputRing.read(backend->mInputScratch.data(), needed)) {
                    backend->mCaptureGap.note(0);
                }
                inputData = backend->mInputScratch.data();
            }
        }

        // REQ-009 S3 (3.3b/3.4b) — ANTES de entregar el bloque, y en el thread de
        // SALIDA, que es el que escribe el ring del afinador. Levanta lo que el
        // callback de entrada haya dejado en el buzon (overrun) y lo que este
        // mismo callback acabe de dejar (underrun), y lo entrega ya en las
        // coordenadas correctas. Estamparlo desde el thread de entrada lo
        // pondria en un instante que no corresponde al lugar del hueco.
        {
            const uint64_t gap = backend->mCaptureGap.take();
            if (gap != wma::backends::CaptureGapMailbox::kEmpty) {
                cb->onCaptureDiscontinuity(gap);
            }
        }

        if (numBuffers == 1) {
            // Interleaved: the engine honored our format — hand the buffer
            // straight to the mixer, zero conversion.
            float* out = static_cast<float*>(outputData->mBuffers[0].mData);
            cb->onAudioReady(out, inputData, frames);
        } else {
            // Deinterleaved fallback: mix into the pre-allocated interleaved
            // scratch, then split into the L/R planar buffers.
            float* scratch = backend->mScratchInterleaved.data();
            cb->onAudioReady(scratch, inputData, frames);

            float* left  = static_cast<float*>(outputData->mBuffers[0].mData);
            float* right = static_cast<float*>(outputData->mBuffers[1].mData);
            for (int i = 0; i < frames; ++i) {
                left[i]  = scratch[2 * i];
                right[i] = scratch[2 * i + 1];
            }
        }

        *isSilence = NO;
#if TARGET_OS_OSX
        CoreAudioMacDiag::noteRenderExit(macEntryTicks, static_cast<uint32_t>(frames));
#endif

        if (backend->mActiveCallbacks.fetch_sub(1, std::memory_order_release) == 1 &&
            backend->mStopping.load(std::memory_order_acquire)) {
            backend->mStopCondition.notify_all();
        }
        return noErr;
    };

    // ---- 5. Wire up the engine graph ----
    AVAudioEngine* engine = [[AVAudioEngine alloc] init];
    AVAudioSourceNode* sourceNode =
        [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:renderBlock];

    mImpl->engine     = engine;
    mImpl->sourceNode = sourceNode;

    // The @try here is the same lesson as the capture branch below, learned the
    // hard way: these three calls raise NSExceptions, they do not return errors.
    // An ObjC exception crossing back into Kotlin/Native hits its terminate
    // handler and **takes the process down** — no error, no false, a SIGABRT.
    //
    // That is exactly what happened the first time the WA-5.5 harness got this
    // far: `connect:` threw `-10868` (format not supported) and the app died
    // mid-tap. The capture branch had a guard since the input path was written;
    // the output branch never did, because until now nothing had ever reached it
    // on iOS.
    @try {
        [engine attachNode:sourceNode];
        // Touching mainMixerNode instantiates it and lazily connects it to
        // outputNode. Connect the source into it with our format.
        AVAudioMixerNode* mixer = engine.mainMixerNode;
        [engine connect:sourceNode to:mixer format:format];
    } @catch (NSException* e) {
        LOGE("Failed to wire the output graph: %s — %s",
             e.name ? e.name.UTF8String : "unknown",
             e.reason ? e.reason.UTF8String : "unknown");
        mImpl->engine     = nil;
        mImpl->sourceNode = nil;
        return BackendResult::ERROR_STREAM_FAILED;
    }

    // ---- 5b. Capture branch (full duplex) ----
    // Failure here is never fatal: an app whose user denied the microphone, or a
    // simulator with no input device, must still get its output. Capture stays
    // off and isCaptureActive() reports the truth.
    //
    // The @try is not defensive padding. `engine.inputNode` and `connect:` report
    // "there is no usable input" by RAISING, not by returning an error — so
    // without it, a user who denied the microphone would take the whole process
    // down instead of losing capture. Nothing inside allocates C++ state that a
    // raise would leak: the sink node is ARC-managed and the flags are set last.
    if (wantCapture) @try {
        AVAudioInputNode* input = engine.inputNode;
        AVAudioFormat* inputFormat = [input inputFormatForBus:0];

        const double inRate     = inputFormat.sampleRate;
        const AVAudioChannelCount inChannels = inputFormat.channelCount;

        if (inRate <= 0.0 || inChannels == 0) {
            // The documented tell for "there is no usable input device": the
            // hardware format comes back all zeros rather than an error.
            LOGW("Capture requested but the input format is empty "
                 "(rate=%.0f, channels=%u) — no microphone access. Output only.",
                 inRate, (unsigned)inChannels);
        } else {
            const int captureChannels = (int)inChannels;

            AVAudioSinkNodeReceiverBlock captureBlock =
                ^OSStatus(const AudioTimeStamp* /*timestamp*/,
                          AVAudioFrameCount frameCount,
                          const AudioBufferList* inputBuffers) {

                backend->mActiveCallbacks.fetch_add(1, std::memory_order_acquire);

                int frames = static_cast<int>(frameCount);
                if (frames > backend->mMaxFrames) {
                    frames = backend->mMaxFrames;  // defensive; matches the scratch
                }

                if (frames > 0 && !backend->mStopping.load(std::memory_order_acquire)) {
                    float* scratch = backend->mCaptureScratch.data();
                    const UInt32 inBuffers = inputBuffers->mNumberBuffers;

                    // Normalise whatever the OS hands us to interleaved stereo:
                    // that is the only shape the rest of the engine speaks. The
                    // built-in iPhone mic is mono, so the duplicate path is the
                    // common one, not the exotic one.
                    if (inBuffers == 1 && captureChannels == 1) {
                        const float* src =
                            static_cast<const float*>(inputBuffers->mBuffers[0].mData);
                        for (int i = 0; i < frames; ++i) {
                            scratch[2 * i]     = src[i];
                            scratch[2 * i + 1] = src[i];
                        }
                    } else if (inBuffers == 1) {
                        // Interleaved with >= 2 channels: take the first two.
                        const float* src =
                            static_cast<const float*>(inputBuffers->mBuffers[0].mData);
                        for (int i = 0; i < frames; ++i) {
                            scratch[2 * i]     = src[i * captureChannels];
                            scratch[2 * i + 1] = src[i * captureChannels + 1];
                        }
                    } else {
                        // Planar: one buffer per channel. Mono planar duplicates.
                        const float* left =
                            static_cast<const float*>(inputBuffers->mBuffers[0].mData);
                        const float* right = (inBuffers >= 2)
                            ? static_cast<const float*>(inputBuffers->mBuffers[1].mData)
                            : left;
                        for (int i = 0; i < frames; ++i) {
                            scratch[2 * i]     = left[i];
                            scratch[2 * i + 1] = right[i];
                        }
                    }

                    // Overflow drops the block instead of blocking. The reader is
                    // the output RT thread; making the capture thread wait for it
                    // would trade a dropout for an xrun on both paths.
                    //
                    // REQ-009 S3 (3.4b): y ese bloque descartado es un hueco que
                    // el afinador tiene que cruzar sin declararlo convergido. Se
                    // deja en el buzon la PROFUNDIDAD DE LA COLA, no la posicion:
                    // este es el thread de ENTRADA y no puede posicionar nada.
                    //
                    // 🔴 Y la profundidad no es cosmetica. Un write() que falla
                    // significa que `mInputRing` esta LLENO —un segundo entero de
                    // estereo— asi que el hueco no se va a entregar hasta ~48000
                    // frames mas tarde. Avisarlo sin esa distancia deja la costura
                    // adelantada 5,9x la capacidad del ring del afinador, el
                    // lector se pone al dia antes del salto real, y el motor
                    // publica CONVERGIDO sobre una lectura equivocada. Hallazgo F.
                    if (!backend->mInputRing.write(scratch,
                                                   static_cast<size_t>(frames) * 2)) {
                        backend->mCaptureGap.note(
                            backend->mInputRing.availableToRead() / 2);
                    }
                }

                if (backend->mActiveCallbacks.fetch_sub(1, std::memory_order_release) == 1 &&
                    backend->mStopping.load(std::memory_order_acquire)) {
                    backend->mStopCondition.notify_all();
                }
                return noErr;
            };

            AVAudioSinkNode* sinkNode =
                [[AVAudioSinkNode alloc] initWithReceiverBlock:captureBlock];
            mImpl->sinkNode = sinkNode;

            [engine attachNode:sinkNode];
            // Connect with the hardware's own format — forcing ours here is what
            // makes AVAudioEngine throw on a mic that is 44.1 kHz mono.
            [engine connect:input to:sinkNode format:inputFormat];

            mCaptureActive.store(true, std::memory_order_release);
            LOGI("Capture attached: %.0f Hz, %u ch", inRate, (unsigned)inChannels);
        }
    } @catch (NSException* e) {
        LOGW("Capture setup raised (%s) — continuing output-only",
             e.reason ? e.reason.UTF8String : "unknown");
        mImpl->sinkNode = nil;
        mCaptureActive.store(false, std::memory_order_release);
    }

#if TARGET_OS_OSX
    // ---- 5c. macOS: el buffer del HAL lo fija el backend (hallazgo 3) ----
    // Sobre la AUHAL del outputNode y antes de prepare/start: es una propiedad POR
    // PROCESO del dispositivo, no toca a las otras apps. Se relee despues de start().
    OSStatus macBufferStatus = noErr;
    AudioObjectID macAuDevice = macDevice;
    {
        AudioUnit au = engine.outputNode.audioUnit;
        if (au != nullptr) {
            UInt32 size = sizeof(macAuDevice);
            const OSStatus devStatus = AudioUnitGetProperty(
                au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                &macAuDevice, &size);
            if (devStatus != noErr) {
                LOGW("macOS: no pude leer el dispositivo de la AUHAL (OSStatus %d)", (int)devStatus);
                macAuDevice = macDevice;
            } else if (macAuDevice != macDevice) {
                // La tasa y el rango se negociaron contra el de salida por defecto; si la
                // AUHAL quedo en otro (cambio de dispositivo durante el open) se dice, y el
                // paso 6b publica la tasa nominal del que de verdad suena.
                LOGW("macOS: la AUHAL usa el dispositivo %u y se negocio contra %u",
                     (unsigned)macAuDevice, (unsigned)macDevice);
            }
            macBufferStatus = AudioUnitSetProperty(au, kAudioDevicePropertyBufferFrameSize,
                                                   kAudioUnitScope_Global, 0, &macBufferTarget,
                                                   sizeof(macBufferTarget));
        } else {
            macBufferStatus = kAudioUnitErr_Uninitialized;
        }
        if (macBufferStatus != noErr) {
            LOGW("macOS: no pude fijar el buffer en %u frames (OSStatus %d)",
                 (unsigned)macBufferTarget, (int)macBufferStatus);
        }
    }
#endif // TARGET_OS_OSX

    // ---- 6. Start ----
    [engine prepare];
    NSError* startErr = nil;
    if (![engine startAndReturnError:&startErr]) {
        LOGE("Failed to start AVAudioEngine: %s",
             startErr ? startErr.localizedDescription.UTF8String : "unknown");
        return BackendResult::ERROR_STREAM_FAILED;
    }

#if TARGET_OS_OSX
    // ---- 6b. macOS: lo que el dispositivo DICE que quedo, y su latencia real ----
    // Hallazgo 2 de MINI-028: sin AVAudioSession, la latencia de salida era sólo el
    // buffer. La del HAL es dispositivo + safety offset + stream + el buffer de IO.
    {
        const AudioObjectID dev = macAuDevice != kAudioObjectUnknown ? macAuDevice : macDevice;
        UInt32 actualFrames = 0, devLat = 0, safety = 0;
        Float64 devNominal = 0;
        // Una lectura que falla se dice: sin ella, la latencia volveria a ser "solo el
        // buffer" en silencio, que es el defecto del hallazgo 2.
        const OSStatus stFrames = macGetProp(dev, kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal, sizeof(actualFrames), &actualFrames);
        const OSStatus stLat = macGetProp(dev, kAudioDevicePropertyLatency,
            kAudioObjectPropertyScopeOutput, sizeof(devLat), &devLat);
        const OSStatus stSafety = macGetProp(dev, kAudioDevicePropertySafetyOffset,
            kAudioObjectPropertyScopeOutput, sizeof(safety), &safety);
        const OSStatus stRate = macGetProp(dev, kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal, sizeof(devNominal), &devNominal);
        if (stFrames != noErr || stLat != noErr || stSafety != noErr || stRate != noErr) {
            LOGW("macOS: lecturas del HAL fallidas (buffer %d, latencia %d, safety %d, tasa %d): "
                 "la latencia publicada no las incluye", (int)stFrames, (int)stLat,
                 (int)stSafety, (int)stRate);
        }
        if (stRate == noErr && std::lround(devNominal) != std::lround(negotiatedSampleRate)) {
            LOGW("macOS: el dispositivo %u corre a %.0f Hz y el stream abrio a %.0f Hz: "
                 "AVAudioEngine remuestrea", (unsigned)dev, devNominal, negotiatedSampleRate);
        }
        const UInt32 streamLat = macFirstOutputStreamLatency(dev);

        if (actualFrames > 0) {
            negotiatedFrames = static_cast<int>(actualFrames);
            ioBufferDuration = static_cast<double>(actualFrames) / negotiatedSampleRate;
        }
        sessionOutputLatency = static_cast<float>(
            static_cast<double>(devLat + safety + streamLat) / negotiatedSampleRate);

        // Presupuesto por frame para el contador de sobre-presupuesto, en punto fijo.
        macDiag().budgetNumTicks.store(
            macNsToTicks(1e9 / negotiatedSampleRate * static_cast<double>(1u << 20)),
            std::memory_order_relaxed);

        // El estado reporta si HAY listener sobre `dev`, no si se intento: un 0 sin
        // listener haria pasar por limpio un contador que no puede contar.
        if (gMacListenedDevice != kAudioObjectUnknown && gMacListenedDevice != dev) {
            AudioObjectRemovePropertyListener(gMacListenedDevice, &kMacOverloadAddress,
                                              macOverloadListener, nullptr);
            gMacListenedDevice = kAudioObjectUnknown;
        }
        OSStatus listenerStatus = (dev == kAudioObjectUnknown)
            ? static_cast<OSStatus>(kAudioHardwareBadDeviceError) : noErr;
        if (gMacListenedDevice == kAudioObjectUnknown && dev != kAudioObjectUnknown) {
            listenerStatus = AudioObjectAddPropertyListener(dev, &kMacOverloadAddress,
                                                            macOverloadListener, nullptr);
            if (listenerStatus == noErr) {
                gMacListenedDevice = dev;
            } else {
                LOGW("macOS: no pude escuchar kAudioDeviceProcessorOverload (OSStatus %d)",
                     (int)listenerStatus);
            }
        }

        WmaMacDeviceInfo info{};
        info.deviceId               = dev;
        info.nominalSampleRate      = stRate == noErr ? devNominal : 0.0;
        info.streamSampleRate       = negotiatedSampleRate;
        info.bufferFramesRequested  = macBufferTarget;
        info.bufferFramesActual     = actualFrames;
        info.bufferFramesMin        = static_cast<uint32_t>(macBufferRange.mMinimum);
        info.bufferFramesMax        = static_cast<uint32_t>(macBufferRange.mMaximum);
        info.deviceLatencyFrames    = devLat;
        info.safetyOffsetFrames     = safety;
        info.streamLatencyFrames    = streamLat;
        info.presentationLatencySec = engine.outputNode.presentationLatency;
        info.bufferSetStatus        = static_cast<int32_t>(macBufferStatus);
        info.overloadListenerStatus = static_cast<int32_t>(listenerStatus);
        {
            std::lock_guard<std::mutex> lock(macDiag().deviceMutex);
            macDiag().device      = info;
            macDiag().deviceValid = true;
        }
        LOGI("macOS HAL: device=%u rate=%.0f buffer pedido=%u efectivo=%u [%u..%u] "
             "latencia dev=%u safety=%u stream=%u frames, presentationLatency=%.2f ms",
             (unsigned)dev, negotiatedSampleRate, (unsigned)macBufferTarget,
             (unsigned)actualFrames, info.bufferFramesMin, info.bufferFramesMax,
             (unsigned)devLat, (unsigned)safety, (unsigned)streamLat,
             info.presentationLatencySec * 1000.0);
    }
#endif // TARGET_OS_OSX

    // ---- 7. Publish the negotiated stream info ----
    const float ioBufferMs = (ioBufferDuration > 0.0)
        ? (float)(ioBufferDuration * 1000.0)
        : ((float)negotiatedFrames / (float)negotiatedSampleRate) * 1000.0f;
    const float outLatencyMs = sessionOutputLatency * 1000.0f + ioBufferMs;
    mOutputLatencyMs.store(outLatencyMs, std::memory_order_release);

    const bool captureLive = mCaptureActive.load(std::memory_order_acquire);

    // Input latency is the hardware capture path plus the ring's phase offset:
    // the sink block writes a block that the render block reads on its next turn,
    // which is one buffer. Reporting only the session value would understate the
    // round trip by exactly the amount this design costs.
    const float inLatencyMs = captureLive
        ? (sessionInputLatency * 1000.0f + ioBufferMs)
        : 0.0f;
    mInputLatencyMs.store(inLatencyMs, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(mStreamInfoMutex);
        mCachedStreamInfo.sampleRate      = (int)std::lround(negotiatedSampleRate);
        // Los canales del FORMATO con el que el grafo quedó conectado (paso 5), no
        // un literal: es la propiedad del stream abierto, que es lo que REQ-045 (D10)
        // pide transportar en vez de inventar.
        mCachedStreamInfo.channelCount    = (int)format.channelCount;
        // Y el modo de latencia queda DESCONOCIDO a propósito: AVAudioSession no
        // tiene un modo análogo al `PerformanceMode` de Oboe, y derivarlo de
        // `IOBufferDuration` sería volver a inventar el valor — el defecto exacto que
        // este REQ vino a borrar, una capa más abajo. Viaja a Kotlin como `null`.
        mCachedStreamInfo.lowLatency      = StreamInfo::LowLatency::UNKNOWN;
        mCachedStreamInfo.framesPerBuffer = negotiatedFrames;
        mCachedStreamInfo.format          = AudioFormat::FLOAT_32;
        mCachedStreamInfo.outputLatencyMs = outLatencyMs;
        mCachedStreamInfo.inputLatencyMs  = inLatencyMs;
        mCachedStreamInfo.isFullDuplex    = captureLive;
        mCachedStreamInfo.backendType     = BackendType::COREAUDIO;
        mCachedStreamInfo.deviceName      = "Core Audio";
        mStreamInfoValid.store(true);
    }

    LOGI("=== CoreAudio STREAM OPENED ===");
    LOGI("  Sample rate:      %d Hz", mCachedStreamInfo.sampleRate);
    LOGI("  Frames/buffer:    %d", negotiatedFrames);
    LOGI("  Output latency:   %.2f ms", outLatencyMs);
    LOGI("  Capture:          %s", captureLive ? "active" : "off");
    if (captureLive) {
        LOGI("  Input latency:    %.2f ms", inLatencyMs);
    }
    LOGI("===============================");

    return BackendResult::OK;
}

void CoreAudioBackend::closeEngineLocked() {
    if (mImpl->engine != nil) {
        [mImpl->engine stop];
    }

    // Capture goes down first so nothing reads a half-torn-down state: the flag
    // is what both RT blocks consult before touching the ring.
    mCaptureActive.store(false, std::memory_order_release);
    mCapturePrimed.store(false, std::memory_order_release);
    mInputRing.clear();
    // Un hueco de la sesion anterior no describe a la que viene. Ver clear().
    mCaptureGap.clear();

    // Dropping the strong refs releases the engine, its nodes and their blocks
    // (ARC). Done off the RT path.
    mImpl->sinkNode   = nil;
    mImpl->sourceNode = nil;
    mImpl->engine     = nil;
    mImpl->format     = nil;

#if TARGET_OS_OSX
    if (gMacListenedDevice != kAudioObjectUnknown) {
        AudioObjectRemovePropertyListener(gMacListenedDevice, &kMacOverloadAddress,
                                          macOverloadListener, nullptr);
        gMacListenedDevice = kAudioObjectUnknown;
    }
    {
        std::lock_guard<std::mutex> lock(macDiag().deviceMutex);
        macDiag().deviceValid = false;  // el dispositivo del stream que se cierra ya no vale
    }
    // La unica escritura de un `rt*` fuera del RT, y es segura: aca el engine ya esta
    // detenido (stop() drena los callbacks antes; en un start() fallido nunca corrio).
    // Sin esto, el reloj de muestras del stream siguiente contaria una discontinuidad.
    macDiag().rtNextSampleTime.store(-1.0, std::memory_order_relaxed);
#endif

#if TARGET_OS_IOS
    NSError* err = nil;
    [[AVAudioSession sharedInstance] setActive:NO
                                   withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                         error:&err];
#endif
}

} // namespace watermelon_audio
