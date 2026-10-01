/**
 * test_usb_stream_start.cpp — REQ-050 S2, AC-050.3.
 *
 * `startStreaming` sin motor o sin callback devolvía un STREAMING_ERROR genérico:
 * la JNIEXPORT colapsaba en un `jboolean` cinco causas distintas, y el log nativo
 * ("No audio callback set", LibusbBackend.cpp) era lo único que decía cuál.
 *
 * La decisión de QUÉ causa nombrar vive en `classifyUsbStreamStart` (BackendManager.h),
 * pura, para poder afirmarla acá. La JNIEXPORT sólo junta los hechos: en el host no
 * hay backend libusb (`createUsbAudioBackend()` devuelve nullptr), así que el camino
 * "sin callback" no se alcanza desde el arnés JNI. Se afirma acá, y el camino de ÉXITO
 * lo cubre el smoke en el device.
 */

#include "backends/BackendManager.h"

#include <gtest/gtest.h>

namespace {

using watermelon_audio::UsbStreamStartFacts;
using watermelon_audio::UsbStreamStartStatus;
using watermelon_audio::classifyUsbStreamStart;

/// Todo listo para arrancar: cada test apaga UN hecho.
UsbStreamStartFacts ready() {
    UsbStreamStartFacts f;
    f.alreadyStreaming = false;
    f.engineExists = true;
    f.deviceInitialized = true;
    f.backendPresent = true;
    f.modeValid = true;
    f.backendHasCallback = true;
    return f;
}

// El gemelo positivo: sin él, un clasificador que rechazara todo pasaría los de abajo.
TEST(UsbStreamStart, AC050_3_WithEverythingInPlaceItProceeds) {
    EXPECT_EQ(classifyUsbStreamStart(ready()), UsbStreamStartStatus::PROCEED);
}

// Bug que atrapa: volver a colapsar la falta de callback en un fallo genérico.
TEST(UsbStreamStart, AC050_3_WithoutTheEngineCallbackItNamesTheCallback) {
    auto f = ready();
    f.backendHasCallback = false;
    EXPECT_EQ(classifyUsbStreamStart(f), UsbStreamStartStatus::NO_CALLBACK);
}

// Bug que atrapa: que "sin motor" se lea como "sin device" o como fallo genérico.
TEST(UsbStreamStart, AC050_3_WithoutTheEngineItNamesTheEngine) {
    auto f = ready();
    f.engineExists = false;
    // Sin motor tampoco hay callback, y el backend pudo quedar en el manager de
    // respaldo: la causa a nombrar es la de raíz, el motor.
    f.backendHasCallback = false;
    f.backendPresent = false;
    EXPECT_EQ(classifyUsbStreamStart(f), UsbStreamStartStatus::NO_ENGINE);
}

TEST(UsbStreamStart, AC050_3_WithoutAnInitializedDeviceItNamesTheDevice) {
    auto f = ready();
    f.deviceInitialized = false;
    f.backendPresent = false;
    f.backendHasCallback = false;
    EXPECT_EQ(classifyUsbStreamStart(f), UsbStreamStartStatus::NOT_INITIALIZED);
}

TEST(UsbStreamStart, AC050_3_WithoutTheBackendItNamesTheBackend) {
    auto f = ready();
    f.backendPresent = false;
    f.backendHasCallback = false;
    EXPECT_EQ(classifyUsbStreamStart(f), UsbStreamStartStatus::NO_BACKEND);
}

TEST(UsbStreamStart, AC050_3_AnUnknownModeIsRejectedBeforeStarting) {
    auto f = ready();
    f.modeValid = false;
    EXPECT_EQ(classifyUsbStreamStart(f), UsbStreamStartStatus::INVALID_MODE);
}

// El contrato previo, conservado: si ya está transmitiendo, pedirlo otra vez es éxito
// sin tocar nada (la JNIEXPORT devolvía `true`). `alreadyStreaming` lo arma la JNIEXPORT
// sólo con un backend libusb VIVO y corriendo: la marca de gUsbDeviceState sola queda en
// true después del fallback de salud, que destruye el backend.
TEST(UsbStreamStart, AC050_3_AlreadyStreamingIsSuccessWithoutRestarting) {
    auto f = ready();
    f.alreadyStreaming = true;
    f.backendHasCallback = false;
    EXPECT_EQ(classifyUsbStreamStart(f), UsbStreamStartStatus::OK);
}

// Los valores cruzan el JNI como `jint` y Kotlin los mapea uno por uno: un
// renumerado silencioso cambiaría la causa que ve el consumidor.
TEST(UsbStreamStart, AC050_3_TheWireValuesAreStable) {
    EXPECT_EQ(static_cast<int>(UsbStreamStartStatus::OK), 0);
    EXPECT_EQ(static_cast<int>(UsbStreamStartStatus::NOT_INITIALIZED), 1);
    EXPECT_EQ(static_cast<int>(UsbStreamStartStatus::NO_ENGINE), 2);
    EXPECT_EQ(static_cast<int>(UsbStreamStartStatus::NO_BACKEND), 3);
    EXPECT_EQ(static_cast<int>(UsbStreamStartStatus::NO_CALLBACK), 4);
    EXPECT_EQ(static_cast<int>(UsbStreamStartStatus::INVALID_MODE), 5);
    EXPECT_EQ(static_cast<int>(UsbStreamStartStatus::START_FAILED), 6);
}

}  // namespace
