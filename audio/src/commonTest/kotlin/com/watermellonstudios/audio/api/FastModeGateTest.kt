package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.tuning.Tuning
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import com.watermellonstudios.audio.internal.tuner.FakeTunerBridge
import com.watermellonstudios.audio.internal.tuner.TunerImpl
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * REQ-037 S1 (AC-037.1) — EL CONSUMIDOR MÍNIMO QUE QUIERE MODO RÁPIDO, ESCRITO CONTRA LA PUERTA.
 *
 * El motor tiene modo rápido desde REQ-001 S5: se le declaran las cuerdas del instrumento y él
 * elige y engancha el objetivo solo, con histéresis. Es el camino donde el consumidor mide —sólo
 * ahí una altura sin soporte cae en `NO_LOCK` y no en `NO_SIGNAL` (R-API-49, R-API-52)—.
 *
 * 🔴 LO QUE ESTE TEST MIDE, Y POR QUE NACE ROJO. `ITuner` ya deja **declarar el instrumento**
 * (`TunerFactory.create(configuration)`, y `configuration` es reasignable) y ya deja **enganchar
 * una cuerda a mano** (`selectedString`). Lo que NO deja es lo del medio: pedirle al motor que
 * elija él. Con el instrumento declarado y sin cuerda elegida, hoy el afinador queda **sin
 * objetivo** —`selectedString = null` empuja `0f`— o sea que un consumidor que quiere modo rápido
 * tiene que ir a `ITunerBridge.setTunerCandidates`, y para tener el puente tiene que nombrar
 * `@InternalWatermelonApi`: superficie sin contrato de compatibilidad ni en un patch (R-API-2).
 *
 * Es exactamente lo que R-API-3 describe como "falta una puerta, no un permiso".
 *
 * El doble lo confirma desde el otro lado: `FakeTunerBridge` declara `setTunerCandidates` y
 * `lockTunerString` **fuera de alcance** y explota si alguien las llama, con el comentario de que
 * el día que se cablee el modo rápido "el test grita". Este es ese día.
 */
class FastModeGateTest {

    private fun guitarra() = TuningConfiguration(tuning = Tuning.GUITAR_STANDARD)

    /**
     * AC-037.1 — con el instrumento declarado y sin cuerda elegida, el afinador **no** le ofrece las
     * cuerdas al motor: no hay modo rápido por la puerta pública.
     *
     * 🔴 ESTO ESTÁ ESCRITO COMO TRINQUETE, NO COMO DESEO, y la diferencia importa. Escrito al revés
     * —afirmando que SÍ hay candidatos— nace rojo, y se verificó que nace rojo: falla en el assert
     * de los candidatos con el objetivo empujado en `[0.0]`. Pero un rojo permanente en la suite
     * rompe el gate de cada etapa hasta que S2 exista, así que se deja congelado el estado de HOY:
     * **el día que S2 abra la puerta, este test se pone rojo y hay que darlo vuelta**. Es la misma
     * forma que `kKnownOutsideFineBudget` y los baselines de lint: se declara lo que se sabe, con
     * nombre, y cambiarlo tiene que verse en el diff.
     */
    @Test
    fun `hoy no hay modo rapido por la puerta publica y eso es el hueco de REQ-037`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())

        assertTrue(tuner.start(), "precondicion: el afinador tiene que arrancar")
        assertEquals(6, tuner.targets.size, "precondicion: la guitarra estandar declara seis cuerdas")

        assertTrue(
            bridge.candidatesPushed.isEmpty(),
            "el afinador YA le declara cuerdas al motor (${bridge.candidatesPushed.size} veces): la " +
                "puerta del modo rápido se abrió, así que REQ-037 S2 llegó — dar vuelta este test y " +
                "afirmar que los candidatos son las cuerdas del instrumento",
        )
        // Y no empuja NADA, ni siquiera "sin objetivo": `syncTargetWithEngine` se saltea el empuje
        // cuando el motor ya está en el valor deseado, y sin cuerda elegida ese valor es 0, que es
        // donde el motor arranca. Medido, no supuesto: la primera version de este assert esperaba
        // `[0f]` y la lista estaba vacia.
        assertEquals(
            emptyList(), bridge.pushedHz,
            "sin cuerda elegida el afinador le empujó algo al motor: cambió el comportamiento que " +
                "REQ-037 vino a completar",
        )
        assertEquals(0f, bridge.getTunerTargetHz(), "el motor quedó con un objetivo distinto de 'ninguno'")
    }

    /**
     * El GEMELO, y sin él lo de arriba se satisface declarando candidatos siempre.
     *
     * Cuando el consumidor SÍ elige cuerda, manda su elección: el modo rápido no puede reelegir
     * por él. Hoy esto ya pasa (por `setTunerTargetHz`), y tiene que seguir pasando después de S2.
     */
    @Test
    fun `con una cuerda elegida el objetivo lo manda el consumidor`() {
        val bridge = FakeTunerBridge()
        val tuner: ITuner = TunerImpl(bridge, guitarra())
        tuner.selectedString = 6      // la prima, 1-based como la numera el músico

        val esperado = tuner.targets[5].frequency.hz.toFloat()
        assertEquals(esperado, bridge.getTunerTargetHz(), "no se empujó la cuerda elegida")
    }
}
