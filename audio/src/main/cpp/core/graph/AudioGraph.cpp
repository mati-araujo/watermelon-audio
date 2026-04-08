#include "AudioGraph.h"
#include <algorithm>
#include <queue>
#include <set>
#include <functional>

AudioGraph::AudioGraph() {
    mTempInputBuffer.setSize(2, 4096);
}

AudioGraph::~AudioGraph() = default;

NodeHandle AudioGraph::addNode(std::unique_ptr<AudioNode> node) {
    std::lock_guard<std::mutex> lock(mGraphMutex);

    NodeHandle handle = mNextHandle++;
    node->setHandle(handle);

    if (mPrepared.load()) {
        node->prepare(mSampleRate, mMaxBlockSize);
    }

    mNodes[handle] = std::move(node);
    rebuildSnapshotLocked();

    return handle;
}

void AudioGraph::removeNode(NodeHandle handle) {
    std::lock_guard<std::mutex> lock(mGraphMutex);

    // Eliminar conexiones asociadas
    disconnectAll(handle);

    // Eliminar nodo
    mNodes.erase(handle);
    rebuildSnapshotLocked();
}

AudioNode* AudioGraph::getNode(NodeHandle handle) {
    std::lock_guard<std::mutex> lock(mGraphMutex);
    auto it = mNodes.find(handle);
    return it != mNodes.end() ? it->second.get() : nullptr;
}

const AudioNode* AudioGraph::getNode(NodeHandle handle) const {
    std::lock_guard<std::mutex> lock(mGraphMutex);
    auto it = mNodes.find(handle);
    return it != mNodes.end() ? it->second.get() : nullptr;
}

bool AudioGraph::connect(NodeHandle source, int sourceChannel,
                         NodeHandle dest, int destChannel) {
    std::lock_guard<std::mutex> lock(mGraphMutex);

    // Validar conexión
    if (!isConnectionValid(source, sourceChannel, dest, destChannel)) {
        return false;
    }

    // Evitar duplicados
    Connection conn{source, sourceChannel, dest, destChannel};
    auto it = std::find(mConnections.begin(), mConnections.end(), conn);
    if (it != mConnections.end()) {
        return true;  // Ya existe
    }

    mConnections.push_back(conn);

    // Verificar ciclos
    if (detectCycle()) {
        mConnections.pop_back();
        return false;
    }

    rebuildSnapshotLocked();
    return true;
}

bool AudioGraph::disconnect(NodeHandle source, int sourceChannel,
                            NodeHandle dest, int destChannel) {
    std::lock_guard<std::mutex> lock(mGraphMutex);

    Connection conn{source, sourceChannel, dest, destChannel};
    auto it = std::find(mConnections.begin(), mConnections.end(), conn);

    if (it != mConnections.end()) {
        mConnections.erase(it);
        rebuildSnapshotLocked();
        return true;
    }
    return false;
}

void AudioGraph::disconnectAll(NodeHandle node) {
    // Nota: mGraphMutex debe estar tomado por el llamador
    auto it = mConnections.begin();
    while (it != mConnections.end()) {
        if (it->sourceNode == node || it->destNode == node) {
            it = mConnections.erase(it);
        } else {
            ++it;
        }
    }
    // Note: caller (removeNode) will call rebuildSnapshotLocked after this
}

bool AudioGraph::isConnectionValid(NodeHandle source, int sourceChannel,
                                   NodeHandle dest, int destChannel) const {
    // No conectar a sí mismo
    if (source == dest) return false;

    // Verificar que ambos nodos existen
    auto srcIt = mNodes.find(source);
    auto dstIt = mNodes.find(dest);

    if (srcIt == mNodes.end() || dstIt == mNodes.end()) {
        return false;
    }

    // Verificar canales válidos
    const AudioNode* srcNode = srcIt->second.get();
    const AudioNode* dstNode = dstIt->second.get();

    if (sourceChannel < 0 || sourceChannel >= srcNode->getNumOutputChannels()) {
        return false;
    }
    if (destChannel < 0 || destChannel >= dstNode->getNumInputChannels()) {
        return false;
    }

    return true;
}

void AudioGraph::prepare(int sampleRate, int maxBlockSize) {
    std::lock_guard<std::mutex> lock(mGraphMutex);

    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;

    mTempInputBuffer.setSize(2, maxBlockSize);

    for (auto& [handle, node] : mNodes) {
        node->prepare(sampleRate, maxBlockSize);
    }

    rebuildProcessingOrder();
    rebuildSnapshotLocked();
    mPrepared.store(true);
}

