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
// Cuantas veces se borro el temporal con el hilo escritor todavia sin juntar: el orden que
// `abortTake` promete (juntar, despues borrar) es que esto quede en cero.
std::atomic<int> gTunerInputDumpAbortsWithLiveWriter{0};
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
    putU32(out + 4, static_cast<uint32_t>(kInputDumpHeaderBytes - 8) + dataBytes);   // archivo - 8
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
    putU32(out + 54, static_cast<uint32_t>(kInputDumpWmadBodyBytes));
    // Los 40 bytes de v1, en sus offsets: un lector de v1 sigue encontrando cada campo.
    putU32(out + 58, kInputDumpWmadVersion);
    putU64(out + 62, frames);
    putU64(out + 70, s.framesLost);
    putU64(out + 78, s.discontinuities);
    putU32(out + 86, static_cast<uint32_t>(s.sampleRate));
    putU32(out + 90, static_cast<uint32_t>(s.inputSource));
    putU32(out + 94, s.valid ? 1u : 0u);
    // v2: lo no ubicado y el mapa de capacidad fija.
    putU64(out + 98, s.unlocatedFrames);
    putU64(out + 106, s.unlocatedDiscontinuities);
    const uint32_t count = std::min(s.cutCount, kInputDumpCutCapacity);
    putU32(out + 114, count);
    putU32(out + 118, kInputDumpCutCapacity);
    constexpr size_t kCutsAt = 122;
    for (uint32_t i = 0; i < kInputDumpCutCapacity; ++i) {
        uint8_t* c = out + kCutsAt + kInputDumpCutBytes * i;
        const InputDumpCut cut = i < count ? s.cuts[i] : InputDumpCut{};
        putU64(c + 0, cut.fileFrame);
        putU64(c + 8, cut.lostFrames);
        putU32(c + 16, cut.fileSpan);
        putU32(c + 20, cut.kind);
    }

    constexpr size_t kDataAt = 58 + kInputDumpWmadBodyBytes;
    static_assert(kDataAt + 8 == kInputDumpHeaderBytes, "data va justo despues del wmad");
    putTag(out + kDataAt, "data");
    putU32(out + kDataAt + 4, dataBytes);
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
    // El residuo es del ULTIMO aborto: un start que falla por otra causa no hereda el de antes.
    mAbortResidue.clear();
    if (!sink || path.empty()) return false;

    // Todo se reinicia ANTES de abrir: una toma mala no puede envenenar a la siguiente.
    mHead.store(0, std::memory_order_relaxed);
    mTail.store(0, std::memory_order_relaxed);
    mFramesQueued.store(0, std::memory_order_relaxed);
    mFramesWritten.store(0, std::memory_order_relaxed);
    mLostByProducer.store(0, std::memory_order_relaxed);
    mLostByWriter.store(0, std::memory_order_relaxed);
    mUnlocatedByProducer.store(0, std::memory_order_relaxed);
    mUnlocatedDiscontinuities.store(0, std::memory_order_relaxed);
    // El mapa y el estado de la costura: el productor no esta armado (lo arma el buzon despues de
    // que `mActive` se publica), asi que el control los puede tocar aca.
    mCutCount = 0;
    mLastSeamCut = -1;
    mLastLossCut = -1;
    mSeamReadFrame = 0;
    mSeamPending = false;
    mPendingSeam = 0;
    mPendingSeamLo = 0;
    mSeamNotices = 0;
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
#if defined(WMA_TEST_HOOKS)
    if (mWriter.joinable()) gTunerInputDumpAbortsWithLiveWriter.fetch_add(1);
