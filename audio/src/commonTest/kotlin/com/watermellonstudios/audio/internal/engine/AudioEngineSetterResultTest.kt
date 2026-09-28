package com.watermellonstudios.audio.internal.engine

import com.watermellonstudios.audio.api.config.AudioEngineConfig
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.modulator.ModulatorType
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * REQ-045 S2 (AC-045.5, decisión 6) — **los siete setters públicos dicen cuándo el motor no
 * hizo lo pedido**, y no publican estado sobre un rechazo.
 *
 * ## El defecto, que era de DOS capas
 *
 * 1. El bridge tiraba el `jint` que el cruce JNI devolvía (`setModulatorType`,
 *    `setModulatorParameter` y los cinco `*Sync` de efectos: S0 midió siete descartes).
 * 2. `AudioEngineImpl` publicaba en `state` **de todas formas**. `setModulator` era el peor:
 *    escribía `state.modulator`, disparaba `analytics.onModulatorChanged` y logueaba "changed"
 *    **antes** de llamar al motor. Con un id que el motor no acepta, la librería entera —
 *    estado, analytics y log— afirmaba un cambio que no ocurrió.
 *
 * 🔴 Lo segundo no lo arregla propagar el `Result`: son dos mentiras independientes. Ésta es
 * la que un consumidor ve incluso si ignora el retorno, que es exactamente lo que NoisyPad
 * hacía.
 *
 * ## Y cada rechazo va con su gemelo
 *
 * Un bridge que dijera `failure` a todo pasaría la mitad de estos asserts, así que cada uno
 * tiene su camino de éxito afirmado igual de fuerte: con `success` el estado SÍ se publica.
 */
class AudioEngineSetterResultTest {

    private val causa = NativeBridgeException.InvalidEffectIndex(3, 1)

    private fun motor(vararg rechaza: String) = AudioEngineImpl(
        AudioEngineConfig(),
        FakeAudioNativeBridge(setterResults = rechaza.associateWith { Result.failure(causa) }),
    )

    /** La causa tipada de un `failure`, o un fallo que la nombra. */
    private inline fun <reified E : NativeBridgeException> causaDe(
        resultado: Result<Unit>,
        que: String,
    ): E {
        val error = resultado.exceptionOrNull()
        assertNotNull(error, "$que devolvió ÉXITO con el motor rechazando: el fallo se perdió (D3).")
        return assertIs<E>(error, "$que falló con la causa equivocada: $error")
    }

    @Test
    fun `setModulator rechazado no publica el modulador ni avisa del cambio`() = runTest {
        val engine = motor("setModulatorType")

        causaDe<NativeBridgeException.InvalidEffectIndex>(
            engine.setModulator(ModulatorType.FM),
            "setModulator(FM) con el motor rechazando",
        )
        assertEquals(
            ModulatorType.NONE,
            engine.state.value.modulator,
            "el motor rechazó el tipo y state.modulator igual dice FM: la mentira que el " +
                "Result no alcanza a tapar, porque se publicaba ANTES de preguntar",
        )
    }

    @Test
    fun `setModulator aceptado si publica el modulador`() = runTest {
        val engine = motor()

        assertTrue(engine.setModulator(ModulatorType.FM).isSuccess)
        assertEquals(
            ModulatorType.FM,
            engine.state.value.modulator,
            "el gemelo: con el motor diciendo sí, el estado tiene que cambiar — sin esto, un " +
                "impl que no publica nunca pasaría el test de arriba",
        )
    }

    @Test
    fun `el parametro del modulador propaga el rechazo`() = runTest {
        causaDe<NativeBridgeException.InvalidEffectIndex>(
            motor("setModulatorParameter").setModulatorParameter(0, 0.5f),
            "setModulatorParameter con el motor rechazando",
        )
        assertTrue(motor().setModulatorParameter(0, 0.5f).isSuccess)
    }

    @Test
    fun `removeEffect rechazado deja la cadena como estaba`() = runTest {
        val engine = motor("removeEffectSync")
        assertTrue(engine.addEffect(EffectType.REVERB), "premisa: la cadena tiene un efecto")

        causaDe<NativeBridgeException.InvalidEffectIndex>(
            engine.removeEffect(0),
            "removeEffect(0) con el motor rechazando",
        )
        assertEquals(
            listOf(EffectType.REVERB),
            engine.state.value.effectChain.effects.map { it.type },
            "el motor no lo quitó y la cadena de state igual lo perdió",
        )
    }

