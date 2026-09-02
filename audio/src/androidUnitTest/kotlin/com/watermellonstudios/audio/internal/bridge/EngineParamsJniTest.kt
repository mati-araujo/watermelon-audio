package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.engine.EngineParameterDef
import org.junit.AfterClass
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-028 S2 — **la metadata de parámetros de engine, cruzando de verdad** (AC-028.3).
 *
 * Tres `JNIEXPORT` nuevas ejecutadas contra un `JNIEnv` real, y con ellas el
 * `jobjectArray` de **`String`** de salida: `NewObjectArray` + `FindClass` +
 * `SetObjectArrayElement` + `DeleteLocalRef`, que hasta ahora sólo existía en
 * `nativeDrainCapturedLogs` y **ninguna clase del arnés ejercía**.
 *
 * ## Este archivo NO vuelve a declarar los 15 valores, y es deliberado
 *
 * Ya están declarados **una** vez, en `test_c_api_engine_params.cpp`, junto al oráculo
 * que los compara contra la clase concreta del engine. Copiarlos acá sería la cuarta
 * copia de la tabla en un REQ cuyo problema es que existen dos. Lo que se afirma acá es
 * lo que sólo se puede afirmar **cruzando la frontera**:
 *
 * - los seis engines publican sus N parámetros y **suman 15** (AC-028.3);
 * - los cinco campos llegan **bien formados**, no vacíos ni degenerados;
 * - las quince defs son **distintas entre sí** — un getter cableado al parámetro 0, o al
 *   engine 1, pasa cualquier chequeo de forma y muere acá;
 * - **un ancla**: Karplus-Strong[0] es `"Brightness"` / `"BRIGHT"`. Una sola tupla, y no
 *   como catálogo: es lo único que fija que el par `(engineType, índice)` mapea al def
 *   correcto y que `name` y `shortName` no viajan permutados. La distinción sola no lo
 *   ve, porque una permutación deja quince defs igual de distintas;
 * - el rechazo llega como `null` y no como un def relleno (AC-028.5 del otro lado).
 *
 * ## Ningún test de acá arranca el motor, y eso ES una afirmación
 *
 * `ensureEngine()` lo construye perezosamente y los engines nacen en el **constructor**
 * del dispatcher, así que la metadata no depende de `prepare()`, de `start()` ni de que
 * haya audio. Es AC-028.1 visto desde Kotlin: si algún día hiciera falta arrancar, este
 * archivo se pondría rojo sin que nadie tenga que acordarse de probarlo.
 *
 * 🔴 Verde acá NO significa "la metadata está probada": esto valida la **frontera** sobre
 * un backend falso. Que lo expuesto coincida con lo que el engine implementa lo vigila la
 * suite de C++. Ver el KDoc de [JniHarness].
 */
class EngineParamsJniTest {

