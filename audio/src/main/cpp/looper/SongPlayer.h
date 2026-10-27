#pragma once

/**
 * @file SongPlayer.h
 * @brief Cancion programada por frame (REQ-055 S11): una agenda de secciones, cada una con un
 *        clip por pista, que el callback mezcla en lugar de las tomas del looper.
 *
 * El looper no tiene reloj propio (cada TrackBuffer acumula su playhead); el unico reloj es el
 * play frame del Transport, un int64 monotono que NO se resetea. La cancion se deriva de el: la
 * fase de una pista en la seccion k es `(frame - start_k) mod L`, una funcion pura del frame
 * absoluto. Por eso el render no guarda posiciones: un bloque de 1 frame y uno de 4096 dan la
 * misma salida bit a bit.
 *
 * Tres partes, con dueños de hilo distintos:
 *  - CLIPS (control): buffers densos e inmutables de exactamente L frames estereo, construidos
 *    fuera de RT. El callback los lee por una tabla de punteros atomicos.
 *  - AGENDA (control -> RT): un triple buffer wait-free de slots preasignados. El control escribe
 *    la agenda ENTERA en su slot y la intercambia con el del medio; el RT la adopta al inicio del
 *    bloque si esta fresca. Estado por seccion, no deltas: una agenda perdida no corrompe nada.
 *  - VOCES (RT): una por pista, con sus tres smoothers (volumen, mute, paneo, los mismos que
 *    TrackBuffer::mixInto) y un carril de cola para el fundido de una adopcion tardia.
 *
 * Fundido F = max(64, round(0,005 * SR)) en [B - F, B) antes de cada borde B: equal-power (cuarto
 * de seno/coseno) para cambio de clip o salto de fase, coseno elevado para entrar/salir de
 * silencio; nada si el mismo clip sigue alineado. El estado de B rige exacto desde B.
 *
 * Liberacion de clips: sacar un clip de la agenda no lo libera. El RT publica al final de cada
 * bloque que agenda adopto y que clips tiene en uso (la agenda adoptada mas las colas); el
 * control libera recien cuando esa foto es posterior a la baja y el clip no figura. En offline no
 * hay hilo RT concurrente y se libera de inmediato.
 */

#include "LooperExporter.h"
#include "TrackBuffer.h"
#include "../platform/RtCounter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace wm {

constexpr int kSongMaxTracks = 16;      // = AudioLooper::MAX_TRACKS_HW (static_assert alla)
constexpr int kSongMaxClips = 64;
constexpr int kSongMaxSections = 1024;
constexpr int32_t kSongSilence = -1;

/// Que le paso a una operacion de control de la cancion.
enum class SongStatus {
    Ok = 0,
    OutOfRange,         ///< argumento fuera de rango, pool lleno, agenda mal formada
    InvalidOperation,   ///< clip inexistente/retirado/en uso, grabacion, otro largo de loop
    Io,
    UnsupportedFormat,
    BudgetExceeded,
    OutOfMemory,
};

/// Una seccion de la agenda: desde `startFrame` (play frame absoluto) rige un clip por pista.
struct SongSection {
    int64_t startFrame = 0;
    int32_t clip[kSongMaxTracks];   ///< id de clip por pista; kSongSilence = silencio

    SongSection() {
        for (int32_t& c : clip) c = kSongSilence;
    }
};

/// Lo que el looper sabe y el SongPlayer necesita en cada operacion de control.
struct SongContext {
    int sampleRate = 48000;
    int maxActiveTracks = 8;
    size_t budgetBytes = 0;
    size_t usedBytes = 0;        ///< RAM reservada por las TOMAS (los clips los cuenta el SongPlayer)
    bool recordingBusy = false;  ///< grabando o armado
    bool immediate = false;      ///< offline: ningun hilo RT concurrente, se libera sin esperar
};

/// F de un sample rate: 5 ms, con piso de 64 frames.
inline int songFadeFrames(int sampleRate) {
    return std::max(64, static_cast<int>(std::lround(0.005 * static_cast<double>(sampleRate))));
}

/**
 * @brief Normaliza una toma ya recortada a exactamente `loopFrames` frames (PURA, fuera de RT).
 *
 *  - toma >= L: una vista de L (los primeros L frames).
 *  - L multiplo exacto del largo: mosaico tal cual (la toma es un loop; su union es natural).
 *  - resto: mosaico con la union del cierre fundida. El clip termina en `take[r-1]` (r = L mod T)
 *    y vuelve a `take[0]`: un escalon. Los ultimos X frames se mezclan, equal-power, con
 *    `take[T-X..T)`, que es lo que en la toma precede a `take[0]`, asi el cierre llega a
 *    `take[T-1]` y el salto al principio es el de la toma misma.
 * Lanza std::bad_alloc si no hay memoria.
 */
