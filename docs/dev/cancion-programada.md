# Cancion programada por frame (REQ-055 S11)

Una agenda de secciones, cada una con un clip por pista, que el callback mezcla EN LUGAR de las
tomas del looper. Es lo que usa NoisyPad Studio para "armar la cancion" y exportarla por render
offline. La API vive en `api/watermelon_audio.h` (`wma_song_*`, con su contrato); el motor, en
`looper/SongPlayer.h`.

## Por que no es un `TrackBuffer` con el buffer cambiado

El looper no tiene reloj: cada `TrackBuffer` acumula su propio playhead y guarda el audio adentro.
El unico reloj es el play frame del Transport (int64, monotono, **nunca se resetea**). La cancion
se deriva de el: la fase de una pista en la seccion k es `(frame - start_k) mod L`, una funcion
pura del frame absoluto. Por eso el render no guarda posiciones y un bloque de 1 frame da la misma
salida, bit a bit, que uno de 4096.

## Piezas, y de quien es cada hilo

| pieza | hilo | como |
|---|---|---|
| clip | control | buffer denso inmutable de L frames, decodificado con `wm::decodeWavToRate` (el mismo camino que `importTrack`), recortado con `wm::clampLoopRegion` y normalizado (`normalizeSongClip`). Cuenta contra el presupuesto del looper |
| agenda | control -> RT | triple buffer wait-free de 3 slots preasignados (tope 1024 secciones). El control escribe la agenda ENTERA y la intercambia con el slot del medio; el RT la adopta al inicio del bloque. Estado por seccion, no deltas |
| voz | RT | una por pista: los tres smoothers de `TrackBuffer::mixInto` (volumen, mute, paneo) y un carril de cola para la adopcion tardia |
| liberacion | control | sacar un clip de la agenda no lo libera: el RT publica al final de cada bloque que agenda adopto y que clips tiene en uso; el control libera cuando esa foto es posterior a la baja y el clip no figura. Offline libera de inmediato |

El mutex de la cancion es propio. Nunca se toma el del motor para decodificar.

## Fundido

F = max(64, round(0,005 * SR)) en `[B - F, B)` antes de cada borde B: equal-power
(`cos`/`sin` de un cuarto de onda) para cambio de clip o salto de fase, coseno elevado para entrar
o salir de silencio (incluye el inicio en `start_0` y el fin en `end_frame`), nada si el mismo clip
sigue alineado. El estado de B rige exacto desde B: el downbeat entra en B a ganancia plena y la
pre-entrada lee la COLA del clip nuevo. Por eso un clip tiene que ser un loop sin costura.

**Adopcion tardia** (la agenda nueva cambia lo que suena en el frame de adopcion): fundido en
`[t, t + F)` desde el estado viejo y `late_adoptions++`. Editar una seccion futura no cuenta. Si se
adopta dos veces dentro de F, la segunda arranca desde el estado estable del viejo, no desde el
fundido en curso (un click posible, no medido).

## Cableado

- El frame del bloque se toma UNA vez en `AudioEngine::applyEffectsAndLooper` (y en el camino USB)
  y se pasa a `AudioLooper::beginBlock`, antes de `mixFxTracks()` y `process()`: las dos pasadas
  ven la misma fase. Sin `beginBlock` (los tests que llaman a `process()` solos) la cancion no suena.
- Con la cancion activa, `mixTracksInto` llama a la voz de la pista y se salta `mixInto`; volumen,
  paneo, mute y `sendToFx` del usuario valen. No se actualizan el pico ni el progreso por pista.
- `wma_song_set_schedule` habilita el looper. No hace falta que este "playing".
- Grabar/armar con la cancion activa no hace nada (las `wma_looper_arm_*` devuelven -1) y
  `set_schedule` con una grabacion armada o en curso da `INVALID_OPERATION`.
- Un cambio de sample rate del motor invalida los clips (estan resampleados al viejo) y apaga la
  agenda. En un handle offline: `wma_offline_start` PRIMERO, los clips despues.
- Offline: `P0 = wma_transport_get_play_frame(e)` tras `wma_offline_start`; la agenda va en
  `P0 + offsets`. `wma_offline_start` reinicia las voces (smoothers, colas) pero no la agenda ni el
  play frame.

## Lo medido (host macOS, build Debug, 2026-10-27)

- Detector de S2 (`RenderChecks.h`): umbral `kJumpRatio = 2,0` con piso `kJumpFloor = 0,005`.
  5 secciones offline por la C API: razon salto/entorno en las 6 uniones, **max 0,96** con los
  fundidos (margen x2,1); con F = 0 (`SongPlayer::setFadeOverride(0)`), **min 35,3** en las 6.
  El detector solo ve ~54 % de los cortes de banda ancha: la prueba fuerte es la envolvente
  analitica muestra a muestra de `test_song_player.cpp` (error < 1e-6, bit-identica entre bloques
  de 1/64/256/4096 con el borde en bloque + 37).
- Rafaga de 1000 publicaciones de 1024 secciones contra el callback (256 frames, periodo 5333 us):
  0 allocations en el hilo RT, 0 agendas desgarradas, 1000 adopciones, max CPU por callback
  ~200 us. Se corre tambien bajo TSan (el tiempo solo se afirma sin sanitizers).

## Pendiente fuera de esta etapa

JNI (`nativeSong*` vivo y `nativeOfflineSong*` por handle), `ISongBridge` en Kotlin y el smoke de
xruns reales en desktop (CoreAudio): ningun test de host los cubre.
