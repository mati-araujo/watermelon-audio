package com.watermellonstudios.audio.domain.tuner

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull

/**
 * REQ-001 S1 — la traducción del snapshot nativo a algo con lo que se pueda
 * escribir una UI.
 *
 * Acá no hay análisis: eso vive en C++ y lo miden los tests de host. Lo que vive
 * en commonMain, y es donde se pierde o se conserva la verdad, es **el mapeo**:
 * el orden de los ocho valores, el enum de estado, y sobre todo la frontera
 * entre "no hay dato" y "el dato es cero".
 *
 * POR QUE ESTO NO ES CEREMONIA
 * ----------------------------
 * El motor publica NaN en los campos que todavía no tiene estimador para llenar,
 * y lo hace a propósito: `0.0` cents es "afinado exacto", un valor plausible que
 * una UI dibujaría como medición. Si esta capa aplanara NaN a 0f —o a null sin
 * que nada lo verifique— el cuidado de la capa de abajo se perdería justo antes
 * de llegar al usuario. Esta librería ya shippeó dos stubs que devolvían ceros y
 * derrotaron los fallbacks de sus propios callers.
 */
class TunerSnapshotTest {

    /// Ninguno de los valores es 0, 1 ni 48000: todos son distinguibles de un
    /// default, de un índice y de la constante que el motor tenía cableada.
    private fun nativeValues(
        rate: Float = 44100f,
        rms: Float = 0.137f,
        frames: Float = 96000f,
        dropped: Float = 3f,
        state: Float = 2f,
        cents: Float = Float.NaN,
        phase: Float = Float.NaN,
        uncertainty: Float = Float.NaN,
    ) = floatArrayOf(rate, rms, frames, dropped, state, cents, phase, uncertainty)

    @Test
    fun elOrdenDeLosOchoValoresEsElDelContratoNativo() {
        val snap = assertNotNull(TunerSnapshot.fromNative(nativeValues()))

        assertEquals(44100, snap.captureSampleRate)
        assertEquals(0.137f, snap.levelRms)
        assertEquals(96000L, snap.framesAnalyzed)
        assertEquals(3L, snap.droppedFrames)
        assertEquals(TunerState.MEASURING, snap.state)
    }

    /**
     * El corazón del archivo: NaN es ausencia, y llega como `null` — que el
     * compilador obliga a considerar. Un `0f` acá sería una medición inventada.
     */
    @Test
    fun losCamposSinEstimadorLleganComoNullYNoComoCero() {
        val snap = assertNotNull(TunerSnapshot.fromNative(nativeValues()))

        assertNull(snap.cents, "NaN es ausencia de dato; 0f sería 'afinado exacto'")
        assertNull(snap.phaseAngle)
        assertNull(snap.uncertainty)
    }

    /// Y al revés: un cero REAL publicado por un estimador es un dato, y tiene
    /// que sobrevivir. Sin esta mitad, mapear todo a null pasaría el test de
    /// arriba.
    @Test
    fun unCeroMedidoDeVerdadNoSeConfundeConAusencia() {
        val snap = assertNotNull(
            TunerSnapshot.fromNative(nativeValues(cents = 0f, phase = 0f, uncertainty = 0f))
        )

        assertEquals(0f, snap.cents, "un 0 publicado por el estimador ES una medición")
        assertEquals(0f, snap.phaseAngle)
        assertEquals(0f, snap.uncertainty)
    }

    @Test
    fun losCuatroEstadosSeMapeanYUnoDesconocidoNoSeDisfrazaDeSilencio() {
        assertEquals(TunerState.NO_SIGNAL, TunerState.fromNative(0f))
        assertEquals(TunerState.NO_LOCK, TunerState.fromNative(1f))
        assertEquals(TunerState.MEASURING, TunerState.fromNative(2f))
        assertEquals(TunerState.CONVERGED, TunerState.fromNative(3f))

        // Un motor más nuevo que esta librería. Colapsarlo a NO_SIGNAL haría que
        // se vea como un afinador roto sin decir por qué.
        assertEquals(TunerState.UNKNOWN, TunerState.fromNative(7f))
    }

    /**
     * Un array corto significa que el contrato con la capa nativa se rompió
     * —por ejemplo, una app vieja contra un motor nuevo que agregó un campo—.
     * Devolver un snapshot a medias sería peor que no devolver nada: los campos
     * que faltan tomarían el valor de otros.
     */
    @Test
    fun unArrayCortoNoProduceUnSnapshotAMedias() {
        assertNull(TunerSnapshot.fromNative(floatArrayOf(44100f, 0.1f, 10f)))
        assertNull(TunerSnapshot.fromNative(floatArrayOf()))
    }

    @Test
    fun elConteoDeValoresEspejaLaConstanteNativa() {
        // Si esto cambia sin que cambie WMA_TUNER_SNAPSHOT_VALUES, el consumidor
        // pasa un array de otro tamaño que el que la C API va a llenar. Del lado
        // de C++ lo para un static_assert; de este lado, esta línea.
        assertEquals(8, TunerSnapshot.VALUE_COUNT)
        assertEquals(TunerSnapshot.VALUE_COUNT, nativeValues().size)
    }
}