inline std::vector<float> normalizeSongClip(const float* take, int64_t takeFrames,
                                            int64_t loopFrames, int fadeFrames) {
    std::vector<float> out(static_cast<size_t>(loopFrames) * 2);
    const int64_t t = takeFrames;
    if (t >= loopFrames) {
        std::memcpy(out.data(), take, static_cast<size_t>(loopFrames) * 2 * sizeof(float));
        return out;
    }
    const int64_t full = loopFrames / t;
    const int64_t r = loopFrames % t;
    for (int64_t k = 0; k < full; ++k) {
        std::memcpy(out.data() + static_cast<size_t>(k * t) * 2, take,
                    static_cast<size_t>(t) * 2 * sizeof(float));
    }
    if (r > 0) {
        std::memcpy(out.data() + static_cast<size_t>(full * t) * 2, take,
                    static_cast<size_t>(r) * 2 * sizeof(float));
        const int64_t x = std::min<int64_t>(std::max(fadeFrames, 1), r);
        for (int64_t i = 0; i < x; ++i) {
            const float th = static_cast<float>(i + 1) / static_cast<float>(x + 1) * 1.5707964f;
            const float gA = std::cos(th);
            const float gB = std::sin(th);
            const size_t dst = static_cast<size_t>(loopFrames - x + i) * 2;
            const size_t a = static_cast<size_t>(r - x + i) * 2;
            const size_t b = static_cast<size_t>(t - x + i) * 2;
            out[dst] = take[a] * gA + take[b] * gB;
            out[dst + 1] = take[a + 1] * gA + take[b + 1] * gB;
        }
    }
    return out;
}

/**
 * @brief Hornea el recorte [regionStart, regionEnd) de una toma decodificada (en frames YA
 *        resampleados) y la normaliza a `loopFrames`. `regionEnd <= 0` = toda la toma. El recorte
 *        lo fija `clampLoopRegion`, la misma regla que `TrackBuffer::setLoopRegion`: nunca vacio,
 *        nunca pasa el largo, una region corta se estira hasta el minimo si la toma da.
 */
inline std::vector<float> bakeSongClip(const float* decoded, int decodedFrames, int64_t regionStart,
                                       int64_t regionEnd, int64_t loopFrames, int fadeFrames) {
    const int start = static_cast<int>(std::clamp<int64_t>(regionStart, 0, INT32_MAX));
    const int end = static_cast<int>(std::clamp<int64_t>(regionEnd, 0, INT32_MAX));
    const LoopRegion region = clampLoopRegion(start, end, decodedFrames);
    return normalizeSongClip(decoded + static_cast<size_t>(region.start) * 2,
                             region.end - region.start, loopFrames, fadeFrames);
}

/// La agenda publicada. Los arrays grandes no se inicializan: el slot se llena entero antes de
/// publicarse y el RT lee solo `count` filas.
struct SongAgenda {
    int64_t loopFrames;
    int64_t endFrame;
    int64_t fade;        ///< F con que se publico (el RT no vuelve a derivarlo)
    uint64_t clipMask;   ///< clips que referencia (bit = id)
    uint64_t seq;
    uint64_t checksum;
    int32_t count;
    int64_t starts[kSongMaxSections];
    int32_t clips[kSongMaxSections][kSongMaxTracks];
};

inline uint64_t songAgendaChecksum(const SongAgenda& a) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    mix(static_cast<uint64_t>(a.loopFrames));
    mix(static_cast<uint64_t>(a.endFrame));
    mix(static_cast<uint64_t>(a.fade));
    mix(a.clipMask);
    mix(a.seq);
    mix(static_cast<uint64_t>(a.count));
    for (int k = 0; k < a.count; ++k) {
        mix(static_cast<uint64_t>(a.starts[k]));
        for (int t = 0; t < kSongMaxTracks; ++t) mix(static_cast<uint64_t>(a.clips[k][t]));
    }
    return h;
}

inline int64_t songPosMod(int64_t x, int64_t m) {
    const int64_t r = x % m;
    return r < 0 ? r + m : r;
}

/// Seccion que rige en `f`: -1 antes del primer start, `count` desde end_frame (o con agenda vacia).
inline int songSectionAt(const SongAgenda& a, int64_t f) {
    if (a.count <= 0) return 0;
    if (f >= a.endFrame) return a.count;
    if (f < a.starts[0]) return -1;
    int lo = 0;
    int hi = a.count - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) >> 1;
        if (a.starts[mid] <= f) lo = mid; else hi = mid - 1;
    }
    return lo;
}

