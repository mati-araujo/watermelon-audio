package com.watermellonstudios.audio.internal.usb

import java.io.File
import javax.xml.parsers.DocumentBuilderFactory
import kotlin.test.Test
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-050 S2 (D9, AC-050.7) — **`WAKE_LOCK` lo declara la librería.**
 *
 * `UsbAudioManager.startStreaming` toma un wake lock parcial. La librería no declaraba el
 * permiso, así que una app que tampoco lo declaraba recibía `SecurityException` (MINI-041 #5);
 * NoisyPad zafaba porque se lo traía otro de sus módulos. Que el permiso llegue a la app por el
 * MERGE lo prueba el smoke en el g42 con el harness, que no lo declara (gemelo de abajo).
 *
 * 🔴 **El manifest que entra al AAR es `src/androidMain/AndroidManifest.xml`**, no
 * `src/main/AndroidManifest.xml`: con el plugin de Kotlin Multiplatform el source set `main` de
 * Android lee el de `androidMain`. La primera versión de esta etapa declaró el permiso en
 * `src/main` y este test lo afirmaba en VERDE mientras el AAR publicado salía sin él; lo agarró
 * `diff-published-artifact.py`. La autoridad sobre lo publicado es ese diff (declara el hash del
 * manifest del AAR) y el smoke en el device; este test es el chequeo barato de cada build.
 *
 * Los tests de unidad de Gradle corren con el directorio del módulo (`audio/`) como directorio
 * de trabajo: de ahí salen las rutas.
 */
class LibraryManifestTest {

    private fun permissions(path: String): Set<String> {
        val file = File(path)
        assertTrue(file.isFile, "no encontré el manifest en ${file.absolutePath}")
        val nodes = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }
            .newDocumentBuilder().parse(file).getElementsByTagName("uses-permission")
        return (0 until nodes.length).map {
            nodes.item(it).attributes.getNamedItemNS(ANDROID_NS, "name").nodeValue
        }.toSet()
    }

    /** Bug que atrapa: sacar el permiso del manifest de la librería (vuelve el SecurityException). */
    @Test
    fun `AC-050_7 el manifest de la libreria declara WAKE_LOCK`() {
        val declared = permissions("src/androidMain/AndroidManifest.xml")
        assertTrue(WAKE_LOCK in declared, "el manifest que se empaqueta en el AAR no declara WAKE_LOCK: $declared")
    }

    /**
     * El gemelo: el harness NO lo declara, así que si en el device el lock se toma, llegó por el
     * merge. Bug que atrapa: volver a declararlo en el harness, que dejaría al smoke probando el
     * manifest del harness y no el de la librería.
     */
    @Test
    fun `AC-050_7 gemelo - el harness no declara WAKE_LOCK, asi que el smoke prueba el merge`() {
        val declared = permissions("../harness/src/androidMain/AndroidManifest.xml")
        // Control del parser: el permiso que el harness sí declara se lee. Sin esto, un parser que no
        // leyera nada pasaría la ausencia de abajo.
        assertTrue("android.permission.RECORD_AUDIO" in declared, "el parser no leyó los permisos: $declared")
        assertFalse(WAKE_LOCK in declared, "el harness declara WAKE_LOCK: el smoke ya no prueba el merge")
    }

    private companion object {
        const val ANDROID_NS = "http://schemas.android.com/apk/res/android"
        const val WAKE_LOCK = "android.permission.WAKE_LOCK"
    }
}
