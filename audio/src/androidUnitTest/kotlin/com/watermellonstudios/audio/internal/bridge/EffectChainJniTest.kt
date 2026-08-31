package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.effect.EffectType
import kotlinx.coroutines.runBlocking
import org.junit.AfterClass
import org.junit.Before
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-023 S1 — **el camino de efectos, ejecutado.**
 *
 * Se eligió este grupo y no otro de los 236 que faltaban por lo que **cruza**, no por su
 * tamaño:
 *
 * - **Un enum.** `addEffect(typeId)` manda el `EffectType.id` de Kotlin y `getEffectType`
 *   devuelve el del C++. Un desfasaje de numeración entre los dos enums **no lo ve ningún
 *   gate**: compila, linkea, pasa `check-jni-symbols.py` —que compara nombres— y agrega
 *   el efecto equivocado en el device.
 * - **Dos arrays a la vez.** `setEffectParametersBatch(index, IntArray, FloatArray)` es
 *   del otro lado `GetIntArrayElements` + `GetFloatArrayElements`. Un pinneo mal liberado
 *   o un largo mal calculado sólo se ve **ejecutando**.
 * - Floats, booleanos, y la manipulación de la cadena.
 *
 * Y no necesita render: la cadena y sus parámetros son **estado**, no audio.
 *
 * ## Dos valores por par, ninguno potencia de dos
 *
 * REQ-022 lo volvió a medir con un mutante: cablear un getter a una constante **sobrevive
 * al primer assert** si la constante es el primer valor. Lo mata el segundo.
 *
 * 🔴 **Y los parámetros NO son normalizados**, cosa que este arnés enseñó al primer intento:
 * la versión inicial mandaba `0,375` al parámetro 0 de FILTER y volvía **`20.0`**. Ese
 * parámetro es la **frecuencia de corte en Hz (20–20000)**, así que `0,375` se clampeaba al
 * mínimo. Cada parámetro tiene su rango real (`EffectParameter.kt`), y un test que use
 * valores normalizados **mide el clamp, no el cruce**. De ahí `1234,5` / `440,25` Hz para la
 * frecuencia y `2,375` / `6,625` para la resonancia (0,1–10).
 *
 * 🔴 Verde acá NO significa "los efectos están probados": esto valida la **frontera**, no
 * que un efecto suene. Ver el KDoc de [JniHarness].
 */
class EffectChainJniTest {

