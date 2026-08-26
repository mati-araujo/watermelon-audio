package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.tuner.TunerState
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.pow
import kotlin.math.sin
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-015 S2 · AC-015.1 — el puerto offline, por la superficie Kotlin ENTERA.
 *
 * 🔴 **Es el único test que cruza Kotlin → cinterop → C API → núcleo**, y por eso vive acá y
 * no en `commonTest`: allá el puente es un doble y lo que se probaría es el doble. Acá corre
 * en el simulador, contra el `.a` linkeado de verdad.
 *
 * Y prueba **exactamente lo que dice el AC**: no hay `AudioInputFactory`, no hay
 * `ITuner.start()`, no hay permiso de micrófono y no hay stream abierto. Hay un array de
 * floats. Los dos defectos que motivaron REQ-015 se encontraron a mano, con un teléfono y una
 * guitarra; un CI no tiene ninguna de las dos cosas.
 *
 * Los números salen del mismo material sintético que usan los tests de C++ del núcleo: una
 * cuerda de 4 parciales a 44,1 kHz — **nunca 48000**, que es la constante que este motor tuvo
 * cableada y usarla como valor de prueba haría que un defecto de propagación pase inadvertido.
 */
class OfflineTunerTest {

    /**
     * AC-015.1 — se analiza una grabación y sale la lectura, sin micrófono y sin arrancar nada.
     */
    @Test
    fun analysesARecordingWithoutMicrophoneOrStream() {
        val snapshot = OfflineTuner.analyze(
            samples = pluckedString(detunedBy(DETUNE_CENTS)),
            sampleRate = RATE,
            targetHz = TARGET_HZ,
        )

        assertNotNull(snapshot, "el puerto no devolvió nada: sin esto el consumidor no tiene test")
        assertEquals(RATE, snapshot.captureSampleRate, "publicó un rate que no es el del material")

        val cents = assertNotNull(snapshot.cents, "no midió altura: cents llegó en null")
        assertTrue(
            abs(cents - DETUNE_CENTS) <= BUDGET_CENTS,
            "midió $cents cents contra $DETUNE_CENTS reales",
        )
        assertTrue(
            snapshot.state == TunerState.CONVERGED || snapshot.state == TunerState.MEASURING,
            "estado inesperado con señal y objetivo: ${snapshot.state}",
        )
    }

    /**
     * AC-015.2 visto desde arriba: el mismo buffer dos veces da lo mismo, **exacto**.
     *
     * Igualdad de `data class`, no una tolerancia: el determinismo es la razón de ser del
     * puerto y un "casi igual" no sirve para regresión. Que la propiedad ya esté probada en
     * C++ no la hace redundante acá — lo que se afirma es que el CRUCE no le agrega ruido,
     * que es exactamente lo que un envoltorio puede romper.
     */
    @Test
    fun theSameBufferTwiceReadsIdentically() {
        val samples = pluckedString(detunedBy(DETUNE_CENTS))
        val first = OfflineTuner.analyze(samples, RATE, TARGET_HZ)
        val second = OfflineTuner.analyze(samples, RATE, TARGET_HZ)

        assertNotNull(first)
        assertEquals(first, second, "el mismo buffer dio dos lecturas distintas")
    }

    /**
     * AC-015.1 — mono y estéreo del mismo audio se leen igual.
     *
     * El material grabado es mono y el camino de captura habla estéreo intercalado; que los
     * dos den lo mismo es lo que impide que la adaptación se vuelva un segundo motor.
     */
    @Test
    fun monoAndInterleavedStereoReadTheSame() {
        val mono = pluckedString(detunedBy(DETUNE_CENTS))
        val stereo = FloatArray(mono.size * 2) { mono[it / 2] }

        val fromMono = OfflineTuner.analyze(mono, RATE, TARGET_HZ, OfflineTuner.MONO)
        val fromStereo =
            OfflineTuner.analyze(stereo, RATE, TARGET_HZ, OfflineTuner.STEREO_INTERLEAVED)

        assertNotNull(fromMono)
        assertEquals(fromMono, fromStereo, "el mismo audio se leyó distinto según el layout")
    }

    /** Sin objetivo no se inventa uno: `cents` en null, y aún así se detecta la nota. */
    @Test
    fun withoutATargetItReportsNoReadingInsteadOfGuessing() {
        val snapshot = OfflineTuner.analyze(
            samples = pluckedString(TARGET_HZ.toDouble()),
            sampleRate = RATE,
            targetHz = OfflineTuner.NO_TARGET_HZ,
        )

        assertNotNull(snapshot)
        assertNull(snapshot.cents, "publicó cents sin un objetivo contra el cual medir")
        assertNotNull(snapshot.detectedHz, "tampoco detectó la nota: no analizó nada")
    }

    /**
     * Argumentos que no describen audio: `null`, no un snapshot en cero.
     *
     * Un array de ceros **no** es dato ausente, es dato plausible — esta librería ya shippeó
     * dos stubs que devolvían ceros y derrotaron los fallbacks elvis de sus propios callers.
     */
    @Test
    fun refusesArgumentsThatDoNotDescribeAudio() {
        val samples = pluckedString(TARGET_HZ.toDouble())

        assertNull(OfflineTuner.analyze(FloatArray(0), RATE, TARGET_HZ), "aceptó un buffer vacío")
        assertNull(OfflineTuner.analyze(samples, 0, TARGET_HZ), "aceptó un rate de 0")
        assertNull(OfflineTuner.analyze(samples, -1, TARGET_HZ), "aceptó un rate negativo")
        assertNull(
            OfflineTuner.analyze(samples, RATE, TARGET_HZ, channels = 3),
            "aceptó un layout que no existe: leerlo igual mide una señal que nadie mandó",
        )
    }

    private companion object {
        const val RATE = 44_100
        const val TARGET_HZ = 110f          // A2
        const val DETUNE_CENTS = 3f
        const val BUDGET_CENTS = 0.1f
        const val FRAMES = 66_150           // ~1,5 s: de sobra para que la integración converja

        fun detunedBy(cents: Float): Double =
            TARGET_HZ.toDouble() * 2.0.pow(cents.toDouble() / 1200.0)

        /** Cuerda de 4 parciales, mono, con la fase continua por construcción. */
        fun pluckedString(f0: Double): FloatArray = FloatArray(FRAMES) { i ->
            var s = 0.0
            for (n in 1..4) {
                s += (0.5 / n) * sin(2.0 * PI * f0 * n * i / RATE)
            }
            s.toFloat()
        }
    }
}
