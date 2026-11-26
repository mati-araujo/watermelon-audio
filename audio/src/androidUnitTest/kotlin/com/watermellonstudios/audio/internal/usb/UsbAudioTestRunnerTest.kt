package com.watermellonstudios.audio.internal.usb

import com.watermellonstudios.audio.api.IUsbAudioManager
import com.watermellonstudios.audio.domain.usb.AltsettingInfo
import com.watermellonstudios.audio.domain.usb.AudioFormatInfo
import com.watermellonstudios.audio.domain.usb.ClockSourceInfo
import com.watermellonstudios.audio.domain.usb.ClockSourceType
import com.watermellonstudios.audio.domain.usb.IRoundTripLatencyTester
import com.watermellonstudios.audio.domain.usb.RoundTripTestConfig
import com.watermellonstudios.audio.domain.usb.RoundTripTestProgress
import com.watermellonstudios.audio.domain.usb.RoundTripTestResult
import com.watermellonstudios.audio.domain.usb.ScoredAltsetting
import com.watermellonstudios.audio.domain.usb.StreamPreference
import com.watermellonstudios.audio.domain.usb.UsbAudioCapabilities
import com.watermellonstudios.audio.domain.usb.UsbAudioDevice
import com.watermellonstudios.audio.domain.usb.UsbAudioError
import com.watermellonstudios.audio.domain.usb.UsbCapabilitySnapshot
import com.watermellonstudios.audio.domain.usb.UsbConnectionState
import com.watermellonstudios.audio.domain.usb.UsbDeviceEvent
import com.watermellonstudios.audio.domain.usb.UsbHealthEvent
import com.watermellonstudios.audio.domain.usb.UsbLatencyProfile
import com.watermellonstudios.audio.domain.usb.UsbResult
import com.watermellonstudios.audio.domain.usb.UsbStreamRestore
import com.watermellonstudios.audio.domain.usb.UsbStreamingMode
import com.watermellonstudios.audio.domain.usb.UsbSyncMode
import com.watermellonstudios.audio.domain.usb.UsbTestConfig
import com.watermellonstudios.audio.domain.usb.UsbTestPresets
import com.watermellonstudios.audio.domain.usb.UsbTestResult
import com.watermellonstudios.audio.domain.usb.UsbTestStatus
import com.watermellonstudios.audio.domain.usb.UsbTestType
import com.watermellonstudios.audio.domain.usb.UsbTransferStats
import com.watermellonstudios.audio.domain.usb.UsbVolumeCapabilities
import com.watermellonstudios.audio.domain.usb.UsbVolumeState
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

/**
 * REQ-050 S3 — **el runner USB mide lo que declara** (AC-050.8, AC-050.9; D16–D19).
 *
 * El doble de [IUsbAudioManager] es una CM720 del g42 (UAC2, tres altsettings 2ch de 16/24/32
 * bits sin rates propios, un reloj de 44,1 a 384 kHz) con el stream abierto como lo abre el
 * harness: 48 kHz, 24 bits pedidos, sin selección manual. Cada lectura de stats avanza el
 * stream un paso de tráfico perfecto: 192 paquetes en vuelo, 37,5 ms de latencia y un techo
 * declarado de 61,3 ms (los números medidos en `smoke-20261001-154601-71029`).
 *
 * El reloj del runner es el del scheduler de test: la ventana de medición corre en tiempo
 * virtual, no se duerme nada.
 */
class UsbAudioTestRunnerTest {

    private val cm720 = UsbCapabilitySnapshot(
        vendorId = 0x2B89, productId = 0x64EC, productName = "CM720", manufacturer = "UGREEN",
        serialNumber = "", uacVersion = 2,
        playbackAltsettings = listOf(16, 24, 32).mapIndexed { i, bits ->
            AltsettingInfo(
                interfaceNumber = 1, alternateSetting = i + 1,
                formats = listOf(AudioFormatInfo(2, bits, bits / 8, emptyList(), false, 0, 0)),
                syncType = UsbSyncMode.ADAPTIVE, hasFeedbackEndpoint = false, hasImplicitFeedback = false,
                dataEndpointAddress = 0x01, terminalLinkId = 2,
            )
        },
        captureAltsettings = emptyList(),
        clockSources = listOf(
            ClockSourceInfo(
                clockId = CLOCK, type = ClockSourceType.INTERNAL_FIXED, syncedToSof = false,
                hasFrequencyControl = true, hasValidityControl = false,
                sampleRates = listOf(44100, 48000, 96000, 192000, 384000),
            )
        ),
        featureUnits = emptyList(),
    )

