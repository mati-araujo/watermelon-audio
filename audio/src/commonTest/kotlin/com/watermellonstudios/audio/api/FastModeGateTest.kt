package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.domain.tuning.Tuning
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import com.watermellonstudios.audio.internal.tuner.FakeTunerBridge
import com.watermellonstudios.audio.internal.tuner.TunerImpl
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * REQ-037 S2 — LA PUERTA DEL MODO RÁPIDO, DESDE LA API PÚBLICA.
 *
 * El motor tiene modo rápido desde REQ-001 S5: se le declaran las cuerdas del instrumento y él
 * elige y engancha el objetivo solo, con histéresis. Es el camino donde el consumidor mide —sólo
 * ahí una altura sin soporte cae en `NO_LOCK` y no en `NO_SIGNAL` (R-API-49, R-API-52)—.
 *
 * 🔴 ESTE ARCHIVO ERA UN TRINQUETE Y SE DIO VUELTA, que es exactamente lo que S1 dejó pedido.
 * Afirmaba que el afinador **no** declara candidatos, con el mensaje *"la puerta del modo rápido
 * se abrió… dar vuelta este test"*. Ese día llegó: `ITuner.automaticStringSelection` existe.
 *
 * LA SEMÁNTICA QUE SE FIJA ACÁ, Y POR QUÉ NO ES UN FLAG MÁS
 * ---------------------------------------------------------
 * El automático es la **política de fallback** de [ITuner.selectedString], no un eje paralelo:
 * gobierna qué pasa cuando no hay cuerda elegida. Con cuerda elegida **manda el consumidor**,
 * siempre. Por eso no existe el estado ilegal "automático encendido Y cuerda elegida, ¿quién
 * gana?": la pregunta no se puede formular mal.
 */
class FastModeGateTest {

    private fun guitarra() = TuningConfiguration(tuning = Tuning.GUITAR_STANDARD)

    /** Los 18 floats del snapshot, con lo mínimo para que [TunerSnapshot.fromNative] lo acepte. */
    private fun snapshotConEnganche(lockedString: Int): FloatArray =
        FloatArray(TunerSnapshot.VALUE_COUNT) { Float.NaN }.also {
            it[0] = 48000f                        // captureSampleRate
            it[1] = 0.1f                          // levelRms
            it[2] = 1024f                         // framesAnalyzed
            it[3] = 0f                            // droppedFrames
            it[4] = 3f                            // state
            it[9] = 0.99f                         // detectionClarity
            it[11] = 0f                           // inharmonicity: sin medir
            it[12] = lockedString.toFloat()       // lockedString — 0-BASED, ver el test de abajo
            it[13] = 2f                           // fastModeState: enganchado
            it[15] = 0f                           // inputDiscontinuity
            it[16] = 0f                           // discontinuityCount
        }

