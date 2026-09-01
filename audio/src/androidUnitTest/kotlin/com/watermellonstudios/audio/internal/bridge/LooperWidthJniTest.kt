package com.watermellonstudios.audio.internal.bridge

import org.junit.AfterClass
import org.junit.Before
import java.io.File
import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

/**
 * REQ-026 S2 — **los cruces de 64 bits del looper, con el VALOR afirmado.**
 *
 * ## Qué compra esto, y por qué "cruzó" no alcanza
 *
 * `check-jni-signatures.py` (REQ-025) ya garantiza que donde Kotlin dice `Long` el C++
 * pone `jlong`. Lo que **ninguna firma** puede decir es si el valor llega entero: un
 * intermediario que estreche a 32 bits —un `static_cast<int>` de más, una cuenta hecha en
 * `int`— tiene la firma perfecta y devuelve basura. Por eso acá **ningún assert se
 * conforma con "no es cero"**:
 *
 * - `armInFrames` recibe un offset **mayor que 2³²** y su retorno se compara contra
 *   `transportGetPlayFrame() + offset`. Los 32 bits altos tienen que sobrevivir el viaje
 *   de ida y el de vuelta.
 * - `findContentBounds` devuelve **dos `int` empaquetados en un `jlong`**: el primer frame
 *   audible en los 32 **altos** y el último exclusivo en los bajos. Un `jlong` que se
 *   trunque deja el primer frame en cero, que es justo lo que un fixture con silencio de
 *   cabeza distingue.
 * - `setCapabilities` no tiene getter, así que su `jlong` se afirma **por
 *   comportamiento**: un presupuesto mayor que 2³² tiene que permitir una importación que
 *   con uno chico falla.
 *
 * ## Por qué es alcanzable sin render
 *
 * Igual que [LooperIoJniTest]: `importTrack` llena la pista sincrónicamente en el thread
 * de control. Eso alcanza para los tres `arm*` que necesitan capacidad, y —con
 * `resumeTrack`— para el que necesita una pista de **referencia reproduciendo**, que es
 * un estado del thread de control, no del callback.
 *
 * 🔴 El transporte **no avanza** en host: `transportGetPlayFrame()` se queda donde está
 * porque lo mueve el callback. Eso no debilita nada de lo de arriba —el ancla se lee y se
 * suma, no se supone— pero sí quiere decir que el valor esperado de `armAtNextBar` se
 * **deriva** de `transportFramesPerBar`, en vez de escribirse a mano.
 */
class LooperWidthJniTest {