    private companion object {
        const val CLOCK = 9
        const val IN_FLIGHT = 192L
    }

    // ==================== AC-050.8 — la fila se mide a su config, o no aplica ====================

    /**
     * AC-050.8 (D17, D18). Una fila de 96 kHz/24 bits sobre el stream de 48 kHz: el runner lo para,
     * elige el altsetting de 24 bits y el reloj que ofrece 96 k, lo reabre a 96 k, mide, y
     * después devuelve el stream del consumidor a como estaba (selección automática y 48 k).
     *
     * Bug que atrapa: MINI-039 — `runPlaybackTest` ignoraba `config.sampleRate` y medía el stream
     * de 48 k para la fila de 96 k.
     */
    @Test
    fun `AC-050_8 una fila a otro rate reabre el stream a su config, mide y lo restaura`() = runRig { rig ->
        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
        assertEquals(96000, res.streamSampleRateHz, "la fila no se midió al rate que declara")
        assertEquals(
            listOf(
                "stop", "alt(1,2,0)", "clock($CLOCK)", "start(96000,2,24,PLAYBACK_ONLY)",
                "stop", "alt(-1,-1,-1)", "clock(0)", "start(48000,2,24,PLAYBACK_ONLY)",
            ),
            rig.usb.log,
        )
        assertEquals(UsbStreamRestore.RESTORED, res.streamRestore, "restauración: ${res.streamRestoreMessage}")
        assertEquals(48000, rig.usb.active?.sampleRate, "el stream del consumidor no volvió a 48 k")
        assertEquals(null, rig.usb.active?.altsetting, "quedó una selección manual pegada al consumidor")
    }