#endif
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
    // El productor ya esta desarmado (precondicion; el buzon da el happens-before): su estado
    // plano, el mapa incluido, se lee desde aca. Una costura todavia pendiente se cierra en lo
    // escrito: el salto cae en algun lugar entre su `lo` y el final, o despues (D25).
    if (mSeamPending) closeDumpSeamSpan(s.framesWritten);
    s.framesLost = framesLost();
    s.unlocatedFrames = mUnlocatedByProducer.load(std::memory_order_relaxed) +
                        mLostByWriter.load(std::memory_order_relaxed);
    s.unlocatedDiscontinuities = mUnlocatedDiscontinuities.load(std::memory_order_relaxed);
    s.discontinuities = mSeamNotices + s.unlocatedDiscontinuities;
    s.sampleRate = mSampleRate.load(std::memory_order_relaxed);
    s.inputSource = mInputSource;
    // El mapa ORDENADO por frame del archivo (insercion estable): un tramo de costura se anota al
    // cerrarse, despues de perdidas que caen dentro de el.
    s.cutCount = mCutCount;
    for (uint32_t i = 0; i < mCutCount; ++i) {
        uint32_t j = i;
        while (j > 0 && s.cuts[j - 1].fileFrame > mCuts[i].fileFrame) {
            s.cuts[j] = s.cuts[j - 1];
            --j;
        }
        s.cuts[j] = mCuts[i];
    }
    // Lo UBICADO no invalida: el mapa lo declara. Lo no ubicado si.
    s.valid = s.unlocatedFrames == 0 && s.unlocatedDiscontinuities == 0 && s.sampleRate > 0 &&
              s.framesWritten > 0 && !mSinkFailed.load(std::memory_order_relaxed);

    // En la pila (6 KiB, hilo de control): sin asignar, el cierre no suma un modo de falla.
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
    mSeamReadFrame = mFramesQueued.load(std::memory_order_relaxed);
    // Una costura vigente DELANTE del cursor es un hueco que la toma todavia tiene que cruzar:
    // ya le pertenece, y queda pendiente desde el principio del archivo. Una detras (o justo en el
    // cursor) es anterior a la toma.
    if (mLastSeam > mCursor) {
        ++mSeamNotices;
        mSeamPending = true;
        mPendingSeam = mLastSeam;
        mPendingSeamLo = mSeamReadFrame;
    }
}

void TunerInputDump::noteDumpLocatedLoss(uint64_t lost) noexcept {
    const uint64_t at = mFramesQueued.load(std::memory_order_relaxed);
    if (mLastLossCut >= 0 && mCuts[mLastLossCut].fileFrame == at) {
        mCuts[mLastLossCut].lostFrames += lost;   // contigua a la anterior: una sola entrada
        return;
    }
    if (mCutCount >= kInputDumpCutCapacity) {
        // Mapa lleno: ya no se puede declarar DONDE falta. La toma queda invalida.
        mUnlocatedByProducer.fetch_add(lost, std::memory_order_relaxed);
        return;
    }
    mCuts[mCutCount] = InputDumpCut{at, lost, 0, kInputDumpCutLoss};
    mLastLossCut = static_cast<int>(mCutCount);
    ++mCutCount;
}