    companion object {
        private const val OWNER = "LooperWidthJniTest"

        /** La pista que se arma. */
        private const val TRACK = 0

        /** La pista de **referencia**: activa y reproduciendo, para el armado sincronizado. */
        private const val REF = 1

        private const val FRAMES = 33_000
        private val CONTINUO = listOf(MinimalWav.Region(5_000, 27_000))

        /**
         * El offset del armado: **mayor que 2³²** a propósito, y no es potencia de dos.
         *
         * `Int.MAX_VALUE` es 2 147 483 647; esto lo pasa por más del doble, así que un
         * estrechamiento a `jint` no puede acertar por casualidad — ni siquiera de signo.
         */
        private const val OFFSET_ANCHO = 5_000_000_003L

        /** Latencia del armado sincronizado. Entra en un `int` (la C API lo documenta) y no es redonda. */
        private const val LATENCIA = 7_331L

        /** Cuanto de la referencia se espera antes de arrancar. Divide mal a propósito: no es un submúltiplo. */
        private const val CUANTO = 12_000

        /**
         * Presupuesto de memoria **mayor que 2³²**, y uno que no alcanza.
         *
         * 🔴 `PRESUPUESTO_ANCHO` no es un número grande cualquiera: es `2³² +
         * PRESUPUESTO_CHICO`, o sea que sus **32 bits bajos son justamente el presupuesto
         * que NO alcanza**. Un valor grande al azar no sirve para esto — con
         * `6_000_000_003`, por ejemplo, el truncado a 32 bits da 1 705 032 707, que
         * todavía sobra para el fixture y el mutante sobreviviría. Elegido así, un
         * estrechamiento hace fallar la importación y el test lo ve.
         */
        private const val PRESUPUESTO_CHICO = 99_991L
        private const val PRESUPUESTO_ANCHO = 4_294_967_296L + PRESUPUESTO_CHICO

        /**
         * Lo que esta clase declara cubrir. **Trinquete bidireccional** — ver
         * `JniCoverage.ratchet`: ejercer de menos es rojo, y ejercer de más también.
         */
        private val COVERED = setOf(
            "nativeStartTuner",
            "nativeLooperClearTrack",
            "nativeLooperPrepareTrack",
            "nativeLooperImportTrack",
            "nativeLooperIsTrackActive",
            "nativeLooperGetTrackLengthFrames",
            "nativeLooperSetCapabilities",
            "nativeLooperResumeTrack",
            "nativeLooperPauseTrack",
            "nativeLooperIsTrackPlaying",
            "nativeTransportGetPlayFrame",
            "nativeTransportFramesPerBar",
            "nativeLooperArmInFrames",
            "nativeLooperArmAtNextBar",
            "nativeLooperArmSyncedToLoop",
            "nativeLooperArmSyncedToLoopQuantized",
            "nativeLooperCancelArm",
            "nativeLooperFindContentBounds",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private lateinit var dir: File

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    /**
     * Motor arriba, las dos pistas vacías, el armado cancelado y el presupuesto **restaurado**.
     *
     * El presupuesto se restaura acá y no al final del test que lo baja porque el motor es
     * un singleton de proceso: si ese test se cae en el medio, dejaría a los demás
     * importando contra 99 991 bytes y el veredicto dependería del orden de JUnit.
     */
    @Before
    fun engineUpAndTracksEmpty() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")
        jni("nativeLooperSetCapabilities") {
            it.looperSetCapabilities(PRESUPUESTO_ANCHO, maxTracks = 0, maxFreeSeconds = 0)
        }
        jni("nativeLooperCancelArm") { it.looperCancelArm() }
        listOf(TRACK, REF).forEach { t -> jni("nativeLooperClearTrack") { it.looperClearTrack(t) } }
        assertFalse(
            jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(REF) },
            "quedó contenido de otro test en la pista de referencia",
        )
        dir = Files.createTempDirectory("req026-width").toFile()
    }

    /**
     * AC-026.7 — **el offset de 64 bits viaja entero, ida y vuelta**.
     *
     * `wma_looper_arm_in_frames` devuelve `getPlayFrame() + offset`
     * (`watermelon_audio.cpp:2203`), así que el valor esperado **se deriva** del ancla que
     * el propio motor reporta en vez de escribirse a mano. Con un offset por encima de
     * 2³², cualquier estrechamiento a `jint` en el camino —en el parámetro o en el
     * retorno— cambia el resultado.
     *
     * El caso del índice negativo no es relleno: `armAndConfirm` lo mira **antes** de
     * comparar contra `getArmedTrack()`, porque armar la pista −1 se confirmaría solo
     * (`getArmedTrack()` reporta −1 para "nada armado"). Sin ese caso, el test no
     * distinguiría el retorno correcto de un `-1` constante.
     */
    @Test
    fun `armInFrames devuelve el ancla mas un offset que no entra en 32 bits`() {
        prepararPista(TRACK)

        val ancla = jni("nativeTransportGetPlayFrame") { it.transportGetPlayFrame() }
        val trigger = jni("nativeLooperArmInFrames") { it.looperArmInFrames(TRACK, OFFSET_ANCHO) }

        assertEquals(
            ancla + OFFSET_ANCHO,
            trigger,
            "el trigger tiene que ser el play frame más el offset. Si vino truncado, " +
                "el jlong se estrechó en algún tramo del cruce",
        )
        assertTrue(
            trigger > Int.MAX_VALUE.toLong(),
            "el trigger entró en 32 bits: el offset de $OFFSET_ANCHO no sobrevivió",
        )

        assertEquals(
            -1L,
            jni("nativeLooperArmInFrames") { it.looperArmInFrames(-1, OFFSET_ANCHO) },
            "una pista negativa no se puede armar",
        )
    }

