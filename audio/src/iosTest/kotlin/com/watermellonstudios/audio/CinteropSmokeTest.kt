package com.watermellonstudios.audio

import com.watermellonstudios.audio.internal.cinterop.WMA_OK
import com.watermellonstudios.audio.internal.cinterop.wma_effect_add
import com.watermellonstudios.audio.internal.cinterop.wma_effect_chain_size
import com.watermellonstudios.audio.internal.cinterop.wma_effect_clear_all
import com.watermellonstudios.audio.internal.cinterop.wma_effect_get_param
import com.watermellonstudios.audio.internal.cinterop.wma_effect_get_type
import com.watermellonstudios.audio.internal.cinterop.wma_effect_is_bypassed
import com.watermellonstudios.audio.internal.cinterop.wma_effect_set_bypass
import com.watermellonstudios.audio.internal.cinterop.wma_effect_set_param
import com.watermellonstudios.audio.internal.cinterop.wma_engine_create
import com.watermellonstudios.audio.internal.cinterop.wma_engine_destroy
import com.watermellonstudios.audio.internal.cinterop.wma_get_version
import com.watermellonstudios.audio.internal.cinterop.wma_set_xy
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.toKString
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * WA-T.3 — smoke del cinterop (WA-3.1).
 *
 * Lo que se está probando acá no es el DSP: es que **el puente existe y marshalea
 * bien**. La suite C++ ya cubre el comportamiento del motor; lo que ningún test
 * cubría hasta ahora es que Kotlin/Native resuelva los símbolos `wma_*` del `.a`
 * y que cada familia de tipos cruce la frontera intacta.
 *
 * Por eso los casos están elegidos por **categoría de marshalling**, no por
 * feature: puntero opaco, `const char*`, `int`, `float`, `bool` y enum. Si una
 * de esas cruza mal, el síntoma en producción sería basura silenciosa, no un
 * crash.
 *
 * Deliberadamente NO arranca el motor. `wma_engine_start()` abre un stream de
 * CoreAudio, y si el simulador no tiene salida de audio disponible el test se
 * vuelve flaky por una razón que no tiene nada que ver con el cinterop. El
 * sonido real es WA-4.3, en device.
 */
@OptIn(ExperimentalForeignApi::class)
class CinteropSmokeTest {

    /** `const char*` → String. Si el símbolo no resolviera, esto ni linkearía. */
    @Test
    fun versionCrossesTheBoundaryAsAString() {
        val version = wma_get_version()?.toKString()

        assertNotNull(version, "wma_get_version() devolvió null")
        assertTrue(version.isNotEmpty(), "version vacía")
        // Formato semver x.y.z — no se fija el valor porque lo inyecta el build.
        val parts = version.split(".")
        assertEquals(3, parts.size, "version mal formada: '$version'")
        assertTrue(
            parts.all { it.isNotEmpty() && it.all(Char::isDigit) },
            "version mal formada: '$version'",
        )
    }

    /** Handle opaco: crear y destruir sin filtrar ni crashear. */
    @Test
    fun engineHandleRoundTrips() {
        val engine = wma_engine_create()
        assertNotNull(engine, "wma_engine_create() devolvió null")
        wma_engine_destroy(engine)
    }

    /** int de ida y vuelta, incluido el índice que devuelve add(). */
    @Test
    fun effectChainRoundTripsIntegers() {
        val engine = assertNotNull(wma_engine_create())
        try {
            assertEquals(0, wma_effect_chain_size(engine), "la cadena no arranca vacía")

            // FILTER = 0 y REVERB = 1 tienen IDs congelados por compatibilidad
            // (ver EffectTypes.h), así que son seguros de hardcodear.
            val filterIndex = wma_effect_add(engine, FILTER)
            assertEquals(0, filterIndex, "el primer efecto debería quedar en el índice 0")

            val reverbIndex = wma_effect_add(engine, REVERB)
            assertEquals(1, reverbIndex, "el segundo efecto debería quedar en el índice 1")

            assertEquals(2, wma_effect_chain_size(engine))
            assertEquals(FILTER, wma_effect_get_type(engine, 0))
            assertEquals(REVERB, wma_effect_get_type(engine, 1))

            assertEquals(WMA_OK, wma_effect_clear_all(engine))
            assertEquals(0, wma_effect_chain_size(engine))
        } finally {
            wma_engine_destroy(engine)
        }
    }

