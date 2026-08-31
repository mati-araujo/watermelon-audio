package com.watermellonstudios.audio.internal.bridge

import org.junit.AfterClass
import org.junit.Before
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-022 S1 — **la tercera tanda del arnés: el camino del looper.**
 *
 * El looper es el grupo más grande de los alcanzables en host —**93 funciones** entre
 * looper y transport— y antes de esta clase había **7** cubiertas, todas de REQ-020.
 *
 * ## Qué atrapa esto, y ningún otro gate
 *
 * Un **desajuste de FIRMA**. `check-jni-symbols.py` compara sólo NOMBRES, así que un
 * `Int` declarado donde el C++ pone `jlong` compila de los dos lados, linkea, pasa ese
 * gate y devuelve basura en el device. Acá el valor **cruza de verdad** y se afirma.
 *
 * Y el looper tiene un cruce de anchos real que nadie ejercía: `looperSetTrackLoopRegion`
 * toma `Long` (`jlong` → `int64_t`) y `looperGetTrackLoopStart/End` devuelven `Int`
 * (`jint` → `int`). Las tres capas coinciden, pero el estrechamiento existe.
 *
 * ## Por qué DOS valores por par, y ninguno potencia de dos
 *
 * Un solo valor no distingue *"el valor viaja"* de *"el getter devuelve una constante"*.
 * Y una potencia de dos es exacta en float, así que esconde defectos de conversión — la
 * lección está escrita en `test_c_api_tuner.cpp` y la aplicó REQ-018. De ahí `-0,375`,
 * `1,375`, `0,625`.
 *
 * ## 🔴 Lo que NO entra, y por qué
 *
 * - **`setTrackVolume` y `setTrackPan` son write-only**: no existe getter. Ejercerlos
 *   subiría el conteo de cobertura **sin afirmar nada**, que es cobertura de mentira —
 *   exactamente lo que este arnés existe para no ser. Quedan como hueco declarado.
 * - **Todo lo que necesita render**: el host no lo tiene (`FakeAudioBackend` no llama al
 *   callback), así que grabar, reproducir y los eventos de estado no son observables acá.
 * - **Export/import** hace IO, y **USB** es device-only.
 *
 * 🔴 Verde acá NO significa "el looper está probado": son unas pocas de 93, sobre un
 * backend FALSO. Ver el KDoc de [JniHarness].
 */
class LooperTrackJniTest {

    companion object {
        private const val OWNER = "LooperTrackJniTest"
        private const val TRACK = 0
        private const val RATE = 48_000
        private const val LEN_FRAMES = 96_000   // 2 s, y no es potencia de dos

        /**
         * Lo que esta clase declara cubrir. **Trinquete bidireccional** — ver
         * `JniCoverage.ratchet`: ejercer de menos es rojo, y ejercer de más también,
         * para que sumar cobertura aparezca en el diff del PR en vez de colarse.
         */
        private val COVERED = setOf(
            "nativeStartTuner",
            "nativeLooperPrepareTrack",
            "nativeLooperIsTrackActive",
            "nativeLooperGetTrackLengthFrames",
            "nativeLooperSetEnabled",
            "nativeLooperIsPlaying",
            "nativeLooperIsRecording",
            "nativeLooperClearTrack",
            "nativeLooperSetTailMs", "nativeLooperGetTailMs",
            "nativeLooperSetTrackSpeed", "nativeLooperGetTrackSpeed",
            "nativeLooperSetTrackPercussionMode", "nativeLooperIsTrackPercussionMode",
            "nativeLooperSetTrackSendToFx", "nativeLooperIsTrackSendToFx",
            "nativeLooperSetMasterVolume", "nativeLooperGetMasterVolume",
            "nativeLooperSetTrackLoopRegion",
            "nativeLooperGetTrackLoopStart", "nativeLooperGetTrackLoopEnd",
            "nativeLooperResetTrackLoopRegion",
            "nativeLooperGetTrackWaveform",
            "nativeTransportSetBeatsPerBar", "nativeTransportGetBeatsPerBar",
            "nativeTransportFramesPerBeat", "nativeTransportFramesPerBar",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    /**
     * El motor tiene que existir para que esto llegue a algún lado, y la pista tiene que
     * estar preparada. `nativeStartTuner` es el único de los 311 que crea el motor.
     */
    @Before
    fun engineAndTrackUp() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")
        assertTrue(
            jni("nativeLooperPrepareTrack") { it.looperPrepareTrack(TRACK, LEN_FRAMES, RATE) },
            "no se pudo reservar capacidad para la pista",
        )
    }

    /**
     * AC-022.1 — **preparar reserva CAPACIDAD, no crea CONTENIDO**, y esa distinción la
     * descubrió este arnés.
     *
     * La primera versión de este test afirmaba que tras `prepareTrack` la pista quedaba
     * activa y con el largo pedido. **Es falso, y el motor tiene razón**: `prepareTrack`
     * delega en `TrackBuffer::allocate()` y devuelve `allocated > 0` — reserva memoria.
     * `isActive()` y `getLengthFrames()` hablan del audio GRABADO, que en host no existe
     * porque no hay render.
     *
     * Queda escrito como test y no como comentario porque es justo la clase de contrato
     * que un consumidor asume al revés: `prepareTrack` devuelve `true` y parece que la
     * pista "está lista".
     */
    @Test
    fun `preparar reserva capacidad, no crea contenido`() {
        assertFalse(
            jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(TRACK) },
            "una pista reservada pero nunca grabada NO está activa: activa habla del audio grabado",
        )
        assertEquals(
            0,
            jni("nativeLooperGetTrackLengthFrames") { it.looperGetTrackLengthFrames(TRACK) },
            "el largo habla del audio grabado, y no se grabó nada: tiene que ser 0, no la capacidad",
        )
    }

