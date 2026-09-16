# Nota de bump para NoisyPad — la perilla de la ambiencia del font

**Fecha**: 2026-09-15 · **Cambio**: REQ-042 (S1) · **Release**: la siguiente (2.19.0), `feat` ·
**Pedida por**: NoisyPad, con carta (`carta-noisypad-2026-09-15-respuesta-v2.18.0.md`, §2).

Esta nota dice **qué superficie nueva hay, qué hace exactamente y qué NO hace**. No cambia ningún
sonido por defecto: con la perilla en su default (1/1) el render es **byte a byte** el de v2.18.0.

## Lo que hay, en tres frases

1. **Tres funciones, en las tres superficies.** C API `wma_sf_set_ambience(engine, reverb, chorus)`,
   `wma_sf_get_ambience_reverb(engine)`, `wma_sf_get_ambience_chorus(engine)`; Kotlin
   `ISoundFontBridge.sfSetAmbience(reverb, chorus)`, `sfGetAmbienceReverb()`, `sfGetAmbienceChorus()`
   (Android por JNI, iOS por cinterop, el mismo contrato). **Por instancia, sin CC.**
2. **Escala TODOS los sends antes de las unidades**: el generador del font (`reverbEffectsSend` /
   `chorusEffectsSend`, 225 / 140 presets de GeneralUser) y los moduladores, incluido el default #8/#9
   del spec con CC91/CC93 en reset (el +6,3 % parejo de 2.18.0). **Unidad lineal 0..1 sobre la
   amplitud del send**: 0,5 = −6 dB de wet, 0,1 = −20 dB. Default **1/1** (= FluidSynth = 2.18.0). Con
   **0/0** el render es, muestra a muestra, el font seco — lo que su arnés necesita para comparar
   contra 2.17.4 (la única diferencia que queda es lo que MINI-027 cambió).
3. **Es un ajuste del instrumento, no del font**: sobrevive a `reset()`, a cargar o descargar un font
   y a `prepare()` con otro rate. Se fija una vez (p. ej. al arrancar, desde lo que persistan en la
   hoja del SoundFont) y queda. Fuera de rango satura; `NaN` deja ese bus como estaba y deja rastro
   en el registro. No es RT-safe (por ese rastro): thread de control. Al thread de audio sólo llegan
   dos atómicos que se leen una vez por bloque y un slew de 5 ms por muestra (aritmética y compares)
   — sin cola, sin lock, sin allocation.

## Lo que NO hace, a propósito

- **No toca los parámetros de las unidades** (room size, damp, width, level de la reverb; voces,
  level, speed, depth del chorus): siguen fijos en los defaults de FluidSynth 2.6.0. Si un día hace
  falta, es otro REQ con carta.
- **No sigue CC91/CC93 en vuelo** (no hay superficie de CC; R-MOT-42). CC91 vale su reset de GM
  (40 = 6,3 %) y CC93 0, siempre.
- **No apaga el DSP de las unidades con 0/0**: la compuerta de 2 s ya las duerme cuando no entra
  nada (medido en REQ-040: dormidas cuestan el 5 % de 0,65 % del tiempo real).
- **No es por preset ni por toque.**

## Lo medido

- **0/0 ≡ seco, muestra a muestra** (diferencia máxima 0,0 sobre un font mínimo con sends y el
  default #8 vivo; control positivo: 1/1 difiere del seco en 0,38). **1/1 ≡ v2.18.0, byte a byte.**
- **Linealidad**: |wet(0,5) − 0,5 · wet(1)| ≤ 3·10⁻⁸.
- **El escalón 0/0 → 1/1 con una nota sostenida** (cambiar el toggle en caliente) se midió antes de
  decidir si llevaba rampa, y **llevó rampa**. El primer criterio (derivada del wet al conmutar contra
  la derivada del wet en régimen, 0,91× a 480 Hz) dependía de la nota —el régimen es la pendiente del
  seno, ∝ f; el escalón no— y lo tumbó el review. El normalizador es ahora el transitorio del **propio
  note-on**, barrido en 110 / 220 / 440 / 873 Hz: sin rampa, conmutar era **10,3 / 6,8 / 10,2 / 1,2×**
  más brusco que tocar la nota. Con la rampa: **0,94 / 0,94 / 0,95 / 0,90×** (test
  `SwitchingIsNoMoreAbruptThanTheNoteOnItselfAcrossTheKeyboard`; "sin rampa" es su mutante).
- **La rampa**: 5 ms de tiempo fijo a cualquier rate, por muestra, **sólo en el cambio en caliente**.
  Tras `prepare()` el slew queda sin cebar y el primer bloque toma el objetivo de una, así que un set
  antes de arrancar aplica al instante: 0/0 sigue siendo el seco **desde la primera muestra** y 1/1
  sigue siendo v2.18.0 byte a byte. Los getters devuelven el **objetivo**, no el valor en tránsito. Si
  en el device igual se oye un pop al conmutar, ese es el dato que el número no vio.
- **Persistencia**: 0,3 → `reset()` + swap de font + `prepare(96 kHz)` → los getters siguen en 0,3.
- El spec-test entero sigue en **18 F · 11 S · 15 R**. Las tres funciones JNI se **ejecutan** desde
  la JVM (arnés: 126 → 129 de 318).

## Para su arnés

`sfSetAmbience(0f, 0f)` al arrancar la tanda seca; `sfGetAmbienceReverb()` / `Chorus()` para afirmar
que el motor quedó ahí antes de medir. La línea de base seca de 2.19.0 contra 2.17.4 tiene que dar:
niveles y colas iguales salvo MINI-027 (el corte modulado en 111 presets) — y esa es la pregunta
activa del aviso.
