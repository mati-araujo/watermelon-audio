#ifndef WMA_FAKE_OS_BACKEND_H
#define WMA_FAKE_OS_BACKEND_H

/* Ver fake_os_backend.c. Header C, usable desde el test C++. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Los bytes que el dispositivo falso sirve como descriptor de configuracion
 * activo. No se copian: tienen que vivir mientras se los consulte. */
void wma_fake_usb_set_active_config(const uint8_t *raw, size_t len);

#ifdef __cplusplus
}
#endif

#endif
