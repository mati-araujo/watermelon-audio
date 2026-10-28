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
 *    TrackBuffer::mixInto) y una pila de COLAS para el fundido de las adopciones tardias.
 *
 * Fundido F = max(64, round(0,005 * SR)) en [B - F, B) antes de cada borde B: equal-power (cuarto
 * de seno/coseno) para cambio de clip o salto de fase, coseno elevado para entrar/salir de
 * silencio; nada si el mismo clip sigue alineado. El estado de B rige exacto desde B.
 *
 * Adopcion tardia: si lo que suena EN EL FRAME DE ADOPCION t cambia (incluido un fundido de borde
 * ya empezado), la voz congela el SEGMENTO viejo (cur, nxt, bordes, fundido) y lo renderiza como una
 * cola: exactamente lo que renderizaba la agenda vieja, a ganancia gOut en [t, t + F) mientras lo
 * nuevo entra con gIn. Una segunda adopcion dentro de la ventana apila otra cola; el peso de cada
 * capa es el producto de las ganancias de los eventos que la alcanzan, asi que es recursivamente
 * correcto con un tope fijo (kSongMaxTails) y sin allocation.
 *
 * Liberacion de clips: sacar un clip de la agenda no lo libera. El RT publica al final de cada
 * bloque que agenda adopto y que clips tiene en uso (la agenda adoptada mas las colas); el
 * control libera recien cuando esa foto es posterior a la baja y el clip no figura. En offline no
 * hay hilo RT concurrente y se libera de inmediato; con el stream parado (pista del motor) se
 * libera tras verificar, con un handshake Dekker, que el RT no esta dentro de un bloque.
 */

#include "LooperExporter.h"
#include "TrackBuffer.h"
#include "../platform/RtCounter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace wm {

constexpr int kSongMaxTracks = 16;      // = AudioLooper::MAX_TRACKS_HW (static_assert alla)
constexpr int kSongMaxClips = 64;
constexpr int kSongMaxSections = 1024;
constexpr int kSongMaxTails = 8;        // adopciones tardias encadenadas dentro de una ventana F
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
    bool rtStopped = false;      ///< pista del motor: sin stream. Se verifica antes de liberar
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

/// Un tramo esta dentro de su fundido de borde en `t` (a ganancia de entrada > 0).
inline bool songSegmentInFade(const SongSegment& g, int64_t t) {
    return g.kind != kSongFadeNone && t > g.fadeBegin;
}

/// Suena algo en `t`: un clip de la seccion o, dentro del fundido, el de la siguiente.
inline bool songSegmentAudibleAt(const SongSegment& g, int64_t t) {
    return g.cur >= 0 || (songSegmentInFade(g, t) && g.nxt >= 0);
}

/// Los dos tramos dan la MISMA salida en `t`: si es asi, la agenda nueva toma el relevo sin
/// escalon (lo que difiera mas adelante entra desde ganancia 0, o lo trae la agenda misma).
inline bool songSegmentsAlikeAt(const SongSegment& o, int64_t oLoop, int64_t oFade,
                                const SongSegment& n, int64_t nLoop, int64_t nFade, int64_t t) {
    if (o.cur != n.cur) return false;
    if (o.cur >= 0 && (oLoop != nLoop || songPosMod(t - o.startCur, oLoop) !=
                                             songPosMod(t - n.startCur, nLoop))) {
        return false;
    }
    const bool oIn = songSegmentInFade(o, t);
    if (oIn != songSegmentInFade(n, t)) return false;
    if (!oIn) return true;
    if (o.nxt != n.nxt || o.kind != n.kind || o.fadeBegin != n.fadeBegin || oFade != nFade) {
        return false;
    }
    return o.nxt < 0 || (oLoop == nLoop && songPosMod(t - o.startNxt, oLoop) ==
                                               songPosMod(t - n.startNxt, nLoop));
}

