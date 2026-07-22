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

#include "backends/PlatformBackends.h"

namespace wma_test {
namespace {
FakeAudioBackend* g_lastCreated = nullptr;
}  // namespace

FakeAudioBackend* lastCreatedSystemBackend() { return g_lastCreated; }

void resetLastCreatedSystemBackend() { g_lastCreated = nullptr; }

}  // namespace wma_test

namespace watermelon_audio {

std::unique_ptr<IAudioBackend> createSystemAudioBackend() {
    auto backend = std::make_unique<wma_test::FakeAudioBackend>();
    wma_test::g_lastCreated = backend.get();
    return backend;
}

std::unique_ptr<IAudioBackend> createUsbAudioBackend() {
    // USB stays unsupported on the host, same as the real non-Android build.
    return nullptr;
}

LibusbBackend* asLibusbBackend(IAudioBackend*) {
    return nullptr;
}

}  // namespace watermelon_audio
