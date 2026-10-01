package com.watermellonstudios.audio.internal.bridge

import java.io.File
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * REQ-050 S2 (AC-050.3) — **los valores de `UsbStreamStartStatus` son los mismos de los dos lados
 * del JNI.**
 *
 * `nativeStartUsbStreamingWithMode` devuelve un `jint` que Kotlin traduce a un error tipado. Los
 * tests del contrato usan la constante de Kotlin en el doble y en la aserción, así que renumerar
 * SÓLO el lado Kotlin (cambiar NO_BACKEND por NO_CALLBACK, por ejemplo) no ponía nada en rojo y
 * el consumidor recibía otra causa (review de S2). Esto lee el enum de `BackendManager.h`, la
 * única definición del lado C++, y lo compara con el objeto de Kotlin valor por valor.
 *
 * Los tests de unidad corren con `audio/` como directorio de trabajo.
 */
class UsbStreamStartStatusAnchorTest {

    @Test
    fun `AC-050_3 UsbStreamStartStatus coincide valor por valor con el enum de C++`() {
        val header = File("src/main/cpp/backends/BackendManager.h")
        assertTrue(header.isFile, "no encontré ${header.absolutePath}")
        val body = Regex("""enum class UsbStreamStartStatus\s*:\s*int\s*\{(.*?)\};""", RegexOption.DOT_MATCHES_ALL)
            .find(header.readText())?.groupValues?.get(1)
            ?: error("no encontré `enum class UsbStreamStartStatus` en BackendManager.h")
        val cpp = Regex("""(\w+)\s*=\s*(-?\d+)""").findAll(body)
            .associate { it.groupValues[1] to it.groupValues[2].toInt() }
            .filterKeys { it != "PROCEED" } // no cruza el JNI: ver el KDoc del enum

        val kotlin = mapOf(
            "OK" to UsbStreamStartStatus.OK,
            "NOT_INITIALIZED" to UsbStreamStartStatus.NOT_INITIALIZED,
            "NO_ENGINE" to UsbStreamStartStatus.NO_ENGINE,
            "NO_BACKEND" to UsbStreamStartStatus.NO_BACKEND,
            "NO_CALLBACK" to UsbStreamStartStatus.NO_CALLBACK,
            "INVALID_MODE" to UsbStreamStartStatus.INVALID_MODE,
            "START_FAILED" to UsbStreamStartStatus.START_FAILED,
        )
        assertEquals(cpp, kotlin, "los dos lados del JNI no numeran igual las causas")
    }
}
