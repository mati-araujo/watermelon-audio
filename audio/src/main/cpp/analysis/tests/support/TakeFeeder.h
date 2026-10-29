/**
 * TakeFeeder.h — REQ-055 S0 · 0.3 (D37–D40, D42). EL ALIMENTADOR de una toma real del afinador: la
 * pasa por el motor como la vio producción, con sus cortes y sus cambios de objetivo.
 *
 * UNA PASADA (D37). El archivo entero, una vez (sin `noteEndFrames`, sin duplicar), por `AnalysisRing`
 * + `AnalysisThread::drainOnce()` sin hilo, como `feedTwice`, SIN candidatos (el harness que graba no
 * los empuja). Tandas de `kTakeChunkFrames` (256) en la grilla desde el frame 0; solo la tanda que
 * contiene un corte, el FIN de una costura o un objetivo se parte en dos AHI (la grilla no se
 * re-alinea). Tras cada pieza se drena hasta `kRingEmpty` y se registra cada publicacion
 * (pos = `readPosition()`). INVARIANTE: todo frame del archivo se escribe al ring, una vez y en orden:
 * `writePosition()` == frame del archivo, y `pub.pos`, los t0 del oraculo y los cortes estan en la
 * misma coordenada. La mezcla `0,5f·(x + x)` del mono duplicado por `readWav` es `x` bit a bit.
 *
 * EL AVISO DE DISCONTINUIDAD (D38). Por `AnalysisRing::reportCaptureDiscontinuity(framesAhead)`, la
 * puerta publica por la que producción llega a la guarda de `drainOnce` (el alimentador es el unico
 * escritor del ring, como pide su contrato). En INI de cada corte, con el ring drenado:
 * `report(FIN − INI)`.
 *   · Perdida (FIN = INI): ademas un `drainOnce()` con el ring vacio, que cruza la costura en el acto:
 *     reinicia la integracion SIN descartar nada (el eje del ring en producción).
 *   · Costura: el motor descarta (`kSkipped`) hasta `readPosition() >= FIN` y reinicia; con la pieza
 *     partida en FIN descarta exactamente [INI, FIN).
 *   · Una perdida dentro de una costura pendiente: el maximo del ring la absorbe (no adelanta FIN).
 *   · Corte en el frame 0: sin aviso (no hay integracion que cortar).
 *
 * EL OBJETIVO (D39, D42). En FRAME, con el ring drenado: `setTargetHz(HZ)` + `drainOnce()` vacio (su
 * `skipToNewest` no descarta nada). Si coincide con un corte, va DESPUES del aviso. ANTES de aplicarlo,
 * `applyTakeTarget` mide el RMS del archivo en [FRAME − 0,5 s, FRAME) sin cruzar un corte (si lo
 * cruza, solo el lado de FRAME): por encima de `kTakeTargetSilenceDbfs` la corrida FALLA con el FRAME
 * y el nivel. Nunca se aplica un objetivo sobre sonido sin avisar.
 */
#pragma once

#include "LatchInstrument.h"
#include "TakeManifest.h"

#include "../../AnalysisRing.h"
#include "../../AnalysisSnapshot.h"
#include "../../AnalysisThread.h"
#include "../../../looper/WavFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace wma_test::take {

/// La tanda de la grilla, en frames (la del instrumento por ataque).
inline constexpr int kTakeChunkFrames = 256;

/// D42: un `# objetivo` cae en silencio si el RMS en [FRAME − 0,5 s, FRAME) es <= esto (8 dB sobre
/// la sala medida en el g42, −48,5 dBFS, con las cuerdas apagadas de D8).
inline constexpr double kTakeTargetSilenceDbfs = -40.0;

/// La ventana de silencio previa a un objetivo, en segundos.
inline constexpr double kTakeTargetSilenceSeconds = 0.5;

/// D40: un ataque a <= esto de un `# aplauso` es el aplauso (la tolerancia del detector del oraculo).
inline constexpr int64_t kTakeClapToleranceFrames = 256;

struct TakeFeedRun {
    int rate = 0;
    int64_t frames = 0;   ///< frames escritos al ring (== los del archivo si `error` esta vacio)
    std::vector<latch::Publication> pubs;
    /// El snapshot completo de cada publicacion, paralelo a `pubs` (contador de discontinuidades,
    /// frames analizados, marca de discontinuidad...).
    std::vector<std::array<float, wma::analysis::kSnapshotValueCount>> snaps;
    std::vector<latch::TargetEvent> events;
    /// Donde termino cada pieza escrita al ring (posiciones de escritura, en orden).
    std::vector<int64_t> pieceEnds;
    /// Lo que se escribio al ring, como mono (`appendMonoMix`, tanda a tanda).
    std::vector<float> fedMono;
    /// `ring.writePosition()` al terminar.
    int64_t ringWritePosition = 0;
    /// Por que fallo la corrida ("" = no fallo). Con error la corrida se detiene ahi.
    std::string error;
};