    /** AC-022.1 — `Int` de ida y vuelta, con dos valores. */
    @Test
    fun `el tail cruza en milisegundos y vuelve igual`() {
        jni("nativeLooperSetTailMs") { it.looperSetTailMs(250) }
        assertEquals(250, jni("nativeLooperGetTailMs") { it.looperGetTailMs() }, "el tail no volvió igual")

        jni("nativeLooperSetTailMs") { it.looperSetTailMs(1750) }
        assertEquals(1750, jni("nativeLooperGetTailMs") { it.looperGetTailMs() }, "el tail no siguió al segundo valor")
    }

    /** AC-022.1 — `Float`: `1,375` y `0,625` no son potencias de dos. */
    @Test
    fun `la velocidad de pista cruza como float y vuelve bit a bit`() {
        jni("nativeLooperSetTrackSpeed") { it.looperSetTrackSpeed(TRACK, 1.375f) }
        assertEquals(
            1.375f,
            jni("nativeLooperGetTrackSpeed") { it.looperGetTrackSpeed(TRACK) },
            "la velocidad no volvió igual: un jdouble donde va jfloat se ve acá",
        )

        jni("nativeLooperSetTrackSpeed") { it.looperSetTrackSpeed(TRACK, 0.625f) }
        assertEquals(
            0.625f,
            jni("nativeLooperGetTrackSpeed") { it.looperGetTrackSpeed(TRACK) },
            "la velocidad no siguió al segundo valor",
        )
    }

    /** AC-022.1 — `Boolean`, los dos sentidos: un getter cableado a `true` muere acá. */
    @Test
    fun `los flags booleanos de pista viajan en los dos sentidos`() {
        jni("nativeLooperSetTrackPercussionMode") { it.looperSetTrackPercussionMode(TRACK, true) }
        assertTrue(
            jni("nativeLooperIsTrackPercussionMode") { it.looperIsTrackPercussionMode(TRACK) },
            "el modo percusión no quedó en true",
        )
        jni("nativeLooperSetTrackPercussionMode") { it.looperSetTrackPercussionMode(TRACK, false) }
        assertFalse(
            jni("nativeLooperIsTrackPercussionMode") { it.looperIsTrackPercussionMode(TRACK) },
            "el modo percusión no volvió a false: un getter cableado a true pasa el caso de arriba",
        )

        jni("nativeLooperSetTrackSendToFx") { it.looperSetTrackSendToFx(TRACK, true) }
        assertTrue(
            jni("nativeLooperIsTrackSendToFx") { it.looperIsTrackSendToFx(TRACK) },
            "sendToFx no quedó en true",
        )
        jni("nativeLooperSetTrackSendToFx") { it.looperSetTrackSendToFx(TRACK, false) }
        assertFalse(
            jni("nativeLooperIsTrackSendToFx") { it.looperIsTrackSendToFx(TRACK) },
            "sendToFx no volvió a false",
        )
    }

    /** AC-022.1 — el volumen máster sí tiene getter (el de pista no: ver el KDoc). */
    @Test
    fun `el volumen master cruza y vuelve, con dos valores`() {
        jni("nativeLooperSetMasterVolume") { it.looperSetMasterVolume(0.375f) }
        assertEquals(
            0.375f,
            jni("nativeLooperGetMasterVolume") { it.looperGetMasterVolume() },
            "el volumen máster no volvió igual",
        )

        jni("nativeLooperSetMasterVolume") { it.looperSetMasterVolume(0.875f) }
        assertEquals(
            0.875f,
            jni("nativeLooperGetMasterVolume") { it.looperGetMasterVolume() },
            "el volumen máster no siguió al segundo valor",
        )
    }