    companion object {
        private const val OWNER = "EngineParamsJniTest"

        private val COVERED = setOf(
            "nativeGetEngineParameterCount",
            "nativeGetEngineParameterNames",
            "nativeGetEngineParameterRange",
            "nativeGetEngineType",
            "nativeSetEngineType",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        /** CLASSIC (0) queda afuera: no es un `SynthEngine`, usa los osciladores legacy. */
        private const val CLASSIC = 0
        private const val KARPLUS_STRONG = 1
        private val SYNTH_ENGINES = listOf(1, 2, 3, 4, 5, 6)

        /** AC-028.3, y el único número de catálogo que este archivo repite. */
        private const val TOTAL_PARAMS = 15
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    private fun cuantos(engineType: Int): Int =
        jni("nativeGetEngineParameterCount") { it.getEngineParameterCount(engineType) }

    /**
     * Un solo `getEngineParameterDef` cruza **dos** `JNIEXPORT` —los nombres y el
     * rango—, así que se anotan las dos. [JniHarness.exercise] anota una sola, y anotar
     * de menos dejaría la otra como cobertura no declarada: el trinquete es
     * bidireccional, así que eso también es rojo.
     *
     * Se entra por `AudioNativeBridge.getInstance()`, el mismo camino de producción que
     * usa `exercise`, después de exigir la librería igual que él.
     */
    private fun def(engineType: Int, paramIndex: Int): EngineParameterDef? {
        JniHarness.requireNativeLibrary()
        val d = AudioNativeBridge.getInstance().getEngineParameterDef(engineType, paramIndex)
        JniCoverage.record(OWNER, "nativeGetEngineParameterNames")
        JniCoverage.record(OWNER, "nativeGetEngineParameterRange")
        return d
    }

    // ===================================================================
    // AC-028.3 — los seis engines, sus 15 parámetros, sus cinco campos
    // ===================================================================

    @Test
    fun `los seis engines publican sus parametros y suman quince`() {
        val porEngine = SYNTH_ENGINES.associateWith { cuantos(it) }

        porEngine.forEach { (engineType, count) ->
            assertTrue(count > 0, "el engine $engineType no publicó ningún parámetro")
        }
        assertEquals(
            TOTAL_PARAMS,
            porEngine.values.sum(),
            "el catálogo que cruzó no suma 15: $porEngine",
        )
    }

    @Test
    fun `los cinco campos llegan bien formados para los quince`() {
        var vistos = 0
        for (engineType in SYNTH_ENGINES) {
            for (i in 0 until cuantos(engineType)) {
                val d = assertNotNull(def(engineType, i), "engine $engineType param $i no cruzó")

                assertTrue(d.name.isNotBlank(), "engine $engineType param $i: nombre vacío")
                assertTrue(d.shortName.isNotBlank(), "engine $engineType param $i: etiqueta vacía")
                assertTrue(
                    d.minValue < d.maxValue,
                    "engine $engineType param $i: rango degenerado ${d.minValue}..${d.maxValue}",
                )
                assertTrue(
                    d.defaultValue in d.minValue..d.maxValue,
                    "engine $engineType param $i: el default ${d.defaultValue} cae fuera de su rango",
                )
                assertTrue(
                    d.name != "Unknown",
                    "engine $engineType param $i: cruzó el centinela del override con cara de dato",
                )
                vistos++
            }
        }
        assertEquals(TOTAL_PARAMS, vistos)
    }

    /**
     * El gemelo del test de arriba. Sin esto, un getter cableado al `(1, 0)` publica
     * quince defs perfectamente bien formadas y pasa todo lo anterior.
     */
    @Test
    fun `las quince defs son distintas entre si`() {
        val todas = SYNTH_ENGINES.flatMap { engineType ->
            (0 until cuantos(engineType)).map { i -> def(engineType, i) }
        }

        assertEquals(TOTAL_PARAMS, todas.size)
        assertEquals(
            TOTAL_PARAMS,
            todas.toSet().size,
            "hay defs repetidas: algún índice o algún tipo de engine no llega al otro lado",
        )
    }

    /**
     * El ancla. Una tupla, y fija dos cosas que la distinción no puede ver: que
     * `(engineType, índice)` mapea al parámetro correcto, y que `name` y `shortName` no
     * viajan permutados por el `jobjectArray`.
     */
    @Test
    fun `el parametro cero de Karplus-Strong cruza entero`() {
        val d = assertNotNull(def(KARPLUS_STRONG, 0))

        assertEquals("Brightness", d.name, "¿se permutaron name y shortName en el array?")
        assertEquals("BRIGHT", d.shortName)
        assertEquals(0f, d.minValue)
        assertEquals(1f, d.maxValue)
        assertEquals(0.5f, d.defaultValue)
    }

    // ===================================================================
    // AC-028.5 desde este lado — el rechazo llega como null
    // ===================================================================

    @Test
    fun `un indice fuera de rango y un engine invalido llegan como null`() {
        for (engineType in SYNTH_ENGINES) {
            val count = cuantos(engineType)
            assertNull(def(engineType, count), "engine $engineType: el índice $count está fuera de rango")
            assertNull(def(engineType, -1), "engine $engineType: un índice negativo no es un def")
        }

        for (invalido in listOf(CLASSIC, -1, 7, 999)) {
            assertEquals(0, cuantos(invalido), "el tipo $invalido no es un synth engine")
            assertNull(def(invalido, 0), "el tipo $invalido no puede devolver metadata")
        }
    }

    /**
     * El engine ACTIVO no cambia lo que la metadata devuelve (AC-028.1 por Kotlin).
     * Se mueve el activo entre las dos lecturas y se exige el mismo def.
     */
    @Test
    fun `la metadata no depende de cual sea el engine activo`() {
        val antes = assertNotNull(def(4, 0), "granular param 0")

        jni("nativeSetEngineType") { it.setEngineType(KARPLUS_STRONG) }
        assertEquals(
            KARPLUS_STRONG,
            jni("nativeGetEngineType") { it.getEngineType() },
            "el engine activo no se movió: el test siguiente no probaría nada",
        )

        assertEquals(antes, def(4, 0), "la metadata de granular cambió al cambiar el engine activo")
    }
}