void TunerInputDump::noteDumpSeamSpan(uint64_t lo, uint64_t hi) noexcept {
    if (hi < lo) hi = lo;
    if (mLastSeamCut >= 0) {
        InputDumpCut& last = mCuts[mLastSeamCut];
        const uint64_t lastEnd = last.fileFrame + last.fileSpan;
        if (lo <= lastEnd) {   // se solapa (o toca) el tramo anterior: se fusiona
            const uint64_t end = std::max(lastEnd, hi);
            const uint64_t from = std::min<uint64_t>(last.fileFrame, lo);
            last.fileFrame = from;
            last.fileSpan = static_cast<uint32_t>(end - from);
            return;
        }
    }
    if (mCutCount >= kInputDumpCutCapacity) {
        mUnlocatedDiscontinuities.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // El largo cabe en 32 bits: una toma no pasa de `kMaxTakeFrames` (2^28) frames.
    mCuts[mCutCount] = InputDumpCut{lo, 0, static_cast<uint32_t>(hi - lo), kInputDumpCutSeam};
    mLastSeamCut = static_cast<int>(mCutCount);
    ++mCutCount;
}

void TunerInputDump::closeDumpSeamSpan(uint64_t hi) noexcept {
    mSeamPending = false;
    noteDumpSeamSpan(mPendingSeamLo, hi);
}

void TunerInputDump::readDumpSeam(const AnalysisRing& ring) noexcept {
    const uint64_t seam = ring.captureSeamPosition();
    // `lo` de un aviso nuevo = el frame del archivo en la lectura ANTERIOR: todo aviso posterior a
    // ella vale >= el cursor de entonces (el aviso estampa la posicion de escritura, que ya habia
    // pasado ese cursor). Un aviso intermedio que el ring piso con uno mas lejano cae en el tramo.
    const uint64_t lo = mSeamReadFrame;
    mSeamReadFrame = mFramesQueued.load(std::memory_order_relaxed);
    if (seam < mLastSeam) {
        mLastSeam = seam;   // el ring se reseteo: el retroceso de posicion ya se anoto aparte
        return;
    }
    if (seam == mLastSeam) return;
    ++mSeamNotices;
    mLastSeam = seam;
    if (mSeamPending) {
        // Otra costura con una pendiente: el tramo se extiende hasta la nueva (la pendiente esta
        // delante del cursor, y la nueva, mas lejos todavia).
        mPendingSeam = seam;
        return;
    }
    if (seam > mCursor) {
        mSeamPending = true;   // delante: la copia se topa en `seam` para cerrar el tramo exacto
        mPendingSeam = seam;
        mPendingSeamLo = lo;
    } else {
        // Ya copiada: el salto quedo en algun frame entre la lectura anterior y ahora.
        noteDumpSeamSpan(lo, mSeamReadFrame);
    }
}

void TunerInputDump::captureFromRing(const AnalysisRing& ring, int captureRate) noexcept {
    if (!mActive.load(std::memory_order_acquire)) return;

    // Las posiciones retrocedieron: el ring se reseteo. Sin esto `w - cursor` seria una cantidad
    // enorme sin signo y el volcado quedaria congelado hasta que la posicion vuelva a pasar el
    // cursor. Se reubica en lo que el ring todavia conserva. No se puede ubicar: invalida.
    const uint64_t w = ring.writePosition();
    if (mCursor > w) {
        noteInputDiscontinuity();
        if (mSeamPending) closeDumpSeamSpan(mFramesQueued.load(std::memory_order_relaxed));
        mCursor = w > AnalysisRing::kDumpWindowFrames ? w - AnalysisRing::kDumpWindowFrames : 0;
    }

    // La costura se lee ANTES de copiar (despues de `writePosition`): asi una costura delante del
    // cursor topa la copia de esta misma vuelta.
    readDumpSeam(ring);

    uint64_t copied = 0;
    for (int round = 0; round < kMaxCopyRounds; ++round) {
        if (mSeamPending && mCursor >= mPendingSeam) {
            closeDumpSeamSpan(mFramesQueued.load(std::memory_order_relaxed));
        }
        uint64_t want = mChunk.size();
        if (mSeamPending) want = std::min<uint64_t>(want, mPendingSeam - mCursor);

        const uint64_t before = mCursor;
        uint64_t lost = 0;
        const int n = ring.copyForInputDump(mCursor, mChunk.data(), static_cast<int>(want), lost);
        if (lost > 0) {
            // Pisado en el ring antes de copiar: falta JUSTO ANTES de lo que se encola ahora.
            mLostByProducer.fetch_add(lost, std::memory_order_relaxed);
            noteDumpLocatedLoss(lost);
            // Una perdida que salta la costura pendiente cierra el tramo ahi.
            if (mSeamPending && mCursor - static_cast<uint64_t>(n > 0 ? n : 0) >= mPendingSeam) {
                closeDumpSeamSpan(mFramesQueued.load(std::memory_order_relaxed));
            }
        }
        if (n > 0) {
            copied += static_cast<uint64_t>(n);
            // El tope por toma: lo que no entra se pierde y se cuenta, sin ubicar (la toma queda
            // invalida). Solo este hilo encola, asi que `mFramesQueued` no se mueve entre la
            // lectura y el push.
            const uint64_t queued = mFramesQueued.load(std::memory_order_relaxed);
            const uint64_t room = mMaxTakeFrames > queued ? mMaxTakeFrames - queued : 0;
            const uint64_t accepted = std::min<uint64_t>(static_cast<uint64_t>(n), room);
            const uint64_t overCap = static_cast<uint64_t>(n) - accepted;
            if (overCap > 0) {
                mLostByProducer.fetch_add(overCap, std::memory_order_relaxed);
                mUnlocatedByProducer.fetch_add(overCap, std::memory_order_relaxed);
            }
            if (accepted == 0) continue;
            if (tryPushFrames(mChunk.data(), static_cast<size_t>(accepted))) {
                mFramesQueued.fetch_add(accepted, std::memory_order_relaxed);
            } else {
                // Cola llena: el escritor no da abasto. Nunca se espera; se pierde, se cuenta y se
                // ubica (el archivo sigue en el mismo frame).
                mLostByProducer.fetch_add(accepted, std::memory_order_relaxed);
                noteDumpLocatedLoss(accepted);
            }
            continue;
        }
        // Sin frames: o no hay nada (cursor quieto, listo) o una copia desgarrada movio el cursor
        // y se reintenta desde ahi.
        if (mCursor == before) break;
    }
    // La ultima vuelta pudo dejar el cursor justo en la costura.
    if (mSeamPending && mCursor >= mPendingSeam) {
        closeDumpSeamSpan(mFramesQueued.load(std::memory_order_relaxed));
    }
    // Y otra lectura al final: acota el `lo` del proximo aviso a lo copiado en esta vuelta (sin
    // ella, un aviso durante la copia de esta vuelta tendria que abarcarla entera).
    readDumpSeam(ring);

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
