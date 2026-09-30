package com.watermellonstudios.audio.harness.smoke

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * MINI-038, tarea 5 — FIJA el formato de las líneas `HARNESS-SMOKE` que lee
 * `scripts/smoke-device.sh`. Si este test cambia, el script (y su log grabado del `--self-test`)
 * cambia con él: son las dos puntas del mismo contrato.
 */
class HarnessSmokeTest {

    /** Bug que atrapa: reordenar, renombrar o sacar uno de los cinco campos fijos rompe al script. */
    @Test
    fun theLineHasTheFixedFieldsInOrderThenTheExtras() {
        val line = HarnessSmoke.format(
            run = "r1", panel = "sf2", step = "carga", ok = true,
            fields = listOf("presets" to 1, "archivo" to "wma-fixture.sf2"),
        )
        assertEquals(
            "HARNESS-SMOKE v=1 run=r1 panel=sf2 step=carga ok=true presets=1 archivo=wma-fixture.sf2",
            line,
        )
    }

    /** Bug que atrapa: un valor con espacios parte la línea y el script lee claves que no existen. */
    @Test
    fun valuesNeverContainBlanksAndNeverAreEmpty() {
        val line = HarnessSmoke.format(
            "r1", "usb", "conectar", false,
            listOf("mensaje" to "Failed to open\tUSB device", "vacio" to "", "nulo" to null),
        )
        assertEquals(
            "HARNESS-SMOKE v=1 run=r1 panel=usb step=conectar ok=false " +
                "mensaje=Failed_to_open_USB_device vacio=- nulo=-",
            line,
        )
        assertEquals(1 + 5 + 3, line.split(' ').size)
    }

    /** Bug que atrapa: una clave con `=` o espacios, o que pisa `ok`, hace ambigua la línea. */
    @Test
    fun keysThatWouldMakeTheLineAmbiguousAreRejected() {
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "sf2", "carga", true, listOf("a b" to 1)) }
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "sf2", "carga", true, listOf("a=b" to 1)) }
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "sf2", "carga", true, listOf("ok" to true)) }
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "sf2", "carga", true, listOf("Presets" to 1)) }
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "SF 2", "carga", true) }
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "sf2", "", true) }
    }

    /** Bug que atrapa: el paso humano con otro nombre, o sin la acción, deja al script sin el HUMANO. */
    @Test
    fun waitingForHumanIsItsOwnStepAndCarriesTheAction() {
        val lines = mutableListOf<String>()
        SmokeReporter({ lines += it }, run = "r9").waitingForHuman("usb", "aceptar el dialogo", "dispositivo" to "2b89:64ec")
        assertEquals(
            listOf(
                "HARNESS-SMOKE v=1 run=r9 panel=usb step=esperando-humano ok=false " +
                    "accion=aceptar_el_dialogo dispositivo=2b89:64ec",
            ),
            lines,
        )
    }

    /** Bug que atrapa: un reporter que emite un `ok` y le devuelve otro a quien decide el paso siguiente. */
    @Test
    fun reportReturnsTheVerdictItEmitted() {
        val lines = mutableListOf<String>()
        val r = SmokeReporter({ lines += it }, run = "r")
        assertEquals(false, r.report("sf3", "nota", false))
        assertEquals(true, r.report("sf3", "nota", true))
        assertTrue(lines[0].contains(" ok=false") && lines[1].contains(" ok=true"))
    }

    @Test
    fun theWholePlanRunsInTheCanonicalOrder() {
        val plan = assertIs<SmokePlan.Valid>(SmokePlan.parse("todo"))
        assertEquals(listOf("salida", "captura", "sf2", "sf3", "usb"), plan.panels.map { it.id })
    }

    /** Bug que atrapa: correr `usb` (que espera a un humano) antes que los automáticos. */
    @Test
    fun orderComesFromThePanelsNotFromTheText() {
        val plan = assertIs<SmokePlan.Valid>(SmokePlan.parse("usb, sf3,salida,sf3"))
        assertEquals(listOf(Panel.SALIDA, Panel.SF3, Panel.USB), plan.panels)
    }

    /** Bug que atrapa: un plan mal escrito que corre "lo que entendió" y da verde con menos pasos. */
    @Test
    fun anUnknownOrEmptyPlanIsInvalidNotGuessed() {
        assertEquals(SmokePlan.Invalid("panel-desconocido:sf4"), SmokePlan.parse("sf2,sf4"))
        assertEquals(SmokePlan.Invalid("panel-desconocido:"), SmokePlan.parse("sf2,,sf3"))
        assertEquals(SmokePlan.Invalid("plan-vacio"), SmokePlan.parse("  "))
        assertEquals(SmokePlan.Invalid("plan-vacio"), SmokePlan.parse(null))
    }
}
