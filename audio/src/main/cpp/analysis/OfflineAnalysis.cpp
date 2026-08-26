#include "OfflineAnalysis.h"

#include "AnalysisRing.h"
#include "AnalysisThread.h"

namespace wma::analysis {

bool analyzeBuffer(const float* interleaved, int frames, int sampleRate,
                   double targetHz, float* outValues) noexcept {
    if (interleaved == nullptr || outValues == nullptr) return false;
    if (frames <= 0 || sampleRate <= 0) return false;

    // 🔴 TODO NUEVO EN CADA LLAMADA, Y ESO ES LA MITAD DEL DETERMINISMO.
    // El estimador integra fase a lo largo de segundos: un ring o un strobe
    // reusados harian que la lectura dependa de QUE SE ANALIZO ANTES, o sea que
    // el mismo buffer daria resultados distintos segun el orden de los tests.
    // Es la misma razon por la que `onSourceChanged()` existe en el camino vivo.
    AnalysisRing ring;
    AnalysisSnapshot snapshot;
    AnalysisThread analysis(ring, snapshot);

    ring.setCaptureRate(sampleRate);
    analysis.setTargetHz(targetHz);
    // NO se llama a `start()`: acá el conductor somos nosotros. Ver el contrato
    // de `drainOnce()` — dos conductores sobre el mismo estado es una carrera.

    const int capacity = static_cast<int>(AnalysisRing::kCapacityFrames);
    int written = 0;
    while (written < frames) {
        // Se escribe de a lo que entra y se DRENA antes de seguir. Alimentar de
        // corrido pisaria frames sin leer, y entonces el estimador integraria
        // fase sobre muestras no contiguas: una lectura bien formada y
        // equivocada, que es peor que un error. Acá no hace falta adivinar el
        // ritmo —no hay otro thread— así que la contigüidad es exacta.
        const int chunk = (frames - written) < capacity ? (frames - written) : capacity;
        ring.writeStereo(interleaved + static_cast<size_t>(written) * 2, chunk);
        written += chunk;

        while (analysis.drainOnce() != AnalysisThread::DrainOutcome::kRingEmpty) {
            // vuelta a vuelta hasta agotar lo que se acaba de escribir
        }
    }

    return snapshot.read(outValues);
}

}  // namespace wma::analysis
