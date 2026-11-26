// REQ-047 S1 — AC-047.5: los dos defectos de descriptor.c que arregla libusb
// 1.0.30 (upstream bc088617, issue libusb#1813; CVE-2026-47104 y
// CVE-2026-23679).
//
// Corre el nucleo REAL de libusb vendorizado (thirdparty/libusb) sobre un
// backend de SO falso (fake_os_backend.c) y entra por la misma puerta que
// LibusbBackend en Android: libusb_wrap_sys_device(). El dispositivo falso
// sirve los bytes que elige el test, como lo haria un dispositivo hostil.
//
//   A) parse_interface(): un interface descriptor con bNumEndpoints > 0 cuyos
//      endpoints no llegan (un descriptor extra truncado antes). El snapshot
//      viejo devolvia bNumEndpoints = 1 con endpoint == NULL, y el primero que
//      recorre el array (libusb_get_max_packet_size, un ejemplo de libusb, un
//      consumidor) desreferencia NULL. El invariante lo documenta libusb.h.
//
//   B) parse_iad_array(): la guarda del bucle comparaba contra el tamano
//      TOTAL, asi que con un solo byte de resto leia buf[1] un byte despues
//      del malloc. Es una lectura fuera de limites que SOLO ve ASan: sin ASan
//      el byte de mas se lee en silencio. Por eso este test se SALTEA fuera de
//      un build con ASan — un verde que no pudo ver el defecto no se puede
//      leer como cobertura. Lo corren el paso de sanitizers de gate.sh y el job
//      cpp-tests-asan de ubuntu.
//
// Cada caso tiene su control positivo: un descriptor VALIDO por el mismo
// camino, para que un backend falso que no sirviera nada no pase por "no
// crasheo".

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "libusb.h"

#include "fake_os_backend.h"

#if defined(__SANITIZE_ADDRESS__)
#define WMA_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WMA_HAS_ASAN 1
#endif
#endif

namespace {

// El descriptor minimo valido de upstream (tests/fuzz/corpus/descriptor_parsers/
// min_valid_config.bin en v1.0.30): config + interface con 1 endpoint + endpoint.
const std::vector<uint8_t> kValidConfig = {
    0x09, 0x02, 0x19, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x00,  // config, wTotalLength 25
    0x09, 0x04, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00,  // interface, bNumEndpoints 1
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,              // endpoint 0x81 bulk IN
};

// Defecto A. El reproductor de upstream (regression_bug_a_endpoint_null.bin):
// el config promete 25 bytes y el dispositivo entrega 20. Despues del
// interface (bNumEndpoints 1) queda un header 07 05 de 2 bytes: el descriptor
// "extra" es mas largo que lo que queda y parse_interface() vuelve temprano,
// antes de alocar el array de endpoints.
const std::vector<uint8_t> kEndpointsDeclaredButMissing = {
    0x09, 0x02, 0x19, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00,
    0x07, 0x05,
};

// Defecto B, por el camino publico. Un config de 10 bytes con wTotalLength 10:
// tras el header de 9 queda UN byte, y el bucle de conteo de IADs lee el
// bDescriptorType del descriptor siguiente (buf[1]) fuera del malloc de 10.
const std::vector<uint8_t> kIadOneByteLeft = {
    0x09, 0x02, 0x0a, 0x00, 0x00, 0x01, 0x00, 0xa0, 0x00,
    0x02,
};

// Un config valido con un IAD: control positivo del camino de IADs.
const std::vector<uint8_t> kValidConfigWithIad = {
    0x09, 0x02, 0x11, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,  // config, wTotalLength 17
    0x08, 0x0b, 0x00, 0x02, 0x01, 0x01, 0x00, 0x00,        // IAD: interfaces 0..1, audio
};

class LibusbDescriptorCve : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(libusb_init_context(&ctx_, nullptr, 0), LIBUSB_SUCCESS);
        ASSERT_EQ(libusb_wrap_sys_device(ctx_, /*sys_dev=*/47, &handle_), LIBUSB_SUCCESS);
        dev_ = libusb_get_device(handle_);
        ASSERT_NE(dev_, nullptr);
    }

    void TearDown() override {
        wma_fake_usb_set_active_config(nullptr, 0);
        if (handle_) libusb_close(handle_);
        if (ctx_) libusb_exit(ctx_);
    }

    void serve(const std::vector<uint8_t>& raw) {
        wma_fake_usb_set_active_config(raw.data(), raw.size());
    }

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    libusb_device* dev_ = nullptr;
};

struct ConfigGuard {
    libusb_config_descriptor* p = nullptr;
    ~ConfigGuard() { if (p) libusb_free_config_descriptor(p); }
};

