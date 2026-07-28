package com.watermellonstudios.audio.domain

/**
 * Qué backend está moviendo el audio (WA-1.3).
 *
 * ## Por qué vive acá y no en `domain/usb/`
 *
 * Nació en `domain/usb/UsbAudioTypes.kt` con el doc comment "USB Audio backend
 * type", y eso nunca describió al tipo: de sus tres valores **uno solo** es USB.
 * Lo consumen [com.watermellonstudios.audio.api.AudioEngine.getAudioBackend] y
 * [com.watermellonstudios.audio.api.AudioEngine.setAudioBackend], que son API
 * central y no tienen nada que ver con USB — así que un consumidor de iOS, donde
 * USB está fuera de alcance por plataforma (D4), tenía que importar de un paquete
 * `usb` para preguntar por su backend.
 *
 * El acoplamiento medido de `commonMain` con `domain/usb/` era de **tres** puntos;
 * éste era el único que no era genuinamente USB. Los otros dos
 * (`isUsbBackendAvailable`, `setUsbLatencyProfile`) viven en `IAudioNativeBridge`,
 * son USB de verdad, y iOS ya los responde honestamente (`false` y un
 * `Result.failure` que nombra D4) en vez de fingir.
 *
 * El alias viejo sigue publicado y `@Deprecated` en `domain/usb/`, así que esto
 * **no rompe a nadie**: el consumidor migra el import cuando quiera.
 */
enum class AudioBackendType(val id: Int, val displayName: String) {
    /** No hay backend, o no se pudo determinar cuál. */
    NONE(0, "None"),

    /** El backend del sistema. Oboe (AAudio/OpenSL ES) en Android. */
    OBOE(1, "Oboe (System)"),

    /** USB directo por libusb. Sólo Android — ver D4. */
    LIBUSB(2, "USB Direct");

    companion object {
        /**
         * [NONE] para cualquier id desconocido: un id que no se reconoce es
         * ausencia de dato, no un backend concreto.
         */
        fun fromId(id: Int): AudioBackendType = entries.find { it.id == id } ?: NONE
    }
}