    /**
     * float de ida y vuelta — la categoría del path de control de alta frecuencia.
     *
     * El param 0 de FILTER es el cutoff **en Hz**, no un valor normalizado: se
     * usa 1234.5 (no redondo, para que un default no pueda hacer pasar el test
     * por accidente) y tiene que volver idéntico.
     */
    @Test
    fun effectParameterRoundTripsFloat() {
        val engine = assertNotNull(wma_engine_create())
        try {
            wma_effect_add(engine, FILTER)

            assertEquals(WMA_OK, wma_effect_set_param(engine, 0, CUTOFF_HZ, 1234.5f))
            assertEquals(1234.5f, wma_effect_get_param(engine, 0, CUTOFF_HZ), 1e-6f)
        } finally {
            wma_engine_destroy(engine)
        }
    }

    /**
     * La C API **no** es un passthrough de bytes: aplica el dominio del motor.
     *
     * `FilterEffect::setCutoff` clampea a [20, 20000] Hz (FilterEffect.cpp:21), y
     * ese clamp tiene que seguir vigente cruzando el puente. Este test nació de un
     * falso positivo: la primera versión del round-trip de float pedía 0.25 y
     * recibía 20.0, que parecía un bug de marshalling y era el motor haciendo lo
     * correcto. Queda como test para que la próxima persona no repita el susto.
     */
    @Test
    fun outOfRangeParameterIsClampedByTheEngineNotSilentlyAccepted() {
        val engine = assertNotNull(wma_engine_create())
        try {
            wma_effect_add(engine, FILTER)

            wma_effect_set_param(engine, 0, CUTOFF_HZ, 0.25f)
            assertEquals(20.0f, wma_effect_get_param(engine, 0, CUTOFF_HZ), 1e-6f)

            wma_effect_set_param(engine, 0, CUTOFF_HZ, 96_000.0f)
            assertEquals(20_000.0f, wma_effect_get_param(engine, 0, CUTOFF_HZ), 1e-6f)
        } finally {
            wma_engine_destroy(engine)
        }
    }

    /** bool de ida y vuelta: `stdbool.h` mapea a Boolean, no a Int. */
    @Test
    fun bypassRoundTripsBoolean() {
        val engine = assertNotNull(wma_engine_create())
        try {
            wma_effect_add(engine, FILTER)
            assertEquals(false, wma_effect_is_bypassed(engine, 0))

            assertEquals(WMA_OK, wma_effect_set_bypass(engine, 0, true))
            assertEquals(true, wma_effect_is_bypassed(engine, 0))
        } finally {
            wma_engine_destroy(engine)
        }
    }

    /**
     * `wma_set_xy` es el path que corre una vez por frame de gesto (D1: el motivo
     * por el que el puente es cinterop directo y no un shim). Acá solo se verifica
     * que la llamada cruce; la latencia se mide en WA-4.3.
     */
    @Test
    fun setXyAcceptsTheControlPath() {
        val engine = assertNotNull(wma_engine_create())
        try {
            wma_set_xy(engine, 0.0f, 0.0f)
            wma_set_xy(engine, 0.5f, 0.5f)
            wma_set_xy(engine, 1.0f, 1.0f)
        } finally {
            wma_engine_destroy(engine)
        }
    }

    private companion object {
        // EffectTypes.h congela estos dos por compatibilidad hacia atrás.
        const val FILTER = 0
        const val REVERB = 1

        /** FilterEffect::setParam, case 0 → setCutoff (Hz). */
        const val CUTOFF_HZ = 0
    }
}