enum SongFadeKind { kSongFadeNone = 0, kSongFadeEqual = 1, kSongFadeCos = 2 };

/// El tramo de una pista entre dos bordes: la seccion `s` y la que sigue.
struct SongSegment {
    int32_t cur;
    int32_t nxt;
    int64_t startCur;
    int64_t startNxt;
    int64_t bn;          ///< borde B donde la seccion siguiente pasa a regir
    int64_t fadeBegin;   ///< B - F si hay fundido; si no, B
    int sNext;
    int kind;
};

inline SongSegment songSegmentAt(const SongAgenda& a, int s, int track) {
    SongSegment g{};
    const int count = a.count;
    const bool inside = s >= 0 && s < count;
    g.cur = inside ? a.clips[s][track] : kSongSilence;
    g.startCur = inside ? a.starts[s] : 0;
    if (s < 0) {
        g.sNext = 0;
        g.bn = a.starts[0];
    } else if (s < count - 1) {
        g.sNext = s + 1;
        g.bn = a.starts[s + 1];
    } else if (s == count - 1) {
        g.sNext = count;
        g.bn = a.endFrame;
    } else {
        g.sNext = count;
        g.bn = INT64_MAX;
    }
    const bool hasNext = g.sNext < count;
    g.nxt = hasNext ? a.clips[g.sNext][track] : kSongSilence;
    g.startNxt = hasNext ? a.starts[g.sNext] : 0;
    if (g.cur < 0 && g.nxt < 0) {
        g.kind = kSongFadeNone;
    } else if (g.cur < 0 || g.nxt < 0) {
        g.kind = kSongFadeCos;
    } else if (g.cur == g.nxt && songPosMod(g.startNxt - g.startCur, a.loopFrames) == 0) {
        g.kind = kSongFadeNone;
    } else {
        g.kind = kSongFadeEqual;
    }
    const bool fades = g.kind != kSongFadeNone && g.bn != INT64_MAX && a.fade > 0;
    g.fadeBegin = fades ? g.bn - a.fade : g.bn;
    return g;
}

/// Ganancias (salida, entrada) del fundido en el frame `j` de `fade`.
inline void songFadeGains(int kind, int64_t j, int64_t fade, float& gOut, float& gIn) {
    if (kind == kSongFadeEqual) {
        const float th = static_cast<float>(j) * (1.5707964f / static_cast<float>(fade));
        gOut = std::cos(th);
        gIn = std::sin(th);
    } else {
        const float ph = static_cast<float>(j) * (3.1415927f / static_cast<float>(fade));
        const float c = std::cos(ph);
        gOut = 0.5f * (1.0f + c);
        gIn = 0.5f * (1.0f - c);
    }
}

/// Que suena en una pista en el frame `f`, sin contar fundidos (el estado "estable").
struct SongState {
    int32_t clip;
    int64_t phase;
    int64_t loop;
};

inline SongState songStateAt(const SongAgenda& a, int64_t f, int track) {
    SongState st{kSongSilence, 0, a.loopFrames};
    const int s = songSectionAt(a, f);
    if (s >= 0 && s < a.count) {
        st.clip = a.clips[s][track];
        if (st.clip >= 0) st.phase = songPosMod(f - a.starts[s], a.loopFrames);
    }
    return st;
}

class SongPlayer {
public:
    SongPlayer() : mAgendas(new SongAgenda[3]) {
        for (int i = 0; i < 3; ++i) {
            SongAgenda& a = mAgendas[i];
            a.loopFrames = 1;
            a.endFrame = 0;
            a.fade = 0;
            a.clipMask = 0;
            a.seq = 0;
            a.checksum = 0;
            a.count = 0;
        }
        mCur = &mAgendas[0];
    }
    ~SongPlayer() = default;

    SongPlayer(const SongPlayer&) = delete;
    SongPlayer& operator=(const SongPlayer&) = delete;

    // ===================================================================
    // Control (hilo de UI / IO). Todo lo de abajo toma el mutex PROPIO de la cancion: nunca el del
    // motor, porque decodificar un clip tarda y no debe frenar a nadie mas.
    // ===================================================================

