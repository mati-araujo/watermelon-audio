#pragma once

// OJO CON EL DIRECTORIO: `core/graph/` ya NO contiene un grafo. La clase
// AudioGraph se borro (era inalcanzable por construccion y nunca se uso); lo
// que queda aca son las dos primitivas, que SI estan vivas:
//
//   AudioNode.h    clase base de MixerNode, OscillatorNode, EffectChainNode e
//                  InputNode, que el motor maneja DIRECTAMENTE desde sus
//                  metodos de render — no hay grafo que los recorra.
//   AudioBuffer.h  buffer multi-canal, usado por esos nodos y tambien por el
//                  looper (ChunkedAudioBuffer, TrackStorage).
//
// El nombre del directorio quedo por historia. No se movieron los archivos para
// no mezclar un movimiento con el borrado; donde deberia vivir AudioBuffer —que
// el looper tambien usa— es una pregunta propia.

#include "AudioBuffer.h"
#include <atomic>
#include <string>
#include <cstdint>

/**
 * @enum NodeType
 * @brief Tipos de nodos de audio.
 *
 * Sobrevive al borrado del grafo: los nodos siguen existiendo y declarando su
 * tipo, lo que ya no existe es algo que los conecte entre si.
 */
enum class NodeType {
    INPUT,          ///< Nodo de entrada (micrófono, audio file)
    OSCILLATOR,     ///< Generador de señal (osciladores)
    MIXER,          ///< Mezclador de señales
    EFFECT_CHAIN,   ///< Cadena de efectos
    OUTPUT,         ///< Salida final (DAC)
    MODULATOR,      ///< Modulador de señal
    ANALYZER        ///< Analizador (FFT, metering)
};

using NodeHandle = uint32_t;
constexpr NodeHandle INVALID_NODE_HANDLE = 0xFFFFFFFF;

/**
 * @class AudioNode
 * @brief Clase base abstracta para todos los nodos del Audio Graph
 *
 * Diseño RT-Safe:
 * - process() no debe alocar memoria ni usar locks
 * - Parámetros modificables usan std::atomic
 * - Buffer de salida pre-alocado en prepare()
 */
class AudioNode {
public:
    virtual ~AudioNode() = default;

    // Identificación
    virtual NodeType getType() const = 0;
    virtual const char* getName() const = 0;

    /**
     * @brief Configuración inicial (llamado desde UI thread)
     * @param sampleRate Sample rate en Hz
     * @param maxBlockSize Tamaño máximo de bloque
     */
    virtual void prepare(int sampleRate, int maxBlockSize) {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mBuffer.setSize(mNumOutputChannels, maxBlockSize);
    }

    /**
     * @brief Reset del estado interno
     */
    virtual void reset() {
        mBuffer.clear();
    }

    /**
     * @brief Procesamiento de audio (RT-Safe, llamado desde audio thread)
     * @param inputBuffer Buffer de entrada (mezcla de todas las conexiones entrantes)
     * @param numFrames Número de frames a procesar
     *
     * Debe escribir la salida en mBuffer (getOutputBuffer())
     */
    virtual void process(AudioBuffer& inputBuffer, int numFrames) = 0;

    // Output buffer del nodo
    AudioBuffer& getOutputBuffer() { return mBuffer; }
    const AudioBuffer& getOutputBuffer() const { return mBuffer; }

    // Estado
    bool isActive() const { return mActive.load(std::memory_order_acquire); }
    void setActive(bool active) { mActive.store(active, std::memory_order_release); }

    // Routing info
    int getNumInputChannels() const { return mNumInputChannels; }
    int getNumOutputChannels() const { return mNumOutputChannels; }

    // Handle asignado por el grafo
    void setHandle(NodeHandle handle) { mHandle = handle; }
    NodeHandle getHandle() const { return mHandle; }

protected:
    std::atomic<bool> mActive{true};
    int mNumInputChannels = 2;
    int mNumOutputChannels = 2;
    int mSampleRate = 48000;
    int mMaxBlockSize = 4096;
    NodeHandle mHandle = INVALID_NODE_HANDLE;

    // Buffer de salida interno
    AudioBuffer mBuffer;
};