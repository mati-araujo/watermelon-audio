/**
 * test_platform_backends.cpp — TEST DOUBLE, host test build only.
 *
 * Replaces backends/PlatformBackends.cpp in the core test binary. That file is
 * deliberately excluded from this target (see ../CMakeLists.txt); this one
 * provides the same three symbols.
 *
 * Why substitute the translation unit instead of injecting an object?
 * BackendManager creates its backend in its own constructor, via
 * createSystemAudioBackend(), and exposes no seam to hand it one. On the host
 * the real factory returns nullptr — correct for production (no Oboe, no
 * CoreAudio yet) but it leaves selectBackend(OBOE) failing, so nothing can ever
 * report a running stream and the BackendManager path stays untestable.
 *
 * WA-2.7 made every concrete backend nameable from exactly one translation
 * unit. That is precisely the seam: swapping that one file gives the manager a
 * fake without a single #ifdef or test hook in production code.
 */

#include "FakeAudioBackend.h"
#include "FakeCaptureStream.h"

#include "nodes/InputNode.h"

#include "backends/PlatformBackends.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace wma_test {
namespace {
// Atomico desde REQ-055 S2: dos motores creados a la vez (render offline en
// paralelo) escriben aca desde hilos distintos. El TSan de Linux lo vio.
std::atomic<FakeAudioBackend*> g_lastCreated{nullptr};
}  // namespace

FakeAudioBackend* lastCreatedSystemBackend() { return g_lastCreated.load(); }

void resetLastCreatedSystemBackend() { g_lastCreated.store(nullptr); }

// ---- Captura propia del InputNode (REQ-054) --------------------------------
namespace {

std::atomic<bool> g_captureAvailable{false};
std::atomic<bool> g_openFails{false};
std::atomic<bool> g_startFails{false};
std::atomic<int> g_sampleRate{48000};
std::atomic<int> g_opens{0};
std::atomic<int> g_closes{0};
std::atomic<int> g_violations{0};
std::atomic<bool> g_isOpen{false};

// Protege `g_live` y el estado del stream vivo: `deliver` no puede usar un objeto
// que otro hilo esta destruyendo. Solo del fake; no es codigo RT.
std::mutex g_liveMutex;

class FakeCaptureStream;
FakeCaptureStream* g_live = nullptr;

class FakeCaptureStream final : public watermelon_audio::INodeCaptureStream {
public:
    explicit FakeCaptureStream(InputNode& node) : mNode(&node) {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        g_live = this;
    }
    ~FakeCaptureStream() override {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        if (mOpen) {
            // Destruido sin cerrar: el nodo lo cierra siempre antes.
            g_violations.fetch_add(1);
            g_isOpen.store(false);
        }
        if (g_live == this) g_live = nullptr;
    }

    bool openCaptureStream(InputSource) override {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        if (mOpen) g_violations.fetch_add(1);
        if (g_openFails.load()) return false;
        mOpen = true;
        mStarted = false;
        g_isOpen.store(true);
        g_opens.fetch_add(1);
        return true;
    }

    bool startCaptureStream() override {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        if (!mOpen) {
            g_violations.fetch_add(1);
            return false;
        }
        if (g_startFails.load()) return false;
        mStarted = true;
        return true;
    }

    void stopAndCloseCaptureStream() override {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        if (!mOpen) {
            g_violations.fetch_add(1);
            return;
        }
        mOpen = false;
        mStarted = false;
        g_isOpen.store(false);
        g_closes.fetch_add(1);
    }

    bool isCaptureStreamOpen() const override {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        return mOpen;
    }
    int captureStreamSampleRate() const override {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        return mOpen ? g_sampleRate.load() : 0;
    }
    int64_t captureStreamLatencyFrames() const override { return 0; }

    // Con g_liveMutex tomado por el llamador.
    bool deliverLocked(const float* data, int frames) {
        if (!mOpen || !mStarted) return false;
        std::vector<float> copy(data, data + static_cast<size_t>(frames) * 2);
        return mNode->processInputBlock(copy.data(), frames, 2, 0);
    }

private:
    InputNode* mNode;
    bool mOpen = false;
    bool mStarted = false;
};

}  // namespace

void setFakeCaptureStreamAvailable(bool available) { g_captureAvailable.store(available); }

void resetFakeCaptureStream() {
    g_captureAvailable.store(false);
    g_openFails.store(false);
    g_startFails.store(false);
    g_sampleRate.store(48000);
    g_opens.store(0);
    g_closes.store(0);
    g_violations.store(0);
    g_isOpen.store(false);
}

void setFakeCaptureStreamOpenFails(bool fails) { g_openFails.store(fails); }
void setFakeCaptureStreamStartFails(bool fails) { g_startFails.store(fails); }
void setFakeCaptureStreamSampleRate(int sampleRate) { g_sampleRate.store(sampleRate); }

int fakeCaptureStreamOpens() { return g_opens.load(); }
int fakeCaptureStreamCloses() { return g_closes.load(); }
bool fakeCaptureStreamIsOpen() { return g_isOpen.load(); }
int fakeCaptureStreamViolations() { return g_violations.load(); }

bool fakeCaptureStreamDeliver(const float* interleavedStereo, int frames) {
    std::lock_guard<std::mutex> lock(g_liveMutex);
    if (!g_live) return false;
    return g_live->deliverLocked(interleavedStereo, frames);
}

}  // namespace wma_test

namespace watermelon_audio {

std::unique_ptr<IAudioBackend> createSystemAudioBackend() {
    auto backend = std::make_unique<wma_test::FakeAudioBackend>();
    wma_test::g_lastCreated.store(backend.get());
    return backend;
}

std::unique_ptr<IAudioBackend> createUsbAudioBackend() {
    // USB stays unsupported on the host, same as the real non-Android build.
    return nullptr;
}

LibusbBackend* asLibusbBackend(IAudioBackend*) {
    return nullptr;
}

std::unique_ptr<INodeCaptureStream> createNodeCaptureStream(InputNode& node) {
    // Apagado por defecto: sin la perilla, igual que el build de host real.
    if (!wma_test::g_captureAvailable.load()) return nullptr;
    return std::make_unique<wma_test::FakeCaptureStream>(node);
}

}  // namespace watermelon_audio
