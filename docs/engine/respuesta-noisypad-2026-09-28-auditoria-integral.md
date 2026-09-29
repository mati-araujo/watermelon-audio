---
title: "Respuesta a NoisyPad — las cartas de la auditoría integral 2026-09 (#348–#364)"
type: reference
status: current
created: 2026-09-28
---

# Respuesta a NoisyPad — 2026-09-28 · las 25 cartas de la auditoría integral

**watermelon-audio → NoisyPad.** Contesta `docs/audit/auditoria_integral_2026-09/plan/cartas-motor.md`
(issues #348–#364), carta por carta, contra master **2.20.0**. Ustedes pinnean 2.19.2; todo lo que se
dice "ya existe" existía también en 2.19.2 salvo que se aclare.

> **Redactado el 2026-09-28, actualizado el 2026-09-29 con lo que REQ-045 entregó. SIN ENVIAR**: lo
> envía el humano y se marca acá. REQ-045 sale en **`v2.21.0`, verificada en el registro, 4/4
> coordenadas** (audio, audio-android, audio-iosarm64, audio-iossimulatorarm64), Publish run
> 36573315788; PRs #365, #366, #367 y el de release #368. Lo marcado ❓ es pregunta para ustedes.
> **Antes de subir de versión, lean §4**: hay nueve líneas suyas que dejan de compilar, con su arreglo.

## 0 · Lo más importante: su auditoría encontró defectos nuestros que no podía ver

Su anexo D dice que el motor "no estaba disponible" y marcó `PLAUSIBLE` lo que dependía de él.
Contrastado con el código, **varias cosas que ustedes mitigan son defectos de la librería**, y los
arreglamos nosotros en **REQ-045 — lo que la librería dice que hizo**:

| Qué | Dónde les pega | Carta |
|---|---|---|
| En Android, `start/stop/pause/resumeEngineWithFade` (las `suspend`) devuelven `Result.success` **siempre**, aunque el stream no abra. iOS sí propaga el fallo. | Si migran a las `suspend` hoy, el `Result` les miente | W1 |
| `AudioEngine.start/stop/pause/resume` usan los `*Sync` sin mutex y publican `RUNNING` aunque el nativo falle. | ENG-2/ENG-14 | W1 |
| **50** configuraciones (`setBpm`, arp, vocoder, noise gate, dual-touch, pistas del looper…) son **no-op mudos** si el motor todavía no se creó. Lo medimos: eran más que los ~36 que estimábamos. Su lectura de ENG-7 era correcta. Al arreglarlo aparecieron **tres más** que van por otro camino (modo USB, config del backend USB y perfil de latencia USB) y **seis** de entrada que necesitaban el nodo de entrada, no sólo el motor. | ENG-7 (familia #190) | W2 |
| `setModulatorType` rechaza ids fuera de 0..7 y Kotlin descarta el rechazo. `setUsbStreamingMode` descarta el resultado de la captura. | BUG-016, BUG-008 | W11 |
| `setCapabilities(0, …)` **resetea** a los defaults, cuando el doc dice "0 = no tocar". A ustedes no les pega hoy, porque pasan los tres valores. | — | WA-8 |
| `saveUndo` devuelve `true` siempre, y cada llamada reserva más pool. | MEM-N5 | WA-2 |
| `importTrack` decodifica y resamplea **antes** de mirar el presupuesto (≈ 221 MB para un WAV de 5 min antes de rechazar) y, si falla la asignación, **ya borró la pista destino**. | MEM-N6 | WA-4 |
| `StreamInfo.channelCount` e `isLowLatency` son defaults, no medidos. | ENG-8 | W4 |

Además, dos cosas del SoundFont que afectan su **descarga diferida** (T2.05), para que las tengan en
cuenta antes de medir M-E6:

- **`unloadSoundFont()` puede no liberar.** Si cae mientras el audio está renderizando, el font queda
  retenido hasta el próximo load/unload: nada llama la limpieza pendiente. Va a REQ-046.
- **Posible cruce entre fonts.** Tras `unload` + `load`, si el allocator reusa la dirección, los 16
  canales pueden no reconfigurarse. Es un fix de una línea y sale suelto, antes.

## 1 · Ya existe, o lo resuelven ustedes con lo que hay

| Carta | Respuesta |
|---|---|
| **W4** | Sample rate y buffer: sí, `getStreamInfoArray()` lee el **stream abierto** al retornar `start()`. **La latencia NO**, y lo medimos en su g42 el 28/09: en el camino Oboe directo se calcula por llamada y justo después de `start` da `-1` (todavía no hay timestamps). Además nuestro `AudioEngine.state.streamInfo` la leía una sola vez, así que quedaba en `-1` para siempre. En REQ-045 pasa a **ausente** (`null`) hasta que se pueda medir, y se refresca en el polling. Para su telemetría (T3.10): tomen la latencia del **primer poll con valor**, no de `start`, y nunca registren un `-1`. Otra cosa: su `AudioEngineStateManager.kt:1633-1639` arma `StreamInfo` con `channelCount = 2` / `isLowLatency = true` fijos. El array ahora trae 5 valores (canales y low-latency medidos, `-1` = desconocido): léanlos de ahí. |
| **W-CHORD** | `ChordGenerator.generateChordFrequenciesInto(out, …): Int` y `generateChordMidiNotesInto` existen desde **v2.4.0**. El comentario de `XyVoiceStateHolder.kt:653` está desactualizado. ⚠️ `updateChordNotes` usa `frequencies.size`, así que el buffer tiene que medir **exactamente** el acorde: si sobran lugares, suenan notas viejas. Usen un buffer por tamaño de acorde. Ningún bridge retiene el array: su "alloc-ok" era prudente pero innecesario. |
| **W8** (entrada) | `AudioInputFactory` → `AudioInput.metering()` / `meteringFlow(intervalMs)`, en commonMain, Android e iOS. Da los 7 valores (L/R dB y lineal, clip, gate, latencia) en **un** cruce. `InputAudioControllerAdapter` hoy hace 4 por canal. Lo que falta de nuestro lado (variante con buffer del llamador, `getOutputLevels` en común) va a un REQ posterior. |
| **W12** | `getWaveformSamples(buffer, size)` ya escribe en **su** buffer, sin asignar. La asignación por tick es el `copyOf` de `WaveformProviderAdapter.kt:75`. |
| **W3** (parte) | `AudioEngine.state: StateFlow<AudioState>` trae lifecycle, pausa, fade, error de stream y `streamInfo`. Un colector reemplaza los ~11 getters. Push nativo real no va a haber: el hilo de audio no entra a Kotlin. Un snapshot en batch es posible, pero después de REQ-045. |
| **W11** (doc) | `EffectManagerFactory.create` y `ModeTransitionFactory.create` crean instancias Kotlin **independientes**, sin estado en `companion`. Comparten solo el bridge y el motor nativo. Dos `EffectManager` ven **la misma cadena** con cachés Kotlin separadas que pueden divergir. Su T3.09 (una instancia de alcance app) es lo correcto. |
| **WA-5.1 / 5.5** | Muestras en RAM: **float32**, en SF2 y en SF3. El SF2 int16 se convierte al cargar y el SF3 se decodifica **entero** con stb_vorbis. Huella ≈ 4 B × muestras: 2× el chunk `smpl` de un SF2, y el PCM completo de un SF3. El mmap es **transitorio** (`munmap` después del parseo). No hay decode por preset. |
| **WA-5.2** | Hoy **no**: se parsea B y recién después se retira A, así que el pico es A + B (más el transitorio de conversión). Para llegar a "≤ max(A, B)" hay que soltar A antes de parsear B, y eso es **silencio durante la carga**. ❓ ¿Lo quieren así? Mientras tanto, un `unloadSoundFont()` explícito antes del load les da ese pico, con la salvedad de §0. |
| **WA-5.3** | El swap es seguro con el audio corriendo: hazard pointer de un lector y sin UAF, con tests bajo audio. La salvedad es la de §0: el unload puede diferir la liberación. |
| **WA-5.6** | `AudioNativeBridge.loadSoundFontFromFd(fd, offset, length)` existe en Android (alineado a página). No está en `ISoundFontBridge` a propósito, porque iOS no tiene fd de asset. Mapea y **copia**, así que no es zero-copy persistente. Les ahorra la copia en la JVM, no la huella. |
| **WA-5.7** | Confirmado: `mSoundFontEngine2` y `mSFPool` no existen. Hay un solo manager y un solo engine. |
| **WA-1** | Float32 estéreo intercalado, en chunks de 32768 frames (256 KB). `prepareTrack` **pre-asigna la toma entera** (no a demanda). `trimTrack` devuelve los chunks al allocator (`delete`) y baja el contador; conserva 2 chunks por pista. Que el SO recupere las páginas depende del allocator: no lo medimos con RSS. |
| **WA-6** | El export renderiza a **un buffer entero** (`frames × 8 B`), más el vector de picos del limitador y, si cambia el rate, un buffer resampleado completo. El transcoder AAC **sí** es streaming (chunks de 1024), pero parte de un WAV temporal completo, así que el pico es el del render. Render por bloques: diferido. |
| **W-UJD** | De los 12 hallazgos de `AUDIT_UI_JNI_DATAFLOW.md`, 11 están corregidos o ya no aplican; la mayoría ya estaba corregida al extraer el motor. Sigue BUG-008 (setters de oscilador y modulador sin resultado), que va a REQ-045. |

## 2 · Lo que se hace, y en qué orden

| Tema | Cartas | Dónde |
|---|---|---|
| Resultados honestos | W1, W2, W4 (parte), W11, WA-2 y WA-4 (lo que devuelve "no"), WA-8 (el reset) | **REQ-045**, primero: coincide con su "Alta (E2)" |
| Memoria medible y liberable | WA-5.4 (`sfGetMemoryBytes`), WA-3, WA-2 (`discardUndo`, `undoBytes`), WA-7/WA-9, WA-8 (bajar bajo presión) | REQ-046, después |
| Superficie pública | W13, W10, W8 (salida y buffer del llamador), W7 | después |
| Eventos del looper | W-RESUME, W9 | después. ❓ Antes de diseñar, necesitamos una respuesta: al reanudar en la barra, ¿la pista retoma **donde quedó** o **alineada a la referencia**? |
| Escena atómica | W5, W6 | último: es diseño nuevo en el hilo de audio |

**W13 en particular:** el `@RequiresOptIn` "de contrato estable" que piden no existe acá. Nuestro
`@InternalWatermelonApi` es de nivel ERROR y **sin** contrato, a propósito. Lo que corresponde es una
**puerta pública**. Casi todo lo que importan de `internal` es `public` en un paquete llamado
`internal`, así que promoverlo es barato. La excepción es `AudioNativeBridge`: de sus ~318 funciones,
ustedes usan 6 que no están en la interfaz (`getDetailedLatencyInfo`, `looperExportMixCompressed`,
`getOutputLevels`, `setUsbOutputVolume`/`getUsbOutputVolume`, `isUsbDeviceInitialized`). Se promueven
esas 6, no la clase entera. Dos notas más: `getAudioBridge`, `AudioSessionEvent` y `RouteChangeReason`
no estaban en su lista y también los importan. Y los imports del ratchet están en **5** módulos, no en 6.

**Diferido o no:** decode por preset (WA-5.5), render y decode por bloques (WA-6, WA-4 en su parte de
bloques), push nativo de lifecycle (W3).

## 3 · Preguntas para ustedes

1. **Modo USB 1 (`CAPTURE_ONLY`)**: hasta v2.20.0 se trataba igual que el 0. Desde v2.21.0 se
   **rechaza con resultado** (`Result.failure`, sin tocar el modo vigente), en forma provisional.
   ¿Lo usan? Si lo usan, lo reabrimos: la decisión es reversible.
2. **WA-5.2**: ¿aceptan silencio durante la carga a cambio de un pico ≤ max(A, B)?
3. **W-RESUME**: ¿retoma donde quedó o alineada a la referencia?
4. **W2**: los setters previos al init van a **llegar** (decidido): crean el motor sin abrir el
   stream. Su re-aplicación en `startAudio` (T3.10) sigue siendo inocua. ¿Algún setter que ustedes
   **cuenten** con que se pierda antes del init?
5. **W1, para que lo sepan antes del bump — y esto les pide un cambio**: ustedes no usan
   `AudioEngine`, usan `bridge.start/stop/pause/resumeEngineWithFadeSync`
   (`AudioEngineStateManager.kt:269,363,400,427`, `MainActivity.kt:404`) y publican `RUNNING`
   incondicional. Los `*Sync` **no pueden** devolver resultado sin romperles la compilación: tienen
   tres dobles que los overridean (`NoOpAudioNativeBridge`, `FakeEngineBridge`). Por eso quedan
   `@Deprecated`, con `ReplaceWith` hacia las variantes `suspend …WithFade(fadeMs): Result<Unit>`
   del mismo bridge, que ahora sí dicen la verdad en Android (antes devolvían `success` siempre).
   Y pasan a **loguear** el código que descartan. Migrar a las `suspend` en su `engineControl`
   (T2.01) es lo que cierra W1 de su lado. `AudioEngine.start/stop/pause/resume` también pasan a
   `Result<Unit>`, por si lo adoptan. Con `fadeMs > 0`, `stop` devuelve éxito cuando el motor
   **aceptó** parar; la detención termina asíncrona.
6. **Nullables nuevos (rompen en fuente si hacen aritmética)**: `StreamInfo.channelCount`,
   `isLowLatency` y `latencyMillis` pasan a nullables: `null` = no medido. `saveUndo` puede devolver
   `false`. `setCapabilities(0, …)` ya no resetea. Un import fallido ya no vacía ni mutea la pista,
   salvo un OOM del allocator después de validar, que se reporta como
   `NativeBridgeException.MemoryAllocationFailed`.

## 4 · Al subir de versión: lo que cambia, y las nueve líneas que dejan de compilar

Medido contra su árbol (`38b7bf3e`), no estimado. Es una **minor** por convención, pero con cambios de
fuente para quien **implementa** nuestras interfaces o **reenvía** un setter con cuerpo-expresión.

**Dejan de compilar (nueve líneas, todas con un arreglo de una línea).** Estos miembros de
`IAudioNativeBridge` pasan de `Unit` a `Result<…>` porque ahora dicen cuando el motor rechazó:
`setModulatorType`, `setModulatorParameter`, `removeEffectSync`, `setEffectParameterSync`,
`setEffectBypassSync`, `setEffectsBypassSync`, `reorderEffectsSync` → `Result<Unit>`, y
`setUsbStreamingMode` → `Result<CaptureOutcome>`.

| Dónde | Por qué rompe | Arreglo |
|---|---|---|
| `core-audio-engine/src/commonTest/.../NoOpAudioNativeBridge.kt:87, 88, 89, 90, 94, 95, 97, 138` | overridea los ocho con `Unit` | cambiar el tipo de retorno; el cuerpo `notModeled(...)` puede quedar si devuelve `Nothing` |
| `core-audio-engine/src/commonMain/.../adapter/GuitarAudioControllerAdapter.kt:93` | `override fun setEffectBypassed(...) = bridge.setEffectBypassSync(...)`: el cuerpo-expresión ahora infiere `Result<Unit>` y su interfaz declara `Unit` | cuerpo con llaves, o `.getOrElse { … }` si quieren actuar ante el rechazo (recomendado: hoy lo tiran) |

Los llamadores que usan esos miembros como sentencia **no** cambian: ignorar el `Result` compila.
`AudioEngine.setModulator` / `setModulatorParameter` / `removeEffect` / `setEffectParameter` /
`setEffectBypass` / `setEffectsBypass` / `reorderEffects` también pasan a `Result<Unit>`; ustedes no
implementan `AudioEngine`, así que ahí no rompe nada.

**Cambian de comportamiento sin romper la compilación:**

- Los setters que fallan **dicen que no** (Android e iOS, misma causa para la misma llamada), y ante un
  rechazo no se publica estado ni evento de analytics. `reorderEffects` ya no tira
  `IndexOutOfBoundsException`.
- Las configuraciones previas al init **llegan** (W2). El costo de crear el motor se mueve de `start`
  al **primer setter**, que es sincrónico: si lo llaman desde el main thread antes del arranque, esa
  primera llamada paga la construcción.
- `start/stop/pause/resumeEngineWithFade` (`suspend`) ahora devuelven `failure` cuando el motor falla
  (W1, §3.5). Los `*Sync` del ciclo de vida siguen siendo `Unit` (no les rompen los dobles), quedan
  `@Deprecated` y loguean el código que no pueden devolver.
- `getStreamInfoArray()` trae **5** valores (canales y low-latency medidos, `-1` = desconocido); la
  latencia es `-1` hasta que haya timestamps (W4). Su `StreamInfo` es propio, así que nuestros nullables
  no les pegan: sólo dejen de fijar `channelCount = 2` / `isLowLatency = true`.
- `saveUndo` puede devolver `false`; `setCapabilities(0, …)` ya no resetea; un import fallido ya no
  vacía la pista. Para la causa del import: `looperImportTrackResult(...)`, nuevo en `ILooperBridge`
  con implementación por defecto (no rompe dobles).