    @Test
    fun `removeEffect con un indice que no existe se rechaza sin tocar el motor`() = runTest {
        val bridge = FakeAudioNativeBridge()
        val engine = AudioEngineImpl(AudioEngineConfig(), bridge)

        causaDe<NativeBridgeException.InvalidEffectIndex>(
            engine.removeEffect(0),
            "removeEffect(0) sobre una cadena vacía",
        )
        assertEquals(
            emptyList(),
            bridge.calls,
            "con un índice inválido no hay nada que pedirle al motor, y antes de REQ-045 esto " +
                "era un `return` mudo: el consumidor no distinguía 'no existe' de 'listo'",
        )
    }

    @Test
    fun `removeEffect aceptado si saca el efecto`() = runTest {
        val engine = motor()
        assertTrue(engine.addEffect(EffectType.REVERB))

        assertTrue(engine.removeEffect(0).isSuccess)
        assertEquals(emptyList(), engine.state.value.effectChain.effects)
    }

    @Test
    fun `setEffectParameter rechazado no publica el parametro`() = runTest {
        val engine = motor("setEffectParameterSync")
        assertTrue(engine.addEffect(EffectType.REVERB))

        causaDe<NativeBridgeException.InvalidEffectIndex>(
            engine.setEffectParameter(0, 0, 0.75f),
            "setEffectParameter con el motor rechazando",
        )
        assertEquals(
            emptyMap(),
            engine.state.value.effectChain.effects[0].parameters,
            "el motor no lo aplicó y state igual publica el valor",
        )

        val ok = motor().also { it.addEffect(EffectType.REVERB) }
        assertTrue(ok.setEffectParameter(0, 0, 0.75f).isSuccess)
        assertEquals(0.75f, ok.state.value.effectChain.effects[0].parameters[0])
    }

    @Test
    fun `setEffectBypass rechazado no publica el bypass`() = runTest {
        val engine = motor("setEffectBypassSync")
        assertTrue(engine.addEffect(EffectType.REVERB))

        causaDe<NativeBridgeException.InvalidEffectIndex>(
            engine.setEffectBypass(0, true),
            "setEffectBypass con el motor rechazando",
        )
        assertFalse(
            engine.state.value.effectChain.effects[0].isBypassed,
            "el motor no bypasseó y state dice que sí",
        )

        val ok = motor().also { it.addEffect(EffectType.REVERB) }
        assertTrue(ok.setEffectBypass(0, true).isSuccess)
        assertTrue(ok.state.value.effectChain.effects[0].isBypassed)
    }

    @Test
    fun `setEffectsBypass rechazado no publica el bypass global`() = runTest {
        val engine = motor("setEffectsBypassSync")

        causaDe<NativeBridgeException.InvalidEffectIndex>(
            engine.setEffectsBypass(true),
            "setEffectsBypass con el motor rechazando",
        )
        assertFalse(engine.state.value.effectChain.isGloballyBypassed)

        val ok = motor()
        assertTrue(ok.setEffectsBypass(true).isSuccess)
        assertTrue(ok.state.value.effectChain.isGloballyBypassed)
    }

    /**
     * El más caro de los siete: el `removeAt`/`add` corría **igual** cuando el motor no
     * había reordenado nada, así que un índice inválido no era un no-op mudo sino un
     * `IndexOutOfBoundsException` desde adentro de `_state.update` — un no-op en el motor y
     * una excepción en el consumidor, por la misma llamada.
     */
    @Test
    fun `reorderEffects rechazado deja el orden intacto y no explota`() = runTest {
        val engine = motor("reorderEffectsSync")
        assertTrue(engine.addEffect(EffectType.REVERB))
        assertTrue(engine.addEffect(EffectType.DELAY))
        val antes = engine.state.value.effectChain.effects.map { it.type }

        causaDe<NativeBridgeException.InvalidEffectIndex>(
            engine.reorderEffects(0, 9),
            "reorderEffects(0, 9) con el motor rechazando",
        )
        assertEquals(antes, engine.state.value.effectChain.effects.map { it.type })

        val ok = motor()
        ok.addEffect(EffectType.REVERB)
        ok.addEffect(EffectType.DELAY)
        assertTrue(ok.reorderEffects(0, 1).isSuccess)
        assertEquals(
            listOf(EffectType.DELAY, EffectType.REVERB),
            ok.state.value.effectChain.effects.map { it.type },
        )
    }
}