    /**
     * AC-037.2 — con el instrumento declarado y **sin** cuerda elegida, el afinador le ofrece las
     * cuerdas al motor para que elija solo. Es la capacidad entera de este REQ.
     *
     * En orden de cuerda, que no es orden de frecuencia: en ukelele high-G y en banjo la más aguda
     * no es la primera (AC-001.15).
     */
    @Test
    fun `con el automatico encendido el afinador le declara las cuerdas al motor`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())

        assertTrue(tuner.start(), "precondicion: el afinador tiene que arrancar")
        assertEquals(6, tuner.targets.size, "precondicion: la guitarra estandar declara seis cuerdas")

        tuner.automaticStringSelection = true

        val declarados = bridge.candidatesPushed.lastOrNull()
        assertNotNull(declarados, "el afinador no le declaró NADA al motor: no hay modo rápido")
        assertEquals(
            tuner.targets.map { it.frequency.hz.toFloat() },
            declarados.toList(),
            "los candidatos no son las cuerdas del instrumento EN ORDEN DE CUERDA",
        )
    }

    /**
     * 🔴 El bug plausible que este test existe para atrapar: que el afinador, además de declarar
     * candidatos, le empuje "sin objetivo" al motor y **borre el enganche que el modo rápido acaba
     * de hacer**.
     *
     * Sin cuerda elegida el objetivo deseado es `0f`, así que un `syncTargetWithEngine` ingenuo
     * empuja `0f` en cuanto el motor engancha algo distinto — y el afinador se apaga solo, en
     * silencio, justo cuando empezó a funcionar. En automático **el objetivo lo manda el motor**.
     */
    @Test
    fun `en automatico el afinador no le empuja objetivo al motor`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())
        tuner.automaticStringSelection = true

        // El motor enganchó la quinta cuerda por su cuenta, como hace el modo rápido.
        bridge.setTunerTargetHz(tuner.targets[4].frequency.hz.toFloat())
        val empujesAntes = bridge.pushedHz.size

        // Cualquier cosa que dispare una re-sincronización mientras el automático manda.
        tuner.configuration = guitarra()

        assertEquals(
            empujesAntes, bridge.pushedHz.size,
            "el afinador le empujó un objetivo al motor estando en automático: le borra el enganche",
        )
    }

    /**
     * AC-037.3 — el gemelo, y sin él lo de arriba se satisface declarando candidatos siempre.
     *
     * Cuando el consumidor elige cuerda, manda su elección **aunque el automático esté encendido**:
     * los candidatos se retiran (lista vacía, ver el KDoc de `ITunerBridge.setTunerCandidates`) y el
     * objetivo vuelve a ser el suyo.
     */
    @Test
    fun `con una cuerda elegida manda el consumidor aunque el automatico este encendido`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())
        tuner.automaticStringSelection = true

        tuner.selectedString = 6      // la prima, 1-based como la numera el músico

        assertEquals(
            emptyList(), bridge.candidatesPushed.last().toList(),
            "el afinador dejó los candidatos puestos: el modo rápido puede reelegir por el consumidor",
        )
        assertEquals(
            tuner.targets[5].frequency.hz.toFloat(), bridge.getTunerTargetHz(),
            "no se empujó la cuerda elegida",
        )
    }

    /** Apagar el automático retira los candidatos: la capacidad se suelta tan explícito como se pide. */
    @Test
    fun `apagar el automatico retira los candidatos`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())
        tuner.automaticStringSelection = true
        assertTrue(bridge.candidatesPushed.last().isNotEmpty(), "precondicion: se declararon candidatos")

        tuner.automaticStringSelection = false

        assertEquals(
            emptyList(), bridge.candidatesPushed.last().toList(),
            "el automático se apagó y el motor sigue con las cuerdas declaradas",
        )
    }

    /**
     * El automático **nace apagado**, y no es una preferencia de estilo: encenderlo por default le
     * cambiaría el comportamiento a cualquier consumidor existente sin que lo pida. Es lo que Tunio
     * pidió explícitamente que NO hiciéramos.
     */
    @Test
    fun `el automatico nace apagado`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())
        tuner.start()

        assertEquals(
            false, tuner.automaticStringSelection,
            "el automático viene encendido de fábrica: le cambia el comportamiento a quien no lo pidió",
        )
        assertTrue(
            bridge.candidatesPushed.all { it.isEmpty() },
            "un afinador recién creado ya le declaró cuerdas al motor sin que nadie se lo pida",
        )
    }

    /**
     * 🔴 REQ-030 POR LA PUERTA DE AL LADO, y `targetAppliedByUser` no lo ve.
     *
     * `FastModeTracker::setCandidates` llama `release()` **incondicionalmente**: re-declarar los
     * mismos Hz **suelta el enganche** y tira la integración del strobe. Un ViewModel que reasigna
     * `configuration` en cada frame dejaría al modo rápido buscando para siempre — y el contador de
     * objetivos aplicados quedaría en cero, porque acá no se empuja ningún objetivo.
     *
     * Medido en el motor, no supuesto: `setCandidates` no compara con lo que ya tenía.
     */
    @Test
    fun `reasignar la misma configuracion no vuelve a declarar los candidatos`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())
        tuner.start()
        tuner.automaticStringSelection = true
        val declaracionesAntes = bridge.candidatesPushed.size

        repeat(5) { tuner.configuration = guitarra() }

        assertEquals(
            declaracionesAntes, bridge.candidatesPushed.size,
            "el afinador re-declaró candidatos idénticos: cada una suelta el enganche del motor",
        )
    }

    /**
     * AC-037.5 — declarar el instrumento **una vez** y que la señal recorra las cuerdas no puede
     * re-aplicar el objetivo. La puerta no reintroduce REQ-030 por la ventana.
     *
     * REQ-030 midió 27 re-targets del usuario contra 26 del modo rápido en la misma corrida, cada
     * uno tirando el ring del strobe. Acá el equivalente observable es `pushedHz`: en automático el
     * afinador **no empuja nada**, por muchas veces que el motor cambie de cuerda y por muchas
     * lecturas que el consumidor pida.
     */
    @Test
    fun `en automatico recorrer las cuerdas no re-aplica el objetivo`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())
        tuner.start()
        tuner.automaticStringSelection = true
        val empujesTrasDeclarar = bridge.pushedHz.size
        val declaracionesTrasDeclarar = bridge.candidatesPushed.size

        // El motor va enganchando una cuerda tras otra, y el consumidor lee en cada tick.
        repeat(3) { vuelta ->
            tuner.targets.indices.forEach { cuerda ->
                bridge.setTunerTargetHz(tuner.targets[cuerda].frequency.hz.toFloat())
                bridge.snapshot = snapshotConEnganche(lockedString = cuerda)
                assertNotNull(tuner.reading(), "vuelta $vuelta cuerda $cuerda: no publicó lectura")
            }
        }

        assertEquals(
            empujesTrasDeclarar + 18, bridge.pushedHz.size,
            "el afinador empujó objetivos por su cuenta: los 18 de este test son del propio test " +
                "simulando al motor, y cualquier extra es la puerta re-aplicando",
        )
        assertEquals(
            declaracionesTrasDeclarar, bridge.candidatesPushed.size,
            "el afinador re-declaró el instrumento mientras la señal recorría las cuerdas",
        )
    }

    /**
     * AC-037.4 — en automático la lectura dice **contra qué objetivo** se publicó.
     *
     * Sin esto el consumidor ve `target = null` mientras el motor mide contra una cuerda, y no
     * puede llevar los cents a Hz absolutos sin adivinarla — el error exacto que REQ-035 midió del
     * lado del barrido.
     *
     * 🔴 Y FIJA LA BASE DEL ÍNDICE, QUE NO ES LA MISMA DE [ITuner.selectedString]. `lockedString`
     * es **0-based**: el motor lo usa para indexar el array de candidatos que le pasamos
     * (`FastModeTracker::lockTo` valida `0 <= index < count`, y `mCandidates[mLocked]`).
     * `selectedString`, en cambio, es **1-based** — "numerada desde 1, como la numera el músico".
     * Confundirlas devuelve la cuerda de al lado con cara de lectura válida, que es el bug que
     * AC-001.15 existe para evitar.
     */
    @Test
    fun `en automatico la lectura dice contra que cuerda se midio y lockedString es cero based`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())
        tuner.automaticStringSelection = true

        // El motor enganchó el candidato de índice 4 = la QUINTA cuerda (targets[4]).
        bridge.snapshot = snapshotConEnganche(lockedString = 4)

        val lectura = tuner.reading()
        assertNotNull(lectura, "precondicion: hay snapshot, tiene que haber lectura")
        assertEquals(
            tuner.targets[4], lectura.target,
            "la lectura no dice contra qué cuerda se midió, o trató `lockedString` como 1-based",
        )
    }

    /** Sin enganche del motor y sin cuerda elegida no hay objetivo, y `null` no se fabrica. */
    @Test
    fun `sin enganche y sin cuerda elegida la lectura no inventa un objetivo`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())
        tuner.automaticStringSelection = true
        bridge.snapshot = snapshotConEnganche(lockedString = -1)

        val lectura = tuner.reading()
        assertNotNull(lectura, "precondicion: hay snapshot, tiene que haber lectura")
        assertEquals(null, lectura.target, "se fabricó un objetivo que el motor no enganchó")
    }
}