    /**
     * AC-026.7 (segunda mitad) — `armAtNextBar` contra el compás **derivado**, no contra 0.
     *
     * En host el transporte no avanza, así que el ancla vale 0 y el próximo límite de
     * compás cae ahí mismo. Escribir `assertEquals(0L, ...)` sería afirmar una constante:
     * el valor esperado se calcula con la misma aritmética que `Transport::nextBarBoundary`
     * a partir de `transportFramesPerBar`, así que el día que el ancla deje de ser 0 el
     * test sigue diciendo la verdad.
     */
    @Test
    fun `armAtNextBar cae en el limite de compas derivado del transporte`() {
        prepararPista(TRACK)

        val ancla = jni("nativeTransportGetPlayFrame") { it.transportGetPlayFrame() }
        val porCompas = jni("nativeTransportFramesPerBar") { it.transportFramesPerBar(1) }.toLong()
        assertTrue(porCompas > 0, "el transporte reporta $porCompas frames por compás: no se puede derivar nada")

        val resto = ancla % porCompas
        val esperado = if (resto == 0L) ancla else ancla + (porCompas - resto)

        assertEquals(
            esperado,
            jni("nativeLooperArmAtNextBar") { it.looperArmAtNextBar(TRACK) },
            "el trigger no cae en el próximo límite de compás",
        )
        assertEquals(
            -1L,
            jni("nativeLooperArmAtNextBar") { it.looperArmAtNextBar(-1) },
            "una pista negativa no se puede armar",
        )
    }

    /**
     * AC-026.8 — el armado sincronizado, con **los dos triggers derivados de la referencia**.
     *
     * Sin cuanto, la captura arranca en el próximo cierre de vuelta de la referencia
     * (`refLen - refPos`); con cuanto, en el próximo múltiplo del cuanto adentro del ciclo.
     * Con la referencia recién importada el playhead está en 0, así que los dos valores se
     * derivan: `refLen + latencia` y `cuanto + latencia`.
     *
     * 🔴 Que sean **distintos entre sí** es la mitad del punto: son dos `JNIEXPORT`
     * separadas y el `quantumFrames` es el único parámetro que las diferencia. Si las dos
     * devolvieran lo mismo, el test no distinguiría que ese `jint` llegó.
     */
    @Test
    fun `armSyncedToLoop deriva su trigger de la referencia, y el cuanto lo cambia`() {
        prepararPista(TRACK)
        val refLen = importar(REF).toLong()
        jni("nativeLooperResumeTrack") { it.looperResumeTrack(REF) }
        assertTrue(
            jni("nativeLooperIsTrackPlaying") { it.looperIsTrackPlaying(REF) },
            "la referencia no quedó reproduciendo: sin eso el armado sincronizado no tiene de qué agarrarse",
        )

        val sinCuanto = jni("nativeLooperArmSyncedToLoop") {
            it.looperArmSyncedToLoop(TRACK, LATENCIA)
        }
        assertEquals(
            refLen + LATENCIA,
            sinCuanto,
            "sin cuanto la captura arranca al cerrar la vuelta de la referencia, más la latencia",
        )

        val conCuanto = jni("nativeLooperArmSyncedToLoopQuantized") {
            it.looperArmSyncedToLoopQuantized(TRACK, LATENCIA, CUANTO)
        }
        assertEquals(
            CUANTO + LATENCIA,
            conCuanto,
            "con cuanto la captura arranca en el próximo múltiplo del cuanto, más la latencia",
        )
        assertNotEquals(
            sinCuanto,
            conCuanto,
            "los dos triggers salieron iguales: el quantumFrames no llegó al otro lado",
        )
    }

