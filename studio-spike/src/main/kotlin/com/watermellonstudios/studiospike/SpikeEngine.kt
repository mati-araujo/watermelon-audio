package com.watermellonstudios.studiospike

import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge
import kotlinx.coroutines.runBlocking
import java.io.File
import kotlin.math.roundToInt

/** Los 7 engines del motor, en el orden de `setEngineType`. */
val ENGINES = listOf("CLASSIC", "KARPLUS", "FM", "WAVETABLE", "GRANULAR", "SUPERSAW", "SOUNDFONT")
const val SOUNDFONT = 6

/**
 * El motor visto desde la espiga: carga los dos dylibs del bundle por ruta absoluta
 * (ADR-0009 §4) y expone lo que tocan el pad y la medición.
 *
 * El error de carga se captura con su texto REAL: bajo hardened runtime, una dylib que
 * library validation rechaza falla acá, y ese mensaje es evidencia de S3.
 */
class SpikeEngine private constructor(
    val nativeDir: File,
    val bridge: AudioNativeBridge,
) {
    @Volatile var engineType: Int = 0
        private set
    @Volatile var soundFontLoaded: Boolean = false
        private set
    private var sfNote = -1

    fun start(masterVolume: Float): Result<Unit> {
        bridge.setMasterVolume(masterVolume)
        bridge.setSynthVolume(0f)
        return runBlocking { bridge.startEngine() }
    }

    fun stop(): Result<Unit> = runBlocking { bridge.stopEngine() }

    fun loadSoundFont(path: String): Boolean {
        soundFontLoaded = File(path).isFile && bridge.loadSoundFontFromPath(path)
        if (soundFontLoaded) bridge.setSoundFontPreset(0)
        return soundFontLoaded
    }

    fun selectEngine(type: Int) {
        release()
        engineType = type
        bridge.setEngineType(type)
    }

    /** Lo que hace un toque del pad. El estampado va ÚLTIMO: después de que el parámetro está escrito. */
    fun press(x: Float, y: Float) {
        if (engineType == SOUNDFONT) {
            sfNote = noteFor(x)
            bridge.sfNoteOn(0, sfNote, 0.9f)
        } else {
            bridge.setXY(x, 1f - y, false)
        }
        bridge.setSynthVolume(1f)
        SpikeNative.markTouch()
    }

    fun drag(x: Float, y: Float) {
        if (engineType == SOUNDFONT) {
            val n = noteFor(x)
            if (n != sfNote) {
                bridge.sfNoteOff(0)
                sfNote = n
                bridge.sfNoteOn(0, n, 0.9f)
            }
        } else {
            bridge.setXY(x, 1f - y, false)
        }
    }

    fun release() {
        if (sfNote >= 0) {
            bridge.sfNoteOff(0)
            sfNote = -1
        }
        bridge.setSynthVolume(0f)
    }

    fun outputRms(): Float = maxOf(bridge.getOutputRmsLevel(0), bridge.getOutputRmsLevel(1))

    private fun noteFor(x: Float) = 48 + (x.coerceIn(0f, 1f) * 24f).roundToInt()

    companion object {
        const val LIBRARY_PROPERTY = "watermelon.audio.library"
        const val MAIN_LIB = "libwatermelon_audio.dylib"
        const val SHIM_LIB = "libstudio_spike_jni.dylib"

        /** Directorio de los dylibs: el de recursos de la app (Contents/app/resources). */
        fun resolveNativeDir(override: String?): File? {
            val dir = override ?: System.getProperty("compose.application.resources.dir") ?: return null
            return File(dir).absoluteFile
        }

        fun load(nativeDir: File): Result<SpikeEngine> = runCatching {
            val main = File(nativeDir, MAIN_LIB)
            val shim = File(nativeDir, SHIM_LIB)
            require(main.isFile) { "no existe ${main.path}" }
            require(shim.isFile) { "no existe ${shim.path}" }
            // Primero a mano, para que un rechazo de library validation llegue con su
            // texto; el NativeLibraryLoader de :audio lo traga en un log.
            System.load(main.path)
            System.setProperty(LIBRARY_PROPERTY, main.path)
            val bridge = AudioNativeBridge.getInstance()
            System.load(shim.path)
            SpikeEngine(nativeDir, bridge)
        }
    }
}
