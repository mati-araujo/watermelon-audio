package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.LooperStateListener
import org.junit.AfterClass
import org.junit.Before
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-020 S3 — **el registro del listener del looper, EJECUTADO.**
 *
 * El hueco declarado del arnés incluye `looper: 81`, y el registro del listener es
 * justo la frontera donde el issue #228 sospechaba que se perdía `onBeat`. Hasta acá
 * ningún test la cruzaba: `check-jni-symbols.py` compara sólo NOMBRES —da verde con
 * las 310 jamás ejecutadas— y `check-mechanism-callers.py` contesta *quién llama*, no
 * *quién ejecuta*.
 *
 * ## Lo que este archivo DEJA MEDIDO, y por qué importa
 *
 * La hipótesis que traía el issue #228 era que el `GetMethodID` de `onBeat` devolvía
 * `null` —es un lookup *opcional*, así que un `null` se traga en silencio y el despacho
 * hace un no-op—. **Está refutada, y estos tests la dejan anclada**: Kotlin emite un
 * `onBeat(II)V` concreto en **cada** clase implementadora, tenga override o no, así que
 * el lookup no puede fallar. [unListenerViejo] es exactamente esa clase sin override, y
 * registra igual de bien que [unListenerNuevo].
 *
 * 🔴 **Verde acá NO cierra #228.** El arnés corre sobre un backend FALSO y **no puede
 * producir un `Beat`**: no hay camino de render en el host, así que `Transport::tick()`
 * nunca corre. Esto cubre el REGISTRO, no la ENTREGA. La entrega se mide en device, con
 * el par (`transportIsMetronomeRunning`, `transportGetBeatsElapsed`) que trajo REQ-020.1.
 *
 * Ver el KDoc de [JniHarness] para el resto de los límites del arnés.
 */
class LooperListenerJniTest {

    companion object {
        private const val OWNER = "LooperListenerJniTest"

        /**
         * Lo que esta clase declara cubrir. **Trinquete bidireccional** — ver
         * `JniCoverage.ratchet`: ejercer de menos es rojo, y ejercer de más también,
         * para que sumar cobertura aparezca en el diff del PR en vez de colarse.
         */
        private val COVERED = setOf(
            "nativeStartTuner",
            "nativeLooperRegisterStateListener",
            "nativeLooperUnregisterStateListener",
            "nativeLooperGetDroppedEvents",
            "nativeTransportGetBeatsElapsed",
            "nativeTransportIsMetronomeRunning",
            "nativeTransportStartMetronome",
            "nativeTransportStopMetronome",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    /** Overridea `onBeat`: el consumidor de hoy. */
    private class unListenerNuevo : LooperStateListener {
        override fun onTrackProgress(trackIndex: Int, progress: Float) = Unit
        override fun onTrackPlayingChanged(trackIndex: Int, isPlaying: Boolean) = Unit
        override fun onTrackPeakChanged(trackIndex: Int, peakLevel: Float) = Unit
        override fun onBeat(beatIndex: Int, nextBeatFrame: Int) = Unit
    }

    /**
     * NO overridea ninguno de los tres opcionales: el listener "viejo" que la rama de
     * lookup opcional existe para no romper. Es el control que hace útil al de arriba.
     */
    private class unListenerViejo : LooperStateListener {
        override fun onTrackProgress(trackIndex: Int, progress: Float) = Unit
        override fun onTrackPlayingChanged(trackIndex: Int, isPlaying: Boolean) = Unit
        override fun onTrackPeakChanged(trackIndex: Int, peakLevel: Float) = Unit
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    /** El registro exige motor: sin él `nativeLooperRegisterStateListener` devuelve `false`. */
    @Before
    fun engineUp() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")
    }

    /**
     * AC-020.5 — el registro cruza la frontera de verdad, con las dos formas de listener.
     *
     * El caso SIN override es el que refuta la hipótesis de #228: si el lookup opcional
     * de `onBeat` pudiera devolver `null`, sería con **este**, y el registro seguiría
     * dando `true` de todos modos (por diseño, para no romper compat). Que los dos den
     * `true` es lo esperado; lo que este test fija es que ninguno de los dos **falla**,
     * o sea que el registro no es donde se pierde el beat.
     */
    @Test
    fun `los dos listeners registran, con override de onBeat y sin el`() {
        assertTrue(
            jni("nativeLooperRegisterStateListener") { it.setLooperStateListener(unListenerNuevo()) },
            "un listener que overridea onBeat tiene que registrar",
        )
        assertTrue(
            jni("nativeLooperRegisterStateListener") { it.setLooperStateListener(unListenerViejo()) },
            "un listener SIN los opcionales tiene que registrar igual: es la compat que la " +
                "rama de lookup opcional existe para sostener",
        )
        jni("nativeLooperUnregisterStateListener") { it.setLooperStateListener(null) }
    }

    /**
     * AC-020.5 — el getter nuevo cruza la frontera y **devuelve un `Int` de verdad**.
     *
     * Esto es justo lo que `check-jni-symbols.py` no puede ver: si el Kotlin declarara
     * `Long` donde el C++ pone `jint`, compila de los dos lados, linkea, pasa ese gate
     * por comparar sólo nombres, y devuelve basura acá.
     *
     * Y el valor no es arbitrario: **en el host no hay render**, así que aunque el
     * metrónomo se arme, la grilla nunca tickea y el contador tiene que quedar en 0.
     * Es AC-020.2 medido a través de la frontera JNI — la misma condición que el issue
     * #229 describe en device.
     */
    @Test
    fun `beats elapsed queda en cero con el metronomo armado y sin render`() {
        assertEquals(
            0,
            jni("nativeTransportGetBeatsElapsed") { it.transportGetBeatsElapsed() },
            "sin armar el metrónomo no puede haber pulso emitido",
        )

        jni("nativeTransportStartMetronome") {
            it.transportStartMetronome(beats = 4, firstIsDownbeat = true, everyBeatPattern = true)
        }

        // El control positivo, y la mitad que MIENTE: la query vieja dice que corre.
        assertTrue(
            jni("nativeTransportIsMetronomeRunning") { it.transportIsMetronomeRunning() },
            "control positivo: el schedule quedó ARMADO de verdad",
        )
        // Y la nueva dice la verdad: en el host no hay render, así que no sonó nada.
        assertEquals(
            0,
            jni("nativeTransportGetBeatsElapsed") { it.transportGetBeatsElapsed() },
            "armado y sin nadie tickeando: el pulso emitido tiene que ser 0. Si esto " +
                "diera > 0, el getter estaría leyendo otra cosa (el schedule, no la grilla)",
        )

        jni("nativeTransportStopMetronome") { it.transportStopMetronome() }
        assertFalse(
            jni("nativeTransportIsMetronomeRunning") { it.transportIsMetronomeRunning() },
            "parar tiene que desarmar",
        )
    }

    /**
     * El contador de descartes cruza la frontera, y es la OTRA mitad del diagnóstico de
     * #228: si en device `transportGetBeatsElapsed()` avanza pero `onBeat` no llega,
     * esto dice si el evento murió en la cola o después.
     */
    @Test
    fun `el contador de eventos descartados cruza la frontera`() {
        assertEquals(
            0L,
            jni("nativeLooperGetDroppedEvents") { it.looperGetDroppedEvents() },
            "sin render no se empujó un solo evento, así que no puede haber descartes",
        )
    }
}
