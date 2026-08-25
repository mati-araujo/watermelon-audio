package com.watermellonstudios.audio.domain.tuner

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-001 S1 — la traducción del snapshot nativo a algo con lo que se pueda
 * escribir una UI.
 *
 * Acá no hay análisis: eso vive en C++ y lo miden los tests de host. Lo que vive
 * en commonMain, y es donde se pierde o se conserva la verdad, es **el mapeo**:
 * el orden de los valores, el enum de estado, y sobre todo la frontera
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
        detectedHz: Float = 82.41f,
        clarity: Float = 0.97f,
        inharmonicityB: Float = 1.37e-4f,
        inharmonicityMeasured: Float = 1f,
        lockedString: Float = 2f,
        fastModeState: Float = 2f,
        usableRangeCents: Float = 118.9f,
        inputDiscontinuity: Float = 0f,
        discontinuityCount: Float = 4f,
    ) = floatArrayOf(rate, rms, frames, dropped, state, cents, phase, uncertainty,
                     detectedHz, clarity, inharmonicityB, inharmonicityMeasured,
                     lockedString, fastModeState, usableRangeCents, inputDiscontinuity,
                     discontinuityCount)

    @Test
    fun elOrdenDeLosValoresEsElDelContratoNativo() {
        val snap = assertNotNull(TunerSnapshot.fromNative(nativeValues()))

        assertEquals(44100, snap.captureSampleRate)
        assertEquals(0.137f, snap.levelRms)
        assertEquals(96000L, snap.framesAnalyzed)
        assertEquals(3L, snap.droppedFrames)
        assertEquals(TunerState.MEASURING, snap.state)
        assertTrue(!snap.inputDiscontinuity)

        // Los dos que agregó la detección gruesa, AL FINAL del layout.
        assertEquals(82.41f, snap.detectedHz)
        assertEquals(0.97f, snap.detectionClarity)
    }

    /**
     * La detección **sin objetivo** es un dato distinto de la desviación.
     *
     * `detectedHz` dice *qué nota es* —con error de decenas de cents— y existe aunque nadie
     * haya puesto un objetivo; `cents` dice *cuán desafinada está* y sólo existe si hay uno.
     * Confundirlos haría que la app muestre "afinado" sobre una cuerda que ni siquiera es la
     * que el usuario eligió.
     */
    @Test
    fun laDeteccionExisteAunqueNoHayaMedicionDeDesviacion() {
        val snap = assertNotNull(TunerSnapshot.fromNative(nativeValues()))
        assertNull(snap.cents, "sin objetivo no hay desviación")
        assertEquals(82.41f, assertNotNull(snap.detectedHz), "pero SÍ hay nota detectada")
    }

    /** Sin nota, el motor publica 0 Hz y eso llega como `null` — no como "0 hercios". */
    @Test
    fun sinNotaDetectadaLlegaNullYNoCero() {
        val snap = assertNotNull(TunerSnapshot.fromNative(nativeValues(detectedHz = 0f)))
        assertNull(snap.detectedHz, "0 Hz no es una nota: es la ausencia de una")
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
        // Y un array del tamaño VIEJO (8): es exactamente el caso de una app compilada
        // contra la versión anterior del contrato, y tiene que rechazarse entero.
        assertNull(TunerSnapshot.fromNative(FloatArray(8) { 1f }))
        assertNull(TunerSnapshot.fromNative(floatArrayOf()))
    }

    @Test
    fun elConteoDeValoresEspejaLaConstanteNativa() {
        // Si esto cambia sin que cambie WMA_TUNER_SNAPSHOT_VALUES, el consumidor
        // pasa un array de otro tamaño que el que la C API va a llenar. Del lado
        // de C++ lo para un static_assert; de este lado, esta línea.
        assertEquals(17, TunerSnapshot.VALUE_COUNT)
        assertEquals(TunerSnapshot.VALUE_COUNT, nativeValues().size)
    }

    /**
     * REQ-003 · 2.3 — **la promesa del append-only, ejercitada**.
     *
     * El orden crece por el final para que un motor MÁS NUEVO que esta librería
     * siga sirviendo: manda un array más largo y se lee el prefijo conocido.
     * Hasta acá eso era una promesa escrita en un comentario y **nunca probada**.
     *
     * El caso inverso —motor viejo, librería nueva— NO se acepta a propósito, y
     * la asimetría es deliberada: leer un índice que el motor no llenó daría
     * basura con cara de medición, que es justo lo que este REQ existe para
     * sacar del producto.
     */
    /**
     * REQ-014 S3 — el contador acumulado llega entero, y NO se confunde con la
     * bandera viva.
     *
     * Los dos campos describen la misma familia de evento y significan cosas
     * distintas: la bandera es la lectura de AHORA y se baja sola; el contador
     * es la MEMORIA y no baja nunca. Un consumidor que los mezcle o muestra un
     * aviso que no se va, o no se entera nunca de un corte.
     */
    @Test
    fun elContadorAcumuladoYLaBanderaVivaSonCamposDistintos() {
        val snap = assertNotNull(TunerSnapshot.fromNative(
            nativeValues(inputDiscontinuity = 0f, discontinuityCount = 4f)))
        assertEquals(4L, snap.discontinuityCount)
        assertEquals(false, snap.inputDiscontinuity)

        // Y al revés: rota AHORA, y es el primer corte de la sesión.
        val rota = assertNotNull(TunerSnapshot.fromNative(
            nativeValues(inputDiscontinuity = 1f, discontinuityCount = 1f)))
        assertEquals(1L, rota.discontinuityCount)
        assertEquals(true, rota.inputDiscontinuity)
    }

    @Test
    fun unMotorMasNuevoQueEstaLibreriaSigueSiendoLegible() {
        val delFuturo = nativeValues() + floatArrayOf(7f, 8f, 9f)
        val snap = assertNotNull(TunerSnapshot.fromNative(delFuturo))

        assertEquals(44100, snap.captureSampleRate)
        assertEquals(118.9f, snap.usableRangeCents)
    }

    /**
     * REQ-009 · 2.4 — el slot 15 llega como `Boolean`, y en las DOS direcciones.
     *
     * Un test de una sola dirección lo pasa un campo clavado en su default. Por
     * eso van los dos lados con la misma función.
     */
    @Test
    fun laMarcaDeHuecoLlegaEnLasDosDirecciones() {
        val roto = assertNotNull(TunerSnapshot.fromNative(nativeValues(inputDiscontinuity = 1f)))
        val sano = assertNotNull(TunerSnapshot.fromNative(nativeValues(inputDiscontinuity = 0f)))

        assertTrue(roto.inputDiscontinuity, "el motor marcó el hueco y no llegó")
        assertTrue(!sano.inputDiscontinuity, "sin hueco la marca no puede estar prendida")
    }

    /**
     * REQ-009 · 2.4 — y NO se lee del acumulado `droppedFrames`.
     *
     * La tentación es derivarla (`droppedFrames > 0`), y está medido que eso es
     * un defecto: el acumulado es monótono, así que después del primer desborde
     * una app que lo usara diría "revisá el cable" el resto de la sesión. Este
     * test fija el caso que los separa — frames perdidos hace rato, entrada sana
     * ahora — y mataría a quien la derivara.
     */
    @Test
    fun laMarcaNoSeDerivaDelAcumuladoDeFramesPerdidos() {
        val snap = assertNotNull(
            TunerSnapshot.fromNative(nativeValues(dropped = 329728f, inputDiscontinuity = 0f))
        )
        assertEquals(329728L, snap.droppedFrames)
        assertTrue(
            !snap.inputDiscontinuity,
            "se perdieron frames en algún momento de la sesión, pero la lectura VIVA " +
                "está sana: derivar la marca del acumulado la dejaría trabada para siempre",
        )
    }

    /**
     * REQ-003 · 2.2 — sin objetivo el rango llega como `null`, no como 0.
     *
     * Cero es un rango PLAUSIBLE —nulo— y una app lo dibujaría como "nunca
     * confíes", que dice algo distinto de "no hay contra qué medir".
     */
    @Test
    fun sinObjetivoElRangoLlegaComoNullYNoComoCero() {
        val snap = assertNotNull(
            TunerSnapshot.fromNative(nativeValues(usableRangeCents = Float.NaN))
        )
        assertNull(snap.usableRangeCents)
    }

    /**
     * REQ-001 S7. La marca de "medido" va APARTE del valor, y por eso hay dos
     * casos que un centinela dentro del número no podría distinguir: un B de 0
     * medido de verdad (cuerda ideal) y un B ausente.
     */
    @Test
    fun unBSinMedirLlegaComoNullAunqueTraigaUnNumero() {
        val snap = TunerSnapshot.fromNative(
            nativeValues(inharmonicityB = 1.37e-4f, inharmonicityMeasured = 0f)
        )
        assertNotNull(snap)
        assertNull(
            snap.inharmonicityB,
            "el motor mandó un B con la marca en 0: sin medición no puede pasar por medición"
        )
    }

    @Test
    fun unBDeCeroMedidoDeVerdadNoSeConfundeConAusencia() {
        val snap = TunerSnapshot.fromNative(
            nativeValues(inharmonicityB = 0f, inharmonicityMeasured = 1f)
        )
        assertNotNull(snap)
        assertEquals(
            0f, snap.inharmonicityB,
            "una cuerda ideal medida da B=0, y eso ES una medición: no puede llegar como null"
        )
    }

    /**
     * REQ-001 S5. `-1` significa "ninguna cuerda enganchada" y tiene que llegar
     * como `null`, no como el número -1: un índice negativo indexando un array de
     * cuerdas es un crash esperando.
     */
    @Test
    fun sinCuerdaEnganchadaLlegaNullYNoMenosUno() {
        val snap = TunerSnapshot.fromNative(nativeValues(lockedString = -1f))
        assertNotNull(snap)
        assertNull(snap.lockedString)
    }
}
