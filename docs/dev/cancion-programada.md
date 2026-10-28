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
| voz | RT | una por pista: tres smoothers (ganancia, paneo y envio; los mismos one-pole que `TrackBuffer::mixInto`, pero con la **mezcla propia de la cancion** desde 2.24.0: `docs/dev/mezcla-y-master.md`) y una pila de hasta `kSongMaxTails` (8) colas para las adopciones tardias |
| liberacion | control | sacar un clip de la agenda no lo libera: el RT publica al final de cada bloque que agenda adopto y que clips tiene en uso (la agenda mas todas las capas de cola); el control libera cuando esa foto es posterior a la baja y el clip no figura. Offline libera en la proxima llamada de control tras un bloque en el que ninguna cola ni la agenda adoptada lo lean; con el motor sin stream, tras el handshake de abajo |

El mutex de la cancion es propio (`mCtl`). Nunca se toma el del motor para decodificar, y
**tampoco se decodifica con `mCtl` tomado**: `load_clip` reserva slot y bytes bajo el lock
(estado `Reserved`), decodifica y hornea afuera, y vuelve a tomarlo para instalar. Las cargas se
serializan entre si (`mLoadSerial`) para que el presupuesto se mida contra lo ya instalado;
`set_schedule`, `unload` y `invalidateForRate` no esperan a ninguna. Si el sample rate cambio
entre la reserva y la instalacion, `invalidateForRate` ya libero la reserva y la carga termina
con `INVALID_OPERATION`.

## Fundido

F = max(64, round(0,005 * SR)) en `[B - F, B)` antes de cada borde B: equal-power
(`cos`/`sin` de un cuarto de onda) para cambio de clip o salto de fase, coseno elevado para entrar
o salir de silencio (incluye el inicio en `start_0` y el fin en `end_frame`), nada si el mismo clip
sigue alineado. El estado de B rige exacto desde B: el downbeat entra en B a ganancia plena y la
pre-entrada lee la COLA del clip nuevo. Por eso un clip tiene que ser un loop sin costura.

**Adopcion tardia** (la agenda nueva cambia lo que suena en el frame de adopcion `t`):
`late_adoptions++` y fundido en `[t, t + F)`. La comparacion es de lo que SUENA en `t`
(`songSegmentsAlikeAt`): clip y fase de la seccion, y, si el fundido de borde ya empezo (`t >
B - F`), tambien la siguiente, el tipo y el inicio del fundido. Cambiar la seccion siguiente con
`t` dentro de `[B - F, B)`, mover el borde de modo que `t` caiga en su fundido o un `play` cuyo
`start_0` queda a menos de `F` del frame de adopcion cuentan y se funden. Editar una seccion
futura que todavia no suena no cuenta: lo que difiere entra desde ganancia 0 por la agenda misma.

La cola es el **tramo viejo congelado** (`SongTail`: `cur`, `nxt`, bordes, `F` de la agenda
vieja), no un clip: renderiza exactamente lo que habria renderizado la agenda vieja (cur estable,
la mezcla del fundido de borde, nxt estable desde el borde) a ganancia `gOut` mientras lo nuevo
entra con `gIn` (equal-power si suenan los dos lados, coseno elevado si no). Dentro de la
ventana, el tramo viejo nunca pasa del borde siguiente mas `F` (las secciones miden >= 2F), asi que
un tramo basta.

**Colas encadenadas.** Una segunda adopcion dentro de la ventana de la primera apila otra capa.
Con eventos `0..n-1` (`gOut_k`, `gIn_k`) y la capa `j` retirada por el evento `j`:
`salida = vivo * gIn_{n-1} + sum_j capa_j * gIn_{j-1} * prod_{k>=j} gOut_k` (`gIn_{-1} = 1`). Una
capa muere cuando termina cualquier evento `k >= j`. El tope es 8; con la pila llena cae la mas
vieja (su peso ya es el producto de 8 `gOut`: inaudible salvo adopciones de a pocos frames, que
un control humano no genera). No aloca: es un arreglo fijo por voz.

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
  `P0 + offsets`. `wma_offline_start` deja el estado canonico de una instancia fresca: reinicia las voces
  (smoothers, colas), baja `mScrub`/`mSkipTailOnce` y APAGA la agenda (`mCur` vacia, tambien la
  publicacion pendiente de antes del start); los clips y el play frame no se tocan. La agenda se
  publica DESPUES del start: asi el arranque depende solo de las llamadas desde el start, no de
  dirty/scrub de la vida anterior (exportar dos veces desde el mismo handle da los mismos bits que
  una instancia fresca, tambien a 44,1 kHz).

