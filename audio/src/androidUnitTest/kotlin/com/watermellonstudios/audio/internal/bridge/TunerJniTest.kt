package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.domain.tuner.TunerState
import org.junit.AfterClass
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.sin
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-016 S1 — **el cable entero, con una función.**
 *
 * Ejerce `nativeAnalyzeTunerBuffer` entrando por `AudioNativeBridge`: el mismo
 * camino que recorre Tunio, que hasta acá no tenía un solo test que lo
 * ejecutara. Se eligió esta función y no otra porque es la única del afinador
 * que **no necesita motor arrancado** (el puerto offline de REQ-015), así que si
 * el cable no cierra se descubre con un test y no con trece.
 *
 * 🔴 Verde acá NO significa "el JNI está probado": son 13 funciones de 310, y
 * corren sobre un backend falso. Ver el KDoc de [JniHarness].
 *
 * **El control positivo de esta etapa no está en este archivo**: es renombrar el
 * símbolo `Java_..._nativeAnalyzeTunerBuffer` del lado C++ y ver que este test se
 * ponga rojo con `UnsatisfiedLinkError`. Sin esa mutación, verde no distingue
 * "cruzó la frontera" de "no ejecutó nada" — está medida en las notas de la etapa.
 */
class TunerJniTest {

    companion object {
        private const val RATE = 48_000
        private const val TARGET_HZ = 440.0f
        private const val AMPLITUDE = 0.5f

        /** RMS de un seno de amplitud A es A/√2 = 0,35355 para A = 0,5. */
        private const val EXPECTED_RMS = 0.35355f

        private const val OWNER = "TunerJniTest"

        /**
         * Lo que esta clase declara cubrir. **Trinquete bidireccional**: si deja de
         * ejercer una, rojo; si ejerce una que no está acá, también. Su diff es la
         * revisión — ver `JniCoverage.ratchet`.
         */
        private val COVERED = setOf("nativeAnalyzeTunerBuffer")

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        /** Un segundo de La 440 en mono, amplitud 0,5 — el estímulo de H2. */
        private fun sine440(): FloatArray =
            FloatArray(RATE) { i -> AMPLITUDE * sin(2.0 * PI * TARGET_HZ * i / RATE).toFloat() }
    }

    private fun analyze(samples: FloatArray, channels: Int): FloatArray? =
        JniHarness.exercise(OWNER, "nativeAnalyzeTunerBuffer") { bridge ->
            bridge.analyzeTunerBuffer(samples, channels, RATE, TARGET_HZ)
        }

    @Test
    fun `un seno de 440 cruza la frontera y vuelve como un snapshot medido`() {
        val values = assertNotNull(
            analyze(sine440(), channels = 1),
            "nativeAnalyzeTunerBuffer devolvió null con un estímulo válido",
        )

        // Que el array tenga el largo del contrato ya es una afirmación sobre el
        // JNI: lo arma NewFloatArray + SetFloatArrayRegion del otro lado.
        assertEquals(TunerSnapshot.VALUE_COUNT, values.size, "largo del snapshot nativo")

        val snapshot = assertNotNull(TunerSnapshot.fromNative(values), "fromNative rechazó el array")

        // Los valores son los MEDIDOS en la sonda de H2 con un JNIEnv real, no
        // aserciones de "no explotó": si el array volviera en cero —o con basura
        // de un pinneo mal liberado— ninguno de estos tres pasa.
        assertEquals(RATE, snapshot.captureSampleRate, "el rate que se le pasó tiene que volver")
        assertTrue(
            abs(snapshot.levelRms - EXPECTED_RMS) < 0.005f,
            "RMS esperado ~$EXPECTED_RMS, medido ${snapshot.levelRms}",
        )
        assertEquals(TunerState.CONVERGED, snapshot.state, "un seno limpio de 1 s tiene que converger")

        val cents = assertNotNull(snapshot.cents, "con objetivo y señal limpia tiene que haber cents")
        assertTrue(abs(cents) < 10f, "440 contra objetivo 440 debería dar ~0 cents, dio $cents")
        assertEquals(0L, snapshot.droppedFrames, "el puerto offline no puede perder frames")
    }

    /**
     * El rechazo, anclado.
     *
     * Un `assertNull` suelto es verde por construcción cuando el estímulo no
     * alcanza para producir resultado — ya pasó en este repo: siete `EXPECT_FALSE`
     * verdes porque el material era demasiado corto, y el rechazo nunca se probó.
     * Por eso este test analiza **el mismo material** primero y exige que ESE dé
     * un resultado, antes de exigir que `channels = 3` no lo dé.
     */
    @Test
    fun `channels invalido se rechaza, y el control positivo es el mismo material`() {
        val samples = sine440()

        assertNotNull(
            analyze(samples, channels = 1),
            "control positivo: con channels=1 este mismo material TIENE que dar snapshot",
        )
        assertNull(
            analyze(samples, channels = 3),
            "channels=3 no es mono ni estéreo: la función tiene que devolver null",
        )
    }
}
