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

    /**
     * Bug que atrapa (REQ-050 S3): una fila no aplicable emitida con `ok=true`, o sin la marca que
     * lee el script — se juzgaría como PASS.
     */
    @Test
    fun notApplicableIsOkFalseWithTheMarkAndTheReason() {
        val lines = mutableListOf<String>()
        val returned = SmokeReporter({ lines += it }, run = "r").notApplicable("usb", "suite-3", "no-aplicable", "rate-config" to 88200)
        assertEquals(false, returned)
        assertEquals(
            listOf("HARNESS-SMOKE v=1 run=r panel=usb step=suite-3 ok=false aplica=false motivo=no-aplicable rate-config=88200"),
            lines,
        )
        // Nadie puede escribir la marca a mano con otro valor, ni la vieja de D11.
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "usb", "suite-1", true, listOf("aplica" to true)) }
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "usb", "suite-1", true, listOf("medido" to false)) }
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

    /**
     * REQ-053 S1 (1.3) — la línea `step=precondicion` tiene formato fijo: `id`, `cumplida`,
     * `evidencia`, en ese orden, con `ok` igual a `cumplida`.
     *
     * Bug que atrapa: un `ok` que no copia a `cumplida` (el juez lo da `no-verificable` y bloquea
     * una corrida sana), o las claves con otro nombre (el juez no encuentra la precondición y la
     * da `no-verificable`, nunca cumplida — pero la corrida no se puede juzgar nunca más).
     */
    @Test
    fun aPreconditionLineHasIdMetAndEvidenceAndOkMirrorsMet() {
        val lines = mutableListOf<String>()
        val r = SmokeReporter({ lines += it }, run = "r7")
        assertEquals(false, r.precondition("usb", "permiso-usb", met = false, evidence = "dialogo:denegado"))
        assertEquals(true, r.precondition("captura", "mic-abre", met = true, evidence = "aceptado:true corriendo:true"))
        assertEquals(
            listOf(
                "HARNESS-SMOKE v=1 run=r7 panel=usb step=precondicion ok=false id=permiso-usb cumplida=false evidencia=dialogo:denegado",
                "HARNESS-SMOKE v=1 run=r7 panel=captura step=precondicion ok=true id=mic-abre cumplida=true " +
                    "evidencia=aceptado:true_corriendo:true",
            ),
            lines,
        )
    }

    /**
     * Bug que atrapa: una precondición armada a mano con `format(step = "precondicion")` (sin
     * `cumplida`, o con un `ok` que no la copia), o una línea de la app que dice
     * `verificador=host` y se hace pasar por la verificación del host (el juez la aceptaría como
     * la del verificador declarado en la ficha).
     */
    @Test
    fun aPreconditionCannotBeForgedThroughFormatNorClaimToBeTheHost() {
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "usb", "precondicion", true) }
        assertFailsWith<IllegalArgumentException> {
            HarnessSmoke.format("r", "usb", "conectar", true, listOf("verificador" to "host"))
        }
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.precondition("r", "usb", "Permiso USB", true, "x") }
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.precondition("r", "usb", "", true, "x") }
    }

    /**
     * REQ-053 S3 (D6) — un juicio de sensor lo escribe el SCRIPT en su propio registro, nunca la app.
     *
     * Bug que atrapa: una línea `step=sensor` armada por la app (o por cualquier app con el tag de
     * logcat) que el juez tomaría como "el oyente dijo que sí".
     */
    @Test
    fun aSensorJudgmentCannotBeForgedByTheApp() {
        assertFailsWith<IllegalArgumentException> { HarnessSmoke.format("r", "sf2", "sensor", true) }
        assertFailsWith<IllegalArgumentException> {
            SmokeReporter({}, run = "r").report("sf2", HarnessSmoke.STEP_SENSOR, true, "veredicto" to "presente")
        }
        // El aviso de una ventana SÍ es de la app: no es un veredicto, como esperando-humano.
        assertEquals(
            "HARNESS-SMOKE v=1 run=r panel=sf2 step=escuchar ok=true n=1",
            HarnessSmoke.format("r", "sf2", HarnessSmoke.STEP_LISTEN, true, listOf("n" to 1)),
        )
    }

    /**
     * REQ-053 S3 (AC-053.10) — la semilla llega por el extra `harness.smoke.semilla`.
     *
     * Bug que atrapa: una semilla inválida que cae callada a otra (el orden ya no sale de la que el
     * script registró), o una ausente que se toma como inválida y no deja correr a mano.
     */
    @Test
    fun theSeedIsAnIntegerFromZeroToIntMaxOrAProblem() {
        assertEquals(SeedRequest(12345L, null), SeedRequest.parse("12345"))
        assertEquals(SeedRequest(0L, null), SeedRequest.parse("0"))
        assertEquals(SeedRequest(2147483647L, null), SeedRequest.parse("2147483647"))
        assertEquals(SeedRequest(null, null), SeedRequest.parse(null))
        assertEquals(SeedRequest(null, "semilla-invalida:-1"), SeedRequest.parse("-1"))
        assertEquals(SeedRequest(null, "semilla-invalida:2147483648"), SeedRequest.parse("2147483648"))
        assertEquals(SeedRequest(null, "semilla-invalida:abc"), SeedRequest.parse("abc"))
        assertEquals(SeedRequest(null, "semilla-invalida:-"), SeedRequest.parse(""))
    }

    /** Bug que atrapa: una evidencia vacía parte la línea (`evidencia=` sin valor) en vez de `-`. */
    @Test
    fun aPreconditionWithoutEvidenceSaysDash() {
        assertEquals(
            "HARNESS-SMOKE v=1 run=r panel=usb step=precondicion ok=true id=permiso-usb cumplida=true evidencia=-",
            HarnessSmoke.precondition("r", "usb", "permiso-usb", true, ""),
        )
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
