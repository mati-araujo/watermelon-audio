/**
 * INodeCaptureStream.h
 *
 * El stream de captura PROPIO de un InputNode (el que abre `wma_tuner_start`),
 * detras de una interfaz sin tipos de plataforma. Es el seam que deja a la suite
 * de host observar el ciclo abrir/arrancar/cerrar del nodo sin Oboe.
 *
 * Los nombres son distintivos a proposito: el walker de check-rt-safety.py pierde
 * cobertura si aparece un segundo `start`/`open`/`run` en el arbol.
 */

#pragma once

#include <cstdint>

enum class InputSource;

namespace watermelon_audio {

class INodeCaptureStream {
public:
    virtual ~INodeCaptureStream() = default;

    /** Crea y configura el stream. false = no abrio (queda cerrado). */
    virtual bool openCaptureStream(InputSource source) = 0;

    /** requestStart. false = no arranco; el stream queda ABIERTO. */
    virtual bool startCaptureStream() = 0;

    /** stop + close + reset. Idempotente. */
    virtual void stopAndCloseCaptureStream() = 0;

    virtual bool isCaptureStreamOpen() const = 0;

    /** Rate real del stream; 0 si no hay stream. */
    virtual int captureStreamSampleRate() const = 0;

    /** Latencia en frames (medida, o estimada si no hay medicion); 0 si no hay stream. */
    virtual int64_t captureStreamLatencyFrames() const = 0;
};

}  // namespace watermelon_audio