void AudioGraph::reset() {
    std::lock_guard<std::mutex> lock(mGraphMutex);

    for (auto& [handle, node] : mNodes) {
        node->reset();
    }
}

void AudioGraph::process(int numFrames) {
    // Lock-free: just read the atomic snapshot pointer
    ProcessingSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);

    // Procesar nodos en orden topológico
    for (size_t i = 0; i < snapshot->order.size(); ++i) {
        AudioNode* node = snapshot->nodes[i];
        if (!node || !node->isActive()) continue;

        NodeHandle handle = snapshot->order[i];

        // Recopilar entradas
        mTempInputBuffer.clear();
        gatherInputsForNode(handle, mTempInputBuffer, numFrames);

        // Procesar nodo
        node->process(mTempInputBuffer, numFrames);
    }
}

void AudioGraph::rebuildSnapshotLocked() {
    // Called from UI thread with mGraphMutex held.
    // Build the new snapshot into the inactive buffer, then atomic-swap.

    ProcessingSnapshot* target = mInactiveSnapshot;

    target->nodes.clear();
    target->order.clear();

    topologicalSort(target->order);

    for (NodeHandle h : target->order) {
        auto it = mNodes.find(h);
        if (it != mNodes.end()) {
            target->nodes.push_back(it->second.get());
        }
    }
    target->connections = mConnections;

    // Atomic swap — audio thread will pick up the new snapshot on next process() call
    mActiveSnapshot.store(target, std::memory_order_release);

    // Flip: the previously-active snapshot is now the inactive one for next rebuild
    mInactiveSnapshot =
        (target == &mSnapshot1) ? &mSnapshot2 : &mSnapshot1;
}

void AudioGraph::rebuildProcessingOrder() {
    mProcessingOrder.clear();
    topologicalSort(mProcessingOrder);
}

bool AudioGraph::detectCycle() const {
    // DFS para detectar ciclos
    std::set<NodeHandle> visited;
    std::set<NodeHandle> recursionStack;

    std::function<bool(NodeHandle)> hasCycleFrom = [&](NodeHandle node) -> bool {
        visited.insert(node);
        recursionStack.insert(node);

        for (const auto& conn : mConnections) {
            if (conn.sourceNode == node) {
                if (recursionStack.count(conn.destNode) > 0) {
                    return true;  // Ciclo detectado
                }
                if (visited.count(conn.destNode) == 0) {
                    if (hasCycleFrom(conn.destNode)) {
                        return true;
                    }
                }
            }
        }

        recursionStack.erase(node);
        return false;
    };

    for (const auto& [handle, node] : mNodes) {
        if (visited.count(handle) == 0) {
            if (hasCycleFrom(handle)) {
                return true;
            }
        }
    }

    return false;
}

void AudioGraph::topologicalSort(std::vector<NodeHandle>& order) const {
    // Kahn's algorithm
    std::unordered_map<NodeHandle, int> inDegree;

    // Inicializar in-degree
    for (const auto& [handle, node] : mNodes) {
        inDegree[handle] = 0;
    }

    // Contar aristas entrantes
    for (const auto& conn : mConnections) {
        inDegree[conn.destNode]++;
    }

    // Cola con nodos sin dependencias
    std::queue<NodeHandle> queue;
    for (const auto& [handle, degree] : inDegree) {
        if (degree == 0) {
            queue.push(handle);
        }
    }

    // Procesar
    while (!queue.empty()) {
        NodeHandle current = queue.front();
        queue.pop();
        order.push_back(current);

        for (const auto& conn : mConnections) {
            if (conn.sourceNode == current) {
                inDegree[conn.destNode]--;
                if (inDegree[conn.destNode] == 0) {
                    queue.push(conn.destNode);
                }
            }
        }
    }
}

void AudioGraph::gatherInputsForNode(NodeHandle handle, AudioBuffer& inputBuffer,
                                     int numFrames) {
    ProcessingSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);

    for (const auto& conn : snapshot->connections) {
        if (conn.destNode == handle) {
            // Encontrar nodo fuente
            for (AudioNode* node : snapshot->nodes) {
                if (node->getHandle() == conn.sourceNode) {
                    const AudioBuffer& sourceBuffer = node->getOutputBuffer();
                    inputBuffer.addFrom(conn.destChannel,
                                        sourceBuffer, conn.sourceChannel,
                                        numFrames);
                    break;
                }
            }
        }
    }
}

int AudioGraph::getNodeCount() const {
    std::lock_guard<std::mutex> lock(mGraphMutex);
    return static_cast<int>(mNodes.size());
}
