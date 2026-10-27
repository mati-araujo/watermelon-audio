#include "TunerInputDump.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>

#if defined(WMA_TEST_HOOKS)
#include <system_error>

// ===========================================================================
// COMPUERTA DE TEST (WMA_TEST_HOOKS) — la define solo analysis/tests.
//
// Que `std::thread` no pueda crear el hilo escritor (EAGAIN: el proceso sin hilos) no se provoca
// desde un test sin tumbar la suite. Con la compuerta, `beginTake` lanza la MISMA excepcion que el
// constructor del hilo justo donde el hilo se crearia, con el `.part` ya abierto: el estado del
// bug B. En el binario que shippea esto compila a nada.
// ===========================================================================
std::atomic<bool> gTunerInputDumpFailWriterThread{false};
#endif

namespace wma::analysis {

namespace {

constexpr char kWavExtension[] = ".wav";
constexpr size_t kWavExtensionLen = sizeof(kWavExtension) - 1;

// El header se escribe byte a byte: el orden del WAV es little-endian en cualquier CPU, y no se
// quiere depender del de la que compila.
void putU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void putU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void putU64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void putTag(uint8_t* p, const char (&tag)[5]) { std::memcpy(p, tag, 4); }

}  // namespace

bool hasWavExtension(const std::string& path) {
    // Solo la extension FINAL cuenta: el ".wav" de un directorio no es la del archivo.
    return path.size() >= kWavExtensionLen &&
           path.compare(path.size() - kWavExtensionLen, kWavExtensionLen, kWavExtension) == 0;
}

std::string invalidDumpPathFor(const std::string& path) {
    if (!hasWavExtension(path)) return path + ".invalid.wav";
    return path.substr(0, path.size() - kWavExtensionLen) + ".invalid.wav";
}

void writeInputDumpHeader(uint8_t* out, const InputDumpStats& s) {
    const uint64_t frames = s.framesWritten;
    const uint32_t dataBytes = static_cast<uint32_t>(frames * sizeof(float));
    const uint32_t rate = static_cast<uint32_t>(s.sampleRate);

    putTag(out + 0, "RIFF");
    putU32(out + 4, 98u + dataBytes);   // archivo - 8
    putTag(out + 8, "WAVE");

    putTag(out + 12, "fmt ");
    putU32(out + 16, 18);
    putU16(out + 20, 3);                // WAVE_FORMAT_IEEE_FLOAT
    putU16(out + 22, 1);                // mono: lo que ve el afinador
    putU32(out + 24, rate);
    putU32(out + 28, rate * 4u);
    putU16(out + 32, 4);
    putU16(out + 34, 32);
    putU16(out + 36, 0);                // cbSize

    putTag(out + 38, "fact");
    putU32(out + 42, 4);
    putU32(out + 46, static_cast<uint32_t>(frames));

    putTag(out + 50, "wmad");
    putU32(out + 54, 40);
    putU32(out + 58, 1);                // version del chunk
    putU64(out + 62, frames);
    putU64(out + 70, s.framesLost);
    putU64(out + 78, s.discontinuities);
    putU32(out + 86, static_cast<uint32_t>(s.sampleRate));
    putU32(out + 90, static_cast<uint32_t>(s.inputSource));
    putU32(out + 94, s.valid ? 1u : 0u);

    putTag(out + 98, "data");
    putU32(out + 102, dataBytes);
}

TunerInputDump::TunerInputDump(size_t queueFrames, uint64_t maxTakeFrames)
    : mMaxTakeFrames(std::min(maxTakeFrames, kMaxTakeFrames)),
      mQueue(std::bit_ceil(std::max<size_t>(queueFrames, 1)), 0.0f),
      mQueueMask(mQueue.size() - 1),
      mChunk(std::min(kChunkFrames, mQueue.size()), 0.0f) {}

TunerInputDump::~TunerInputDump() { endTake(); }

bool TunerInputDump::beginTake(std::unique_ptr<IDumpSink> sink, const std::string& path,
                               int inputSource) {
    if (mActive.load(std::memory_order_acquire)) return false;
    if (!sink || path.empty()) return false;

    // Todo se reinicia ANTES de abrir: una toma mala no puede envenenar a la siguiente.
    mHead.store(0, std::memory_order_relaxed);
    mTail.store(0, std::memory_order_relaxed);
    mFramesQueued.store(0, std::memory_order_relaxed);
    mFramesWritten.store(0, std::memory_order_relaxed);
    mLostByProducer.store(0, std::memory_order_relaxed);
    mLostByWriter.store(0, std::memory_order_relaxed);
    mDiscontinuities.store(0, std::memory_order_relaxed);
    mSampleRate.store(0, std::memory_order_relaxed);
    mSinkFailed.store(false, std::memory_order_relaxed);
    mStopWriter.store(false, std::memory_order_relaxed);

    if (!sink->openPart(path, kInputDumpHeaderBytes)) return false;

    mSink = std::move(sink);
    mPath = path;
    mInputSource = inputSource;
    try {
#if defined(WMA_TEST_HOOKS)
        if (gTunerInputDumpFailWriterThread.load(std::memory_order_acquire)) {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
#endif
        mWriter = std::thread([this] { writerLoop(); });
    } catch (...) {
        // Sin hilo no hay toma, y el start va a devolver false: en disco no puede quedar nada de
        // ella (bug B). Ningun hilo escribe el temporal (el escritor no llego a existir): se borra.
        abortSink();
        return false;
    }
    mActive.store(true, std::memory_order_release);
    return true;
}

bool TunerInputDump::abortSink() {
    mAbortResidue.clear();
    if (!mSink) return true;
    const std::string part = mSink->partPath();
    const bool clean = mSink->abortPart();
    if (!clean) mAbortResidue = part.empty() ? mPath + ".part" : part;
    mSink.reset();
    return clean;
}

bool TunerInputDump::abortTake() {
    if (!mActive.exchange(false, std::memory_order_acq_rel)) return true;
    // El orden es el contrato: el escritor se para y se junta ANTES de borrar, asi nadie escribe
    // el temporal mientras se lo borra (ni lo recrea: el sink ya esta abierto, no reabre).
    mStopWriter.store(true, std::memory_order_release);
    if (mWriter.joinable()) mWriter.join();
    return abortSink();
}

InputDumpStats TunerInputDump::endTake(std::string* finalPath, std::string* leftPath) {
    if (finalPath != nullptr) finalPath->clear();
    if (leftPath != nullptr) leftPath->clear();
    InputDumpStats s;
    if (!mActive.exchange(false, std::memory_order_acq_rel)) return s;

    // El escritor lee `mStopWriter` ANTES de mirar la cola: si ve el pedido, ya ve todo lo que el
    // productor (desarmado antes de llegar aca) dejo, y no sale con frames sin escribir.
    mStopWriter.store(true, std::memory_order_release);
    if (mWriter.joinable()) mWriter.join();

    s.framesWritten = mFramesWritten.load(std::memory_order_relaxed);
    s.framesLost = framesLost();
    s.discontinuities = mDiscontinuities.load(std::memory_order_relaxed);
    s.sampleRate = mSampleRate.load(std::memory_order_relaxed);
    s.inputSource = mInputSource;
    s.valid = s.framesLost == 0 && s.discontinuities == 0 && s.sampleRate > 0 &&
              s.framesWritten > 0 && !mSinkFailed.load(std::memory_order_relaxed);

    uint8_t header[kInputDumpHeaderBytes];
    writeInputDumpHeader(header, s);
    std::string finalName = mSink->finishPart(header, sizeof header, s.valid);
    // Sin archivo nombrado no hay WAV sano que ofrecer, aunque cada frame se haya escrito.
    if (finalName.empty()) s.valid = false;
    if (leftPath != nullptr) *leftPath = finalName.empty() ? mSink->partPath() : finalName;
    if (finalPath != nullptr) *finalPath = std::move(finalName);
    mSink.reset();
    return s;
}

void TunerInputDump::armAtRing(const AnalysisRing& ring) noexcept {
    mCursor = ring.writePosition();
    mLastSeam = ring.captureSeamPosition();
    mLastRate = 0;
    // Una costura vigente DELANTE del cursor es un hueco que la toma todavia tiene que cruzar:
    // ya le pertenece. Una detras (o justo en el cursor) es anterior a la toma.
    if (mLastSeam > mCursor) noteInputDiscontinuity();
}

void TunerInputDump::captureFromRing(const AnalysisRing& ring, int captureRate) noexcept {
    if (!mActive.load(std::memory_order_acquire)) return;

    // Las posiciones retrocedieron: el ring se reseteo. Sin esto `w - cursor` seria una cantidad
    // enorme sin signo y el volcado quedaria congelado hasta que la posicion vuelva a pasar el
    // cursor. Se reubica en lo que el ring todavia conserva.
    const uint64_t w = ring.writePosition();
    if (mCursor > w) {
        noteInputDiscontinuity();
        mCursor = w > AnalysisRing::kDumpWindowFrames ? w - AnalysisRing::kDumpWindowFrames : 0;
    }

    uint64_t copied = 0;
    for (int round = 0; round < kMaxCopyRounds; ++round) {
        const uint64_t before = mCursor;
        uint64_t lost = 0;
        const int n = ring.copyForInputDump(mCursor, mChunk.data(),
                                            static_cast<int>(mChunk.size()), lost);
        if (lost > 0) mLostByProducer.fetch_add(lost, std::memory_order_relaxed);
        if (n > 0) {
            copied += static_cast<uint64_t>(n);
            // El tope por toma: lo que no entra se pierde y se cuenta (la toma queda invalida). Solo
            // este hilo encola, asi que `mFramesQueued` no se mueve entre la lectura y el push.
            const uint64_t queued = mFramesQueued.load(std::memory_order_relaxed);
            const uint64_t room = mMaxTakeFrames > queued ? mMaxTakeFrames - queued : 0;
            const uint64_t accepted = std::min<uint64_t>(static_cast<uint64_t>(n), room);
            const uint64_t overCap = static_cast<uint64_t>(n) - accepted;
            if (overCap > 0) mLostByProducer.fetch_add(overCap, std::memory_order_relaxed);
            if (accepted == 0) continue;
            if (tryPushFrames(mChunk.data(), static_cast<size_t>(accepted))) {
                mFramesQueued.fetch_add(accepted, std::memory_order_relaxed);
            } else {
                // Cola llena: el escritor no da abasto. Nunca se espera; se pierde y se cuenta.
                mLostByProducer.fetch_add(accepted, std::memory_order_relaxed);
            }
            continue;
        }
        // Sin frames: o no hay nada (cursor quieto, listo) o una copia desgarrada movio el cursor
        // y se reintenta desde ahi.
        if (mCursor == before) break;
    }

    // Toda costura nueva cuenta EN CUANTO SE VE, sin esperar a que el cursor la cruce: el ring se
    // queda con la mas lejana, asi que una costura pendiente puede ser pisada por otra mayor antes
    // de cruzarla. Invalida de mas a proposito (toma que cierra antes de llegar al hueco).
    const uint64_t seam = ring.captureSeamPosition();
    if (seam < mLastSeam) {
        mLastSeam = seam;   // el ring se reseteo: el retroceso de posicion ya se conto aparte
    } else if (seam > mLastSeam) {
        noteInputDiscontinuity();
        mLastSeam = seam;
    }

    // El rate viaja con los frames: se mira solo cuando entraron frames.
    if (copied > 0) {
        if (captureRate <= 0) {
            noteInputDiscontinuity();   // frames de un rate que nadie estampo: no se puede dar fe
        } else if (mSampleRate.load(std::memory_order_relaxed) == 0) {
            mSampleRate.store(captureRate, std::memory_order_relaxed);
            mLastRate = captureRate;
        } else if (captureRate != mLastRate) {
            noteInputDiscontinuity();
            mLastRate = captureRate;
        }
    }
}

bool TunerInputDump::tryPushFrames(const float* src, size_t n) noexcept {
    const uint64_t head = mHead.load(std::memory_order_relaxed);
    const uint64_t tail = mTail.load(std::memory_order_acquire);
    if (mQueue.size() - static_cast<size_t>(head - tail) < n) return false;

    const size_t start = static_cast<size_t>(head) & mQueueMask;
    const size_t firstPart = std::min(n, mQueue.size() - start);
    std::memcpy(&mQueue[start], src, firstPart * sizeof(float));
    if (firstPart < n) std::memcpy(&mQueue[0], src + firstPart, (n - firstPart) * sizeof(float));
    mHead.store(head + n, std::memory_order_release);
    return true;
}

void TunerInputDump::writerLoop() {
    for (;;) {
        const bool stopping = mStopWriter.load(std::memory_order_acquire);
        const uint64_t tail = mTail.load(std::memory_order_relaxed);
        const uint64_t avail = mHead.load(std::memory_order_acquire) - tail;
        if (avail == 0) {
            if (stopping) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(kWriterNapMs));
            continue;
        }

        const size_t start = static_cast<size_t>(tail) & mQueueMask;
        const size_t n = static_cast<size_t>(
            std::min<uint64_t>({avail, mQueue.size() - start, kWriterBatchFrames}));
        if (mSinkFailed.load(std::memory_order_relaxed)) {
            mLostByWriter.fetch_add(n, std::memory_order_relaxed);
        } else if (mSink->appendSamples(&mQueue[start], n)) {
            mFramesWritten.fetch_add(n, std::memory_order_relaxed);
        } else {
            // El disco fallo: lo que sigue se descarta y se cuenta. El productor ni se entera.
            mSinkFailed.store(true, std::memory_order_relaxed);
            mLostByWriter.fetch_add(n, std::memory_order_relaxed);
        }
        // Recien ahora el tramo vuelve a ser del productor.
        mTail.store(tail + n, std::memory_order_release);
    }
}

}  // namespace wma::analysis