    SongStatus loadClip(const char* path, int sampleRate, int64_t regionStart, int64_t regionEnd,
                        int64_t loopFrames, const SongContext& ctx, int32_t& outId) {
        if (path == nullptr || sampleRate <= 0 || loopFrames <= 0 || loopFrames > INT32_MAX) {
            return SongStatus::OutOfRange;
        }
        std::lock_guard<std::mutex> lock(mCtl);
        reclaimLocked(ctx.immediate);
        int slot = -1;
        for (int i = 0; i < kSongMaxClips; ++i) {
            if (mSlots[i].state == SlotState::Free) { slot = i; break; }
        }
        if (slot < 0) return SongStatus::OutOfRange;   // pool lleno (retirados pendientes incluidos)

        const size_t clipBytes = static_cast<size_t>(loopFrames) * 2 * sizeof(float);
        const size_t used = ctx.usedBytes + mReservedBytes.load(std::memory_order_relaxed);
        if (used + clipBytes > ctx.budgetBytes) return SongStatus::BudgetExceeded;

        DecodedStereo decoded;
        const ImportStatus st = decodeWavToRate(path, sampleRate, ctx.budgetBytes, used,
                                                &SongPlayer::denseReservationBytes, decoded);
        switch (st) {
            case ImportStatus::Ok: break;
            case ImportStatus::InvalidTrack:
            case ImportStatus::InvalidArgument: return SongStatus::OutOfRange;
            case ImportStatus::Io: return SongStatus::Io;
            case ImportStatus::UnsupportedFormat: return SongStatus::UnsupportedFormat;
            case ImportStatus::BudgetExceeded: return SongStatus::BudgetExceeded;
            case ImportStatus::OutOfMemory: return SongStatus::OutOfMemory;
        }
        try {
            Slot& s = mSlots[slot];
            s.storage = bakeSongClip(decoded.samples.data(), decoded.frames, regionStart, regionEnd,
                                     loopFrames, songFadeFrames(sampleRate));
            s.frames = loopFrames;
            s.sampleRate = sampleRate;
            s.state = SlotState::Loaded;
            mClips[slot].frames.store(loopFrames, std::memory_order_relaxed);
            mClips[slot].data.store(s.storage.data(), std::memory_order_release);
            mReservedBytes.fetch_add(clipBytes, std::memory_order_relaxed);
        } catch (const std::bad_alloc&) {
            return SongStatus::OutOfMemory;
        }
        outId = slot;
        return SongStatus::Ok;
    }

    SongStatus unloadClip(int32_t id, const SongContext& ctx) {
        std::lock_guard<std::mutex> lock(mCtl);
        reclaimLocked(ctx.immediate);
        if (id < 0 || id >= kSongMaxClips || mSlots[id].state != SlotState::Loaded) {
            return SongStatus::InvalidOperation;
        }
        if ((mPubMask >> id) & 1ULL) return SongStatus::InvalidOperation;   // la agenda lo usa
        mSlots[id].state = SlotState::Retired;
        mSlots[id].retireSeq = mSeq;
        reclaimLocked(ctx.immediate);
        return SongStatus::Ok;
    }

    /// `count == 0` apaga la cancion. Ver wma_song_set_schedule para el contrato de errores.
    SongStatus setSchedule(int64_t loopFrames, const SongSection* sections, int32_t count,
                           int64_t endFrame, const SongContext& ctx) {
        if (count < 0 || count > kSongMaxSections) return SongStatus::OutOfRange;
        std::lock_guard<std::mutex> lock(mCtl);
        reclaimLocked(ctx.immediate);
        const int64_t fade = fadeFor(ctx.sampleRate);

        if (count == 0) {
            if (mPubCount == 0) return SongStatus::Ok;
            publishLocked(1, nullptr, 0, 0, fade, 0);
            return SongStatus::Ok;
        }
        if (sections == nullptr || loopFrames <= 0 || loopFrames > INT32_MAX) {
            return SongStatus::OutOfRange;
        }
        // 1. forma de la agenda
        if (sections[0].startFrame < 0) return SongStatus::OutOfRange;
        for (int32_t k = 1; k < count; ++k) {
            if (sections[k].startFrame - sections[k - 1].startFrame < std::max<int64_t>(2 * fade, 1)) {
                return SongStatus::OutOfRange;   // no creciente, o una seccion < 2F
            }
        }
        if (endFrame - sections[count - 1].startFrame < std::max<int64_t>(2 * fade, 1)) {
            return SongStatus::OutOfRange;
        }
        // 2. pistas: un clip en una pista que no esta activa nunca sonaria
        for (int32_t k = 0; k < count; ++k) {
            for (int t = ctx.maxActiveTracks; t < kSongMaxTracks; ++t) {
                if (sections[k].clip[t] != kSongSilence) return SongStatus::OutOfRange;
            }
        }
        // 3. estado: grabando/armado, y que cada clip exista y sea de este largo y rate
        if (ctx.recordingBusy) return SongStatus::InvalidOperation;
        uint64_t mask = 0;
        for (int32_t k = 0; k < count; ++k) {
            for (int t = 0; t < kSongMaxTracks; ++t) {
                const int32_t id = sections[k].clip[t];
                if (id == kSongSilence) continue;
                if (id < 0 || id >= kSongMaxClips) return SongStatus::InvalidOperation;
                const Slot& s = mSlots[id];
                if (s.state != SlotState::Loaded || s.frames != loopFrames ||
                    s.sampleRate != ctx.sampleRate) {
                    return SongStatus::InvalidOperation;
                }
                mask |= 1ULL << id;
            }
        }
        publishLocked(loopFrames, sections, count, endFrame, fade, mask);
        return SongStatus::Ok;
    }

