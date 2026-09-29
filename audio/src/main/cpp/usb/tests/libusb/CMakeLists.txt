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
