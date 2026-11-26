/**
 * test_usb_selection_request.cpp — REQ-050 S3 (AC-050.8, D17).
 *
 * El runner renegocia el altsetting y el reloj de cada fila con `selectAltsetting` /
 * `selectClockSource`, y esa selección es PEGAJOSA: el backend la aplica en cada arranque
 * hasta que alguien la limpie. Para devolverle al consumidor la selección automática, las
 * dos JNIEXPORT que ya existen aceptan un centinela: (-1, -1, -1) para el altsetting y 0
 * para el reloj.
 *
 * Lo que se afirma acá es la otra mitad del contrato: el centinela es EXACTO. Cualquier
 * otro valor fuera de rango se sigue rechazando, para que un índice negativo mal calculado
 * no se lea como "volver a automático".
 */

#include "backends/BackendManager.h"

#include <gtest/gtest.h>

namespace {

using watermelon_audio::UsbSelectionRequest;
using watermelon_audio::classifyAltsettingRequest;
using watermelon_audio::classifyClockSourceRequest;

TEST(UsbSelectionRequest, AC050_8_TheExactAltsettingSentinelClears) {
    EXPECT_EQ(classifyAltsettingRequest(-1, -1, -1), UsbSelectionRequest::CLEAR);
}

// Bug que atrapa: tomar CUALQUIER negativo como centinela.
TEST(UsbSelectionRequest, AC050_8_AnyOtherNegativeAltsettingIsRejected) {
    EXPECT_EQ(classifyAltsettingRequest(-1, -1, 0), UsbSelectionRequest::REJECT);
    EXPECT_EQ(classifyAltsettingRequest(-1, 0, -1), UsbSelectionRequest::REJECT);
    EXPECT_EQ(classifyAltsettingRequest(0, -1, -1), UsbSelectionRequest::REJECT);
    EXPECT_EQ(classifyAltsettingRequest(-2, -2, -2), UsbSelectionRequest::REJECT);
    EXPECT_EQ(classifyAltsettingRequest(1, 2, -3), UsbSelectionRequest::REJECT);
}

// El gemelo positivo: un pedido válido sigue siendo una selección.
TEST(UsbSelectionRequest, AC050_8_AValidAltsettingIsASelection) {
    EXPECT_EQ(classifyAltsettingRequest(1, 3, 0), UsbSelectionRequest::SELECT);
    EXPECT_EQ(classifyAltsettingRequest(0, 0, 0), UsbSelectionRequest::SELECT);
}

TEST(UsbSelectionRequest, AC050_8_ClockZeroClears) {
    EXPECT_EQ(classifyClockSourceRequest(0), UsbSelectionRequest::CLEAR);
}

// Bug que atrapa: un negativo o un id que no entra en un byte leído como "automático".
TEST(UsbSelectionRequest, AC050_8_OutOfRangeClocksAreRejected) {
    EXPECT_EQ(classifyClockSourceRequest(-1), UsbSelectionRequest::REJECT);
    EXPECT_EQ(classifyClockSourceRequest(256), UsbSelectionRequest::REJECT);
}

TEST(UsbSelectionRequest, AC050_8_AValidClockIsASelection) {
    EXPECT_EQ(classifyClockSourceRequest(1), UsbSelectionRequest::SELECT);
    EXPECT_EQ(classifyClockSourceRequest(255), UsbSelectionRequest::SELECT);
}

}  // namespace
