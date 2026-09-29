/*
 * Backend de SO FALSO para libusb (REQ-047 S1, AC-047.5).
 *
 * Deja correr el nucleo REAL de libusb (core.c, descriptor.c, io.c...) en el
 * host, entrando por la MISMA puerta que usa LibusbBackend en Android:
 * libusb_wrap_sys_device(). El "sys_dev" no es un fd: es ignorado, y el
 * dispositivo que se crea sirve como descriptor de configuracion activo los
 * bytes crudos que el test cargo con wma_fake_usb_set_active_config().
 *
 * Es lo minimo para que el camino publico llegue a parse_configuration() y a
 * parse_iad_array() con bytes que elige el test — o sea, lo que haria un
 * dispositivo USB hostil conectado al telefono. No reimplementa nada de
 * libusb: implementa la costura que libusb define para los backends de SO
 * (struct usbi_os_backend, libusbi.h).
 */
#include <config.h>

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "libusbi.h"

#include "fake_os_backend.h"

static const uint8_t *g_active_config = NULL;
static size_t g_active_config_len = 0;

void wma_fake_usb_set_active_config(const uint8_t *raw, size_t len)
{
	g_active_config = raw;
	g_active_config_len = len;
}

static int fake_wrap_sys_device(struct libusb_context *ctx,
	struct libusb_device_handle *handle, intptr_t sys_dev)
{
	struct libusb_device *dev = usbi_alloc_device(ctx, (unsigned long)sys_dev);
	int r;

	if (!dev)
		return LIBUSB_ERROR_NO_MEM;

	/* Device descriptor valido, una sola configuracion. */
	dev->device_descriptor.bLength = LIBUSB_DT_DEVICE_SIZE;
	dev->device_descriptor.bDescriptorType = LIBUSB_DT_DEVICE;
	dev->device_descriptor.bcdUSB = 0x0200;
	dev->device_descriptor.bMaxPacketSize0 = 64;
	dev->device_descriptor.bNumConfigurations = 1;

	r = usbi_sanitize_device(dev);
	if (r < 0) {
		libusb_unref_device(dev);
		return r;
	}

	handle->dev = dev;
	return LIBUSB_SUCCESS;
}

static void fake_close(struct libusb_device_handle *handle)
{
	(void)handle;
}

/* Como un dispositivo real: devuelve hasta `len` bytes de lo que tiene, aunque
 * su wTotalLength prometa mas (asi entra un descriptor truncado). */
static int serve_config(uint8_t *buffer, size_t len)
{
	size_t n;

	if (!g_active_config)
		return LIBUSB_ERROR_NOT_FOUND;
	n = len < g_active_config_len ? len : g_active_config_len;
	memcpy(buffer, g_active_config, n);
	return (int)n;
}

static int fake_get_active_config_descriptor(struct libusb_device *dev,
	void *buffer, size_t len)
{
	(void)dev;
	return serve_config(buffer, len);
}

static int fake_get_config_descriptor(struct libusb_device *dev,
	uint8_t config_index, void *buffer, size_t len)
{
	(void)dev;
	if (config_index != 0)
		return LIBUSB_ERROR_NOT_FOUND;
	return serve_config(buffer, len);
}

#if defined(__APPLE__)
/* En Apple, libusbi.h NO define los relojes inline: los pone el backend de SO
 * (os/darwin_usb.c), que aca no se compila. Mismo cuerpo que el de Linux. */
void usbi_get_monotonic_time(struct timespec *tp)
{
	clock_gettime(CLOCK_MONOTONIC, tp);
}

void usbi_get_real_time(struct timespec *tp)
{
	clock_gettime(CLOCK_REALTIME, tp);
}
#endif

const struct usbi_os_backend usbi_backend = {
	.name = "wma-fake (host test)",
	.caps = 0,
	.wrap_sys_device = fake_wrap_sys_device,
	.close = fake_close,
	.get_active_config_descriptor = fake_get_active_config_descriptor,
	.get_config_descriptor = fake_get_config_descriptor,
};
