package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.LooperStateListener
import org.junit.AfterClass
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * MINI-014 — **el registro que fallaba mudo, ahora dice cuál fue la causa.**
 *
 * Es el desenlace del issue **#228**, cerrado `NOT_PLANNED` porque la causa raíz estaba
 * en el **consumidor**: NoisyPad registraba el listener **antes de inicializar el motor**
 * y **descartaba el `Boolean`** de retorno. La API funcionaba.
 *
 * 🔴 **Pero el camino de falla era MUDO**: `nativeLooperRegisterStateListener` arrancaba
 * con `if (!g_jniState.engine) return JNI_FALSE;` sin una sola línea. O sea que la falla
 * exacta que costó **una sesión entera de device** no escribía nada en logcat. Es la
 * misma familia que REQ-020 —que hizo hablar al *lookup* de un callback opcional en
 * `null`— un eslabón antes, y el mismo precedente de MINI-008 y MINI-013: un "no" que no
 * dice **cuál** de las causas fue manda a investigar al lugar equivocado.
 *
 * ## Por qué esto se puede AFIRMAR y no es prosa
 *
 * Un `LOGE` agregado y no verificado es exactamente lo que este repo dejó de aceptar. El
 * instrumento ya existía: el **segundo sink** del `Logger` (App V §3.2) captura a un ring
 * en memoria, y ese ring **ya cruza el JNI** (`nativeSetLogCaptureEnabled` /
 * `nativeDrainCapturedLogs` / `nativeGetLogCaptureDropped`). Así que acá la línea se
 * **lee**, no se supone.
 *
 * Efecto lateral declarado: esas tres estaban en el hueco del arnés, y traen el camino de
 * retorno `jobjectArray` + `NewStringUTF` — que **ninguna otra clase del arnés ejerce**.
 *
 * ## Un solo `@Test`, y es a propósito
 *
 * El escenario **necesita un motor virgen** y después uno arriba, en ese orden. Cada clase
 * del arnés corre en su propia JVM (`forkEvery = 1`), pero los `@Test` de una misma clase
 * comparten el motor —que es un singleton de proceso— y JUnit **no garantiza su orden**.
 * Partirlo en dos métodos haría que el veredicto dependiera de ese orden: verde o rojo
 * según cómo los corra Gradle. Un solo método deja la secuencia fijada por el código.
 *
 * 🔴 **El caso de éxito NO es decoración**: sin él, un `LOGE` incondicional pasaría el
 * primer assert igual de bien. Es el gemelo obligatorio de todo test de "acá tiene que
 * hablar" — ver el KDoc de [JniHarness] para el resto de los límites del arnés.
 *
 * ## Lo que este archivo NO cubre, dicho
 *
 * Los otros dos `JNI_FALSE` de esa función también dejaron de ser mudos, y **ninguno de
 * los dos es alcanzable desde acá**:
 *
 * - `!listener` — `setLooperStateListener(null)` enruta a **desregistrar**, y
 *   `nativeLooperRegisterStateListener` es `private external fun`. Llegar ahí exigiría
 *   reflection, o sea entrar por atrás salteando la precondición del camino real.
 * - `NewGlobalRef` / `GetObjectClass` fallando — sólo bajo agotamiento de memoria de la VM.
 */
class LooperRegistrationSilenceJniTest {

