/**
 * PlatformBackends.cpp
 *
 * The one and only translation unit allowed to name concrete backend
 * implementations, and therefore the only one that includes <oboe/Oboe.h> or
 * <libusb.h>. Keeping the conditional compilation confined here is what lets
 * BackendManager.cpp — and everything above it — build unchanged on iOS.
 */

#include "PlatformBackends.h"

#if defined(__ANDROID__)
#include "OboeBackend.h"
#include "LibusbBackend.h"
#endif

namespace watermelon_audio {

#if defined(__ANDROID__)

std::unique_ptr<IAudioBackend> createSystemAudioBackend() {
    return std::make_unique<OboeBackend>();
}

std::unique_ptr<IAudioBackend> createUsbAudioBackend() {
    return std::make_unique<LibusbBackend>();
}

LibusbBackend* asLibusbBackend(IAudioBackend* backend) {
    if (!backend || backend->getType() != BackendType::LIBUSB) return nullptr;
    return static_cast<LibusbBackend*>(backend);
}

#else

std::unique_ptr<IAudioBackend> createSystemAudioBackend() {
    // iOS lands here until CoreAudioBackend exists (WA-2.4). Returning null
    // rather than asserting keeps the manager constructible, so the parts of
    // the engine that never start a stream (offline render, tests) still work.
    return nullptr;
}

std::unique_ptr<IAudioBackend> createUsbAudioBackend() {
    return nullptr;  // D4: USB audio is Android-only.
}

LibusbBackend* asLibusbBackend(IAudioBackend*) {
    return nullptr;
}

#endif  // __ANDROID__

} // namespace watermelon_audio