/// El mono del archivo como lo ve el ring: `0,5f·(L + R)`.
inline std::vector<float> takeMonoOf(const wav::WavData& data) {
    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(data.numFrames));
    latch::appendMonoMix(&mono, data.buffer.data(), data.numFrames);
    return mono;
}

/// El nivel en dBFS de `mono[from, to)`; −inf si la ventana esta vacia o es silencio digital.
inline double takeWindowDbfs(const std::vector<float>& mono, int64_t from, int64_t to) {
    if (to <= from) return -std::numeric_limits<double>::infinity();
    double acc = 0.0;
    for (int64_t i = from; i < to; ++i) acc += static_cast<double>(mono[static_cast<size_t>(i)]) * mono[static_cast<size_t>(i)];
    const double rms = std::sqrt(acc / static_cast<double>(to - from));
    return rms > 0.0 ? 20.0 * std::log10(rms) : -std::numeric_limits<double>::infinity();
}

/// El inicio de la ventana de silencio de un objetivo en `frame`: `frame − 0,5 s` (no antes de 0),
/// corrido al FIN del ultimo corte que caeria adentro (s < FIN < frame): del corte, solo el lado de
/// FRAME. Una perdida en FIN = frame no cruza la ventana (el hueco esta justo despues de ella).
inline int64_t takeSilenceWindowStart(int64_t frame, int rate, const std::vector<TakeCutLine>& cuts) {
    int64_t s = std::max<int64_t>(0, frame - static_cast<int64_t>(std::llround(kTakeTargetSilenceSeconds * rate)));
    for (const TakeCutLine& c : cuts)
        if (c.fin > s && c.fin < frame) s = c.fin;
    return s;
}

/// Registra las publicaciones (`kPublished`) del ring drenado hasta vacio.
inline void drainTakeRing(wma::analysis::AnalysisRing& ring, wma::analysis::AnalysisSnapshot& snapshot,
                          wma::analysis::AnalysisThread& analysis, TakeFeedRun* run) {
    for (;;) {
        const auto outcome = analysis.drainOnce();
        if (outcome == wma::analysis::AnalysisThread::DrainOutcome::kRingEmpty) break;
        // `kSkipped` (el tramo de una costura) NO publica: el snapshot sigue siendo el viejo, y
        // registrarlo fecharia una lectura vieja adentro del hueco (lo encontro `ASeamDiscardsExactlyItsSpan`).
        if (outcome != wma::analysis::AnalysisThread::DrainOutcome::kPublished) continue;
        std::array<float, wma::analysis::kSnapshotValueCount> v{};
        if (!snapshot.read(v.data())) continue;
        latch::Publication p;
        p.pos = static_cast<int64_t>(ring.readPosition());
        p.state = static_cast<int>(v[wma::analysis::kSnapState]);
        p.sigmaCents = static_cast<double>(v[wma::analysis::kSnapUncertainty]);
        const float cents = v[wma::analysis::kSnapCents];
        // kSnapCents es relativo al objetivo del strobe: la altura absoluta sale de ESE objetivo.
        if (!std::isnan(cents))
            p.hz = analysis.strobe().targetHz() * std::pow(2.0, static_cast<double>(cents) / 1200.0);
        run->pubs.push_back(p);
        run->snaps.push_back(v);
    }
}

/// El aviso de un corte, en su INI, con el ring DRENADO (D38). Corte en el frame 0: nada.
inline void announceTakeCut(wma::analysis::AnalysisRing& ring, wma::analysis::AnalysisThread& analysis,
                            const TakeCutLine& cut) {
    if (cut.ini == 0) return;
    ring.reportCaptureDiscontinuity(static_cast<uint64_t>(cut.fin - cut.ini));
    // Perdida: cruzar la costura YA, con el ring vacio. Reinicia sin descartar (no hay nada que leer).
    if (cut.fin == cut.ini) (void)analysis.drainOnce();
}