## Clips retirados con el motor parado

Con el stream parado el RT no avanza la foto de lo que usa, y cada `unload` quedaba retenido:
a los 64 ciclos `load` daba `PARAMETER_OUT_OF_RANGE`. El motor pasa una **pista** (`rtStopped`:
`EngineState::Stopped`, y `start()` pasa a Running antes de abrir el backend) y el `SongPlayer`
la **verifica** con un handshake Dekker, todo `seq_cst`: el control levanta `mFreeze` y mira
`mRtInBlock`; el RT sube `mRtInBlock` en `beginSongBlock` y mira `mFreeze`. O el control ve el
bloque abierto (no libera: queda el camino diferido) o el RT ve el congelamiento (ese bloque
no lee ningun clip: silencio de cancion un bloque, el precio de una carrera con el arranque).
Al liberar se levanta `mScrub`: las colas del RT pueden apuntar a ids que se reusan, asi que el
RT las descarta y la adopcion de ESE bloque no congela cola (se pierde ese fundido, una vez); si
ese bloque no adopta nada la bandera se apaga igual y no se come la edicion siguiente.
Un cambio de sample rate hace lo mismo. Offline (`immediate`) no espera la foto del RT, pero tampoco libera un clip
que la agenda adoptada o una capa de cola todavia leen (`mInUseMask`): queda retirado hasta un bloque
en que nadie lo lea. Un `unload`+`load` sin render en el medio cerca del tope puede dar
MEMORY_BUDGET / PARAMETER_OUT_OF_RANGE.

## Apagar la agenda

Apagar (`count = 0`) funde la cancion a silencio en `F`, pero las pistas vuelven al `TrackBuffer`
de golpe: una toma que estaba sonando en esa pista entra a ganancia plena cuando termina la cola
(posible click). No se arreglo: haria falta mezclar `mixInto` en un scratch con rampa de entrada
y NoisyPad Studio no mezcla tomas con cancion. Esta documentado tambien en `ISongBridge` y en el
header de la C API.

## `get_section`

`wma_song_get_section` es el indice en la agenda **adoptada** por el callback (se publica al
cerrar el bloque), que puede ir un bloque atras de la ultima publicada: tras reprogramar, sigue
siendo el de la agenda vieja hasta la adopcion.

## Lo medido (host macOS, build Debug, 2026-10-27)

- Detector de S2 (`RenderChecks.h`): umbral `kJumpRatio = 2,0` con piso `kJumpFloor = 0,005`.
  5 secciones offline por la C API: razon salto/entorno en las 6 uniones, **max 0,96** con los
  fundidos (margen x2,1); con F = 0 (`SongPlayer::setFadeOverride(0)`), **min 35,3** en las 6.
  El detector solo ve ~54 % de los cortes de banda ancha: la prueba fuerte es la envolvente
  analitica muestra a muestra de `test_song_player.cpp` (error < 1e-6, bit-identica entre bloques
  de 1/64/256/4096 con el borde en bloque + 37).
- Reprogramar dentro de un fundido (AC-055.13), DC por pista, bloques de 64, F = 240; salto maximo
  entre muestras en +-300 frames de `t` (un fundido normal de +-0,5: 0,0033). Antes -> despues:
  S1 siguiente clip con `t = B - 128`: 0,357 -> 0,0058; S2 clip que suena en `[B - F, B)`:
  0,325 -> 0,0050; S3 dos ediciones separadas por 64 frames: 0,537 -> 0,0031; S4 borde movido para
  que `t` caiga en `[B' - F, B')`: 0,328 -> 0,0050; `play` con `start_0 = t + 64`: 0,296 -> menos
  que el fundido de entrada normal. Los tests exigen <= 2x el fundido normal y la salida es
  bit-identica entre bloques de 32/64/128.
- Rafaga de 1000 publicaciones de 1024 secciones contra el callback (256 frames, periodo 5333 us):
  0 allocations en el hilo RT, 0 agendas desgarradas, 1000 adopciones, max CPU por callback
  ~200 us. Se corre tambien bajo TSan (el tiempo solo se afirma sin sanitizers).

## Pendiente fuera de esta etapa

JNI (`nativeSong*` vivo y `nativeOfflineSong*` por handle), `ISongBridge` en Kotlin y el smoke de
xruns reales en desktop (CoreAudio): ningun test de host los cubre.