    /// Un cambio de sample rate invalida los clips (estan resampleados al viejo) y apaga la agenda.
    /// El llamador garantiza que el hilo RT esta quiesced (configureComponentsWithSampleRate).
    void invalidateForRate(int newSampleRate) {
        std::lock_guard<std::mutex> lock(mCtl);
        if (mPubCount > 0) publishLocked(1, nullptr, 0, 0, fadeFor(newSampleRate), 0);
        for (int i = 0; i < kSongMaxClips; ++i) {
            if (mSlots[i].state != SlotState::Free) freeSlotLocked(i);
        }
    }

    /// Libera lo retirado que el RT ya no puede estar leyendo. Devuelve los bytes que quedan.
    size_t reclaim(bool immediate) {
        std::lock_guard<std::mutex> lock(mCtl);
        reclaimLocked(immediate);
        return mReservedBytes.load(std::memory_order_relaxed);
    }

    /// Un render offline arranca de cero: sin colas de fundido ni smoothers del render anterior.
    void resetVoicesForRender() {
        for (int t = 0; t < kSongMaxTracks; ++t) mVoices[t] = Voice{};
        mTailUntil = 0;
    }

    size_t reservedBytes() const { return mReservedBytes.load(std::memory_order_relaxed); }
    bool active() const { return mActive.load(std::memory_order_acquire); }
    int32_t getSection() const { return mSection.load(std::memory_order_relaxed); }
    int64_t getLateAdoptions() const { return mLate.load(std::memory_order_relaxed); }

    // Observabilidad / ganchos de test (el unico llamador de produccion es la C API de test).
    int retiredClips() const {
        std::lock_guard<std::mutex> lock(mCtl);
        int n = 0;
        for (int i = 0; i < kSongMaxClips; ++i) n += mSlots[i].state == SlotState::Retired ? 1 : 0;
        return n;
    }
    int64_t blocksRun() const { return static_cast<int64_t>(mBlocksRun.get()); }
    int64_t adoptedAgendas() const { return static_cast<int64_t>(mAdopted.get()); }
    int64_t tornAgendas() const { return static_cast<int64_t>(mTorn.get()); }
    /// Hace que el RT verifique el checksum de cada agenda que adopta (cuesta O(count)).
    void setVerifyAgendas(bool on) { mVerify.store(on, std::memory_order_relaxed); }
    /// Fuerza F (frames) en las agendas que se publiquen desde ahora; < 0 vuelve al derivado del SR.
    void setFadeOverride(int frames) { mFadeOverride.store(frames, std::memory_order_relaxed); }

    // ===================================================================
    // RT. Nombres unicos a proposito: el walker de check-rt-safety.py resuelve por nombre.
    // ===================================================================

    /// Adopta la agenda fresca y deja listo el bloque. @return true si la cancion se hace cargo
    /// de las pistas en este bloque (agenda activa o una cola de fundido sonando).
    bool beginSongBlock(int64_t blockFrame, int numFrames) {
        mBlockFrame = blockFrame;
        mBlockFrames = numFrames;
        adoptSongSchedule(blockFrame);
        mBlockSection = songSectionAt(*mCur, blockFrame);
        return mCur->count > 0 || blockFrame < mTailUntil;
    }