/// Aplica `# objetivo` con el ring DRENADO (D39), despues de verificar que cae en silencio (D42).
/// false (con `error`) si no: la corrida falla, el objetivo NO se aplica.
inline bool applyTakeTarget(wma::analysis::AnalysisRing& ring, wma::analysis::AnalysisThread& analysis,
                            const std::vector<float>& mono, int rate, const std::vector<TakeCutLine>& cuts,
                            const TakeTargetLine& target, TakeFeedRun* run) {
    const int64_t from = takeSilenceWindowStart(target.frame, rate, cuts);
    const double db = takeWindowDbfs(mono, from, target.frame);
    if (db > kTakeTargetSilenceDbfs) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "objetivo en el frame %lld fuera de silencio: %.2f dBFS en [%lld, %lld) (> %.1f)",
                      static_cast<long long>(target.frame), db, static_cast<long long>(from),
                      static_cast<long long>(target.frame), kTakeTargetSilenceDbfs);
        run->error = buf;
        return false;
    }
    analysis.setTargetHz(target.hz);
    // Ring vacio: el `skipToNewest()` del objetivo nuevo no descarta nada.
    (void)analysis.drainOnce();
    run->events.push_back({static_cast<int64_t>(ring.readPosition()), true});
    return true;
}

/**
 * La corrida de una toma (ver la cabecera). `cuts` y `targets`, ordenados como los deja
 * `parseTakeManifest`. Si un objetivo no cae en silencio, `run.error` lo dice y la corrida se corta.
 */
inline TakeFeedRun feedTakeWithCuts(const wav::WavData& data, const std::vector<TakeCutLine>& cuts,
                                    const std::vector<TakeTargetLine>& targets) {
    TakeFeedRun run;
    run.rate = data.sampleRate;
    const int64_t frames = data.numFrames;
    const std::vector<float> mono = takeMonoOf(data);

    // Donde se parte una tanda: cada INI (aviso), cada FIN de costura (el descarte termina ahi) y cada
    // objetivo.
    std::vector<int64_t> splits;
    for (const TakeCutLine& c : cuts) {
        splits.push_back(c.ini);
        if (c.fin != c.ini) splits.push_back(c.fin);
    }
    for (const TakeTargetLine& t : targets) splits.push_back(t.frame);
    std::sort(splits.begin(), splits.end());

    wma::analysis::AnalysisRing ring;
    wma::analysis::AnalysisSnapshot snapshot;
    wma::analysis::AnalysisThread analysis(ring, snapshot);
    ring.setCaptureRate(data.sampleRate);

    size_t nextCut = 0, nextTarget = 0, nextSplit = 0;
    int64_t f = 0;
    while (f < frames) {
        // Lo que pasa EN f, con el ring drenado: primero los avisos, despues el objetivo.
        while (nextCut < cuts.size() && cuts[nextCut].ini == f) announceTakeCut(ring, analysis, cuts[nextCut++]);
        while (nextTarget < targets.size() && targets[nextTarget].frame == f) {
            if (!applyTakeTarget(ring, analysis, mono, data.sampleRate, cuts, targets[nextTarget++], &run)) {
                run.ringWritePosition = static_cast<int64_t>(ring.writePosition());
                return run;
            }
        }
        while (nextSplit < splits.size() && splits[nextSplit] <= f) ++nextSplit;
        int64_t end = (f / kTakeChunkFrames + 1) * kTakeChunkFrames;
        if (nextSplit < splits.size()) end = std::min(end, splits[nextSplit]);
        end = std::min(end, frames);
        const float* src = data.buffer.data() + static_cast<size_t>(f) * 2;
        const int n = static_cast<int>(end - f);
        latch::appendMonoMix(&run.fedMono, src, n);
        ring.writeStereo(src, n);
        run.frames += n;
        run.pieceEnds.push_back(static_cast<int64_t>(ring.writePosition()));
        drainTakeRing(ring, snapshot, analysis, &run);
        f = end;
    }
    run.ringWritePosition = static_cast<int64_t>(ring.writePosition());
    return run;
}

/// D40: el ataque en `t0` no es un punteo si cae antes del primer objetivo (sin objetivo no hay lectura
/// posible) o a <= `kTakeClapToleranceFrames` de un aplauso. Sigue siendo limite de tEnd; no se mide.
inline bool isNonPluckAttack(int64_t t0, const std::vector<TakeTargetLine>& targets, const std::vector<int64_t>& claps) {
    if (targets.empty() || t0 < targets.front().frame) return true;
    for (int64_t c : claps)
        if (t0 >= c - kTakeClapToleranceFrames && t0 <= c + kTakeClapToleranceFrames) return true;
    return false;
}

}  // namespace wma_test::take
