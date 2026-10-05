package com.watermellonstudios.audio.harness.smoke

import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-053 S1 (1.7) — lo que la app afirma de sus precondiciones. El juez (`smoke-device.sh`)
 * decide BLOQUEADO con esto, así que una precondición que la app da por cumplida sin estarlo
 * esconde un setup roto, y una que da por incumplida sin estarlo bloquea una corrida sana.
 */
class SmokePreconditionsTest {

    /** Bug que atrapa: dar el mic por abierto porque `start()` aceptó, con el stream sin correr. */
    @Test
    fun theMicOpensOnlyIfTheStartWasAcceptedAndTheStreamIsRunning() {
        assertTrue(micOpens(accepted = true, running = true, waitedMs = 50).met)
        assertFalse(micOpens(accepted = true, running = false, waitedMs = 3000).met)
        assertFalse(micOpens(accepted = false, running = false, waitedMs = 0).met)
        assertEquals(
            "aceptado:true,corriendo:false,espera-ms:3000",
            micOpens(accepted = true, running = false, waitedMs = 3000).evidence,
        )
    }

    /** Un reloj falso: la ventana que queda baja con cada pausa. */
    private class Window(var remainingMs: Long) {
        var paused = 0L
        suspend fun pause(ms: Long) {
            paused += ms
            remainingMs -= ms
        }
    }

    /** D13: el humano aceptó → cumplida, y sin gastar la ventana. */
    @Test
    fun aGrantedPermissionIsMetAtOnce() = runTest {
        val w = Window(120_000)
        val reading = assertNotNull(usbPermissionAfterDialog(DialogOutcome.OTHER, { true }, { w.remainingMs }, w::pause))
        assertTrue(reading.met)
        assertEquals("dialogo:concedido", reading.evidence)
        assertEquals(0, w.paused)
    }

    /**
     * D13: el humano negó → incumplida (el juez la da BLOQUEADO), y recién al cerrar la ventana.
     *
     * Bug que atrapa: decidir "negado" en el instante del resultado. Con el diálogo todavía
     * abierto, un broadcast ajeno que abortó la espera (MINI-040) se leería como un humano que
     * negó, y el FAIL de `permiso-falso` quedaría tapado por un BLOQUEADO para siempre.
     */
    @Test
    fun aDenialIsUnmetOnlyAfterTheWholeWindow() = runTest {
        val w = Window(10_000)
        val reading = assertNotNull(usbPermissionAfterDialog(DialogOutcome.DENIED, { false }, { w.remainingMs }, w::pause))
        assertFalse(reading.met)
        assertEquals("resultado=PERMISSION_DENIED,sin-permiso-al-cerrar-la-ventana", reading.evidence)
        assertEquals(10_000, w.paused)
    }

    /**
     * El resultado dijo "negado" pero el humano aceptó después, dentro de la ventana: el permiso
     * está, la precondición se cumple — y por eso `permiso-falso` se juzga y sale FAIL.
     */
    @Test
    fun aDenialFollowedByAGrantInsideTheWindowIsMet() = runTest {
        val w = Window(10_000)
        var reads = 0
        val reading = assertNotNull(
            usbPermissionAfterDialog(DialogOutcome.DENIED, { reads++ >= 3 }, { w.remainingMs }, w::pause),
        )
        assertTrue(reading.met)
        assertEquals("concedido-despues-del-resultado", reading.evidence)
        assertTrue(w.paused in 1 until 10_000)
    }

    /**
     * D13: la ventana venció sin respuesta → SIN línea. El juez lo da HUMANO, como hoy (M4).
     *
     * Bug que atrapa: emitir `cumplida=false` al vencer la ventana. El juez lo leería como una
     * negación y daría BLOQUEADO (exit 4) a un humano que no actuó, que D13 deja en HUMANO.
     */
    @Test
    fun noAnswerInTheWindowEmitsNothing() = runTest {
        val w = Window(0)
        assertNull(usbPermissionAfterDialog(DialogOutcome.NO_ANSWER, { false }, { w.remainingMs }, w::pause))
        assertEquals(0, w.paused)
    }

    /** Sin respuesta de `connectDevice`, pero UsbManager dice que hay permiso: se afirma lo que se ve. */
    @Test
    fun noAnswerButThePermissionIsThereIsMet() = runTest {
        val w = Window(0)
        val reading = assertNotNull(usbPermissionAfterDialog(DialogOutcome.NO_ANSWER, { true }, { w.remainingMs }, w::pause))
        assertTrue(reading.met)
        assertEquals("concedido-sin-respuesta-de-connect", reading.evidence)
    }

    /** Bug que atrapa: una espera que no respeta el techo (un `while (true)` hasta que aparezca). */
    @Test
    fun theWaitAfterADenialNeverExceedsTheWindow() = runTest {
        val w = Window(1_250)
        usbPermissionAfterDialog(DialogOutcome.DENIED, { false }, { w.remainingMs }, w::pause)
        assertEquals(1_250, w.paused)
    }

    /**
     * D13: sólo un PERMISSION_DENIED es una negación. Otro fallo sin permiso al cerrar la ventana
     * no muestra que el humano haya negado: no hay línea, y el juez lo da HUMANO.
     *
     * Bug que atrapa: afirmar "negado" de cualquier fallo de `connectDevice` (un grant falso que
     * terminó en SecurityException se leería como un humano que negó, y se bloquearía).
     */
    @Test
    fun aFailureThatIsNotADenialWithoutPermissionEmitsNothing() = runTest {
        val w = Window(2_000)
        assertNull(usbPermissionAfterDialog(DialogOutcome.OTHER, { false }, { w.remainingMs }, w::pause))
        assertEquals(2_000, w.paused)
    }

    /**
     * `permiso-falso` es CONCLUYENTE (el juez no lo puede bloquear) cuando la regresión de REQ-050
     * está probada sin importar lo que haga el humano: un grant que UsbManager desmiente, o un
     * PERMISSION_DENIED con el permiso concedido al cerrar la ventana.
     *
     * Bug que atrapa: marcar concluyente la negación ambigua (una negación humana de verdad
     * saldría FAIL en vez de BLOQUEADO, contra D13), o no marcar el grant falso (un FAIL probado
     * quedaría tapado por el BLOQUEADO del permiso negado).
     */
    @Test
    fun theForgedCheckIsConclusiveOnlyWhenTheHumanCannotExplainIt() {
        val forgedGrant = forgedPermissionCheck(forgedGrants = 1, denied = false, permissionAtWindowClose = false)
        assertFalse(forgedGrant.ok)
        assertTrue(forgedGrant.conclusive)
        assertEquals("granted-con-usbmanager-diciendo-que-no", forgedGrant.reason)

        val deniedThenGranted = forgedPermissionCheck(forgedGrants = 0, denied = true, permissionAtWindowClose = true)
        assertFalse(deniedThenGranted.ok)
        assertTrue(deniedThenGranted.conclusive)
        assertEquals("negado-con-el-permiso-concedido:broadcast-ajeno", deniedThenGranted.reason)

        val ambiguous = forgedPermissionCheck(forgedGrants = 0, denied = true, permissionAtWindowClose = false)
        assertFalse(ambiguous.ok)
        assertFalse(ambiguous.conclusive)
        assertEquals("negado:humano-o-broadcast-ajeno", ambiguous.reason)

        val clean = forgedPermissionCheck(forgedGrants = 0, denied = false, permissionAtWindowClose = true)
        assertTrue(clean.ok)
        assertFalse(clean.conclusive)
        assertNull(clean.reason)
    }
}
