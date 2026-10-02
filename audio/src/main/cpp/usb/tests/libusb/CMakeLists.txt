# ============================================================================
# libusb_descriptor_tests — REQ-047 S1, AC-047.5
#
# El nucleo REAL de libusb vendorizado (thirdparty/libusb/libusb/*.c) compilado
# para el host, sobre un backend de SO falso (fake_os_backend.c) que sirve los
# descriptores que elige el test. Es un target APARTE de usb_tests a proposito:
# usb_tests declara que no compila libusb (ver su CMakeLists), y ese contrato
# sigue valiendo para el.
#
# Lo que NO es: no compila ningun backend de SO (os/linux_usbfs.c, darwin...),
# ni el config.h de Android. Usa host-config/config.h, que es de este test.
# El .so que shippea lo arma thirdparty/libusb.cmake y no pasa por aca.
#
# Solo se configura desde tests/CMakeLists.txt (el agregador): usa su
# googletest, sus sanitizers y su gate de warnings, y no declara los propios.
#
# Sin WIN32: el nucleo de libusb para Windows es otro (events_windows, threads_
# windows) y la suite de host no corre en Windows en el CI.
# ============================================================================

if(WIN32)
    message(STATUS "libusb_descriptor_tests: salteado en Windows")
    return()
endif()

enable_language(C)

set(WMA_LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../thirdparty/libusb)

# Solo el nucleo portable: el mismo recorte que thirdparty/libusb.cmake, sin el
# backend de Linux (linux_usbfs.c / linux_netlink.c), que reemplaza el falso.
add_library(wma_libusb_host STATIC
    ${WMA_LIBUSB_DIR}/libusb/core.c
    ${WMA_LIBUSB_DIR}/libusb/descriptor.c
    ${WMA_LIBUSB_DIR}/libusb/hotplug.c
    ${WMA_LIBUSB_DIR}/libusb/io.c
    ${WMA_LIBUSB_DIR}/libusb/strerror.c
    ${WMA_LIBUSB_DIR}/libusb/sync.c
    ${WMA_LIBUSB_DIR}/libusb/os/events_posix.c
    ${WMA_LIBUSB_DIR}/libusb/os/threads_posix.c
)
target_include_directories(wma_libusb_host
    PUBLIC
        ${WMA_LIBUSB_DIR}/libusb
        ${CMAKE_CURRENT_SOURCE_DIR}/host-config
        ${WMA_LIBUSB_DIR}/libusb/os
)
# Codigo de terceros: no se lo somete al gate de warnings de la suite (igual
# que a googletest, que se baja ANTES de ese gate). Los sanitizers SI le llegan
# — son add_compile_options del raiz — y son el punto de este test.
target_compile_options(wma_libusb_host PRIVATE -w)
find_package(Threads REQUIRED)
target_link_libraries(wma_libusb_host PUBLIC Threads::Threads)

# El backend falso es NUESTRO: va en el ejecutable, con el gate de warnings.
# libusb llama a `usbi_backend` y el backend llama a usbi_alloc_device(): la
# referencia es circular entre la .a y el .o, y el linker la resuelve porque el
# .o entra entero al link.
add_executable(libusb_descriptor_tests
    test_libusb_descriptor_cves.cpp
    fake_os_backend.c
)
target_link_libraries(libusb_descriptor_tests PRIVATE wma_libusb_host GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(libusb_descriptor_tests)

# ============================================================================
# libusb_backend_lifetime_tests — MINI-042 (M1, D6)
#
# El LibusbBackend REAL —y el UsbTransferManager real— compilados para el host
# sobre el mismo nucleo de libusb + backend de SO falso de arriba, junto con el
# BackendManager real. Es el reproductor de "el backend USB se lee despues de
# liberado": bajo ASan y TSan, contra el acceso con alcance del manager y el
# try_lock de los getters del backend.
#
# Un target APARTE de libusb_descriptor_tests a proposito: aquel compila SOLO el
# nucleo de libusb, y su encabezado lo dice. PlatformBackends.cpp se reemplaza
# por test_platform_backends_libusb.cpp (el downcast real a LibusbBackend).
# ============================================================================
set(WMA_LIFETIME_CPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_executable(libusb_backend_lifetime_tests
    test_libusb_backend_lifetime.cpp
    test_platform_backends_libusb.cpp
    fake_os_backend.c
    ${WMA_LIFETIME_CPP_ROOT}/backends/BackendManager.cpp
    ${WMA_LIFETIME_CPP_ROOT}/backends/SplitBackend.cpp
    ${WMA_LIFETIME_CPP_ROOT}/backends/DriftResampler.cpp
    ${WMA_LIFETIME_CPP_ROOT}/backends/LibusbBackend.cpp
    ${WMA_LIFETIME_CPP_ROOT}/usb/UsbTransferManager.cpp
    ${WMA_LIFETIME_CPP_ROOT}/usb/UsbDescriptorParser.cpp
    ${WMA_LIFETIME_CPP_ROOT}/usb/UsbVolumeControl.cpp
    ${WMA_LIFETIME_CPP_ROOT}/usb/AudioFormatConverter.cpp
    ${WMA_LIFETIME_CPP_ROOT}/platform/Logger.cpp
    ${WMA_LIFETIME_CPP_ROOT}/platform/PlatformAndroid.cpp
)
target_include_directories(libusb_backend_lifetime_tests PRIVATE
    ${WMA_LIFETIME_CPP_ROOT}
    ${WMA_LIFETIME_CPP_ROOT}/core/tests/support
)
target_link_libraries(libusb_backend_lifetime_tests PRIVATE wma_libusb_host GTest::gtest_main)
gtest_discover_tests(libusb_backend_lifetime_tests)

# LibusbBackend.cpp nunca habia pasado por el gate de warnings del host (solo lo
# compilaba el NDK, sin -Werror) y trae seis variables de diagnostico muertas en
# dspThreadFunc. Se degradan ESAS DOS clases y SOLO para ese archivo: borrarlas
# tocaria el hilo RT, que MINI-042 no toca (no-negociable). Los demas warnings,
# y todos los de los otros archivos de este target, siguen en -Werror.
set_source_files_properties(${WMA_LIFETIME_CPP_ROOT}/backends/LibusbBackend.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-but-set-variable;-Wno-unused-variable")