    companion object {
        private const val OWNER = "EffectChainJniTest"

        /** Dos tipos distintos: un enum mal numerado se ve al comparar los DOS. */
        private val FILTRO = EffectType.FILTER
        private val DELAY = EffectType.DELAY

        private val COVERED = setOf(
            "nativeStartTuner",
            "nativeAddEffect",
            "nativeRemoveEffect",
            "nativeClearAllEffects",
            "nativeGetEffectChainSize",
            "nativeGetEffectType",
            "nativeGetEffectParameter",
            "nativeSetEffectParameter",
            "nativeSetEffectParametersBatch",
            "nativeSetEffectBypass",
            "nativeIsEffectBypassed",
            "nativeSetEffectsBypass",
            "nativeIsEffectsBypassed",
            "nativeReorderEffects",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    /**
     * El gemelo para los wrappers `suspend`.
     *
     * Cuatro de los que esta clase ejerce lo son porque pasan por `BridgeConcurrency`
     * (`getEffectType`, `isEffectBypassed`, `clearAllEffects`, `setParametersBatch`), y
     * **entrar por ahí es parte del punto**: es el camino de producción. `runBlocking` va
     * ACÁ y no esparcido en cada llamada, para que los tests se lean igual que los del
     * resto del arnés.
     */
    private fun <T> jniSus(name: String, call: suspend (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name) { b -> runBlocking { call(b) } }

    /**
     * Motor arriba y **cadena vacía**. El `clearAllEffects` no es higiene: cada clase del
     * arnés corre en su propia JVM, pero los `@Test` de ESTA clase comparten el motor —
     * que es un singleton de proceso—, así que sin esto el tamaño de cadena que afirma un
     * test dependería de qué test corrió antes.
     */
    @Before
    fun engineUpAndChainEmpty() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")
        jniSus("nativeClearAllEffects") { it.clearAllEffects() }
        assertEquals(
            0,
            jni("nativeGetEffectChainSize") { it.getEffectChainSize() },
            "la cadena no quedó vacía: los tests de abajo medirían sobre restos de otro test",
        )
    }

    /**
     * AC-023.1 — **el round-trip del enum**, que es lo más valioso de esta clase.
     *
     * Con **dos** tipos: si los enums de Kotlin y C++ estuvieran corridos en una posición,
     * un solo tipo podría coincidir por casualidad; dos no.
     */
    @Test
    fun `el tipo de efecto sobrevive el cruce, con dos tipos distintos`() {
        assertTrue(
            jni("nativeAddEffect") { it.addEffectSync(FILTRO.id) },
            "no se pudo agregar ${FILTRO.displayName}",
        )
        assertEquals(
            FILTRO,
            jniSus("nativeGetEffectType") { it.getEffectType(0) },
            "el tipo volvió distinto del que se mandó: los enums de Kotlin y C++ no coinciden",
        )

        assertTrue(jni("nativeAddEffect") { it.addEffectSync(DELAY.id) }, "no se pudo agregar ${DELAY.displayName}")
        assertEquals(
            DELAY,
            jniSus("nativeGetEffectType") { it.getEffectType(1) },
            "el segundo tipo no volvió igual: un enum corrido en una posición pasa el caso de arriba",
        )
        // Y el primero sigue siendo el primero: agregar no reordenó nada.
        assertEquals(FILTRO, jniSus("nativeGetEffectType") { it.getEffectType(0) })
    }

    /** AC-023.2 — parámetro individual, dos valores, ninguno potencia de dos. */
    @Test
    fun `un parametro de efecto cruza y vuelve, con dos valores`() {
        assertTrue(jni("nativeAddEffect") { it.addEffectSync(FILTRO.id) })

        // Frecuencia de corte, en Hz. Dentro de 20..20000 y exactos en float.
        jni("nativeSetEffectParameter") { it.setEffectParameterSync(0, 0, 1234.5f) }
        assertEquals(
            1234.5f,
            jni("nativeGetEffectParameter") { it.getEffectParameterSync(0, 0) },
            "el parámetro no volvió igual: un jdouble donde va jfloat se ve acá",
        )

        jni("nativeSetEffectParameter") { it.setEffectParameterSync(0, 0, 440.25f) }
        assertEquals(
            440.25f,
            jni("nativeGetEffectParameter") { it.getEffectParameterSync(0, 0) },
            "el parámetro no siguió al segundo valor: un getter cableado a una constante " +
                "sobrevive al primer assert y muere en éste",
        )
    }

    /**
     * AC-023.3 — **el batch, que cruza DOS arrays a la vez.**
     *
     * Del otro lado son `GetIntArrayElements` + `GetFloatArrayElements`. Se verifica
     * leyendo los parámetros **de a uno**, con el getter individual: si el batch se
     * aplicara sólo al primero, o se corriera un índice, se ve.
     */
    @Test
    fun `el batch aplica todos los parametros, y se verifica de a uno`() {
        assertTrue(jni("nativeAddEffect") { it.addEffectSync(FILTRO.id) })

        // Cada uno DENTRO de su rango real: frecuencia 20..20000, resonancia 0,1..10,
        // tipo 0..2. Un valor fuera de rango se clampea y el test mediría el clamp.
        val esperado = mapOf(0 to 1234.5f, 1 to 2.375f, 2 to 1f)
        val r = jniSus("nativeSetEffectParametersBatch") { it.setParametersBatch(0, esperado) }
        assertTrue(r.isSuccess, "el batch falló: ${r.exceptionOrNull()}")

        for ((paramId, valor) in esperado) {
            assertEquals(
                valor,
                jni("nativeGetEffectParameter") { it.getEffectParameterSync(0, paramId) },
                "el batch no aplicó el parámetro $paramId; un índice corrido se ve acá",
            )
        }
    }

    /** AC-023.2 — bypass por efecto y global, **los dos sentidos**. */
    @Test
    fun `el bypass viaja en los dos sentidos, por efecto y global`() {
        assertTrue(jni("nativeAddEffect") { it.addEffectSync(FILTRO.id) })

        jni("nativeSetEffectBypass") { it.setEffectBypassSync(0, true) }
        assertTrue(
            jniSus("nativeIsEffectBypassed") { it.isEffectBypassed(0) },
            "el bypass del efecto no quedó en true",
        )
        jni("nativeSetEffectBypass") { it.setEffectBypassSync(0, false) }
        assertFalse(
            jniSus("nativeIsEffectBypassed") { it.isEffectBypassed(0) },
            "el bypass no volvió a false: un getter cableado a true pasa el caso de arriba",
        )

        jni("nativeSetEffectsBypass") { it.setEffectsBypassSync(true) }
        assertTrue(
            jni("nativeIsEffectsBypassed") { it.isEffectsBypassedSync() },
            "el bypass GLOBAL no quedó en true",
        )
        jni("nativeSetEffectsBypass") { it.setEffectsBypassSync(false) }
        assertFalse(
            jni("nativeIsEffectsBypassed") { it.isEffectsBypassedSync() },
            "el bypass global no volvió a false",
        )
    }

    /**
     * AC-023.4 — la cadena reporta su **tamaño real** en cada paso, y reordenar **no lo
     * cambia** pero sí cambia el orden. Esa segunda mitad es la que hace útil al test:
     * sin ella, un `reorderEffects` que no hiciera nada pasaría igual.
     */
    @Test
    fun `la cadena reporta su tamano al agregar, reordenar y quitar`() {
        assertTrue(jni("nativeAddEffect") { it.addEffectSync(FILTRO.id) })
        assertEquals(1, jni("nativeGetEffectChainSize") { it.getEffectChainSize() }, "tras agregar uno")

        assertTrue(jni("nativeAddEffect") { it.addEffectSync(DELAY.id) })
        assertEquals(2, jni("nativeGetEffectChainSize") { it.getEffectChainSize() }, "tras agregar dos")

        // Reordenar: el tamaño NO cambia y el orden SÍ.
        jni("nativeReorderEffects") { it.reorderEffectsSync(0, 1) }
        assertEquals(2, jni("nativeGetEffectChainSize") { it.getEffectChainSize() }, "reordenar no cambia el tamaño")
        assertEquals(
            DELAY,
            jniSus("nativeGetEffectType") { it.getEffectType(0) },
            "reordenar 0->1 tiene que dejar el DELAY primero; si esto es FILTER, reorder no hizo nada",
        )

        jni("nativeRemoveEffect") { it.removeEffectSync(0) }
        assertEquals(1, jni("nativeGetEffectChainSize") { it.getEffectChainSize() }, "tras quitar uno")
        assertEquals(
            FILTRO,
            jniSus("nativeGetEffectType") { it.getEffectType(0) },
            "quitar el primero tiene que dejar al otro en la posición 0",
        )
    }
}