    /**
     * AC-026.8 (el gemelo) — **sin referencia reproduciendo, los dos dicen `-1`**.
     *
     * Es lo que impide que el test de arriba pase con un motor que devuelve el mismo
     * número pase lo que pase: acá el único cambio es que la referencia está en pausa, y
     * el veredicto tiene que dar vuelta.
     */
    @Test
    fun `sin referencia reproduciendo, el armado sincronizado se rinde`() {
        prepararPista(TRACK)
        importar(REF)
        jni("nativeLooperPauseTrack") { it.looperPauseTrack(REF) }
        assertFalse(
            jni("nativeLooperIsTrackPlaying") { it.looperIsTrackPlaying(REF) },
            "la referencia sigue reproduciendo después del pause",
        )

        assertEquals(
            -1L,
            jni("nativeLooperArmSyncedToLoop") { it.looperArmSyncedToLoop(TRACK, LATENCIA) },
            "sin referencia reproduciendo tiene que rendirse con -1 para que el llamador use el armado simple",
        )
        assertEquals(
            -1L,
            jni("nativeLooperArmSyncedToLoopQuantized") {
                it.looperArmSyncedToLoopQuantized(TRACK, LATENCIA, CUANTO)
            },
            "la variante cuantizada tiene la misma salida de emergencia",
        )
    }

    /**
     * AC-026.9 — **los 32 bits ALTOS del `jlong` empaquetado**.
     *
     * La capa JNI empaqueta dos `int` en un `jlong` porque una llamada JNI no puede
     * devolver dos (`jni_audio_bridge.cpp:2520`). El fixture tiene silencio de cabeza a
     * propósito: con el primer frame audible en 5 000, un `jlong` truncado a 32 bits
     * dejaría ese número en cero y el otro intacto — o sea que este assert distingue
     * exactamente el estrechamiento que ninguna firma ve.
     *
     * 🔴 **El gemelo de silencio es la rama `return 0` de la capa JNI, y hasta MINI-016 era
     * código muerto.** Cuando REQ-026 escribió este test, el silencio devolvía `(0, largo)`:
     * `TrackBuffer::findContentBounds` escribe `outLast = len` **antes** de sus salidas
     * tempranas y devuelve `false`, pero `wma_looper_find_content_bounds` **se tragaba ese
     * `false`** y respondía `true`. O sea que *"no hay contenido"* y *"todo es contenido"*
     * eran el mismo valor para un consumidor, y el `if (!...) return 0` de
     * `jni_audio_bridge.cpp:2516` no se ejecutaba nunca.
     *
     * MINI-016 propagó el rechazo. Ahora el silencio devuelve `0L`, que la capa Kotlin
     * desempaqueta como `(0, 0)` — y **ese par no colisiona con ningún éxito**, porque un
     * resultado exitoso exige `outLast > outFirst`. Este assert es lo que mantiene viva esa
     * rama: si alguien vuelve a poner un `return true` incondicional, se cae acá.
     */
    @Test
    fun `findContentBounds trae el primer frame en los 32 bits altos`() {
        importar(TRACK)

        assertEquals(
            CONTINUO.single().start to CONTINUO.single().endExclusive,
            jni("nativeLooperFindContentBounds") { it.looperFindContentBounds(TRACK, 0.03f) },
            "los bordes no coinciden con el fixture. Si el primero vino en 0 y el segundo bien, " +
                "los 32 bits altos del jlong se perdieron",
        )

        val largo = importarSilencio(TRACK)
        assertTrue(largo > 0, "el gemelo tiene que ser una pista CON frames, o probaría el camino vacío")
        assertEquals(
            0 to 0,
            jni("nativeLooperFindContentBounds") { it.looperFindContentBounds(TRACK, 0.03f) },
            "sobre silencio la C API tiene que RECHAZAR (MINI-016), y la capa JNI devolver 0L. " +
                "Si volviera (0, $largo), el rechazo se está tragando otra vez",
        )
    }

