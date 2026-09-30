package com.watermellonstudios.audio.harness.soundfont

import com.watermellonstudios.audio.api.AudioEngine
import com.watermellonstudios.audio.api.IAudioNativeBridge

/**
 * [SoundFontPort] sobre el puente de verdad. `ISoundFontBridge` es API pública; el tipo de engine,
 * el waveform del bridge y el play frame del transport son la superficie de diagnóstico que el
 * harness ya usa detrás de `@InternalWatermelonApi` (ver `DiagnosticsControl`).
 */
class BridgeSoundFontPort(
    private val bridge: IAudioNativeBridge,
    private val engine: AudioEngine,
) : SoundFontPort {

    private val waveform = FloatArray(WAVEFORM_SAMPLES)

    override fun load(path: String): Boolean = bridge.loadSoundFontFromPath(path)
    override fun isLoaded(): Boolean = bridge.isSoundFontLoaded()
    override fun unload() = bridge.unloadSoundFont()
    override fun presetCount(): Int = bridge.getSoundFontPresetCount()
    override fun presetName(index: Int): String? = bridge.getSoundFontPresetName(index)
    override fun bankProgram(index: Int): IntArray? = bridge.getSoundFontPresetBankProgram(index)
    override fun setPreset(index: Int) = bridge.setSoundFontPreset(index)

    override suspend fun ensureEngineRunning(): Boolean {
        if (engine.isRunning) return true
        return engine.start().isSuccess && engine.isRunning
    }

    override fun engineType(): Int = bridge.getEngineType()
    override fun setEngineType(type: Int) = bridge.setEngineType(type)
    override fun noteOn(midiNote: Int, velocity: Float) = bridge.sfNoteOn(TOUCH_ID, midiNote, velocity)
    override fun noteOff() = bridge.sfNoteOff(TOUCH_ID)

    override fun outputPeak(): Float {
        val n = bridge.getWaveformSamples(waveform, waveform.size)
        return SoundFontCheck.peak(waveform, n)
    }

    override fun playFrame(): Long = bridge.transportGetPlayFrame()

    private companion object {
        const val TOUCH_ID = 0
        const val WAVEFORM_SAMPLES = 2048
    }
}