    /// Mezcla (suma) la voz de `track` en `dst` (estereo intercalado, `n` frames del bloque).
    void renderSongVoice(int track, float* dst, int n, float volTarget, float panTarget,
                         bool muted) {
        if (track < 0 || track >= kSongMaxTracks || n <= 0) return;
        Voice& v = mVoices[track];
        const SongAgenda& a = *mCur;

        // Ganancia por bloque (vol x mute x paneo), la misma que TrackBuffer::mixInto: los tres
        // one-pole avanzan en forma cerrada al valor del final del bloque y la ganancia efectiva
        // rampea lineal entre los extremos.
        constexpr float kSmoothCoeff = 0.995f;
        const float muteTarget = muted ? 0.0f : 1.0f;
        const float alphaPow = std::pow(kSmoothCoeff, static_cast<float>(n));
        const float volEnd = v.vol == volTarget ? volTarget
                                                : alphaPow * v.vol + (1.0f - alphaPow) * volTarget;
        const float muteEnd = v.mute == muteTarget
                                  ? muteTarget
                                  : alphaPow * v.mute + (1.0f - alphaPow) * muteTarget;
        const float panEnd = v.pan == panTarget ? panTarget
                                                : alphaPow * v.pan + (1.0f - alphaPow) * panTarget;
        const auto& panLut = wm::EqualPowerPanLUT::instance();
        const auto pp0 = panLut.lookup(v.pan);
        const auto pp1 = panLut.lookup(panEnd);
        const float gS = v.vol * v.mute;
        const float gE = volEnd * muteEnd;
        const float invN = 1.0f / static_cast<float>(n);
        BlockGain bg;
        bg.dst = dst;
        bg.b0 = mBlockFrame;
        bg.gL = gS * pp0.l;
        bg.gR = gS * pp0.r;
        bg.gLinc = (gE * pp1.l - bg.gL) * invN;
        bg.gRinc = (gE * pp1.r - bg.gR) * invN;
        v.vol = volEnd;
        v.mute = muteEnd;
        v.pan = panEnd;

        const int64_t bEnd = mBlockFrame + n;
        int s = mBlockSection;
        int64_t f = mBlockFrame;
        while (f < bEnd) {
            const SongSegment g = songSegmentAt(a, s, track);
            const bool inFade = f >= g.fadeBegin;
            const bool late = f < v.lateEnd;
            int64_t e = std::min(bEnd, inFade ? g.bn : g.fadeBegin);
            if (late) e = std::min(e, v.lateEnd);
            if (!inFade && !late) {
                renderSongSteady(songClipData(g.cur, a.loopFrames),
                                 g.cur >= 0 ? songPosMod(f - g.startCur, a.loopFrames) : 0,
                                 a.loopFrames, e - f, f, bg);
            } else {
                renderSongSlow(v, a, g, inFade, f, e, bg);
            }
            f = e;
            if (f >= g.bn) s = g.sNext;
        }
        if (bEnd >= v.lateEnd) v.tailClip = kSongSilence;
    }

    /// Cierra el bloque: publica la seccion, la foto de lo que esta en uso y avanza el contador.
    void endSongBlock() {
        const int s = songSectionAt(*mCur, mBlockFrame + mBlockFrames - 1);
        mSection.store(s >= 0 && s < mCur->count ? s : -1, std::memory_order_relaxed);
        uint64_t mask = mCur->clipMask;
        for (int t = 0; t < kSongMaxTracks; ++t) {
            if (mVoices[t].tailClip >= 0 && mBlockFrame < mVoices[t].lateEnd) {
                mask |= 1ULL << mVoices[t].tailClip;
            }
        }
        mInUseMask.store(mask, std::memory_order_release);
        mMaskSeq.store(mAdoptedSeq, std::memory_order_release);
        mBlocksRun.bump();
    }

private:
    enum class SlotState { Free, Loaded, Retired };

    struct Slot {
        std::vector<float> storage;
        int64_t frames = 0;
        int sampleRate = 0;
        SlotState state = SlotState::Free;
        uint64_t retireSeq = 0;
    };

    /// Lo unico del clip que ve el RT.
    struct ClipView {
        std::atomic<const float*> data{nullptr};
        std::atomic<int64_t> frames{0};
    };

    struct Voice {
        float vol = 1.0f;
        float mute = 1.0f;
        float pan = 0.0f;
        int32_t tailClip = kSongSilence;   ///< carril de cola de una adopcion tardia
        int64_t tailPhase0 = 0;
        int64_t tailLoop = 1;
        int64_t lateStart = 0;
        int64_t lateEnd = 0;
        int64_t lateFade = 1;
        int lateKind = kSongFadeCos;
    };

    struct BlockGain {
        float* dst;
        int64_t b0;
        float gL, gR, gLinc, gRinc;
    };

    static constexpr uint32_t kDirty = 4u;

    static size_t denseReservationBytes(int frames) {
        return static_cast<size_t>(frames) * 2 * sizeof(float);
    }

