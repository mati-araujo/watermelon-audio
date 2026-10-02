#pragma once

/**
 * UsbTransferStatsArray.h — MINI-042: el layout de los 21 floats de
 * `nativeGetUsbTransferStats`, como función PURA.
 *
 * Vivía adentro de la JNIEXPORT, y ahí no la ejecuta ningún test de host: el arnés JNI
 * compila un `LibusbBackend` vacío y nunca hay stats que traducir. Un [0]/[1] o un
 * [19]/[20] cruzados pasaban todo el gate. Acá la afirma `libusb_backend_lifetime_tests`
 * índice por índice, y la JNI sólo la llama.
 *
 * Los índices los lee `parseUsbTransferStats` (UsbAudioManagerImpl.kt) y el runner de
 * REQ-050: NO se renumeran. [19] y [20] van al final (REQ-050 D16).
 */

#include "LibusbBackend.h"

#include <optional>

namespace watermelon_audio {

constexpr int kUsbTransferStatsArraySize = 21;

/**
 * Llena `out` con la copia de stats; sin dato (`nullopt`: sin stream, o un start()/stop()
 * en curso) deja los 21 en cero, como siempre devolvió la JNI.
 *
 * El orden enviados -> completados de AC-050.9 lo cumple la COPIA (getTransferStatsSnapshot);
 * acá sólo se traducen valores ya leídos.
 */
inline void usbTransferStatsToArray(
        const std::optional<LibusbBackend::TransferStatsSnapshot>& snap,
        float (&out)[kUsbTransferStatsArraySize]) {
    for (float& v : out) v = 0.0f;
    if (!snap) return;
    out[0] = static_cast<float>(snap->packetsSubmitted);
    out[1] = static_cast<float>(snap->packetsCompleted);
    out[2] = static_cast<float>(snap->packetsErrors);
    out[3] = static_cast<float>(snap->underruns);
    out[4] = static_cast<float>(snap->overruns);
    out[5] = snap->currentLatencyMs;
    out[6] = snap->avgLatencyMs;
    out[7] = snap->currentLatencyMs * 0.8f;
    out[8] = snap->currentLatencyMs * 1.5f;
    out[9] = static_cast<float>(snap->ringBufferLevel);
    out[10] = snap->ringBufferFillPct;
    out[11] = 3840.0f;
    out[12] = out[1] * 192.0f;
    out[13] = snap->currentSampleRateHz;
    out[14] = snap->driftPpm;
    out[15] = snap->feedbackEffectiveFramesPerPacket;
    out[16] = static_cast<float>(snap->feedbackPacketsReceived);
    out[17] = static_cast<float>(snap->feedbackPacketsInvalid);
    out[18] = static_cast<float>(snap->activeClockSourceId);
    // [19] paquetes de salida en vuelo (la cola declarada) y [20] el techo de latencia de
    // salida que el backend declara, en ms (UsbLatencyMath.h). 0 sin stream.
    out[19] = static_cast<float>(snap->outputInFlightDepthPackets);
    out[20] = snap->declaredOutputLatencyCeilingMs;
}

}  // namespace watermelon_audio