    companion object {
        private const val OWNER = "LooperRegistrationSilenceJniTest"

        /**
         * **Trinquete bidireccional** — ver `JniCoverage.ratchet`: ejercer de menos es
         * rojo, y ejercer de más también, para que sumar cobertura aparezca en el diff.
         */
        private val COVERED = setOf(
            "nativeSetLogCaptureEnabled",
            "nativeDrainCapturedLogs",
            "nativeGetLogCaptureDropped",
            "nativeLooperRegisterStateListener",
            "nativeStartTuner",
        )

        /**
         * El discriminador de la causa "motor sin inicializar", y va **al principio** del
         * mensaje a propósito: el ring formatea en un buffer de 300 chars
         * (`LogCaptureBuffer::capture`), así que un discriminador al final sobreviviría al
         * assert hoy y desaparecería en silencio el día que el mensaje crezca.
         */
        private const val CAUSA_MOTOR = "el motor todavia no esta inicializado"

        /** La otra causa del mismo `JNI_FALSE`. Acá se usa para exigir que NO aparezca. */
        private const val CAUSA_LISTENER_NULO = "el listener llego en null"

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private class unListener : LooperStateListener {
        override fun onTrackProgress(trackIndex: Int, progress: Float) = Unit
        override fun onTrackPlayingChanged(trackIndex: Int, isPlaying: Boolean) = Unit
        override fun onTrackPeakChanged(trackIndex: Int, peakLevel: Float) = Unit
        override fun onBeat(beatIndex: Int, nextBeatFrame: Int) = Unit
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    private fun drenar(): List<String> =
        jni("nativeDrainCapturedLogs") { it.drainCapturedLogs() }.toList()

    /**
     * AC-1 y AC-4 — con el motor abajo el registro **dice por qué**; con el motor arriba
     * **no dice nada**.
     *
     * Esta clase **nunca arranca el motor antes del primer tramo**, y eso es la
     * precondición entera: `g_jniState.engine` sólo lo puebla `ensureEngine()`, así que
     * una JVM propia que no lo llamó ve el motor en `nullptr`. Es el mismo patrón que ya
     * usan los tests de ausencia del arnés, y la razón de `forkEvery = 1`.
     */
    @Test
    fun `el registro sin motor deja rastro que nombra la causa, y el exitoso no`() {
        jni("nativeSetLogCaptureEnabled") { it.setLogCaptureEnabled(true) }
        // Todo lo que haya quedado de cargar la librería queda afuera del veredicto.
        drenar()

        // --- Tramo 1: motor ABAJO ---------------------------------------------------
        assertFalse(
            jni("nativeLooperRegisterStateListener") { it.setLooperStateListener(unListener()) },
            "sin motor el registro tiene que fallar: si diera true, el resto de este test " +
                "estaría midiendo otro escenario",
        )

        val sinMotor = drenar()
        assertEquals(
            0,
            jni("nativeGetLogCaptureDropped") { it.getLogCaptureDropped() },
            "el ring descartó líneas: 'no encontré el rastro' dejaría de ser una respuesta " +
                "confiable, porque el rastro podría haberse caído del ring",
        )
        val acusan = sinMotor.filter { it.contains(CAUSA_MOTOR) }
        assertEquals(
            1,
            acusan.size,
            "esperaba UNA línea nombrando la causa 'motor sin inicializar' y hubo ${acusan.size}. " +
                "Lo drenado fue: $sinMotor",
        )
        assertTrue(
            acusan.single().startsWith("E/"),
            "la línea tiene que ser de nivel ERROR —un registro que falla no es informativo—, " +
                "y fue: ${acusan.single()}",
        )
        assertTrue(
            sinMotor.none { it.contains(CAUSA_LISTENER_NULO) },
            "acusó ADEMÁS al listener nulo, que no es lo que pasó. Un mensaje que nombra las " +
                "dos causas no distingue ninguna, que es el defecto que MINI-008 y MINI-013 " +
                "vinieron a borrar. Lo drenado fue: $sinMotor",
        )

        // --- Tramo 2: motor ARRIBA, el gemelo ---------------------------------------
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")
        drenar() // el arranque loguea lo suyo; no es parte del veredicto de abajo.

        assertTrue(
            jni("nativeLooperRegisterStateListener") { it.setLooperStateListener(unListener()) },
            "con el motor arriba el registro tiene que andar",
        )

        val conMotor = drenar()
        assertTrue(
            conMotor.none { it.contains(CAUSA_MOTOR) || it.contains(CAUSA_LISTENER_NULO) },
            "el registro EXITOSO acusó una causa de falla. Sin este assert, un LOGE " +
                "incondicional pasaría el tramo 1 igual de bien. Lo drenado fue: $conMotor",
        )
    }
}