    int64_t fadeFor(int sampleRate) const {
        const int o = mFadeOverride.load(std::memory_order_relaxed);
        return o >= 0 ? o : songFadeFrames(sampleRate);
    }

    void freeSlotLocked(int id) {
        Slot& s = mSlots[id];
        mClips[id].data.store(nullptr, std::memory_order_release);
        mClips[id].frames.store(0, std::memory_order_relaxed);
        mReservedBytes.fetch_sub(static_cast<size_t>(s.frames) * 2 * sizeof(float),
                                 std::memory_order_relaxed);
        std::vector<float>().swap(s.storage);
        s.frames = 0;
        s.state = SlotState::Free;
    }

    void reclaimLocked(bool immediate) {
        const uint64_t maskSeq = mMaskSeq.load(std::memory_order_acquire);
        const uint64_t inUse = mInUseMask.load(std::memory_order_acquire);
        for (int i = 0; i < kSongMaxClips; ++i) {
            Slot& s = mSlots[i];
            if (s.state != SlotState::Retired) continue;
            if (immediate || (maskSeq >= s.retireSeq && !((inUse >> i) & 1ULL))) freeSlotLocked(i);
        }
    }

    void publishLocked(int64_t loopFrames, const SongSection* sections, int32_t count,
                       int64_t endFrame, int64_t fade, uint64_t mask) {
        SongAgenda& a = mAgendas[mBack];
        a.loopFrames = loopFrames;
        a.endFrame = endFrame;
        a.fade = fade;
        a.clipMask = mask;
        a.seq = ++mSeq;
        a.count = count;
        for (int32_t k = 0; k < count; ++k) {
            a.starts[k] = sections[k].startFrame;
            std::memcpy(a.clips[k], sections[k].clip, sizeof(a.clips[k]));
        }
        a.checksum = songAgendaChecksum(a);
        const uint32_t old = mMiddle.exchange(static_cast<uint32_t>(mBack) | kDirty,
                                              std::memory_order_acq_rel);
        mBack = static_cast<int>(old & 3u);
        mPubMask = mask;
        mPubCount = count;
        mActive.store(count > 0, std::memory_order_release);
    }

    // ---- RT ----

    void adoptSongSchedule(int64_t blockFrame) {
        if (!(mMiddle.load(std::memory_order_relaxed) & kDirty)) return;
        // El estado viejo se lee ANTES del intercambio: despues, el slot viejo queda en el medio y
        // el control puede reescribirlo.
        SongState before[kSongMaxTracks];
        for (int t = 0; t < kSongMaxTracks; ++t) before[t] = songStateAt(*mCur, blockFrame, t);
        const uint32_t got = mMiddle.exchange(static_cast<uint32_t>(mFront),
                                              std::memory_order_acq_rel);
        mFront = static_cast<int>(got & 3u);
        mCur = &mAgendas[mFront];
        mAdoptedSeq = mCur->seq;
        mAdopted.bump();
        if (mVerify.load(std::memory_order_relaxed) && songAgendaChecksum(*mCur) != mCur->checksum) {
            mTorn.bump();
        }
        bool late = false;
        for (int t = 0; t < kSongMaxTracks; ++t) {
            const SongState now = songStateAt(*mCur, blockFrame, t);
            const SongState& was = before[t];
            const bool changed = was.clip != now.clip ||
                                 (was.clip >= 0 && (was.phase != now.phase || was.loop != now.loop));
            if (!changed) continue;
            late = true;
            Voice& v = mVoices[t];
            v.tailClip = was.clip;
            v.tailPhase0 = was.phase;
            v.tailLoop = was.loop;
            v.lateStart = blockFrame;
            v.lateFade = std::max<int64_t>(mCur->fade, 1);
            v.lateEnd = mCur->fade > 0 ? blockFrame + mCur->fade : blockFrame;
            v.lateKind = (was.clip >= 0 && now.clip >= 0) ? kSongFadeEqual : kSongFadeCos;
        }
        if (late && mCur->count > 0) mLate.fetch_add(1, std::memory_order_relaxed);
        int64_t until = 0;
        for (int t = 0; t < kSongMaxTracks; ++t) until = std::max(until, mVoices[t].lateEnd);
        mTailUntil = until;
    }

    const float* songClipData(int32_t id, int64_t loopFrames) const {
        if (id < 0 || id >= kSongMaxClips) return nullptr;
        const float* p = mClips[id].data.load(std::memory_order_acquire);
        if (p == nullptr || mClips[id].frames.load(std::memory_order_relaxed) != loopFrames) {
            return nullptr;
        }
        return p;
    }