    /**
     * AC-026.10 — el `jlong` **sin getter**, afirmado por comportamiento.
     *
     * `wma_looper_set_capabilities` sólo pisa el presupuesto cuando el valor es positivo
     * (`watermelon_audio.cpp:2161`), y `importTrack` lo consulta antes de reservar
     * (`LooperExporter.cpp:376`). Con eso, el único observable del parámetro es si la
     * importación entra o no — y usar un presupuesto **mayor que 2³²** es lo que hace que
     * un estrechamiento a `jint` no pueda simular el caso bueno.
     *
     * Los dos casos van juntos: sólo el grande pasaría también con un motor que ignora el
     * presupuesto, y sólo el chico pasaría con uno que rechaza siempre.
     */
    @Test
    fun `el presupuesto de memoria cruza como jlong y decide si la importacion entra`() {
        val fuente = MinimalWav.writeTo(File(dir, "presupuesto.wav"), FRAMES, CONTINUO)

        jni("nativeLooperSetCapabilities") {
            it.looperSetCapabilities(PRESUPUESTO_CHICO, maxTracks = 0, maxFreeSeconds = 0)
        }
        assertFalse(
            jni("nativeLooperImportTrack") { it.looperImportTrack(TRACK, fuente, MinimalWav.RATE) },
            "con $PRESUPUESTO_CHICO bytes de presupuesto no entran ${FRAMES * 2 * 4} de audio",
        )

        jni("nativeLooperSetCapabilities") {
            it.looperSetCapabilities(PRESUPUESTO_ANCHO, maxTracks = 0, maxFreeSeconds = 0)
        }
        assertTrue(
            jni("nativeLooperImportTrack") { it.looperImportTrack(TRACK, fuente, MinimalWav.RATE) },
            "con un presupuesto de $PRESUPUESTO_ANCHO la importación tiene que entrar. Si falla, " +
                "el jlong se estrechó y el motor está viendo un presupuesto chico o negativo",
        )
        assertEquals(
            FRAMES,
            jni("nativeLooperGetTrackLengthFrames") { it.looperGetTrackLengthFrames(TRACK) },
            "entró pero no dejó los frames del fixture",
        )
    }

    // ---- helpers ----

    /** Reserva capacidad sin contenido: es todo lo que `armRecording` exige (`AudioLooper.h:560`). */
    private fun prepararPista(track: Int) {
        assertTrue(
            jni("nativeLooperPrepareTrack") { it.looperPrepareTrack(track, FRAMES, MinimalWav.RATE) },
            "no se pudo reservar capacidad para la pista $track",
        )
    }

    /** Importa el fixture con contenido y devuelve el largo que quedó. */
    private fun importar(track: Int): Int = importarFixture(track, CONTINUO)

    /** Importa silencio exacto — el gemelo de [importar]. */
    private fun importarSilencio(track: Int): Int = importarFixture(track, emptyList())

    private fun importarFixture(track: Int, regiones: List<MinimalWav.Region>): Int {
        val fuente = File(dir, "fuente-$track-${regiones.size}.wav")
        val ruta = MinimalWav.writeTo(fuente, FRAMES, regiones)
        assertTrue(
            jni("nativeLooperImportTrack") { it.looperImportTrack(track, ruta, MinimalWav.RATE) },
            "el fixture no importó en la pista $track",
        )
        assertTrue(
            jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(track) },
            "importó en la pista $track y no quedó activa",
        )
        return jni("nativeLooperGetTrackLengthFrames") { it.looperGetTrackLengthFrames(track) }
    }
}
