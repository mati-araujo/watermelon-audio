---
title: "Aviso a NoisyPad — bump a 2.22.0"
type: reference
status: draft
created: 2026-10-02
released: 2026-10-05
---

# Aviso a NoisyPad — 2.21.0 → 2.22.0

**watermelon-audio → NoisyPad.** 2.22.0 junta cuatro objetivos: **REQ-047** (actualización de
dependencias: libusb 1.0.30, AGP 9.4.1 / SDK 37, NDK r30 + Oboe 1.11), **MINI-036** (el `.so` de
release por fin sale a `-O3` con ThinLTO), **REQ-050** (el camino USB dice la verdad) y **REQ-052** (el dual touch: nivel constante por voz y
release del dedo que se levanta). Además vienen
MINI-033, MINI-034 y MINI-035 (tres arreglos chicos del motor) y MINI-042 (las stats USB después de un
desenchufe).

> **Publicada el 2026-10-05**: tag `v2.22.0` sobre `4893591` (PR de release #388). Verificada **contra el
> registro de GitHub Packages**, no por el log: el POM de 2.22.0 responde en las 4 coordenadas (`audio`,
> `audio-android`, `audio-iosarm64`, `audio-iossimulatorarm64`), con 2.21.0 como control positivo y
> 9.9.9 como control negativo (404).
>
> **SIN ENVIAR.** Lo envía el humano y se marca acá.
> Medido: NoisyPad `db7145ca` compila con **0 errores** contra las ramas de REQ-050 S2 y S3 y contra
> master. Compilamos en un worktree descartable contra `mavenLocal` y no editamos nada de ustedes.

## 1 · Antes de subir: requisitos de consumo

| Qué | Antes | Ahora | ¿Les pega? |
|---|---|---|---|
| `minCompileSdk` del AAR | 36 | **37** | No: ustedes compilan con 37 (`libs.versions.toml:4`). Un consumidor con 36 no compila. |
| `minCompileMinorSdk` | (no estaba) | 0 | No: la escribe AGP 9.4.1 y 0 no exige nada. Lo medimos con AGP 9.2.1. |
| `core-ktx` transitivo | 1.18.0 | 1.19.1 | Sólo si fijan otra versión. |
| `oboe` transitivo / `liboboe.so` | 1.10.0 | 1.11.0 | No hay código suyo que cambiar. |
| `libc++_shared.so` | NDK r27 | NDK r30 | Les llega sólo en nuestro AAR. Crece entre 13 y 17 % por ABI. |
| Manifest | — | `uses-permission WAKE_LOCK` | Se suma a su app por el merge del manifest. Arregla la `SecurityException` del streaming USB (AC-050.7). |
| Tamaño del AAR | 5 447 939 B | **6 077 886 B (+11,6 %)** | El `.so` de -O3 + LTO y el `libc++` de r30. Medido sobre los dos AAR del registro. |

## 2 · Cambios de contrato (lo que conviene revisar)

### 2.1 Parar el motor: la variante `suspend` ahora espera
`stopEngine` / `stopEngineWithFade` (`suspend`) y `AudioEngine.stop` devuelven `success` **cuando el
motor nativo ya llegó a Stopped**. Si no llega dentro del techo, devuelven
`NativeBridgeException.Timeout`. Las `*Sync` no cambian: siguen devolviendo "aceptado".

Además, un segundo `stop` durante el fade ya no reinicia la rampa desde 1.0. Antes, la segunda
llamada volvía a subir el volumen.

**Sugerencia, no obligatoria:** `AudioEngineStateManager.stopAudio` (`:354`) llama
`stopEngineWithFadeSync` y después simula el fade con `delay(fadeTimeMs)` (`:360`). Si migran a
`stopEngineWithFade` (`suspend`), el `Result` dice cuándo terminó de verdad y el `delay` sobra. Hoy
no les rompe nada.

### 2.2 USB: errores tipados (`UsbAudioError`)
Son tres entradas nuevas:
- `NO_ENGINE`: no hay motor;
- `NO_AUDIO_CALLBACK`: el motor existe pero no tiene el callback instalado;
- `DEVICE_BUSY`: `connectDevice` con otro device mientras uno está conectado.

Antes, los tres casos salían como `STREAMING_ERROR` genérico, o como un éxito que no era tal. Un
`when` exhaustivo sobre `UsbAudioError` sin `else` deja de compilar. No encontramos ninguno en su
árbol.

### 2.3 USB: `connectDevice`
- **Crea el motor si no existe** (o si está parado), instala el callback y activa el modo
  BackendManager **antes** de inicializar el device. Antes se podía quedar en Oboe reportando éxito.
- Si el motor ya existe y está corriendo, **no lo toca**.
- **Se serializa:**
  - un segundo `connectDevice` del mismo device ya conectado devuelve éxito sin tocar el stream;
  - otro device devuelve `DEVICE_BUSY`.
- **La espera del permiso no tiene techo**: la corta quien llama, cancelando la corrutina, y cancelar
  libera la conexión.
- Si el connect falla después de preparar el motor, el motor **queda en modo BackendManager**: la
  librería no sabe en qué modo estaba antes. Está en el KDoc.
- `disconnectDevice` **no** se serializa con `connectDevice`.

### 2.4 USB: el permiso
El resultado del diálogo de permiso sale de `UsbManager.hasPermission(device)`, no del extra del
intent, y el receiver se registra `RECEIVER_NOT_EXPORTED`. Otra app ya no puede falsear un
`PermissionGranted`. No hay cambio de API.

### 2.5 USB: el test runner (`UsbAudioTestRunner`)
- `UsbTestStatus.NOT_APPLICABLE` es nuevo: aparece cuando el device no ofrece el rate o los bits de
  la fila. Su `when` de `UsbAudioScreen.kt:1753` tiene `else`, así que compila y lo muestra con el
  ícono `Info`. Si quieren, denle un color propio.
- **Cada fila se mide a su rate y bits.** Si el stream corre en otra config, el runner lo cierra, lo
  reabre a la de la fila y después lo restaura. **Se oye**: en la CM720, sin selección manual de
  altsetting, son tres cortes por suite. El resultado de la restauración viene en
  `UsbTestResult.streamRestore`.
- **Un rate coercionado da `FAILED`** ("ofrecía X, quedó en Y").
- **El techo de latencia** deja de ser 20 ms fijos y pasa a ser el que declara el backend
  (`UsbTestResult.latencyLimitMs`, `UsbTransferStats.declaredMaxLatencyMs`). En la CM720 a 48 kHz
  son 61,33 ms, y el medido fue 37,4.
  - El default de `UsbTestConfig.maxAllowedLatencyMs` pasa a +∞.
  - `LOW_LATENCY` y `HIGH_QUALITY` pierden su techo fijo (10 y 30 ms), que fallaban por construcción.
- **`successRate` descuenta lo que está en vuelo**, y sin paquetes resueltos da 0, no 100.
  - Su `UsbDebugOverlay.kt:180` calcula su propio éxito con `packetsSubmitted`. Ahí siguen contando
    como perdidos los paquetes en vuelo (`UsbTransferStats.packetsInFlight`, 192 en la CM720). Si
    quieren que coincida con el runner, réstenlos.
- **Campos nuevos, todos con default:**
  - `UsbTestResult`: `packetsInFlight`, `latencyLimitMs`, `streamSampleRateHz`, `streamRestore`,
    `streamRestoreMessage`;
  - `UsbTransferStats`: `packetsInFlight`, `declaredMaxLatencyMs`;
  - `UsbTestReport`: `notApplicableCount`.
- **Nuevo** `UsbStreamSelection`: los centinelas para volver a la selección automática de altsetting
  y reloj.

### 2.6 Motor (MINI-033/034/035)
- Un sample rate no positivo ya no prepara el motor.
- El rate que lee el hilo RT sale de un atomic, sin tomar dos mutex.
- `removeEffect` le pregunta al motor antes de rechazar.

### 2.7 Dual touch (REQ-052): cada dedo a su nivel, y el que se levanta hace release
**Cierra su D35, AC-047.2 y AC-047.3 para los engines no SoundFont** (Classic, Karplus-Strong, FM,
Wavetable, Granular, Supersaw). **No tienen que tocar código:** la API es la misma, y
`AudioEngineStateManager.updateDualTouch` ya deja cada dedo en su slot y le manda al que se levanta
`amp = 0` con su última frecuencia, que es lo que el motor necesita.

**Cambio de comportamiento con el mismo valor de enum.** `SUM` (0) y `AVERAGE` (1, el default y el que
usan) **pasan a ser la misma ley: suma a ganancia 1 por voz**, igual que un dedo solo. Antes los dos
multiplicaban la suma por 0,5: al apoyar el 2.º dedo el 1.º bajaba ~6 dB, y al levantar uno el que
quedaba subía ~6 dB. `AVERAGE` conserva el nombre por compatibilidad. El pico de la suma lo cuida el
limitador de salida que ya existe (−0,5 dBFS), que sólo actúa cuando los dos van fuertes.

**La envolvente por dedo**, en los seis modos:
- el dedo que entra lleva un ataque de ~5 ms;
- el que se levanta sigue sonando con su última frecuencia y amplitud bajo un release exponencial de
  ~80 ms a −60 dB, y recién después su engine deja de procesarse;
- si el dedo vuelve durante el release, sube desde donde estaba, sin re-ataque ni reset del engine;
- el **último** dedo también hace release, y después el instrumento queda en silencio.

`MAX`, `CROSSFADE`, `RING` y `AMPLITUDE_BALANCED` ganan la envolvente y conservan su ley con los dos
dedos. Con **un solo** dedo, cualquier modo da ese dedo a ganancia 1, como antes.

**Medido en host** (render offline del motor completo, banda de cada voz con Goertzel en ventanas de
20 ms, 1→2→1→0 en los dos órdenes de salida):

| Qué | Antes | 2.22.0 |
|---|---|---|
| Nivel de la voz que ya sonaba / que queda | −6,04 a −6,11 dB | peor \|Δ\| 0,007 dB, en los seis engines |
| Dedo que se levanta | corte en seco | −21,6 a −27,5 dB a los 20–40 ms; ≤ −66,7 dB a los 80–100 ms (KS es el peor) |
| Salto máximo entre muestras en ataque, release y re-toque (Classic seno) | — | ≤ 1,003× el del régimen: sin click |
| Pico con dos voces a amp 1 | — | 0,8735 (KS: 0,43–0,51), todo finito |
| CPU con dos dedos en régimen | — | sin cambio medible (pareado contra master) |

**Lo que no cambia:**
- SoundFont: su dual touch va por canales y AC-047.2/.3 ya se cumplían ahí;
- el camino de un dedo (`renderSingleTouch`), el voice system y el arpegiador.

**Hueco declarado:** si el modo pasa de dual a single **dentro** de los ~80 ms del release, el release
se corta. Ustedes dejan el modo dual puesto hasta el primer update del gesto siguiente, así que hoy no
les pega.

**Cómo verificarlo en el g42:** con el canal de debug, `export` a WAV de un gesto 1→2→1 en los dos
órdenes (sale primero el dedo principal y, en otra toma, el otro), con un engine no SoundFont. La voz
que queda no cambia de nivel al entrar ni al salir la otra, y el dedo que sale se apaga en ~80 ms sin
click.

## 3 · Seguridad
- **libusb 1.0.30**: cierra CVE-2026-47104 y CVE-2026-23679. Nuestro parser no alcanzaba esos
  caminos, pero el bump vale igual.
- **Permiso USB**: ver §2.4.
- El security-auditor revisó REQ-050: **sin hallazgos críticos ni altos**, y ninguna app ajena tiene
  un vector. Encontró un defecto **anterior**: si el DAC se desconecta (o se reabre el stream) mientras
  se leen las stats USB, el proceso puede caerse con SIGSEGV. Lo arregla **MINI-042** (#385), que entra en 2.22.0,
  junto con una carrera chica de `requestPermission` contra
  `connectDevice`.
- El security-auditor revisó REQ-052 (dual touch): **sin hallazgos críticos, altos ni medios**, y sin
  superficie nueva. Encontró uno **bajo**: un `+Inf` en la amplitud o la frecuencia de un dedo
  (`setDualTouch`) quedaba retenido durante el release y podía volver NaN la salida por ~80 ms. Ya
  venía de antes, pero la retención lo alargaba. **Quedó arreglado en este mismo REQ**: un valor no
  finito no se retiene, y el release sigue en la última frecuencia válida.
- Hueco que queda declarado: llamar a `connectDevice` y `disconnectDevice` **a la vez desde hilos
  distintos** no está protegido. Si los llaman desde Main, como hoy, no les pega.

## 4 · Cómo lo verificamos
- **Smoke en su g42 con la UGREEN CM720** (`smoke-20261001-181848-74184`): 40 PASS, 0 FAIL. Las tres
  filas de la suite se midieron a 48 000, 44 100 y 96 000 Hz, y las tres restauraron el stream.
- La diferencia del artefacto contra 2.21.0 del registro está declarada línea por línea, con su
  razón, en `scripts/published-artifact-vs-2.21.0.txt`.
- MINI-042, verificado en su g42 con la CM720 (`smoke-20261002-140136-87994`): 42 PASS, 0 FAIL, con las
  mismas tres filas de la suite a su rate.
- REQ-052: 187 casos de host nuevos (`test_dual_touch_envelope.cpp`). Cada umbral tiene un mutante
  que lo pone en rojo (volver a ×0,5, sacar el release, sacar el ataque, re-tocar desde 0, entre otros).
- Smoke en su g42 con el harness de `v2.22.0` (05/10): salida, captura, `.sf2` y `.sf3`
  **20 PASS / 0 FAIL** (`smoke-20261005-143206-61362`; los picos del A4 coinciden con el build de
  REQ-047 S3), y USB con la CM720 **22 PASS / 0 FAIL** (`smoke-20261005-144548-66991`: las tres filas
  de la suite a 48 000, 44 100 y 96 000 Hz, 100 % de paquetes, 0 errores, ~37 ms).
- **Lo que no verificamos de oído, y por qué.** El chequeo de que el USB *suena* no se pudo hacer: el
  stream de prueba lleva la salida del motor parado, o sea silencio, y la librería nunca generó un
  tono de 440 Hz en ese camino. Ojo con su «Test Tone» de `UsbAudioViewModel`: asume que
  `startStreaming` genera un seno, y no lo hace. El afinador en el harness tampoco quedó verificado.
  Lo vamos a cubrir con pruebas en device de setup declarado, no con un chequeo suelto.
- **Una condición del entorno que conviene que conozcan.** Si el sistema de audio de Android (o otra
  app) ya tiene tomada la placa USB, `startStreaming` falla con `STREAMING_ERROR` y en el log aparece
  `libusb_claim_interface(0) … LIBUSB_ERROR_BUSY`. Nos pasó con NoisyPad abierto y la placa como
  `usb_headset`; cerrando NoisyPad y reenchufando, pasó entero. No es nuevo de 2.22.0.