    /// `count` frames de un clip estable (sin fundido), desde la posicion `pos` del clip.
    void renderSongSteady(const float* src, int64_t pos, int64_t loop, int64_t count, int64_t f,
                          BlockGain& c) const {
        if (src == nullptr) {
            c.gL += c.gLinc * static_cast<float>(count);
            c.gR += c.gRinc * static_cast<float>(count);
            return;
        }
        int64_t i = f - c.b0;
        while (count > 0) {
            const int64_t run = std::min(count, loop - pos);
            for (int64_t k = 0; k < run; ++k) {
                c.dst[(i + k) * 2] += src[(pos + k) * 2] * c.gL;
                c.dst[(i + k) * 2 + 1] += src[(pos + k) * 2 + 1] * c.gR;
                c.gL += c.gLinc;
                c.gR += c.gRinc;
            }
            i += run;
            count -= run;
            pos = 0;
        }
    }

    /// Frame a frame: dentro de un fundido de borde y/o de la cola de una adopcion tardia.
    void renderSongSlow(const Voice& v, const SongAgenda& a, const SongSegment& g, bool inFade,
                        int64_t f, int64_t e, BlockGain& c) const {
        const int64_t loop = a.loopFrames;
        const float* curP = songClipData(g.cur, loop);
        const float* nxtP = inFade ? songClipData(g.nxt, loop) : nullptr;
        const float* tailP = f < v.lateEnd ? songClipData(v.tailClip, v.tailLoop) : nullptr;
        for (int64_t x = f; x < e; ++x) {
            float l = 0.0f;
            float r = 0.0f;
            if (inFade) {
                float gOut = 0.0f;
                float gIn = 0.0f;
                songFadeGains(g.kind, x - g.fadeBegin, a.fade, gOut, gIn);
                if (curP != nullptr) {
                    const int64_t p = songPosMod(x - g.startCur, loop) * 2;
                    l += curP[p] * gOut;
                    r += curP[p + 1] * gOut;
                }
                if (nxtP != nullptr) {
                    const int64_t p = songPosMod(x - g.startNxt, loop) * 2;
                    l += nxtP[p] * gIn;
                    r += nxtP[p + 1] * gIn;
                }
            } else if (curP != nullptr) {
                const int64_t p = songPosMod(x - g.startCur, loop) * 2;
                l = curP[p];
                r = curP[p + 1];
            }
            if (x < v.lateEnd) {
                float gOut = 0.0f;
                float gIn = 0.0f;
                songFadeGains(v.lateKind, x - v.lateStart, v.lateFade, gOut, gIn);
                l *= gIn;
                r *= gIn;
                if (tailP != nullptr) {
                    const int64_t p = songPosMod(v.tailPhase0 + (x - v.lateStart), v.tailLoop) * 2;
                    l += tailP[p] * gOut;
                    r += tailP[p + 1] * gOut;
                }
            }
            const int64_t i = x - c.b0;
            c.dst[i * 2] += l * c.gL;
            c.dst[i * 2 + 1] += r * c.gR;
            c.gL += c.gLinc;
            c.gR += c.gRinc;
        }
    }

    // ---- estado ----

    // Control (bajo mCtl)
    mutable std::mutex mCtl;
    Slot mSlots[kSongMaxClips];
    int mBack = 2;
    uint64_t mSeq = 0;
    uint64_t mPubMask = 0;
    int32_t mPubCount = 0;

    // Compartido
    std::unique_ptr<SongAgenda[]> mAgendas;   // 3 slots preasignados
    std::atomic<uint32_t> mMiddle{1};          // indice del slot del medio (| kDirty si es fresco)
    ClipView mClips[kSongMaxClips];
    std::atomic<size_t> mReservedBytes{0};
    std::atomic<bool> mActive{false};
    std::atomic<int32_t> mSection{-1};
    std::atomic<int64_t> mLate{0};
    std::atomic<uint64_t> mInUseMask{0};
    std::atomic<uint64_t> mMaskSeq{0};
    std::atomic<bool> mVerify{false};
    std::atomic<int> mFadeOverride{-1};
    wma::RtCounter mBlocksRun;
    wma::RtCounter mAdopted;
    wma::RtCounter mTorn;

    // RT
    int mFront = 0;
    const SongAgenda* mCur = nullptr;
    uint64_t mAdoptedSeq = 0;
    int64_t mBlockFrame = 0;
    int mBlockFrames = 0;
    int mBlockSection = 0;
    int64_t mTailUntil = 0;
    Voice mVoices[kSongMaxTracks];
};

}  // namespace wm
