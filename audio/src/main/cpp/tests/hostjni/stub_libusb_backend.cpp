/**
 * stub_libusb_backend.cpp — CUERPOS VACIOS, sólo para el arnés JNI de host.
 *
 * `jni_usb.cpp` y `jni_audio_bridge.cpp` nombran diez métodos de
 * `LibusbBackend`. La clase entera es Android-only por decisión D4: su
 * implementación habla libusb contra un descriptor de archivo que sólo entrega
 * `UsbDeviceConnection`. En el host no hay nada que implementar.
 *
 * 🔴 ESTO NO PRUEBA NADA DE USB, y no puede: `createUsbAudioBackend()` devuelve
 * `nullptr` en el host (test_platform_backends.cpp, la misma sustitución que usa
 * la suite de C++), así que el acceso con alcance de `BackendManager`
 * (`withLibusbBackend`/`withLibusbBackendLifecycle`, MINI-042) le pasa `nullptr`
 * a la JNI y **ninguno de estos cuerpos se ejecuta jamás**. Existen para que el
 * `.so` cierre sus símbolos con `--no-undefined`, que es lo que convierte un
 * `UnsatisfiedLinkError` tardío en un error de link que dice el nombre.
 *
 * Los 36 exports de USB están declarados fuera de alcance en la spec de REQ-016
 * justamente por esto: se verifican con dispositivo.
 */

#include "backends/LibusbBackend.h"

namespace watermelon_audio {

IAudioCallback* LibusbBackend::swapCallback(IAudioCallback*) { return nullptr; }

void LibusbBackend::setStreamingMode(UsbStreamingMode) {}

bool LibusbBackend::hasCapture() const { return false; }

int LibusbBackend::getUacVersion() const { return 0; }

// MINI-042 (D7): las stats salen por copia y el profiler por métodos, no por puntero.
std::optional<LibusbBackend::TransferStatsSnapshot> LibusbBackend::getTransferStatsSnapshot() const {
    return std::nullopt;
}

usb::UsbProfilingStats LibusbBackend::getProfilingStats() const { return {}; }

bool LibusbBackend::setProfilingEnabled(bool) { return false; }

bool LibusbBackend::resetProfilingStats() { return false; }

int LibusbBackend::getEventLoopSchedResult() const { return -1; }

int LibusbBackend::getJitterBudgetMs() const { return 0; }

int LibusbBackend::getConvergedFloorMs() const { return 0; }

LibusbBackend::DeviceCapabilities LibusbBackend::getCapabilities() const { return {}; }

bool LibusbBackend::selectAltsetting(int, int, int) { return false; }

bool LibusbBackend::selectClockSource(int) { return false; }

// REQ-050 S3 (D17): nunca se llaman en el host (no hay backend vivo), pero la JNIEXPORT los nombra.
bool LibusbBackend::clearManualAltsettingSelection() { return false; }

bool LibusbBackend::clearManualClockSourceSelection() { return false; }

}  // namespace watermelon_audio