struct IadGuard {
    libusb_interface_association_descriptor_array* p = nullptr;
    ~IadGuard() { if (p) libusb_free_interface_association_descriptors(p); }
};

// Recorre el array de endpoints como lo hace find_endpoint() de libusb: si el
// invariante no se cumple, esto es la desreferencia de NULL.
int touchEveryEndpoint(const libusb_config_descriptor& cfg) {
    int sum = 0;
    for (int i = 0; i < cfg.bNumInterfaces; ++i) {
        const libusb_interface& itf = cfg.interface[i];
        for (int a = 0; a < itf.num_altsetting; ++a) {
            const libusb_interface_descriptor& alt = itf.altsetting[a];
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                sum += alt.endpoint[e].bEndpointAddress;
            }
        }
    }
    return sum;
}

}  // namespace

// Control positivo de A: el mismo camino con un descriptor valido entrega el
// endpoint. Sin esto, un backend que no sirviera nada daria verde abajo.
TEST_F(LibusbDescriptorCve, ValidConfigDeliversItsEndpoint_AC_047_5) {
    serve(kValidConfig);
    ConfigGuard cfg;
    ASSERT_EQ(libusb_get_active_config_descriptor(dev_, &cfg.p), LIBUSB_SUCCESS);
    ASSERT_EQ(cfg.p->bNumInterfaces, 1);
    ASSERT_EQ(cfg.p->interface[0].num_altsetting, 1);
    const libusb_interface_descriptor& alt = cfg.p->interface[0].altsetting[0];
    ASSERT_EQ(alt.bNumEndpoints, 1);
    ASSERT_NE(alt.endpoint, nullptr);
    EXPECT_EQ(alt.endpoint[0].bEndpointAddress, 0x81);
}

// Defecto A. Rechazar el descriptor es aceptable; aceptarlo con
// bNumEndpoints > 0 y endpoint == NULL, no.
TEST_F(LibusbDescriptorCve, EndpointsDeclaredButMissingKeepTheInvariant_AC_047_5) {
    serve(kEndpointsDeclaredButMissing);
    ConfigGuard cfg;
    const int r = libusb_get_active_config_descriptor(dev_, &cfg.p);
    if (r != LIBUSB_SUCCESS) {
        EXPECT_LT(r, 0);
        return;
    }
    for (int i = 0; i < cfg.p->bNumInterfaces; ++i) {
        const libusb_interface& itf = cfg.p->interface[i];
        for (int a = 0; a < itf.num_altsetting; ++a) {
            const libusb_interface_descriptor& alt = itf.altsetting[a];
            ASSERT_TRUE(alt.bNumEndpoints == 0 || alt.endpoint != nullptr)
                << "interface " << i << " alt " << a << " declara bNumEndpoints="
                << int(alt.bNumEndpoints) << " con endpoint == NULL: el primero que "
                << "recorra el array desreferencia NULL";
        }
    }
    touchEveryEndpoint(*cfg.p);
}

// Control positivo de B: el camino de IADs cuenta y entrega el IAD valido.
TEST_F(LibusbDescriptorCve, ValidConfigDeliversItsIad_AC_047_5) {
    serve(kValidConfigWithIad);
    IadGuard iad;
    ASSERT_EQ(libusb_get_active_interface_association_descriptors(dev_, &iad.p),
              LIBUSB_SUCCESS);
    ASSERT_EQ(iad.p->length, 1);
    EXPECT_EQ(iad.p->iad[0].bFirstInterface, 0);
    EXPECT_EQ(iad.p->iad[0].bInterfaceCount, 2);
    EXPECT_EQ(iad.p->iad[0].bFunctionClass, LIBUSB_CLASS_AUDIO);
}

// Defecto B. El veredicto lo da ASan (heap-buffer-overflow READ en
// parse_iad_array), no una asercion: sin ASan se saltea.
TEST_F(LibusbDescriptorCve, IadCountLoopDoesNotReadPastTheBuffer_AC_047_5) {
#ifndef WMA_HAS_ASAN
    GTEST_SKIP() << "la lectura fuera de limites solo es observable bajo ASan: "
                    "este build no la puede ver, y un verde aca no seria cobertura";
#else
    serve(kIadOneByteLeft);
    IadGuard iad;
    const int r = libusb_get_active_interface_association_descriptors(dev_, &iad.p);
    if (r == LIBUSB_SUCCESS) {
        EXPECT_EQ(iad.p->length, 0);
    } else {
        EXPECT_LT(r, 0);
    }
#endif
}
