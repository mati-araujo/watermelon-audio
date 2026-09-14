---
title: "NoisyPad → watermelon-audio · respuesta al aviso de v2.17.1 (la atenuación declarada)"
type: reference
status: current
created: 2026-09-14
---

# Respuesta de NoisyPad al aviso de v2.17.1

**Recibida el 2026-09-14.** Contesta `respuesta-noisypad-2026-09-14-v2.17.1.md` (aviso de v2.17.1,
#3 medido, el default #2, #4). Texto íntegro, como llegó.

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo que sí se verificó contra
> nuestro árbol está anotado al final, separado a propósito.

---

1. Adoptamos 2.17.1 (PR de NoisyPad `chore/audio-2-17-1`). Medido en el Moto G42 con el mismo arnés de la carta anterior (tres apoyadas quietas en y=0,97/0,60/0,30, efectos en bypass, RMS en [t0+0,10 s, t0+0,60 s]), nivel absoluto a y=0,97, 2.17.0 → 2.17.1:

* `0:81 Saw Lead`: −11,9 → −23,0 dBFS, −11,1 dB, lo que dice su nota al décimo.
* `0:56 Trumpet` (sin atenuación declarada): −17,4 → −17,4. La referencia no se movió.
* `0:89 Warm Pad`: −24,0 → −25,5 (−1,5 dB).
* La tabla de parámetros por engine: diff vacío. El nivel por Y sigue siendo 20·log10(y) en los tres, la ganancia es pura (bandas <700 Hz y >2 kHz iguales, centroide fijo). Nada que compensar de nuestro lado, como pidieron.

2. Su §2 (Saw Lead, la fila entera): recibido y consistente con lo nuestro a 0,2 dB. Para NoisyPad no cambia el diseño (velocity 1,0 + expresión en el XY); sí cambia la lectura del grid con pianos: la velocity mueve el nivel a 840 cB más la capa. Anotado en nuestro doc del bump.

3. Su §3 (default #2 vivo, brillo a baja velocity): el dato que teníamos ya está en nuestro doc de 2.17.0: en Saw Lead el centroide baja de 1123 a 880 Hz entre velocity 124 y 38 con el motor 2.17.x (build de control), y no se mueve con la expresión (1119 Hz fijo). Con 2.17.1 igual. No tenemos todavía una queja de oído con nombre de preset: cuando la haya, va con número. Si sirve para decidir el MINI, la posición de producto de NoisyPad es que el font suene como lo afina su autor (FluidSynth como referencia), porque es la referencia contra la que GeneralUser está calibrado y contra la que nosotros medimos.

4. Su §4: conforme. El KDoc corregido dice lo que hacemos: velocity 1,0 en el ataque, expresión re-enviada en el mismo frame después de cada note-on.

5. MINI-025/026: sin dato con nombre todavía. Los WAV del banco de 2.17.0 y 2.17.1 están en el scratchpad de la sesión de NoisyPad y se pueden mandar si los quieren; el método coincide con el suyo.

---

## Lo que verificamos de nuestro lado (2026-09-14)

- **#1, los tres niveles absolutos, contra el archivo** (`read-sf2-modulators.py --attenuation`,
  GeneralUser 1.471): `Saw Lead` 370 cB en sus 16 zonas → **−11,1 dB** extra (0,3 × 37); `Trumpet`
  sin atenuación declarada → **0**; `Warm Pad` 50 cB en sus 168 zonas → **−1,5 dB**. Tres de tres,
  al décimo. Es la primera vez que la nota de bump de una release se confirma en el dispositivo
  preset por preset, con la referencia (Trumpet) incluida.
- **#3 no trae dato nuevo del oído** —lo dicen— pero trae dos cosas que MINI-028 necesitaba: la
  **posición de producto** (el font como lo afina su autor; FluidSynth es la referencia contra la
  que GeneralUser está calibrado y contra la que ellos miden) y la confirmación de que el brillo
  **no** se mueve con la expresión (1119 Hz fijo: R-MOT-11, el por toque es una ganancia pura).
  Anotado en MINI-028 como evidencia para la salida (A); no decide.
- **#4 conforme**; nada pendiente.
- **#5**: los WAV de `Warm Pad` son los que pedimos (su −8,4 / −16,0 contra nuestro −7,09 / −17,12);
  siguen ofrecidos.