    /**
     * AC-050.8 (D17). La fila de 48 kHz/16 bits sobre el stream de 48 kHz SIN selección manual: los
     * bits del stream no se conocen (el nativo elige el formato por puntaje), así que también se
     * reabre, con el formato de 16 bits.
     *
     * Bug que atrapa: comparar sólo el rate y medir la fila de 16 bits sobre el formato que el
     * puntaje haya elegido (32 bits en la CM720).
     */
    @Test
    fun `AC-050_8 la fila al mismo rate con otros bits tambien reabre`() = runRig { rig ->
        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 48000, bitDepth = 16, durationMs = 2000))

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
        assertTrue("alt(1,1,0)" in rig.usb.log, "no se seleccionó el formato de 16 bits: ${rig.usb.log}")
        assertEquals(UsbStreamRestore.RESTORED, res.streamRestore)
    }

    /**
     * D18, el gemelo: si el stream ya corre exactamente a la config de la fila (misma selección
     * manual), se mide como está — sin cortar el audio del consumidor.
     */
    @Test
    fun `D18 una fila que coincide con el stream no lo toca`() = runRig(
        initial = UsbActiveStreamConfig(48000, 2, 16, UsbStreamingMode.PLAYBACK_ONLY, UsbAltsettingSelection(1, 1, 0), CLOCK),
    ) { rig ->
        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 48000, bitDepth = 16, durationMs = 2000))

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
        assertEquals(emptyList(), rig.usb.log, "se tocó un stream que ya estaba a la config de la fila")
        assertEquals(UsbStreamRestore.NOT_NEEDED, res.streamRestore)
    }

    /**
     * D18. La restauración devuelve también la selección MANUAL que tenía el consumidor.
     *
     * Bug que atrapa: restaurar siempre a "automático" y pisarle al consumidor el altsetting que
     * había elegido.
     */
    @Test
    fun `D18 la restauracion devuelve la seleccion manual del consumidor`() = runRig(
        initial = UsbActiveStreamConfig(48000, 2, 32, UsbStreamingMode.PLAYBACK_ONLY, UsbAltsettingSelection(1, 3, 0), CLOCK),
    ) { rig ->
        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 44100, bitDepth = 16, durationMs = 2000))

        assertEquals(UsbStreamRestore.RESTORED, res.streamRestore)
        assertEquals(
            listOf("stop", "alt(1,3,0)", "clock($CLOCK)", "start(48000,2,32,PLAYBACK_ONLY)"),
            rig.usb.log.takeLast(4),
        )
    }

    /** D18. Si el stream no se puede volver a abrir, la fila lo DICE en su resultado. */
    @Test
    fun `D18 una restauracion que falla queda en el resultado de la fila`() = runRig { rig ->
        rig.usb.failStartsAfter = 1

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbStreamRestore.FAILED, res.streamRestore)
        assertTrue(res.streamRestoreMessage!!.contains("48000"), "mensaje: ${res.streamRestoreMessage}")
        // Una fila que dejó el stream del consumidor roto no puede contar como pasada.
        assertEquals(UsbTestStatus.FAILED, res.status, "mensaje: ${res.errorMessage}")
    }

    /**
     * D18 (review de S3, M2): si falla restaurar el altsetting, se restaura igual el reloj y se
     * reintenta el arranque. Cortar en el primer error dejaba el reloj de la fila pegado.
     */
    @Test
    fun `D18 la restauracion sigue aunque falle un paso y los nombra`() = runRig { rig ->
        rig.usb.failAutomaticAltsetting = true

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbStreamRestore.FAILED, res.streamRestore)
        assertEquals(listOf("stop", "alt(-1,-1,-1)", "clock(0)", "start(48000,2,24,PLAYBACK_ONLY)"), rig.usb.log.takeLast(4))
        assertTrue(res.streamRestoreMessage!!.contains("altsetting"), "mensaje: ${res.streamRestoreMessage}")
    }

    /**
     * D18 (review de S3, I2). Sin stream (el barrido admite CONNECTED), la restauración devuelve la
     * selección manual que el consumidor tenía pendiente para su próximo arranque, y no arranca
     * nada.
     *
     * Bug que atrapa: restaurar siempre a "automático" cuando no había stream.
     */
    @Test
    fun `D18 sin stream previo se devuelve la seleccion manual pendiente`() = runRig(
        initial = null, pending = UsbManualSelection(UsbAltsettingSelection(1, 3, 0), CLOCK),
    ) { rig ->
        val res = rig.runner.runTest(UsbTestPresets.RATE_NEGOTIATION_SWEEP.first { it.sampleRate == 96000 })

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
        assertEquals(UsbStreamRestore.RESTORED, res.streamRestore)
        assertEquals(listOf("stop", "alt(1,3,0)", "clock($CLOCK)"), rig.usb.log.takeLast(3))
        assertEquals(UsbConnectionState.CONNECTED, rig.usb.connectionState.value, "se dejó un stream que no estaba")
    }

    /**
     * D18 (review de S3, R3). Un stream con selección manual al mismo rate pero OTROS bits no
     * coincide con la fila: se reabre.
     *
     * Bug que atrapa: comparar el rate y el reloj y no el altsetting (los bits).
     */
    @Test
    fun `D18 con seleccion manual de otros bits al mismo rate se reabre`() = runRig(
        initial = UsbActiveStreamConfig(48000, 2, 32, UsbStreamingMode.PLAYBACK_ONLY, UsbAltsettingSelection(1, 3, 0), CLOCK),
    ) { rig ->
        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 48000, bitDepth = 16, durationMs = 2000))

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
        assertTrue("alt(1,1,0)" in rig.usb.log, "no se reabrió con el formato de 16 bits: ${rig.usb.log}")
    }

    /**
     * D18 (review de S3, I6). Con un manager que no es el de la librería, el runner no sabe cómo
     * está configurado el stream y no lo podría devolver: no lo toca, y la fila no pasa.
     */
    @Test
    fun `D18 con un manager ajeno el runner no toca el stream`() = runTest {
        val inner = FakeRunnerUsbManager(cm720, UsbActiveStreamConfig(48000, 2, 24, UsbStreamingMode.PLAYBACK_ONLY, null, null), UsbManualSelection(null, null))
        val foreign: IUsbAudioManager = object : IUsbAudioManager by inner {}
        val runner = UsbAudioTestRunner(foreign, this, NoRoundTrip, { inner.streamRate }, { testScheduler.currentTime })

        val res = runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertEquals(emptyList(), inner.log, "se tocó un stream que no se puede restaurar")
    }

    /**
     * AC-050.8 (D5, D19). Una fila que los descriptores no ofrecen (88,2 kHz no está en el reloj)
     * es NOT_APPLICABLE, nunca PASSED, y el stream del consumidor no se toca.
     */
    @Test
    fun `AC-050_8 una fila que el device no ofrece es NOT_APPLICABLE y no toca el stream`() = runRig { rig ->
        val rate = rig.runner.runTest(UsbTestConfig(sampleRate = 88200, bitDepth = 24, durationMs = 2000))
        val bits = rig.runner.runTest(UsbTestConfig(sampleRate = 48000, bitDepth = 20, durationMs = 2000))

        assertEquals(UsbTestStatus.NOT_APPLICABLE, rate.status)
        assertEquals(UsbTestStatus.NOT_APPLICABLE, bits.status)
        assertFalse(rate.passed || bits.passed, "un no-aplicable se leyó como pasado")
        assertEquals(emptyList(), rig.usb.log, "se tocó el stream por una fila no aplicable")
    }

    /**
     * D19. El device ofrecía 96 kHz y el stream quedó en 48 kHz (coerción): FAILED con los dos
     * números, no NOT_APPLICABLE ni PASSED.
     *
     * Bug que atrapa: confiar en que `startStreaming` devolvió éxito. El nativo reintenta al rate
     * coercionado y arranca OK.
     */
    @Test
    fun `D19 un rate coercionado es FAILED con lo que ofrecia y lo que quedo`() = runRig { rig ->
        rig.usb.coerce[96000] = 48000

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertTrue(
            res.errorMessage!!.contains("96000") && res.errorMessage!!.contains("48000"),
            "mensaje: ${res.errorMessage}",
        )
        assertEquals(UsbStreamRestore.RESTORED, res.streamRestore, "una fila fallida igual restaura")
    }

    // ==================== AC-050.9 — umbrales que el backend puede cumplir ====================

    /**
     * AC-050.9. Sin tráfico nunca PASSED: el stream "corre" pero los completados no crecen.
     *
     * Bug que atrapa: MINI-039 — `runPlaybackTest` daba PASSED con las stats congeladas, porque
     * sólo miraba latencia y underruns.
     */
    @Test
    fun `AC-050_9 sin trafico la fila nunca pasa`() = runRig { rig ->
        rig.usb.trafficPerPoll = 0

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertTrue(res.errorMessage!!.contains("traffic"), "mensaje: ${res.errorMessage}")
    }

    /**
     * AC-050.9 (review de S3, B1). El stream se traba a mitad de la fila: hubo una subida al
     * principio y después nada. Nunca PASSED.
     *
     * Bug que atrapa: juzgar el tráfico con "último > primero", que una sola subida al principio
     * satisface mientras todo lo demás (underruns, latencia, éxito) queda congelado y limpio.
     */
    @Test
    fun `AC-050_9 un stream que se traba a mitad de la fila no pasa`() = runRig { rig ->
        rig.usb.stallAfterPolls = 5

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 5000))

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertTrue(res.errorMessage!!.contains("stalled"), "mensaje: ${res.errorMessage}")
    }

    /**
     * AC-050.9 (review de S3, B1). El stream deja de reportar stats a mitad de la fila (el chequeo
     * de salud lo sacó de STREAMING): la última muestra buena no puede aprobar la fila.
     */
    @Test
    fun `AC-050_9 un stream que deja de reportar stats a mitad no pasa`() = runRig { rig ->
        rig.usb.statsNullAfterPolls = 5

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 5000))

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertTrue(res.errorMessage!!.contains("stopped reporting"), "mensaje: ${res.errorMessage}")
    }

    /**
     * AC-050.9 (review de S3, I4). Sin reabrir, el stream trae contadores de toda su vida: la fila
     * se juzga por lo que pasó DURANTE ella. Un underrun viejo no la hace fallar.
     */
    @Test
    fun `AC-050_9 la fila se juzga con los contadores de su ventana`() = runRig(
        initial = UsbActiveStreamConfig(48000, 2, 16, UsbStreamingMode.PLAYBACK_ONLY, UsbAltsettingSelection(1, 1, 0), CLOCK),
    ) { rig ->
        rig.usb.historicUnderruns = 5

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 48000, bitDepth = 16, durationMs = 2000))

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
        assertEquals(0L, res.underruns)
    }

    /** AC-050.9, el gemelo: una pérdida DURANTE la ventana, diluida en una vida larga, igual falla. */
    @Test
    fun `AC-050_9 gemelo - una perdida en la ventana no se diluye en la historia`() = runRig(
        initial = UsbActiveStreamConfig(48000, 2, 16, UsbStreamingMode.PLAYBACK_ONLY, UsbAltsettingSelection(1, 1, 0), CLOCK),
    ) { rig ->
        rig.usb.historicCompleted = 30_000_000
        rig.usb.lostPerPoll = 60

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 48000, bitDepth = 16, durationMs = 2000))

        assertEquals(UsbTestStatus.FAILED, res.status)
    }

    /** AC-050.9 (review de S3, I5). Los presets públicos ya no traen un techo fijo que falla por construcción. */
    @Test
    fun `AC-050_9 el preset HIGH_QUALITY se juzga contra el techo del backend`() = runRig { rig ->
        val res = rig.runner.runTest(UsbTestConfig.HIGH_QUALITY.copy(durationMs = 2000))

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
        assertTrue(UsbTestConfig.LOW_LATENCY.maxAllowedLatencyMs.isInfinite())
    }

    /**
     * AC-050.9 (D16). Los 192 paquetes en vuelo no son pérdidas: con tráfico perfecto la fila pasa
     * el 99,9 %.
     *
     * Bug que atrapa: MINI-039 — completados / enviados con los en vuelo adentro da 99,66 % con
     * cero errores y falla por construcción.
     */
    @Test
    fun `AC-050_9 los paquetes en vuelo no cuentan como perdidos`() = runRig { rig ->
        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
        assertEquals(IN_FLIGHT, res.packetsInFlight)
        assertEquals(IN_FLIGHT, res.totalPackets - res.successfulPackets, "premisa: la fila reabrió y la diferencia es sólo lo en vuelo")
        assertTrue(res.successRate >= 99.9f, "éxito ${res.successRate}")
    }

    /** AC-050.9, el gemelo: paquetes perdidos de verdad, más allá de los en vuelo, sí fallan. */
    @Test
    fun `AC-050_9 gemelo - una perdida real mas alla de lo en vuelo falla`() = runRig { rig ->
        rig.usb.lostPerPoll = 50

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertTrue(res.errorMessage!!.contains("Success rate"), "mensaje: ${res.errorMessage}")
    }

    /**
     * AC-050.9 (D16). La latencia se juzga contra el techo que declara el backend: 37,5 ms contra
     * 61,3 ms pasa con la config por defecto.
     *
     * Bug que atrapa: MINI-039 — el umbral fijo de 20 ms fallaba por construcción.
     */
    @Test
    fun `AC-050_9 la latencia se juzga contra el techo declarado por el backend`() = runRig { rig ->
        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
        assertEquals(61.3, res.latencyLimitMs, 0.01)
    }

    /** AC-050.9, el gemelo: una latencia por encima del techo declarado falla. */
    @Test
    fun `AC-050_9 gemelo - por encima del techo declarado falla`() = runRig { rig ->
        rig.usb.latencyMs = 70.0

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertTrue(res.errorMessage!!.contains("latency"), "mensaje: ${res.errorMessage}")
    }

    /**
     * AC-050.9. Si el backend no declara techo y la fila no fija uno, no hay contra qué juzgar la
     * latencia: FAILED, no un PASSED por omisión.
     */
    @Test
    fun `AC-050_9 sin techo declarado ni limite propio la fila no pasa`() = runRig { rig ->
        rig.usb.declaredCeilingMs = 0.0

        val res = rig.runner.runTest(UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000))

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertTrue(res.errorMessage!!.contains("ceiling"), "mensaje: ${res.errorMessage}")
    }

    /** AC-050.9. Un límite propio más estricto que el techo del backend sigue mandando. */
    @Test
    fun `AC-050_9 un limite propio mas estricto que el techo sigue mandando`() = runRig { rig ->
        val res = rig.runner.runTest(
            UsbTestConfig(sampleRate = 96000, bitDepth = 24, durationMs = 2000, maxAllowedLatencyMs = 10.0),
        )

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertEquals(10.0, res.latencyLimitMs, 0.0)
    }

    /** AC-050.9. Sin paquetes resueltos el éxito es 0 %, no 100 %. */
    @Test
    fun `AC-050_9 sin paquetes resueltos la tasa de exito es cero`() {
        val res = UsbTestResult(
            testType = UsbTestType.PLAYBACK_TONE, config = UsbTestConfig(), status = UsbTestStatus.RUNNING,
            startTimeMs = 0, endTimeMs = 0, totalPackets = 0, successfulPackets = 0,
        )
        assertEquals(0f, res.successRate)
        assertFalse(res.passed, "un resultado sin tráfico se leyó como pasado")
    }

    /** AC-050.9. El test de captura tampoco pasa sin tráfico. */
    @Test
    fun `AC-050_9 la captura sin trafico no pasa`() = runRig(initial = captureStream) { rig ->
        rig.usb.trafficPerPoll = 0

        val res = rig.runner.runTest(UsbTestConfig(testType = UsbTestType.CAPTURE_LEVEL, durationMs = 2000))

        assertEquals(UsbTestStatus.FAILED, res.status)
    }

    /**
     * AC-050.9 (review de S3, I1). El tráfico de un stream SÓLO de playback no prueba nada de la
     * captura: la fila no pasa.
     */
    @Test
    fun `AC-050_9 la captura sobre un stream sin captura no pasa`() = runRig { rig ->
        val res = rig.runner.runTest(UsbTestConfig(testType = UsbTestType.CAPTURE_LEVEL, durationMs = 2000))

        assertEquals(UsbTestStatus.FAILED, res.status)
        assertTrue(res.errorMessage!!.contains("capture"), "mensaje: ${res.errorMessage}")
    }

    /** El gemelo: con un stream de captura y tráfico, la fila de captura pasa. */
    @Test
    fun `AC-050_9 gemelo - la captura con stream de captura y trafico pasa`() = runRig(initial = captureStream) { rig ->
        val res = rig.runner.runTest(UsbTestConfig(testType = UsbTestType.CAPTURE_LEVEL, durationMs = 2000))

        assertEquals(UsbTestStatus.PASSED, res.status, "mensaje: ${res.errorMessage}")
    }

    // ==================== Qué ofrece el device ====================

    /** UAC1: los rates están en el formato y no hay reloj que elegir. */
    @Test
    fun `AC-050_8 en UAC1 lo ofrecido sale del formato y sin reloj`() {
        val uac1 = cm720.copy(
            uacVersion = 1, clockSources = emptyList(),
            playbackAltsettings = listOf(
                cm720.playbackAltsettings[0].copy(
                    formats = listOf(AudioFormatInfo(2, 16, 2, listOf(44100, 48000), false, 0, 0)),
                ),
            ),
        )
        assertEquals(OfferedPlaybackFormat(UsbAltsettingSelection(1, 1, 0), null), findOfferedPlaybackFormat(uac1, 48000, 2, 16))
        assertEquals(null, findOfferedPlaybackFormat(uac1, 96000, 2, 16))
    }

    /**
     * UAC2 con dos relojes (review de S3, I3): el snapshot no dice cuál llega a la terminal, así
     * que no se fuerza ninguno — la fila va con el reloj automático y el chequeo del rate juzga.
     */
    @Test
    fun `AC-050_8 en UAC2 con varios relojes no se fuerza uno`() {
        val twoClocks = cm720.copy(clockSources = cm720.clockSources + cm720.clockSources[0].copy(clockId = 10))
        assertEquals(OfferedPlaybackFormat(UsbAltsettingSelection(1, 2, 0), null), findOfferedPlaybackFormat(twoClocks, 96000, 2, 24))
        assertEquals(OfferedPlaybackFormat(UsbAltsettingSelection(1, 2, 0), CLOCK), findOfferedPlaybackFormat(cm720, 96000, 2, 24))
    }

    // ==================== El barrido de rates y la suite ====================

    /** AC-050.8 en el barrido: un rate que el device no ofrece no se mide. */
    @Test
    fun `AC-050_8 el barrido marca NOT_APPLICABLE el rate que el device no ofrece`() = runRig { rig ->
        val res = rig.runner.runTest(UsbTestPresets.RATE_NEGOTIATION_SWEEP.first { it.sampleRate == 88200 })

        assertEquals(UsbTestStatus.NOT_APPLICABLE, res.status)
        assertEquals(emptyList(), rig.usb.log)
    }

    /** D19 en el barrido: un rate coercionado falla. */
    @Test
    fun `D19 el barrido falla un rate coercionado`() = runRig { rig ->
        rig.usb.coerce[44100] = 48000

        val res = rig.runner.runTest(UsbTestPresets.RATE_NEGOTIATION_SWEEP.first { it.sampleRate == 44100 })

        assertEquals(UsbTestStatus.FAILED, res.status)
    }

    /** La suite estándar en la CM720: las tres filas se miden a su rate y pasan. */
    @Test
    fun `AC-050_8 la suite estandar mide cada fila a su rate`() = runRig { rig ->
        val report = rig.runner.runTestSuite(UsbTestPresets.STANDARD_SUITE, "CM720", "2B89:64EC", 2)

        assertEquals(listOf(48000, 44100, 96000), report.results.map { it.streamSampleRateHz })
        assertTrue(report.allPassed, report.results.joinToString { "${it.status}: ${it.errorMessage}" })
    }

    /** Un no-aplicable en la suite no deja que el informe diga "todo pasó". */
    @Test
    fun `AC-050_8 un no-aplicable en la suite no es todo pasado`() = runRig { rig ->
        val report = rig.runner.runTestSuite(
            listOf(UsbTestConfig(durationMs = 2000), UsbTestConfig(sampleRate = 88200, durationMs = 2000)),
            "CM720", "2B89:64EC", 2,
        )

        assertFalse(report.allPassed)
        assertEquals(1, report.notApplicableCount)
    }

    // ==================== El arnés ====================

    private val captureStream = UsbActiveStreamConfig(48000, 2, 24, UsbStreamingMode.CAPTURE_ONLY, null, null)

    private class Rig(
        scope: TestScope,
        initial: UsbActiveStreamConfig?,
        pending: UsbManualSelection,
        snapshot: UsbCapabilitySnapshot,
    ) {
        val usb = FakeRunnerUsbManager(snapshot, initial, pending)
        val runner = UsbAudioTestRunner(
            usbManager = usb,
            scope = scope,
            roundTripTester = NoRoundTrip,
            streamSampleRate = { usb.streamRate },
            clock = { scope.testScheduler.currentTime },
        )
    }

    private fun runRig(
        initial: UsbActiveStreamConfig? = UsbActiveStreamConfig(48000, 2, 24, UsbStreamingMode.PLAYBACK_ONLY, null, null),
        pending: UsbManualSelection = UsbManualSelection(initial?.altsetting, initial?.clockSourceId),
        body: suspend (Rig) -> Unit,
    ) = runTest { body(Rig(this, initial, pending, cm720)) }

    private object NoRoundTrip : IRoundTripLatencyTester {
        override val progress: StateFlow<RoundTripTestProgress> = MutableStateFlow(RoundTripTestProgress())
        override suspend fun run(config: RoundTripTestConfig): RoundTripTestResult = error("no usado")
        override fun cancel() = Unit
    }

    /**
     * El manager USB del runner. Anota las operaciones que cambian el stream y simula tráfico:
     * cada `getTransferStats` avanza `trafficPerPoll` paquetes completados, con [IN_FLIGHT]
     * enviados de más (la cola en vuelo) y `lostPackets` perdidos.
     */
    internal class FakeRunnerUsbManager(
        private val snapshot: UsbCapabilitySnapshot,
        initial: UsbActiveStreamConfig?,
        pending: UsbManualSelection,
    ) : IUsbAudioManager, UsbActiveStreamSource {
        val log = mutableListOf<String>()
        var active: UsbActiveStreamConfig? = initial
        val coerce = mutableMapOf<Int, Int>()
        var trafficPerPoll = 600L
        var lostPerPoll = 0L
        var latencyMs = 37.5
        var declaredCeilingMs = 61.3
        /** Después de tantas lecturas de stats el stream se traba (los completados dejan de crecer). */
        var stallAfterPolls = Int.MAX_VALUE
        /** Después de tantas lecturas las stats dan null (el stream salió de STREAMING sin avisar al runner). */
        var statsNullAfterPolls = Int.MAX_VALUE
        /** Contadores que el stream trae de antes de la fila (no se reabre: no se ponen en cero). */
        var historicUnderruns = 0L
        var historicCompleted = 0L
        /** Los arranques después de este número fallan (0 = ninguno falla). */
        var failStartsAfter = 0
        /** Volver al altsetting automático falla. */
        var failAutomaticAltsetting = false
        private var starts = 0
        private var polls = 0
        private var completed = 0L
        private var lost = 0L
        private var selection: UsbAltsettingSelection? = pending.altsetting
        private var clock: Int? = pending.clockSourceId
        var streamRate: Int? = initial?.sampleRate

        private val state = MutableStateFlow(if (initial != null) UsbConnectionState.STREAMING else UsbConnectionState.CONNECTED)
        override val connectionState: StateFlow<UsbConnectionState> = state

        override fun activeStreamConfig(): UsbActiveStreamConfig? = active
        override fun manualSelection(): UsbManualSelection = UsbManualSelection(selection, clock)

        override fun getCurrentCapabilitySnapshot(): UsbCapabilitySnapshot? = snapshot

        override fun stopStreaming() {
            if (state.value != UsbConnectionState.STREAMING) return
            log += "stop"
            state.value = UsbConnectionState.CONNECTED
            active = null
            streamRate = null
        }

        override suspend fun selectAltsetting(interfaceNumber: Int, alternateSetting: Int, formatIndex: Int): UsbResult<Unit> {
            log += "alt($interfaceNumber,$alternateSetting,$formatIndex)"
            if (interfaceNumber == -1 && failAutomaticAltsetting) return UsbResult.Failure(UsbAudioError.INTERNAL_ERROR, "falla inyectada")
            selection = if (interfaceNumber == -1) null else UsbAltsettingSelection(interfaceNumber, alternateSetting, formatIndex)
            return UsbResult.Success(Unit)
        }

        override suspend fun selectClockSource(clockSourceId: Int): UsbResult<Unit> {
            log += "clock($clockSourceId)"
            clock = if (clockSourceId == 0) null else clockSourceId
            return UsbResult.Success(Unit)
        }

        override suspend fun startStreaming(sampleRate: Int, channels: Int, bitDepth: Int, streamingMode: UsbStreamingMode): UsbResult<Unit> {
            log += "start($sampleRate,$channels,$bitDepth,$streamingMode)"
            starts++
            if (failStartsAfter in 1 until starts) return UsbResult.Failure(UsbAudioError.STREAMING_ERROR, "falla inyectada")
            state.value = UsbConnectionState.STREAMING
            active = UsbActiveStreamConfig(sampleRate, channels, bitDepth, streamingMode, selection, clock)
            streamRate = coerce[sampleRate] ?: sampleRate
            completed = 0
            lost = 0
            historicUnderruns = 0
            historicCompleted = 0
            return UsbResult.Success(Unit)
        }

        override suspend fun startStreaming(sampleRate: Int, channels: Int, bitDepth: Int): UsbResult<Unit> =
            startStreaming(sampleRate, channels, bitDepth, UsbStreamingMode.PLAYBACK_ONLY)

        override fun getTransferStats(): UsbTransferStats? {
            if (state.value != UsbConnectionState.STREAMING) return null
            if (polls >= statsNullAfterPolls) return null
            if (++polls <= stallAfterPolls) {
                completed += trafficPerPoll
                lost += lostPerPoll
            }
            return UsbTransferStats(
                packetsSubmitted = historicCompleted + completed + IN_FLIGHT + lost,
                packetsCompleted = historicCompleted + completed,
                underruns = historicUnderruns,
                currentLatencyMs = latencyMs,
                avgLatencyMs = latencyMs,
                packetsInFlight = IN_FLIGHT,
                declaredMaxLatencyMs = declaredCeilingMs,
            )
        }

        override fun hasCapture(): Boolean = true
        override fun supportsFullDuplex(): Boolean = false

        // ---- lo que el runner no usa ----
        override val connectedDevices: StateFlow<List<UsbAudioDevice>> = MutableStateFlow(emptyList())
        override val selectedDevice: StateFlow<UsbAudioDevice?> = MutableStateFlow(null)
        override val deviceEvents: SharedFlow<UsbDeviceEvent> = MutableSharedFlow()
        override val healthEvents: SharedFlow<UsbHealthEvent> = MutableSharedFlow()
        override val currentCapabilitySnapshot: StateFlow<UsbCapabilitySnapshot?> = MutableStateFlow(snapshot)
        override val volumeState: StateFlow<UsbVolumeState> get() = error("no usado")
        override fun getConnectedDevices(): List<UsbAudioDevice> = emptyList()
        override suspend fun refreshDevices() = Unit
        override fun isUsbAudioSupported(): Boolean = true
        override suspend fun connectDevice(device: UsbAudioDevice): UsbResult<Unit> = error("no usado")
        override suspend fun disconnectDevice() = Unit
        override fun getPreferredDevice(): UsbAudioDevice? = null
        override fun hasPermission(device: UsbAudioDevice): Boolean = true
        override suspend fun requestPermission(device: UsbAudioDevice) = Unit
        override suspend fun getDeviceCapabilities(device: UsbAudioDevice): UsbResult<UsbAudioCapabilities> = error("no usado")
        override fun getFileDescriptor(): Int = 3
        override fun getUsbfsPath(): String? = null
        override fun isDeviceReady(): Boolean = true
        override fun getUacVersion(): Int = 2
        override fun rankPlaybackAltsettings(preference: StreamPreference): List<ScoredAltsetting> = emptyList()
        override fun setStreamPreference(preference: StreamPreference) = Unit
        override fun setLatencyProfile(profile: UsbLatencyProfile): UsbResult<Unit> = UsbResult.Success(Unit)
        override fun startMonitoring() = Unit
        override fun stopMonitoring() = Unit
        override fun release() = Unit
        override fun setAutoConnectEnabled(enabled: Boolean) = Unit
        override fun isAutoConnectEnabled(): Boolean = false
        override fun getVolumeCapabilities(): UsbVolumeCapabilities = error("no usado")
        override suspend fun setOutputVolume(volume: Float) = Unit
        override fun getOutputVolume(): Float = 1f
        override suspend fun setInputVolume(volume: Float) = Unit
        override fun getInputVolume(): Float = 1f
        override suspend fun setOutputMuted(muted: Boolean) = Unit
        override fun isOutputMuted(): Boolean = false
        override suspend fun setInputMuted(muted: Boolean) = Unit
        override fun isInputMuted(): Boolean = false
        override suspend fun adjustOutputVolume(delta: Float): Float = 1f
        override suspend fun toggleOutputMute(): Boolean = false
        override fun shouldInterceptVolumeButtons(): Boolean = false
        override fun observeVolumeChanges(): Flow<UsbVolumeState> = error("no usado")
    }
}
