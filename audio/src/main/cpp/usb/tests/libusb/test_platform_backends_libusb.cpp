/**
 * test_platform_backends_libusb.cpp — TEST DOUBLE, sólo para libusb_backend_lifetime_tests.
 *
 * Reemplaza backends/PlatformBackends.cpp en ESE binario (MINI-042, D6). Se parece al de
 * core/tests/support/test_platform_backends.cpp con una diferencia que es el punto:
 * `asLibusbBackend()` hace el downcast REAL, porque acá sí hay un `LibusbBackend` de verdad
 * —compilado para el host sobre el núcleo de libusb vendorizado y el backend de SO falso de
 * REQ-047 S1— y el acceso con alcance de `BackendManager` tiene que encontrarlo.
 *
 * `createUsbAudioBackend()` sigue devolviendo nullptr: el fake de SO no alcanza para que
 * `initializeFromFileDescriptor()` complete, así que el test ADOPTA el backend por la costura
 * `BackendManagerTestAccess`. Que `initializeUsbBackend()` no pueda crear uno no le saca nada
 * al test que lo usa: lo que se afirma ahí es que DESTRUYE el anterior bajo los dos locks.
 */

#include "FakeAudioBackend.h"

#include "backends/LibusbBackend.h"
#include "backends/PlatformBackends.h"

namespace watermelon_audio {

std::unique_ptr<IAudioBackend> createSystemAudioBackend() {
    return std::make_unique<wma_test::FakeAudioBackend>();
}

std::unique_ptr<IAudioBackend> createUsbAudioBackend() {
    return nullptr;
}

LibusbBackend* asLibusbBackend(IAudioBackend* backend) {
    if (!backend || backend->getType() != BackendType::LIBUSB) return nullptr;
    return static_cast<LibusbBackend*>(backend);
}

}  // namespace watermelon_audio
