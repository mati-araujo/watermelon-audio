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
 * - **Export/import** hace IO, y **USB** es device-only. La excepción es UN import
 *   (MINI-030): es el único camino en host que deja una pista ACTIVA, y sin él el
 *   contrato "0 elementos = sin dato" no tendría gemelo. `LooperIoJniTest` lo cubre
 *   como IO; acá se usa como fixture.
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
         * La pista que se llena por import para el caso "con contenido" (MINI-030). Es
         * OTRA que [TRACK] porque el resto de la clase afirma que [TRACK] no tiene audio,
         * y JUnit no promete el orden de los métodos.
         */
        private const val TRACK_CON_AUDIO = 1

        /**
         * Más bins que `AudioLooper::MAX_WAVEFORM_BINS_CACHE` (512): el motor escribe 512,
         * rellena el resto con 0 y devuelve 512. No es potencia de dos.
         */
        private const val BINS_SOBRE_EL_TECHO = 600

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
            // MINI-030: el caso "con contenido" llena la pista por import, sin render.
            "nativeLooperImportTrack",
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
     * AC-M030.1 / AC-M030.3 (a) — **una pista sin contenido devuelve CERO elementos**, no
     * `numBins` ceros.
     *
     * Hasta MINI-030 este test afirmaba `size == 64` con todos en 0 sobre esta misma
     * pista, y admitía en su propio comentario que no podía distinguir "escrito en 0" de
     * "sin tocar": Kotlin crea el array en cero y el bridge descartaba el retorno de C. Un
     * array de 64 ceros sobre una pista inactiva no es silencio, es **la ausencia
     * disfrazada de silencio** — del lado del consumidor "sin señal" y "en silencio" eran
     * indistinguibles (carta de NoisyPad WV-3 §1, 2026-09-16).
     *
     * La pista está preparada y nunca grabada, o sea NO activa (`mActive` sólo se enciende
     * en `finalizeRecording()`, import o restore); `getTrackWaveform` sale con 0 bins
     * escritos y el bridge tiene que traducir ese 0 a `FloatArray(0)`. El `isTrackActive`
     * de arriba es el control: si diera `true`, el tamaño 0 de abajo hablaría de otra cosa.
     *
     * Mutante que mata: descartar el retorno de `nativeLooperGetTrackWaveform` (el código
     * anterior a MINI-030) ⇒ `size == 64` ⇒ rojo en el primer `assertEquals`.
     */
    @Test
    fun `una pista sin contenido devuelve cero elementos, no ceros`() {
        assertFalse(
            jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(TRACK) },
            "la pista tiene que estar INACTIVA para que este test hable de 'sin dato'",
        )

        val forma = jni("nativeLooperGetTrackWaveform") { it.looperGetTrackWaveform(TRACK, 64) }

        assertEquals(
            0,
            forma.size,
            "una pista inactiva no tiene forma de onda: el motor escribió 0 bins y el bridge " +
                "tiene que devolver un array VACÍO, no ${forma.size} ceros que se leen como silencio",
        )
    }

    /**
     * AC-M030.1 / AC-M030.3 (b) — **con contenido, el array tiene `numBins` elementos y
     * trae señal**; y si el motor escribe menos bins que los pedidos, el resto es relleno
     * de silencio y el largo sigue siendo `numBins`.
     *
     * El gemelo del de arriba: sin él, "devuelve tamaño 0" no se distingue de un bridge
     * que devuelve `FloatArray(0)` siempre. La pista se llena por `looperImportTrack` de
     * un WAV con una ráfaga de onda cuadrada —sincrónico, sin render, como lo hace
     * `LooperIoJniTest`— y eso la deja ACTIVA. Va en OTRA pista que la del `@Before`,
     * porque el resto de esta clase afirma que `TRACK` no tiene contenido y JUnit no
     * promete orden.
     *
     * Dos pedidos, y los dos importan:
     *  - `64` bins: el motor escribe los 64 ⇒ largo 64 y un pico `> 0` en la región con
     *    señal (con `AMP_L = 0,625`, el pico es exactamente ese valor: es una onda cuadrada).
     *    El pico es lo que distingue "cruzó el dato" de "cruzó el largo".
     *  - `600` bins: el motor tiene un techo de `MAX_WAVEFORM_BINS_CACHE = 512`, escribe
     *    512, rellena 88 con 0 y devuelve **512** ⇒ el bridge tiene que devolver los
     *    **600** igual (el relleno parcial es silencio y no cambia el largo: los
     *    llamadores de UI no reescalan). Un bridge que hiciera `copyOf(escritos)` pasa
     *    el primer pedido y muere acá.
     */
    @Test
    fun `una pista con contenido devuelve numBins elementos con senal, aun si el motor escribe menos`() {
        val fuente = java.nio.file.Files.createTempFile("mini030-waveform", ".wav").toFile()
        try {
            val ruta = MinimalWav.writeTo(
                fuente,
                frames = LEN_FRAMES,
                regions = listOf(MinimalWav.Region(6_000, 42_000)),
            )
            assertTrue(
                jni("nativeLooperImportTrack") {
                    it.looperImportTrack(TRACK_CON_AUDIO, ruta, MinimalWav.RATE)
                },
                "el fixture no importó: revisá MinimalWav contra wav::readWav",
            )
            assertTrue(
                jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(TRACK_CON_AUDIO) },
                "importó y la pista no quedó activa: lo de abajo no hablaría de 'con dato'",
            )

            val forma = jni("nativeLooperGetTrackWaveform") {
                it.looperGetTrackWaveform(TRACK_CON_AUDIO, 64)
            }
            assertEquals(64, forma.size, "con contenido el array tiene que medir lo pedido")
            assertTrue(forma.all { it.isFinite() }, "la forma trajo NaN o infinito: basura de pinneo")
            val pico = forma.max()
            assertTrue(
                pico > 0.0f,
                "la pista tiene una ráfaga de ±${MinimalWav.AMP_L} y la forma no trae ningún pico: " +
                    "cruzó el largo pero no el dato",
            )
            assertEquals(
                MinimalWav.AMP_L,
                pico,
                "el pico de una onda cuadrada de ±${MinimalWav.AMP_L} tiene que ser exactamente eso",
            )
            assertEquals(
                0.0f,
                forma.first(),
                "los primeros 6000 frames son silencio exacto: el bin 0 tiene que ser 0",
            )

            val sobreElTecho = jni("nativeLooperGetTrackWaveform") {
                it.looperGetTrackWaveform(TRACK_CON_AUDIO, BINS_SOBRE_EL_TECHO)
            }
            assertEquals(
                BINS_SOBRE_EL_TECHO,
                sobreElTecho.size,
                "el motor escribe como mucho 512 bins y rellena el resto con 0: el bridge tiene " +
                    "que devolver los $BINS_SOBRE_EL_TECHO pedidos, no los que el motor escribió",
            )
            assertTrue(sobreElTecho.take(512).any { it > 0.0f }, "los 512 escritos tienen que traer señal")
            assertTrue(
                sobreElTecho.drop(512).all { it == 0.0f },
                "más allá del techo del motor el relleno es silencio exacto",
            )
        } finally {
            jni("nativeLooperClearTrack") { it.looperClearTrack(TRACK_CON_AUDIO) }
            fuente.delete()
        }
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