/// Una capa de cola: el tramo que renderizaba la agenda vieja en el frame de adopcion `evStart`,
/// congelado, y el evento (la adopcion) que lo retira.
struct SongTail {
    SongSegment seg;
    int64_t loop;       ///< L de la agenda vieja
    int64_t segFade;    ///< F de la agenda vieja (su propio fundido de borde)
    int64_t evStart;    ///< frame de la adopcion
    int64_t evFade;     ///< F de la agenda nueva: duracion de la ventana
    int64_t evEnd;      ///< evStart + evFade
    int evKind;         ///< equal-power si suenan los dos lados, coseno elevado si no
};

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
        // Una decodificacion a la vez: el presupuesto se mide contra los clips ya instalados, y
        // dos decodificaciones simultaneas sumarian su pico transitorio sin que nadie lo cuente.
        // Ni set_schedule ni unload ni el cambio de sample rate esperan este mutex.
        std::lock_guard<std::mutex> serial(mLoadSerial);

        const size_t clipBytes = static_cast<size_t>(loopFrames) * 2 * sizeof(float);
        int slot = -1;
        uint64_t ticket = 0;
        size_t used = 0;
        {
            std::lock_guard<std::mutex> lock(mCtl);
            reclaimLocked(ctx.immediate, ctx.rtStopped);
            for (int i = 0; i < kSongMaxClips; ++i) {
                if (mSlots[i].state == SlotState::Free) { slot = i; break; }
            }
            if (slot < 0) return SongStatus::OutOfRange;   // pool lleno (retirados pendientes incluidos)
            used = ctx.usedBytes + mReservedBytes.load(std::memory_order_relaxed);
            if (used + clipBytes > ctx.budgetBytes) return SongStatus::BudgetExceeded;
            // Se reserva el slot y los bytes bajo el lock; decodificar y hornear ocurre afuera.
            Slot& s = mSlots[slot];
            s.state = SlotState::Reserved;
            s.frames = loopFrames;
            ticket = s.ticket = ++mTicketCounter;
            mReservedBytes.fetch_add(clipBytes, std::memory_order_relaxed);
        }

        if (mLoadHook) mLoadHook();
        DecodedStereo decoded;
        const ImportStatus st = decodeWavToRate(path, sampleRate, ctx.budgetBytes, used,
                                                &SongPlayer::denseReservationBytes, decoded);
        SongStatus result = SongStatus::Ok;
        std::vector<float> baked;
        switch (st) {
            case ImportStatus::Ok: break;
            case ImportStatus::InvalidTrack:
            case ImportStatus::InvalidArgument: result = SongStatus::OutOfRange; break;
            case ImportStatus::Io: result = SongStatus::Io; break;
            case ImportStatus::UnsupportedFormat: result = SongStatus::UnsupportedFormat; break;
            case ImportStatus::BudgetExceeded: result = SongStatus::BudgetExceeded; break;
            case ImportStatus::OutOfMemory: result = SongStatus::OutOfMemory; break;
        }
        if (result == SongStatus::Ok) {
            try {
                baked = bakeSongClip(decoded.samples.data(), decoded.frames, regionStart, regionEnd,
                                     loopFrames, songFadeFrames(sampleRate));
            } catch (const std::bad_alloc&) {
                result = SongStatus::OutOfMemory;
            }
        }

        std::lock_guard<std::mutex> lock(mCtl);
        Slot& s = mSlots[slot];
        if (s.state != SlotState::Reserved || s.ticket != ticket) {
            // invalidateForRate libero la reserva mientras se decodificaba: el clip esta
            // resampleado al rate viejo y ya no sirve.
            return SongStatus::InvalidOperation;
        }
        if (result != SongStatus::Ok) {
            freeSlotLocked(slot);
            return result;
        }
        s.storage = std::move(baked);
        s.sampleRate = sampleRate;
        s.state = SlotState::Loaded;
        mClips[slot].frames.store(loopFrames, std::memory_order_relaxed);
        mClips[slot].data.store(s.storage.data(), std::memory_order_release);
        outId = slot;
        return SongStatus::Ok;
    }

    SongStatus unloadClip(int32_t id, const SongContext& ctx) {
        std::lock_guard<std::mutex> lock(mCtl);
        reclaimLocked(ctx.immediate, ctx.rtStopped);
        if (id < 0 || id >= kSongMaxClips || mSlots[id].state != SlotState::Loaded) {
            return SongStatus::InvalidOperation;
        }
        if ((mPubMask >> id) & 1ULL) return SongStatus::InvalidOperation;   // la agenda lo usa
        mSlots[id].state = SlotState::Retired;
        mSlots[id].retireSeq = mSeq;
        reclaimLocked(ctx.immediate, ctx.rtStopped);
        return SongStatus::Ok;
    }

    /// `count == 0` apaga la cancion. Ver wma_song_set_schedule para el contrato de errores.
    SongStatus setSchedule(int64_t loopFrames, const SongSection* sections, int32_t count,
                           int64_t endFrame, const SongContext& ctx) {
        if (count < 0 || count > kSongMaxSections) return SongStatus::OutOfRange;
        std::lock_guard<std::mutex> lock(mCtl);
        reclaimLocked(ctx.immediate, ctx.rtStopped);
        const int64_t fade = fadeFor(ctx.sampleRate);

        if (count == 0) {
            if (mPubCount == 0) return SongStatus::Ok;
            publishLocked(1, nullptr, 0, 0, fade, 0);
            return SongStatus::Ok;
        }
        if (sections == nullptr || loopFrames <= 0 || loopFrames > INT32_MAX) {
            return SongStatus::OutOfRange;
        }
        // 1. forma de la agenda. Estrictamente creciente ANTES de restar: con starts[0] >= 0 y cada
        // start mayor que el anterior, ninguna resta desborda (INT64_MIN - 1 era UB y pasaba).
        // end_frame < INT64_MAX: ese valor es el centinela de "sin borde siguiente".
        const int64_t minSection = std::max<int64_t>(2 * fade, 1);
        if (sections[0].startFrame < 0) return SongStatus::OutOfRange;
        for (int32_t k = 1; k < count; ++k) {
            const int64_t prev = sections[k - 1].startFrame;
            if (sections[k].startFrame <= prev || sections[k].startFrame - prev < minSection) {
                return SongStatus::OutOfRange;   // no creciente, o una seccion < 2F
            }
        }
        const int64_t last = sections[count - 1].startFrame;
        if (endFrame <= last || endFrame == INT64_MAX || endFrame - last < minSection) {
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
    /// Libera tambien las reservas de cargas en curso: su `loadClip` termina con InvalidOperation.
    void invalidateForRate(int newSampleRate) {
        std::lock_guard<std::mutex> lock(mCtl);
        if (mPubCount > 0) publishLocked(1, nullptr, 0, 0, fadeFor(newSampleRate), 0);
        mScrub.store(true, std::memory_order_release);
        for (int i = 0; i < kSongMaxClips; ++i) {
            if (mSlots[i].state != SlotState::Free) freeSlotLocked(i);
        }
    }

    /// Libera lo retirado que el RT ya no puede estar leyendo. Devuelve los bytes que quedan.
    size_t reclaim(bool immediate, bool rtStopped = false) {
        std::lock_guard<std::mutex> lock(mCtl);
        reclaimLocked(immediate, rtStopped);
        return mReservedBytes.load(std::memory_order_relaxed);
    }

    /// Un render offline arranca de un estado canonico, el de una instancia fresca: voces y colas
    /// limpias, flags de scrub bajos y la agenda APAGADA (vacia, sin publicacion pendiente). Asi el
    /// comienzo depende solo de las llamadas hechas desde el start: una agenda se publica despues.
    /// Los clips cargados no se tocan. El llamador garantiza que no hay RT (offline, stream parado).
    void resetVoicesForRender() {
        std::lock_guard<std::mutex> lock(mCtl);
        for (int t = 0; t < kSongMaxTracks; ++t) mVoices[t] = Voice{};
        mTailUntil = 0;
        for (int i = 0; i < 3; ++i) {
            SongAgenda& a = mAgendas[i];
            a.loopFrames = 1;
            a.endFrame = 0;
            a.fade = 0;
            a.clipMask = 0;
            a.seq = mSeq;
            a.count = 0;
            a.checksum = songAgendaChecksum(a);
        }
        mFront = 0;
        mBack = 2;
        mMiddle.store(1, std::memory_order_release);
        mCur = &mAgendas[0];
        mAdoptedSeq = mSeq;
        mPubMask = 0;
        mPubCount = 0;
        mActive.store(false, std::memory_order_release);
        mSection.store(-1, std::memory_order_relaxed);
        mInUseMask.store(0, std::memory_order_release);
        mMaskSeq.store(mSeq, std::memory_order_release);
        mScrub.store(false, std::memory_order_release);
        mSkipTailOnce = false;
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
    /// Gancho de test: corre en loadClip justo antes de decodificar. Se fija antes de cargar.
    void setLoadHook(std::function<void()> hook) { mLoadHook = std::move(hook); }

    // ===================================================================
    // RT. Nombres unicos a proposito: el walker de check-rt-safety.py resuelve por nombre.
    // ===================================================================

    /// Adopta la agenda fresca y deja listo el bloque. @return true si la cancion se hace cargo
    /// de las pistas en este bloque (agenda activa o una cola de fundido sonando).
    bool beginSongBlock(int64_t blockFrame, int numFrames) {
        // Handshake con reclaimWhileIdleLocked: primero se publica "estoy adentro", despues se mira
        // si el control esta liberando. Si lo esta, este bloque no lee ningun clip.
        mRtInBlock.store(true, std::memory_order_seq_cst);
        mClipsFrozen = mFreeze.load(std::memory_order_seq_cst);
        if (mScrub.load(std::memory_order_relaxed) && mScrub.exchange(false, std::memory_order_acq_rel)) {
            scrubSongTails();
            mSkipTailOnce = true;
        }
        mBlockFrame = blockFrame;
        mBlockFrames = numFrames;
        adoptSongSchedule(blockFrame);
        mSkipTailOnce = false;   // vale para UN bloque: si no adopto nada, no se arrastra
        pruneSongTails(blockFrame);
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
            const bool late = f < v.tailEnd;
            int64_t e = std::min(bEnd, inFade ? g.bn : g.fadeBegin);
            if (late) e = std::min(e, v.tailEnd);
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
        if (bEnd >= v.tailEnd) {
            v.tails = 0;
            v.tailEnd = 0;
        }
    }

    /// Cierra el bloque: publica la seccion, la foto de lo que esta en uso y avanza el contador.
    void endSongBlock() {
        const int s = songSectionAt(*mCur, mBlockFrame + mBlockFrames - 1);
        mSection.store(s >= 0 && s < mCur->count ? s : -1, std::memory_order_relaxed);
        uint64_t tails = 0;
        for (int t = 0; t < kSongMaxTracks; ++t) {
            const Voice& v = mVoices[t];
            if (mBlockFrame >= v.tailEnd) continue;
            for (int j = 0; j < v.tails; ++j) {
                if (v.tail[j].seg.cur >= 0) tails |= 1ULL << v.tail[j].seg.cur;
                if (v.tail[j].seg.nxt >= 0) tails |= 1ULL << v.tail[j].seg.nxt;
            }
        }
        mInUseMask.store(mCur->clipMask | tails, std::memory_order_release);
        mMaskSeq.store(mAdoptedSeq, std::memory_order_release);
        mBlocksRun.bump();
        mRtInBlock.store(false, std::memory_order_release);
    }

private:
    enum class SlotState { Free, Reserved, Loaded, Retired };   // Reserved: decodificando, sin instalar

    struct Slot {
        std::vector<float> storage;
        int64_t frames = 0;
        int sampleRate = 0;
        SlotState state = SlotState::Free;
        uint64_t retireSeq = 0;
        uint64_t ticket = 0;   ///< identidad de la reserva: la carga sabe si la invalidaron
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
        int tails = 0;         ///< capas de cola vivas, la mas vieja primero
        int64_t tailEnd = 0;   ///< fin de la ventana del evento mas tardio
        SongTail tail[kSongMaxTails]{};
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
        s.ticket = 0;
        s.state = SlotState::Free;
    }

    void reclaimLocked(bool immediate, bool rtStopped) {
        const uint64_t maskSeq = mMaskSeq.load(std::memory_order_acquire);
        const uint64_t inUse = mInUseMask.load(std::memory_order_acquire);
        bool pending = false;
        for (int i = 0; i < kSongMaxClips; ++i) {
            Slot& s = mSlots[i];
            if (s.state != SlotState::Retired) continue;
            // Offline no hay RT concurrente, pero la agenda adoptada y las colas de fundido siguen
            // leyendo el clip, y la proxima adopcion congela la adoptada como cola.
            if ((immediate || maskSeq >= s.retireSeq) && !((inUse >> i) & 1ULL)) {
                freeSlotLocked(i);
            } else {
                pending = true;
            }
        }
        if (pending && rtStopped) reclaimWhileIdleLocked();
    }

    /// Con el stream parado la foto del RT no avanza y la baja espera para siempre. "Parado" es una
    /// pista del motor, no una prueba: se verifica con un handshake Dekker (todo seq_cst). El
    /// control levanta mFreeze y recien despues mira mRtInBlock; el RT sube mRtInBlock y recien
    /// despues mira mFreeze. En el orden total, o el control ve el bloque abierto (y no libera) o
    /// el RT ve el congelamiento (y ese bloque no lee ningun clip). Las colas del RT pueden apuntar
    /// a ids que se liberan y se reusan: mScrub las descarta antes de la proxima adopcion.
    void reclaimWhileIdleLocked() {
        mFreeze.store(true, std::memory_order_seq_cst);
        if (!mRtInBlock.load(std::memory_order_seq_cst)) {
            mScrub.store(true, std::memory_order_release);
            for (int i = 0; i < kSongMaxClips; ++i) {
                if (mSlots[i].state == SlotState::Retired) freeSlotLocked(i);
            }
        }
        mFreeze.store(false, std::memory_order_release);
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
        // El tramo viejo se lee ANTES del intercambio: despues, el slot viejo queda en el medio y
        // el control puede reescribirlo.
        SongSegment before[kSongMaxTracks];
        const int64_t oldLoop = mCur->loopFrames;
        const int64_t oldFade = mCur->fade;
        const int sOld = songSectionAt(*mCur, blockFrame);
        for (int t = 0; t < kSongMaxTracks; ++t) before[t] = songSegmentAt(*mCur, sOld, t);
        const uint32_t got = mMiddle.exchange(static_cast<uint32_t>(mFront),
                                              std::memory_order_acq_rel);
        mFront = static_cast<int>(got & 3u);
        mCur = &mAgendas[mFront];
        mAdoptedSeq = mCur->seq;
        mAdopted.bump();
        if (mVerify.load(std::memory_order_relaxed) && songAgendaChecksum(*mCur) != mCur->checksum) {
            mTorn.bump();
        }
        const int64_t fade = mCur->fade;
        const int sNew = songSectionAt(*mCur, blockFrame);
        // Tras un liberado con el stream parado la agenda vieja puede nombrar ids que ya son otros
        // clips: esta adopcion no puede congelarla como cola (se pierde, una vez, ese fundido).
        const bool skipTails = mSkipTailOnce;
        mSkipTailOnce = false;
        bool late = false;
        const int tailTracks = skipTails ? 0 : kSongMaxTracks;
        for (int t = 0; t < tailTracks; ++t) {
            const SongSegment now = songSegmentAt(*mCur, sNew, t);
            if (songSegmentsAlikeAt(before[t], oldLoop, oldFade, now, mCur->loopFrames, fade,
                                    blockFrame)) {
                continue;
            }
            late = true;
            if (fade > 0) {
                const bool both = songSegmentAudibleAt(before[t], blockFrame) &&
                                  songSegmentAudibleAt(now, blockFrame);
                pushSongTail(mVoices[t], before[t], oldLoop, oldFade, blockFrame, fade,
                             both ? kSongFadeEqual : kSongFadeCos);
            }
        }
        if (late && mCur->count > 0) mLate.fetch_add(1, std::memory_order_relaxed);
    }

    /// Apila la capa de cola. Con el tope lleno cae la mas vieja (su peso ya es el producto de
    /// kSongMaxTails ganancias de salida: inaudible salvo adopciones de a pocos frames).
    void pushSongTail(Voice& v, const SongSegment& seg, int64_t loop, int64_t segFade, int64_t t,
                      int64_t fade, int kind) {
        if (v.tails == kSongMaxTails) {
            for (int i = 1; i < kSongMaxTails; ++i) v.tail[i - 1] = v.tail[i];
            v.tails = kSongMaxTails - 1;
        }
        SongTail& tl = v.tail[v.tails++];
        tl.seg = seg;
        tl.loop = loop;
        tl.segFade = segFade;
        tl.evStart = t;
        tl.evFade = fade;
        tl.evEnd = t + fade;
        tl.evKind = kind;
        v.tailEnd = std::max(v.tailEnd, tl.evEnd);
    }

    /// Descarta las capas que ya no pesan: la capa j se apaga cuando termina cualquier evento
    /// k >= j (su gOut llego a 0); lo que queda se renumera y la mas vieja deja de tener gIn.
    void pruneSongTails(int64_t blockFrame) {
        int64_t until = 0;
        for (int t = 0; t < kSongMaxTracks; ++t) {
            Voice& v = mVoices[t];
            int k = v.tails - 1;
            while (k >= 0 && v.tail[k].evEnd > blockFrame) --k;
            if (k >= 0) {
                const int drop = k + 1;
                for (int i = drop; i < v.tails; ++i) v.tail[i - drop] = v.tail[i];
                v.tails -= drop;
            }
            v.tailEnd = 0;
            for (int j = 0; j < v.tails; ++j) v.tailEnd = std::max(v.tailEnd, v.tail[j].evEnd);
            until = std::max(until, v.tailEnd);
        }
        mTailUntil = until;
    }

    /// Un liberado con el stream parado puede dejar ids reusados: las colas viejas se tiran.
    void scrubSongTails() {
        for (int t = 0; t < kSongMaxTracks; ++t) {
            mVoices[t].tails = 0;
            mVoices[t].tailEnd = 0;
        }
        mTailUntil = 0;
    }

    const float* songClipData(int32_t id, int64_t loopFrames) const {
        if (mClipsFrozen || id < 0 || id >= kSongMaxClips) return nullptr;
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

    /// Un tramo congelado de la agenda vieja en el frame `x`: cur antes del fundido, la mezcla
    /// cur/nxt dentro y nxt estable desde el borde.
    void songTailFrame(const SongTail& tl, const float* curP, const float* nxtP, int64_t x,
                       float& l, float& r) const {
        const SongSegment& g = tl.seg;
        l = 0.0f;
        r = 0.0f;
        if (x < g.fadeBegin) {
            if (curP != nullptr) {
                const int64_t p = songPosMod(x - g.startCur, tl.loop) * 2;
                l = curP[p];
                r = curP[p + 1];
            }
        } else if (x >= g.bn) {
            if (nxtP != nullptr) {
                const int64_t p = songPosMod(x - g.startNxt, tl.loop) * 2;
                l = nxtP[p];
                r = nxtP[p + 1];
            }
        } else {
            float gOut = 0.0f;
            float gIn = 0.0f;
            songFadeGains(g.kind, x - g.fadeBegin, tl.segFade, gOut, gIn);
            if (curP != nullptr) {
                const int64_t p = songPosMod(x - g.startCur, tl.loop) * 2;
                l += curP[p] * gOut;
                r += curP[p + 1] * gOut;
            }
            if (nxtP != nullptr) {
                const int64_t p = songPosMod(x - g.startNxt, tl.loop) * 2;
                l += nxtP[p] * gIn;
                r += nxtP[p + 1] * gIn;
            }
        }
    }

    /// Frame a frame: dentro de un fundido de borde y/o de las colas de adopciones tardias.
    /// salida = vivo * gIn_ultimo + sum_j capa_j * (gIn_{j-1} * prod_{k>=j} gOut_k).
    void renderSongSlow(const Voice& v, const SongAgenda& a, const SongSegment& g, bool inFade,
                        int64_t f, int64_t e, BlockGain& c) const {
        const int64_t loop = a.loopFrames;
        const float* curP = songClipData(g.cur, loop);
        const float* nxtP = inFade ? songClipData(g.nxt, loop) : nullptr;
        const int n = f < v.tailEnd ? v.tails : 0;
        const float* tailCur[kSongMaxTails];
        const float* tailNxt[kSongMaxTails];
        for (int j = 0; j < n; ++j) {
            tailCur[j] = songClipData(v.tail[j].seg.cur, v.tail[j].loop);
            tailNxt[j] = songClipData(v.tail[j].seg.nxt, v.tail[j].loop);
        }
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
            if (n > 0) {
                float evOut[kSongMaxTails];
                float evIn[kSongMaxTails];
                for (int k = 0; k < n; ++k) {
                    const SongTail& tl = v.tail[k];
                    if (x >= tl.evEnd) {
                        evOut[k] = 0.0f;
                        evIn[k] = 1.0f;
                    } else {
                        songFadeGains(tl.evKind, x - tl.evStart, tl.evFade, evOut[k], evIn[k]);
                    }
                }
                l *= evIn[n - 1];
                r *= evIn[n - 1];
                float weight = 1.0f;
                for (int j = n - 1; j >= 0; --j) {
                    weight *= evOut[j];
                    if (weight == 0.0f) break;
                    float tl = 0.0f;
                    float tr = 0.0f;
                    songTailFrame(v.tail[j], tailCur[j], tailNxt[j], x, tl, tr);
                    const float w = j >= 1 ? weight * evIn[j - 1] : weight;
                    l += tl * w;
                    r += tr * w;
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
    std::mutex mLoadSerial;   // orden de toma: mLoadSerial -> mCtl
    uint64_t mTicketCounter = 0;
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
    std::atomic<bool> mFreeze{false};     // control: "estoy liberando sin esperar al RT"
    std::atomic<bool> mRtInBlock{false};  // RT: dentro de un bloque de la cancion
    std::atomic<bool> mScrub{false};      // control -> RT: descarta las colas antes de adoptar
    std::atomic<bool> mVerify{false};
    std::function<void()> mLoadHook;
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
    bool mSkipTailOnce = false;
    bool mClipsFrozen = false;   // este bloque no lee clips (el control esta liberando)
    Voice mVoices[kSongMaxTracks];
};

}  // namespace wm
