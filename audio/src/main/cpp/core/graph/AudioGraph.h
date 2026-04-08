#pragma once

#include "AudioNode.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>

/**
 * @struct Connection
 * @brief Representa una conexión entre dos nodos
 */
struct Connection {
    NodeHandle sourceNode;
    int sourceChannel;
    NodeHandle destNode;
    int destChannel;

    bool operator==(const Connection& other) const {
        return sourceNode == other.sourceNode &&
               sourceChannel == other.sourceChannel &&
               destNode == other.destNode &&
               destChannel == other.destChannel;
    }
};

/**
 * @class AudioGraph
 * @brief Grafo de procesamiento de audio con routing flexible
 *
 * Arquitectura:
 * - Gestión de nodos (UI thread): addNode, removeNode, connect, disconnect
 * - Procesamiento (Audio thread): process() es lock-free usando snapshots
 * - Snapshot is rebuilt on the UI thread and atomically swapped
 *
 * El grafo procesa nodos en orden topológico para garantizar que
 * las dependencias se procesen antes que los nodos dependientes.
 */
class AudioGraph {
public:
    AudioGraph();
    ~AudioGraph();

    // Prevenir copia y movimiento
    AudioGraph(const AudioGraph&) = delete;
    AudioGraph& operator=(const AudioGraph&) = delete;

    // ========== Gestión de nodos (UI thread) ==========

    /**
     * @brief Agrega un nodo al grafo
     * @param node Nodo a agregar (ownership transferido)
     * @return Handle del nodo (usar para conexiones)
     */
    NodeHandle addNode(std::unique_ptr<AudioNode> node);

    /**
     * @brief Remueve un nodo del grafo
     * @param handle Handle del nodo a remover
     */
    void removeNode(NodeHandle handle);

    /**
     * @brief Obtiene un nodo por handle
     * @return Puntero al nodo (no ownership) o nullptr
     */
    AudioNode* getNode(NodeHandle handle);
    const AudioNode* getNode(NodeHandle handle) const;

    /**
     * @brief Obtiene un nodo con cast a tipo específico
     */
    template<typename T>
    T* getNodeAs(NodeHandle handle) {
        return dynamic_cast<T*>(getNode(handle));
    }

    // ========== Conexiones (UI thread) ==========

    /**
     * @brief Conecta dos nodos
     * @return true si la conexión se creó exitosamente
     */
    bool connect(NodeHandle source, int sourceChannel,
                 NodeHandle dest, int destChannel);

    /**
     * @brief Desconecta dos nodos
     * @return true si la conexión existía y se removió
     */
    bool disconnect(NodeHandle source, int sourceChannel,
                    NodeHandle dest, int destChannel);

    /**
     * @brief Desconecta todas las conexiones de un nodo
     */
    void disconnectAll(NodeHandle node);

    /**
     * @brief Valida si una conexión es posible
     */
    bool isConnectionValid(NodeHandle source, int sourceChannel,
                           NodeHandle dest, int destChannel) const;

    // ========== Preparación (UI thread, antes de iniciar audio) ==========

    /**
     * @brief Prepara el grafo para procesamiento
     * @param sampleRate Sample rate en Hz
     * @param maxBlockSize Tamaño máximo de bloque
     */
    void prepare(int sampleRate, int maxBlockSize);

    /**
     * @brief Reset del estado de todos los nodos
     */
    void reset();

    // ========== Procesamiento RT-Safe (audio thread) ==========

    /**
     * @brief Procesa un bloque de audio
     * @param numFrames Número de frames a procesar
     *
     * Lock-free: reads an atomic snapshot pointer, no mutex needed.
     */
    void process(int numFrames);

    // ========== Estado ==========

    bool isPrepared() const { return mPrepared.load(); }
    int getNodeCount() const;

private:
    // Rebuild snapshot on UI thread (must hold mGraphMutex)
    void rebuildSnapshotLocked();

    // Rebuild processing order cache
    void rebuildProcessingOrder();
    bool detectCycle() const;
    void topologicalSort(std::vector<NodeHandle>& order) const;

    // Obtener inputs para un nodo
    void gatherInputsForNode(NodeHandle handle, AudioBuffer& inputBuffer,
                             int numFrames);

private:
    // Almacenamiento de nodos
    std::unordered_map<NodeHandle, std::unique_ptr<AudioNode>> mNodes;
    NodeHandle mNextHandle{1};  // 0 es inválido

    // Conexiones
    std::vector<Connection> mConnections;

    // Orden de procesamiento (topological sort)
    std::vector<NodeHandle> mProcessingOrder;

    // Thread safety
    mutable std::mutex mGraphMutex;  // Para modificaciones estructurales (UI thread only)

    // Double buffering para procesamiento lock-free
    struct ProcessingSnapshot {
        std::vector<AudioNode*> nodes;
        std::vector<Connection> connections;
        std::vector<NodeHandle> order;
    };
    ProcessingSnapshot mSnapshot1, mSnapshot2;
    std::atomic<ProcessingSnapshot*> mActiveSnapshot{&mSnapshot1};

    // Track which snapshot is the "inactive" one for next UI-thread rebuild
    ProcessingSnapshot* mInactiveSnapshot{&mSnapshot2};

    // Estado
    std::atomic<bool> mPrepared{false};
    int mSampleRate = 48000;
    int mMaxBlockSize = 4096;

    // Buffers temporales para gathering inputs
    AudioBuffer mTempInputBuffer;
};
