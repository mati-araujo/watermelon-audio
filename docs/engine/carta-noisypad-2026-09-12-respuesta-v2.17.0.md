---
title: "NoisyPad → watermelon-audio · respuesta al aviso de v2.17.0 (los moduladores del SoundFont)"
type: reference
status: current
created: 2026-09-12
---

# Respuesta de NoisyPad al aviso de v2.17.0

**Recibida el 2026-09-12.** Contesta el aviso de v2.17.0 (REQ-039, `nota-de-bump-2026-09-11-moduladores-del-soundfont.md`).
Texto íntegro, como llegó.

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo que sí se verificó contra
> nuestro árbol está anotado al final, separado a propósito, y la respuesta es
> `respuesta-noisypad-2026-09-14-v2.17.1.md`.

---

1. Adoptamos 2.17.0 (watermellonAudio = "2.17.0", PR de NoisyPad feature/audio-2-17-0-expresion-por-toque). La tabla de parámetros por engine dio diff vacío contra 2.16.4.

2. Y → volumen: expresión por toque, y velocity fija en 1,0 en el XY pad. Decidimos que el XY es un controlador continuo y no una tecla: sfNoteOn(voz, nota, 1.0) sólo cuando cambia la nota, y sfSetTouchExpression(voz, y) (piso 0,3, paso 1/128, re-enviada en el mismo frame después de cada note-on porque el note-on la reinicia). Vale para la voz continua, las voces de acorde (10..15) y el dual-touch (voces 0 y 1). En el grid (Note/Drum/Piano) la velocity sigue saliendo de la Y dentro de la celda: ahí es un golpe y el font manda; el aftertouch del grid usa expresión relativa al golpe, acotada al golpe más fuerte que posea la voz. Medido en el Moto G42: el nivel por Y es 20·log10(y) a ±1 dB en Warm Pad (0:89) y en Saw Lead (0:81), igual que en 2.16.4, y la ganancia es pura (bandas <700 Hz y >2 kHz iguales, centroide fijo).

3. Hallazgo con nombre de preset: 0:81 Saw Lead NO se comporta como "velocity → nivel anulado". Con 2.17.0 y el pad SIN cambios (control: sólo el bump), tres apoyadas quietas a velocity 124 / 76 / 38 dieron 0 / −6,9 / −16,9 dB de RMS, y la caída está en la banda de la fundamental (<700 Hz: −16,9 dB), no sólo en los agudos (centroide 1123 → 880 Hz). O sea que la velocity SÍ mueve el nivel en ese preset, y más que la curva vieja de tsf (−4,2 / −10,2). Warm Pad (de los 212) sí coincide con su nota: −8,4 / −16,0, unos 4 y 6 dB más baja que en 2.16.4 a esas velocities. Capas de sample por velocity o un modulador de zona que no está anulado: no lo distinguimos desde acá. Los WAV y el arnés están en NoisyPad (docs/audio-bump/req_bump_audio_2_17_0.md, sección Verificación).

4. Un dato del motor que vale para su cola: SoundFontEngine.h:345 descarta un NOTE_ON con la misma nota sobre un toque activo (sólo guarda touch.velocity). NoisyPad se apoyaba en eso sin saberlo: los 16 escalones de velocity que mandaba en un arrastre vertical nunca cambiaron la ganancia (medido: envolvente plana, −22,6 / −20,8 / −22,2 dBFS a lo largo de un arrastre 0,30 → 0,97, en 2.16.4 y en 2.17.0). Con la expresión el mismo arrastre sube +8,7 dB. No pedimos que cambie; lo dejamos anotado por si el KDoc de sfNoteOn ("arranca o actualiza la nota") quiere decir que la velocity no se actualiza.

5. MINI-024..027: nada que reportar con nombre; no medimos presets con atenuación declarada ni percusión afinada.

---

## Lo que verificamos de nuestro lado (2026-09-14)

- **#3 es un hallazgo real, y era nuestro — de la nota, no del motor.** `0:81 Saw Lead` figuraba en
  la nota de 2.17.0 entre 26 presets con *"velocity → nivel anulado"*. Falso en los 26: el
  instrumento pone el default #1 en 0 y la **zona global del preset** lo vuelve a declarar
  (`pmod`, se suma por SF2 §9.5: 800 cB en `Saw Lead`). El lector `--presets` con que se escribió
  esa fila recorría sólo `imod`. Reproducido en el motor con el mismo note-on de producción:
  −7,06 / −17,08 dB a velocity 76 / 38, caída idéntica en la banda < 700 Hz (es atenuación, no
  filtro); su −6,9 / −16,9 está a 0,2 dB. La nota está corregida en su lugar con marca; el detalle
  y el hallazgo colateral sobre el filtro (su centroide 1123 → 880) en la respuesta.
- **#4 es exactamente R-MOT-13**, y el KDoc decía otra cosa. `SoundFontEngine.h:345` no re-ataca
  con la misma nota y descarta la velocity; **sí** reinicia la expresión por toque a 1,0 aunque no
  ataque (R-MOT-14). El KDoc de `sfNoteOn` (interfaz, Android y C API) ahora lo dice, y hay un test
  que lo afirma muestra a muestra.
- **#2**: su receta (velocity 1,0 + expresión por toque en el XY; velocity del golpe en el grid) es
  el camino que el aviso proponía. Nada que hacer; vale como evidencia de que R-MOT-40 se compone.
- **#5**: "nada con nombre" sobre MINI-024..027 es *sin oportunidad*, no verde. El aviso de 2.17.1
  lleva la pregunta activa otra vez.
- Un dato de su #2 que **no** cuadra al décimo y no perseguimos: `Warm Pad` a velocity 76 / 38
  midieron −8,4 / −16,0; el motor con el archivo da −7,09 / −17,12 (800 cB cóncava, sin `pmod`).
  Es 1,3 dB en las dos direcciones sobre un pad con ataque de 0,8 s y capas por tecla; sin sus
  WAV no separamos ventana de curva.
