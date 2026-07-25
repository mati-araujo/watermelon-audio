package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.callback.AudioLogger
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

/**
 * WA-1.4 — contrato de [BridgeConcurrency].
 *
 * Vive en commonTest a propósito: corre en JVM **y** bajo Kotlin/Native en el
 * simulador, así que verifica la primitiva en las dos plataformas que la van a usar
 * (`AudioNativeBridge` e `IosAudioBridge`), no en una sola.
 */
class BridgeConcurrencyTest {

    @Test
    fun sameCategorySerializes() = runTest {
        val concurrency = BridgeConcurrency(StandardTestDispatcher(testScheduler))
        val events = mutableListOf<String>()

        val slow = launch {
            concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "slow") {
                events += "slow-in"
                delay(100)
                events += "slow-out"
                Result.success(Unit)
            }
        }
        val fast = launch {
            concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "fast") {
                events += "fast-in"
                events += "fast-out"
                Result.success(Unit)
            }
        }
        slow.join()
        fast.join()

        // La rápida no puede entrar hasta que la lenta suelte el mutex.
        assertEquals(listOf("slow-in", "slow-out", "fast-in", "fast-out"), events)
    }

    /** El motivo de tener cuatro mutexes y no uno: no se bloquean entre sí. */
    @Test
    fun differentCategoriesDoNotBlockEachOther() = runTest {
        val concurrency = BridgeConcurrency(StandardTestDispatcher(testScheduler))
        val events = mutableListOf<String>()

        val effects = launch {
            concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "effects") {
                events += "effects-in"
                delay(100)
                events += "effects-out"
                Result.success(Unit)
            }
        }
        val mode = launch {
            concurrency.guarded(BridgeConcurrency.Category.MODE, "mode") {
                events += "mode-in"
                delay(10)
                events += "mode-out"
                Result.success(Unit)
            }
        }
        effects.join()
        mode.join()

        // MODE termina primero pese a haber arrancado después: EFFECTS no lo frena.
        assertEquals(listOf("effects-in", "mode-in", "mode-out", "effects-out"), events)
    }

    /**
     * **La regresión que WA-1.4 existe para evitar.**
     *
     * `CancellationException` es una `Exception`, así que un `catch (e: Exception)`
     * alrededor de código de corrutinas se la traga y la devuelve como
     * `Result.failure`. El scope padre nunca se entera de que fue cancelado. Los 22
     * bloques `catch (e: Exception)` de `AudioNativeBridge` hacían exactamente eso
     * antes de este refactor.
     */
    @Test
    fun cancellationPropagatesInsteadOfBecomingAFailure() = runTest {
        val concurrency = BridgeConcurrency(StandardTestDispatcher(testScheduler))

        assertFailsWith<CancellationException> {
            concurrency.guarded<Unit>(BridgeConcurrency.Category.LIFECYCLE, "cancelada") {
                throw CancellationException("cancelada por el scope padre")
            }
        }
    }

    @Test
    fun otherExceptionsBecomeFailures() = runTest {
        val concurrency = BridgeConcurrency(StandardTestDispatcher(testScheduler))

        val result = concurrency.guarded<Unit>(BridgeConcurrency.Category.EFFECTS, "explota") {
            throw IllegalStateException("boom")
        }

        assertTrue(result.isFailure)
        assertEquals("boom", result.exceptionOrNull()?.message)
    }

    /** Un `Result.failure` devuelto por el bloque pasa tal cual, sin re-envolver. */
    @Test
    fun failureFromTheBlockPassesThroughUnchanged() = runTest {
        val concurrency = BridgeConcurrency(StandardTestDispatcher(testScheduler))
        val cause = IllegalArgumentException("índice inválido")

        val result = concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "falla") {
            Result.failure<Unit>(cause)
        }

        assertTrue(result.isFailure)
        assertEquals(cause, result.exceptionOrNull())
    }

    /**
     * Si una excepción dejara el mutex tomado, la categoría entera quedaría muerta
     * para siempre. Es el modo de falla más caro de este archivo.
     */
    @Test
    fun lockIsReleasedAfterAFailure() = runTest {
        val concurrency = BridgeConcurrency(StandardTestDispatcher(testScheduler))

        concurrency.guarded<Unit>(BridgeConcurrency.Category.EFFECTS, "explota") {
            throw IllegalStateException("boom")
        }
        val afterwards = concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "despues") {
            Result.success(42)
        }

        assertEquals(42, afterwards.getOrNull())
    }

    @Test
    fun unhandledExceptionsAreReportedToTheLogger() = runTest {
        val logged = mutableListOf<Pair<String, Throwable?>>()
        val logger = object : AudioLogger {
            override fun debug(tag: String, message: String, params: Map<String, Any>) {}
            override fun info(tag: String, message: String, params: Map<String, Any>) {}
            override fun warn(tag: String, message: String, params: Map<String, Any>) {}
            override fun error(
                tag: String,
                message: String,
                throwable: Throwable?,
                params: Map<String, Any>,
            ) {
                logged += message to throwable
            }
        }
        val concurrency = BridgeConcurrency(StandardTestDispatcher(testScheduler), logger)

        concurrency.guarded<Unit>(BridgeConcurrency.Category.INPUT, "startInputStream") {
            throw IllegalStateException("boom")
        }

        assertEquals(1, logged.size)
        assertTrue(
            logged[0].first.startsWith("startInputStream"),
            "el log debería nombrar la operación: '${logged[0].first}'",
        )
        assertEquals("boom", logged[0].second?.message)
    }

    /** Una operación cancelada no debe quedarse con el mutex. */
    @Test
    fun lockIsReleasedAfterCancellation() = runTest {
        val concurrency = BridgeConcurrency(StandardTestDispatcher(testScheduler))

        assertFailsWith<CancellationException> {
            concurrency.guarded<Unit>(BridgeConcurrency.Category.LIFECYCLE, "cancelada") {
                throw CancellationException("cancelada")
            }
        }
        val afterwards = concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "despues") {
            Result.success("ok")
        }

        assertEquals("ok", afterwards.getOrNull())
    }
}
