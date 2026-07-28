package com.watermellonstudios.audio.domain.looper

/**
 * Profundidad de bits con la que el looper escribe un WAV.
 *
 * ## Por qué está acá y no adentro de `AudioNativeBridge`
 *
 * Vivía como `enum class` **anidado** en el bridge de Android, o sea que el tipo de un
 * parámetro de la API pública sólo existía en `androidMain`. Eso alcanzaba mientras el
 * export fuera Android-only; con [com.watermellonstudios.audio.api.ILooperBridge] en
 * `commonMain` deja de alcanzar — una interfaz común no puede nombrar un tipo que sólo
 * una plataforma tiene.
 *
 * **Es un cambio incompatible para quien lo nombre como `AudioNativeBridge.ExportBitDepth`**,
 * y se sabe quién: NoisyPad, en `LooperControllerAdapter`, un `import`. Dentro de este
 * repo eran 4 líneas, todas del propio bridge.
 *
 * ## [raw] no es cosmético
 *
 * Es el número que viaja a la C API (`WmaExportOptions.bit_depth`), que documenta
 * "16, 24 o 32; cualquier otra cosa se trata como 16". Tenerlo en el enum evita el
 * `when` que cada llamador escribiría — y que es justo donde un 24 se convierte en 16
 * sin que nadie lo note.
 */
enum class ExportBitDepth(val raw: Int) {
    PCM_16(16),
    PCM_24(24),

    /** Float de 32 bits: round-trip sin pérdida, y el que usa la captura de sesión. */
    FLOAT_32(32),
}