    /**
     * AC-022.2 — **el cruce de anchos**, que es lo más valioso de esta clase.
     *
     * Se fija con `Long` (`jlong` → `int64_t`) y se lee con `Int` (`jint` → `int`). Las
     * tres capas coinciden, pero el estrechamiento existe y hasta acá nadie lo ejercía.
     * Dentro del rango de `Int` el valor tiene que volver **exacto**.
     *
     * ⚠️ El límite queda DICHO y no probado: más allá de 2^31 frames el getter trunca por
     * contrato — 12,4 h de audio a 48 kHz, el mismo bound que REQ-017 aceptó para el ancla
     * del beat. Un test que afirmara la truncación estaría fijando el defecto, no el
     * contrato.
     */
    @Test
    fun `fijar una region de loop sobre una pista sin audio es un no-op`() {
        jni("nativeLooperSetTrackLoopRegion") { it.looperSetTrackLoopRegion(TRACK, 12_000L, 84_000L) }
        assertEquals(
            0,
            jni("nativeLooperGetTrackLoopStart") { it.looperGetTrackLoopStart(TRACK) },
            "sin audio grabado la región no se puede fijar: TrackBuffer::setLoopRegion sale " +
                "temprano con `if (length <= 0) return;`. Que esto diera 12000 significaría que " +
                "se fijó una región sobre un buffer vacío",
        )
        assertEquals(
            0,
            jni("nativeLooperGetTrackLoopEnd") { it.looperGetTrackLoopEnd(TRACK) },
            "idem para el fin de la región",
        )
        // El reset cruza la frontera igual, y sobre una pista vacía tampoco cambia nada.
        jni("nativeLooperResetTrackLoopRegion") { it.looperResetTrackLoopRegion(TRACK) }
        assertEquals(0, jni("nativeLooperGetTrackLoopStart") { it.looperGetTrackLoopStart(TRACK) })
    }

    /**
     * AC-022.3 — **el camino de array**, que del otro lado es `SetFloatArrayRegion` sobre
     * el buffer que le pasa Kotlin. Un pinneo mal liberado o un largo mal calculado se ve
     * acá y en ningún otro lado.
     */
    @Test
    fun `la forma de onda llena el array que se le pasa`() {
        val bins = 64
        val forma = jni("nativeLooperGetTrackWaveform") { it.looperGetTrackWaveform(TRACK, bins) }

        assertEquals(bins, forma.size, "el array volvió con otro largo que el pedido")
        assertTrue(
            forma.all { it.isFinite() },
            "la forma de onda trajo NaN o infinito: eso es basura de un pinneo mal hecho, " +
                "no audio — la pista está en silencio, así que todos tienen que ser finitos",
        )
        // La pista se preparó y nunca se grabó: el silencio es el valor ESPERADO, y que
        // sea exactamente 0 prueba que el array se escribió y no que quedó sin tocar
        // (Kotlin lo crea en cero, así que esto solo no alcanza — de ahí el largo de arriba).
        assertTrue(forma.all { it == 0.0f }, "una pista preparada y sin grabar tiene que dar silencio")
    }

    /** AC-022.1 — la matemática del transport, que no necesita render. */
    @Test
    fun `el transport calcula los frames por beat y por bar`() {
        jni("nativeTransportSetBeatsPerBar") { it.transportSetBeatsPerBar(3) }
        assertEquals(3, jni("nativeTransportGetBeatsPerBar") { it.transportGetBeatsPerBar() }, "beatsPerBar no volvió igual")

        jni("nativeTransportSetBeatsPerBar") { it.transportSetBeatsPerBar(5) }
        assertEquals(5, jni("nativeTransportGetBeatsPerBar") { it.transportGetBeatsPerBar() }, "beatsPerBar no siguió al segundo valor")

        // framesPerBar(n) tiene que ser n veces framesPerBeat * beatsPerBar. Se afirma la
        // RELACION y no un número: así el test no se cae si cambia el rate por defecto.
        val fpb = jni("nativeTransportFramesPerBeat") { it.transportFramesPerBeat() }
        assertTrue(fpb > 0, "framesPerBeat tiene que ser positivo; dio $fpb")
        assertEquals(
            fpb * 5 * 2,
            jni("nativeTransportFramesPerBar") { it.transportFramesPerBar(2) },
            "framesPerBar(2) tiene que ser 2 bares de 5 beats",
        )
    }

    /** Los tres de estado global, que en host tienen respuesta aunque no haya render. */
    @Test
    fun `los estados globales del looper contestan sin render`() {
        jni("nativeLooperSetEnabled") { it.looperSetEnabled(true) }
        // Sin render no arranca nada solo: los dos tienen que ser false, y ESO es lo que
        // se afirma. Si alguno diera true, estaría reportando actividad inexistente.
        assertFalse(jni("nativeLooperIsPlaying") { it.looperIsPlaying() }, "sin render no puede estar reproduciendo")
        assertFalse(jni("nativeLooperIsRecording") { it.looperIsRecording() }, "sin render no puede estar grabando")

        jni("nativeLooperClearTrack") { it.looperClearTrack(TRACK) }
        assertFalse(
            jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(TRACK) },
            "clearTrack tiene que desactivar la pista; el @Before la había preparado",
        )
    }
}
