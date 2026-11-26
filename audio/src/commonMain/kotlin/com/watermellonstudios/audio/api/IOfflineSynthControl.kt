package com.watermellonstudios.audio.api

/**
 * La configuración del motor de una instancia de **render offline** (REQ-055 S2 y S15): motor,
 * oscilador, SoundFont, cadena de efectos, ruteo y gestos, sobre el handle de la instancia y nunca
 * sobre el motor en vivo ni sobre el `ScaleQuantizer` global. Es lo que un re-render de una toma
 * necesita para reproducir su sonido (ver [IOfflineRenderer] para el orden de uso).
 *
 * Todo lo que devuelve `Result` falla con `EngineNotInitialized` si la instancia está cerrada, sin
 * cruzar al nativo. A diferencia del motor en vivo, **un valor fuera de rango se rechaza** con
 * `ParameterOutOfRange` en lugar de descartarse en silencio: un re-render con un valor ignorado
 * sonaría como otra toma sin avisar. Los parámetros de engine y de efecto y los gestos (`setXY`,
 * `setFrequencyAndAmplitude`, notas) los resuelve la C API como en vivo, y sólo pueden fallar por la
 * instancia.
 *
 * Cada llamada se serializa del lado nativo con las demás de la misma instancia: no hace falta
 * sincronizar desde afuera, pero un setter espera a que termine el bloque en curso.
 */
interface IOfflineSynthControl {
    /** El engine de síntesis (0..6, el orden de `SynthEngineType`). */
    fun setEngineType(type: Int): Result<Unit>

    fun setEngineParameter(paramId: Int, value: Float): Result<Unit>

    fun setMasterVolume(volume: Float): Result<Unit>

    fun setXY(x: Float, y: Float): Result<Unit>

    /** Frecuencia y amplitud directas (sin el mapeo XY): lo que usa la cuantización de escalas. */
    fun setFrequencyAndAmplitude(frequency: Float, amplitude: Float): Result<Unit>

    /** Carga un `.sf2`/`.sf3` por ruta en ESTA instancia (se decodifica por instancia: ~70 MB con GeneralUser). */
    fun loadSoundFont(path: String): Result<Unit>

    /** Elige el preset del font cargado; fuera de `0 until presetCount` falla con `ParameterOutOfRange`. */
    fun setSoundFontPreset(presetIndex: Int): Result<Unit>

    fun soundFontNoteOn(touchId: Int, midiNote: Int, velocity: Float): Result<Unit>

    fun soundFontNoteOff(touchId: Int): Result<Unit>

    /** Agrega un efecto al final de la cadena; devuelve su índice. */
    fun addEffect(typeId: Int): Result<Int>

    fun clearAllEffects(): Result<Unit>

    fun setEffectParameter(index: Int, paramId: Int, value: Float): Result<Unit>

    /** La forma de onda del oscilador clásico: 0=seno, 1=cuadrada, 2=sierra, 3=triángulo, 4=ruido. */
    fun setOscillatorType(typeId: Int): Result<Unit>

    /** El modulador (0=NONE … 7=GATE); un id inexistente falla con el código de la C API. */
    fun setModulatorType(typeId: Int): Result<Unit>

    /** El rango del mapeo XY, en Hz; `maxHz` tiene que ser mayor que `minHz` y los dos finitos. */
    fun setFrequencyRange(minHz: Float, maxHz: Float): Result<Unit>

    /** El BPM global de los efectos sincronizados; el motor lo recorta a 20..300. */
    fun setBpm(bpm: Float): Result<Unit>

    /** Bypass de UN efecto de la cadena; un índice inexistente falla con `InvalidEffectIndex`. */
    fun setEffectBypass(index: Int, bypass: Boolean): Result<Unit>

    /** El ruteo de la cadena: 0=serie, 1=paralelo, 2=split 2x2, 3=serie-paralelo, 4=paralelo-serie, 5=feedback. */
    fun setRoutingMode(mode: Int): Result<Unit>

    /** El balance del ruteo paralelo (0 = rama A, 1 = rama B); el motor lo recorta a 0..1. */
    fun setParallelMix(mix: Float): Result<Unit>

    /** La realimentación del ruteo feedback; el motor la recorta a 0..0,95. */
    fun setFeedbackAmount(amount: Float): Result<Unit>

    /** La expresión de un toque que YA suena (multiplicador, neutro en 1,0); un toque inexistente se ignora. */
    fun sfSetTouchExpression(touchId: Int, expression: Float): Result<Unit>

    /** Suelta todas las notas del SoundFont. */
    fun sfNoteOffAll(): Result<Unit>

    /**
     * `[teclaMin, teclaMax]` del preset, o `null` sin font cargado o con un índice inválido; también
     * `null` con la instancia cerrada (sin cruzar).
     */
    fun getSoundFontPresetKeyRange(presetIndex: Int): IntArray?

    /** `[banco, programa]` del preset, o `null` en los mismos casos que [getSoundFontPresetKeyRange]. */
    fun getSoundFontPresetBankProgram(presetIndex: Int): IntArray?
}
