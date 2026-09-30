/*
 * config.h de HOST para compilar el nucleo de libusb en la suite de tests
 * (REQ-047 S1). NO es el del build que shippea: ese es thirdparty/libusb/android/config.h.
 *
 * Solo lo que el nucleo necesita en Linux y macOS con PLATFORM_POSIX. Sin
 * HAVE_PIPE2/HAVE_EVENTFD/HAVE_TIMERFD a proposito: macOS no los tiene y el
 * nucleo tiene camino portable para los tres. El backend de SO es falso
 * (fake_os_backend.c), asi que no se compila ningun os/<plataforma>_usb.c.
 */
#define DEFAULT_VISIBILITY __attribute__ ((visibility ("default")))
#define ENABLE_LOGGING 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_NFDS_T 1
#define HAVE_SYS_TIME_H 1
#define PLATFORM_POSIX 1
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
