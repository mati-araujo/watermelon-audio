package com.watermellonstudios.audio.domain.error

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * WA-1.5 — el mapeo código nativo → error, que es la otra mitad sin cubrir.
 *
 * Es lógica pura (no necesita puente ni plataforma) y **ya albergó un bug de esta
 * familia**: una `wma_*` que devolvía `void` no podía transportar un rechazo, así que
 * iOS reportaba `success` habiendo hecho cero. El contrato de qué entero significa
 * éxito y qué excepción sale de cada código no estaba pinchado en ningún lado.
 *
 * Los dos asertos que importan y que no son obvios:
 *
 *  - **`isSuccessCode` es `code >= 0`, no `code == 0`.** Varias funciones devuelven un
 *    índice o una cantidad como valor útil, así que tratar 1 como error rompería el
 *    camino feliz de todas ellas.
 *  - **Un código desconocido NO explota: cae en `UNKNOWN_ERROR` / `NativeError`.** El
 *    C++ puede crecer un código nuevo antes que este enum, y la capa Kotlin tiene que
 *    degradar, no tirar.
 */
class NativeErrorMappingTest {

    @Test
    fun everyDeclaredCodeRoundTripsThroughFromCode() {
        // Si alguien agrega una entrada con un código duplicado, `fromCode` devolvería
        // la primera y esta ida y vuelta lo destapa. Cubre el enum entero, así que una
        // entrada nueva queda cubierta sin tocar el test.
        for (entry in NativeErrorCode.entries) {
            assertEquals(entry, NativeErrorCode.fromCode(entry.code), "round-trip de $entry")
        }
    }

    @Test
    fun successIsNonNegativeAndNotJustZero() {
        // El límite exacto. Varias funciones nativas devuelven un índice como valor
        // útil: si el criterio fuera `== 0`, un índice 1 se leería como error.
        assertTrue(NativeErrorCode.isSuccessCode(0))
        assertTrue(NativeErrorCode.isSuccessCode(1))
        assertTrue(NativeErrorCode.isSuccessCode(Int.MAX_VALUE))
        assertFalse(NativeErrorCode.isSuccessCode(-1))
        assertFalse(NativeErrorCode.isSuccessCode(Int.MIN_VALUE))

        assertTrue(NativeErrorCode.SUCCESS.isSuccess)
        assertFalse(NativeErrorCode.SUCCESS.isError)
        assertTrue(NativeErrorCode.STREAM_ERROR.isError)
        assertFalse(NativeErrorCode.STREAM_ERROR.isSuccess)
    }

    @Test
    fun anUnknownCodeDegradesInsteadOfThrowing() {
        // El C++ puede agregar un código antes que este enum. Degradar es el
        // comportamiento correcto; tirar dejaría al consumidor sin camino.
        assertEquals(NativeErrorCode.UNKNOWN_ERROR, NativeErrorCode.fromCode(-12345))
        assertEquals(NativeErrorCode.UNKNOWN_ERROR, NativeErrorCode.fromCode(Int.MIN_VALUE))

        val e = NativeBridgeException.fromCode(-12345, context = "loopear")
        val unknown: NativeBridgeException.NativeError = assertIs(e)
        assertEquals(-12345, unknown.nativeCode)
        assertEquals(NativeErrorCode.UNKNOWN_ERROR, unknown.errorCode)
    }

    @Test
    fun eachErrorCodeMapsToItsOwnExceptionType() {
        // El mapeo entero, para que agregar un código y olvidarse del `when` se note.
        // Es lo que separa "hay una excepción" de "hay LA excepción": un consumidor que
        // distingue micrófono denegado de cadena llena necesita el tipo, no el mensaje.
        assertIs<NativeBridgeException.EngineNotInitialized>(NativeBridgeException.fromCode(-1))
        assertIs<NativeBridgeException.InvalidEffectIndex>(NativeBridgeException.fromCode(-2))
        assertIs<NativeBridgeException.InvalidParameterId>(NativeBridgeException.fromCode(-3))
        assertIs<NativeBridgeException.ParameterOutOfRange>(NativeBridgeException.fromCode(-4))
        assertIs<NativeBridgeException.EffectChainFull>(NativeBridgeException.fromCode(-5))
        assertIs<NativeBridgeException.MemoryAllocationFailed>(NativeBridgeException.fromCode(-6))
        assertIs<NativeBridgeException.StreamError>(NativeBridgeException.fromCode(-7))
        assertIs<NativeBridgeException.ModeTransitionInProgress>(NativeBridgeException.fromCode(-8))
        assertIs<NativeBridgeException.InvalidOperation>(NativeBridgeException.fromCode(-9))
        assertIs<NativeBridgeException.InvalidEffectType>(NativeBridgeException.fromCode(-10))
        assertIs<NativeBridgeException.Timeout>(NativeBridgeException.fromCode(-11))
    }

    @Test
    fun aSuccessCodeDoesNotProduceASilentGenericFailure() {
        // `fromCode(0)` cae en el `else`. No es un bug —nadie debería pedir la
        // excepción de un éxito— pero SÍ es una trampa: el `else` la convierte en
        // `NativeError(0)`, que se lee como "falló con código 0". Queda pinchado para
        // que el día que alguien lo trate como error, el comportamiento esté escrito y
        // no sea una sorpresa.
        val e = NativeBridgeException.fromCode(NativeErrorCode.SUCCESS.code)
        val generic: NativeBridgeException.NativeError = assertIs(e)
        assertEquals(0, generic.nativeCode)
        assertEquals(NativeErrorCode.SUCCESS, generic.errorCode)
        assertTrue(NativeErrorCode.isSuccessCode(generic.nativeCode),
            "el codigo sigue diciendo exito aunque venga envuelto en una excepcion")
    }
}
