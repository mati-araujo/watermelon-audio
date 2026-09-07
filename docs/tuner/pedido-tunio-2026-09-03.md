---
title: "Respuesta a upstream: candidatesHz sirve, pero no llega a donde vive el defecto"
type: reference
status: current
created: 2026-09-03
supersedes: pedido-watermelon-audio-2026-09-03.md
---

> **Cómo usar esto**: pegalo como prompt de arranque en una sesión sobre el repo `watermelon-audio`.
> Fusiona la respuesta a `docs/tuner/respuesta-tunio-2026-09-02.md` con el addendum del
> subarmónico, que quedaba sin mandar. Reemplaza a `pedido-watermelon-audio-2026-09-03.md`.

---

# Respuesta a la respuesta — y un hallazgo sobre dónde llega el arreglo

**2026-09-03 · sobre 2.15.0 y el PR #252 sin mergear · Tunio → watermelon-audio**

Gracias por la respuesta y por medir el pedido B antes de concederlo. Que lo hayan implementado, lo
hayan encontrado caro **con un número** —seis tests de ausencia, 48,45 Hz en un zumbido— y lo hayan
revertido dejándolo escrito como contrato es mejor para nosotros que si nos hubieran dicho sí: el
`R-PITCH-56` nos ahorra volver a pedirlo dentro de tres meses.

Contestamos sus dos preguntas y después va **un hallazgo que nos parece que cambia el alcance del
PR #252 antes del merge**. Lo pusimos al final porque las dos respuestas son cortas y esto no.

---

## 1. La receta: confirmada, con el `.mid` y el generador que lo produce

**Confirmada tal cual**: GeneralUser GS 2.0.3, banco 0 (MSB y LSB en 0), PC 24 y 25, nota MIDI 55,
5,0 s, velocity 100, sin reverb ni chorus.

Van los `.mid` que pidieron **y el generador que los produce**, en `docs/tuner/adjuntos/`:

```
construir_banco.py          el generador
guitarra-nylon_G3.mid       banco 0 · PC 24 · nota 55   ← la receta que pidieron confirmar
guitarra-acero_G3.mid       banco 0 · PC 25 · nota 55   ← ídem
guitarra-limpia_E4.mid      banco 0 · PC 27 · nota 64   ← el subarmónico del punto 4
guitarra-limpia_G3.mid      banco 0 · PC 27 · nota 55   ← ídem
```

Los `.mid` eliminan la fuente de divergencia que les preocupaba. El generador va igual porque es
mejor a futuro: versionado, determinista y **sin dependencias de Python** —escribe el MIDI a mano,
byte por byte—, así que pueden variarlo (otra nota, otro PC, otro `bend`) sin nosotros de
intermediarios.

```python
midi_nota(path, prog, bank, nota, dur_s=5.0, vel=100, bend=None)
render(sf2, mid, wav, sr=44100)   # fluidsynth -ni -R 0 -C 0 -g 0.9
```

**Verificado antes de prometerlo.** Corrimos el generador y leímos los `.mid` por bytes:

| archivo | bytes relevantes | qué dicen |
|---|---|---|
| `guitarra-nylon_G3.mid` | `c0 18` · `90 37 64` | PC 24 · nota 55, velocity 100 |
| `guitarra-acero_G3.mid` | `c0 19` · `90 37 64` | PC 25 · nota 55, velocity 100 |
| `guitarra-limpia_E4.mid` | `c0 1b` · `90 40 64` | PC 27 · nota 64, velocity 100 |
| todos | `b0 00 00` + `b0 20 00` · `a5 40` | banco 0 MSB/LSB · 4800 ticks = 5,0 s |

Los cuatro difieren **sólo** en los bytes de PC y de nota — ése es el control positivo de que la
receta es la que dice ser y no la que nos acordábamos.

Y el recordatorio que ustedes ya escribieron y nos parece la mitad importante: **la verdad no sale
del nombre del archivo.** El `f0` de cada `.wav` lo medimos después por pendiente de fase sobre cada
parcial; ese SoundFont está desafinado hasta 21,5 cents, así que el nominal miente. Los archivos
donde los parciales no se ponen de acuerdo quedaron marcados como ambiguos y fuera de las
estadísticas.

---

## 2. ¿Nos sirve `candidatesHz`? Sí, y no tenemos ninguna objeción a su forma

La forma es buena: default vacío, append-only, y el punto de entrada anterior sigue existiendo.
Nuestras dos llamadas siguen compilando sin tocarlas — las verificamos, son **dos, y las dos son
tests de iOS**:

```
core-audio/src/iosTest/…/AfinacionOfflineTest.kt:60          (la red de REQ-012)
core-audio/src/iosTest/…/DiagnosticoDeCuerdaAjenaTest.kt:58  (la red de REQ-013)
```

Que sean dos y que sean tests es justamente lo que hace que el próximo punto importe.

---

## 3. 🔴 El hallazgo: el arreglo no llega a donde vive el defecto que reportamos

**El defecto 2 lo medimos en el producto, no en el puerto offline.** Con la app instalada en el
teléfono, la cuerda correcta seleccionada y una cuerda ajena sonando, el afinador no mide. El puerto
offline fue **cómo lo reprodujimos en CI**, no dónde duele.

Y el PR #252 no toca ese camino. Lo medimos por tres vías independientes antes de escribir esto:

| medición | resultado |
|---|---|
| Los 15 archivos del PR #252 (`gh pr view 252 --json files`) | `OfflineTuner.kt`, `ITunerBridge.kt`, los dos bridges, C API, JNI, `OfflineAnalysis.*`. **No** `ITuner.kt`, **no** `TunerFactory.kt`, **no** `TunerImpl.kt` |
| `javap` sobre `audio-release.aar` de 2.15.0 | `OfflineTuner.analyze(float[], int, float, int)` — cuatro params, sin candidatos. Confirma que estamos leyendo la versión sin el PR |
| `grep setTunerCandidates` en `TunerImpl.kt` | **cero matches**. Último commit del archivo: el PR #179 |

### Lo que encontró el `javap`, y es lo que nos sorprendió

**La capacidad de declarar el instrumento ya está en 2.15.0.** No es nueva del PR #252:

```
ITunerBridge.setTunerCandidates(float[])   ← ya existe (modo rápido, REQ-001 S5)
ITunerBridge.lockTunerString(int)          ← ya existe
```

Lo que pasa es que en el camino de la app **nadie la conecta**:

- **`TunerImpl` no la llama.** Recibe la `TuningConfiguration` completa, deriva `targets` con las
  seis cuerdas, y al motor le empuja **una sola cosa**: `setTunerTargetHz(desired)`, el objetivo de
  la cuerda seleccionada. Los candidatos los tiene en la mano y no los baja.
- **Un consumidor no puede bajarlos por su cuenta** sin `@InternalWatermelonApi`, que es nivel
  ERROR y que ustedes declaran *"superficie de diagnóstico, no de consumo"*, que *"puede cambiar o
  desaparecer en cualquier versión, incluida una de patch"*. Construir el afinador de un producto
  sobre eso no es una opción que podamos tomar.
- **Y está declarado deliberado**, así que no lo tratamos como un olvido. `ITuner` dice: *"elegir la
  cuerda por proximidad es detección gruesa, y ésa es otra capa. Una implementación que la agregue
  lo hace explícito."* Nos parece bien la decisión. El problema es dónde queda parada.

### Por qué esto nos parece que toca el alcance del PR

Después del merge queda una asimetría al revés de lo natural: **el puerto offline puede declarar el
instrumento y la app no.** Y el puerto offline existe para reproducir lo que la app ve — lo dice su
propio KDoc, y es la frase con la que estamos de acuerdo:

> *"Es el MISMO análisis del camino de tiempo real. No hay una segunda implementación: una medición
> offline que no fuera la del producto daría un verde que no dice nada."*

Si pasamos `candidatesHz` en nuestra red offline, esa red deja de medir lo que la app mide. Se pone
verde y el producto sigue roto. **Así que el arreglo tal como está nos deja sin poder usarlo**: no
por su forma, sino porque usarlo nos costaría la fidelidad que es la razón de existir de la red.

Concretamente, del lado nuestro:

1. La guarda que escribimos contra el defecto 2 (nombrar la cuerda que suena cuando el motor calla)
   **no se puede retirar**: el camino de la app no cambió.
2. Nuestro barrido de 206 combinaciones de cuerda ajena dejaría de ver `NO_SIGNAL`, y el criterio de
   muerte de esa guarda **se dispararía por un cambio de test, no de comportamiento** — que es
   justamente el falso positivo que ese criterio existe para no tener.

### Lo que pedimos

**Que el modo rápido entre a la API pública**: `setTunerCandidates` y `lockTunerString` alcanzables
desde `ITuner` —o desde una interfaz al lado— sin opt-in a la superficie de diagnóstico.

Nos parece el pedido correcto y no el cómodo, por cuatro razones:

- **Es append-only y no cambia el comportamiento de nadie.** No pedimos que `TunerImpl` baje los
  candidatos solo: eso convertiría el reengache automático en el default y le sacaría el mando a
  `selectedString` para todos sus consumidores. Pedimos que el consumidor **pueda** declararlo.
- **Respeta su decisión de diseño.** La detección gruesa sigue siendo otra capa y sigue siendo
  explícita — exactamente como `ITuner` dice que tiene que ser. Sólo pasa a ser explícita **para el
  consumidor** en vez de inalcanzable para él.
- **Es su propio criterio.** `InternalWatermelonApi` dice: *"algo entra a la API pública porque un
  consumidor real lo necesita, no porque el harness lo necesite. Si mañana NoisyPad pide el looper
  desde `commonMain`, eso es un ticket con su propia justificación."* Somos un consumidor real
  pidiéndolo, con el defecto medido en hardware.
- **Cierra la asimetría en la dirección buena.** Si la app puede declarar el instrumento, entonces
  pasar `candidatesHz` en la red offline vuelve a ser **fidelidad** en vez de infidelidad, y el
  PR #252 pasa a servirnos tal como está.

---

## 4. El subarmónico: el error de período también va HACIA ABAJO — y acá tienen con qué reproducirlo

Esto iba a ser una carta aparte. Va acá porque después de leer su respuesta **es la misma familia
que el defecto 1**, y ustedes lo declararon bloqueado por falta de corpus. Nuestro caso **sí se
reproduce offline y sin instrumento físico**, así que esto no es un reclamo nuevo: es el material
que les falta.

En el pedido del 09-02, **A** describía el enganche en `2·f0` sobre timbres cuyo H2 supera al
fundamental. Midiendo el banco reproducido apareció el mismo defecto **en la otra dirección**:

| archivo (GeneralUser GS 2.0.3) | f0 medido | el motor publica | razón | claridad |
|---|---:|---:|---:|---:|
| `guitarra-limpia_E4` (PC 27, nota 64) | 329,63 Hz | **109,873 Hz** | **f0 / 3** | 0,993 |
| `guitarra-limpia_G3` (PC 27, nota 55) | 195,99 Hz | **39,176 Hz** | **f0 / 5** | 0,999 |

En los dos casos el estado es `NO_SIGNAL` y no hay `cents`: con la cuerda correcta seleccionada,
**esas notas son inafinables**.

### 🔴 No es el estímulo: en esa frecuencia el archivo no tiene nada

Fue la primera pregunta que nos hicimos, y está medida — ventana Hann de 1 s desde 0,3 s, dB
relativos al parcial más fuerte de cada archivo:

| archivo | f0/5 | f0/4 | **f0/3** | f0/2 | f0 | 2·f0 | 3·f0 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `guitarra-limpia_E4` | −68,8 | −59,9 | **−73,4** | −53,2 | −4,8 | −1,8 | **0,0** |
| `guitarra-limpia_G3` | **−65,6** | −69,0 | −77,1 | −72,1 | −9,6 | −9,1 | **0,0** |

El motor publica 109,873 Hz sobre un archivo donde 110 Hz está **73 dB por debajo del pico**. Lo que
ese timbre sí tiene es el **3.er armónico como parcial más fuerte**, con el fundamental 4,8 dB
abajo — el espejo exacto del caso A, donde H2 dominaba. Y hay timbres de control del mismo
SoundFont y la misma nota que **no** fallan: `jazz`, `acero` y `nylon` en E4 se miden bien
(`guitarra-jazz_E4` tiene f0 dominante y 3·f0 a −43,5 dB).

### Reproductor mínimo, sin nuestros archivos

```bash
brew install fluid-synth
curl -sL -o gu.zip https://github.com/mrbumpy409/GeneralUser-GS/archive/refs/heads/main.zip
unzip gu.zip
# banco 0 / PC 27 (clean guitar), nota MIDI 64 (E4), 5 s, velocity 100, sin reverb ni chorus
fluidsynth -ni -R 0 -C 0 -g 0.9 -r 44100 -F limpia_E4.wav \
  GeneralUser-GS-main/GeneralUser-GS.sf2 e4_pc27.mid
```

El `.mid` lo produce el generador adjunto con `midi_nota(path, prog=27, bank=0, nota=64)`. Después
`OfflineTuner.analyze(buffer, 44100, targetHz = 329.6276f)` alcanza: `NO_SIGNAL`, `cents = null`,
`detectedHz ≈ 109,87`.

### ⚠️ Y acá está el cruce con el PR #252, que es la razón de fusionar las dos cartas

Con `candidatesHz` declarado, este caso **puede empeorar en vez de mejorar**, y de una forma que a
los dos repos les importa. Los dos números:

```
el motor publica sobre guitarra-limpia_E4  →  109,873 Hz
A2, la 5.ª cuerda de guitarra estándar     →  110,000 Hz     ← −2 cents
```

O sea que A2 **es uno de los candidatos**. Si el modo rápido reengancha el objetivo a la cuerda más
cercana a la altura detectada, sobre ese archivo elegiría A2 y mediría −2 cents: **`CONVERGED`, A2,
casi afinada — sobre una grabación que toca E4.** Hoy ese mismo archivo da `NO_SIGNAL`, que es
honesto: *"no sé"*. Con candidatos podría pasar a ser un dato plausible y falso, que es el modo de
falla que los dos repos prohíben explícitamente.

🔴 **Esto es una predicción, no una medición, y no la damos por hecha** — no podemos medirla porque
2.15.0 no tiene `candidatesHz`. Se deriva de dos números que sí están medidos (109,873 y 110,000) más
la descripción del reengache en su propio KDoc. Es falsable de su lado en una corrida, con el `.mid`
adjunto, y es lo primero que vamos a medir nosotros en cuanto publiquen.

Si se confirma, cambia la prioridad de A′: deja de ser un defecto de exactitud molesto y pasa a ser
**una precondición del modo rápido**.

### Y por qué no lo podemos tapar del lado del consumidor

En el pedido anterior dijimos que si A era caro podíamos sospechar la octava por la razón
`detectedHz / targetHz ≈ 2`. Lo implementamos. Al aparecer este caso probamos la extensión natural
—callar toda razón entera, `n·f` y `f/n`— y **hay que descartarla, con un número**:

> En temperamento igual, un **A2 real** está a **1900 cents** de E4. La razón **3:1** está a
> **1901,955**. **Dos cents.**

*"Suena un A2 con E4 seleccionada"* —el caso que motivó todo esto— y *"suena una E4 y el motor
dividió el período por 3"* son **la misma frecuencia**. Ninguna guarda del consumidor las separa, y
`detectionClarity` tampoco: 0,993 en el falso, 0,9999 en el verdadero. Silenciar las razones enteras
nos costaba 24 de las 206 combinaciones del catálogo, incluida la insignia, así que **elegimos
nombrar y aceptar el falso positivo**, escrito y con test que lo fija. Pero es una decisión tomada
contra la pared: la información que la resolvería sólo existe del lado del motor.

### Lo que pedimos acá

- **A′** — que una nota cuyo 3.er armónico supera al fundamental **no se enganche en un
  subarmónico**. Es A generalizado: el error de período va en las dos direcciones.
- **Y si el arreglo es caro, lo que de verdad nos destraba**: que el snapshot diga **cuánta energía
  hay en la banda de `detectedHz`** —un nivel del parcial, o una bandera *"el fundamental estimado no
  tiene soporte espectral"*—. Con eso el consumidor distingue 110 Hz-con-energía de
  110 Hz-inventado, que es exactamente lo que hoy no puede. Y sospechamos que también es lo que
  volvería seguro al reengache del modo rápido.

---

## Lo que NO pedimos

- **No** tocar la compuerta de ausencia, ni el rango útil, ni `N`, ni la ventana. Lo del 09-02 sigue
  igual, y el `R-PITCH-56` nos parece bien: no queremos que una cuerda ajena y el ruido de una
  habitación se traten distinto sin instrumento declarado.
- **No** cambiar `detectionClarity`. No delata este caso (0,993) ni el de octava (0,891 y 0,906), y
  para lo que sirve —separar ausencia de señal— anda bien y lo consumimos.
- **No** pedimos que `TunerImpl` baje los candidatos por su cuenta. Ver la razón en el punto 3.
- **No** pedimos que revisen el pedido B. Lo midieron, lo revirtieron y lo escribieron: cerrado.

## Los controles que descartan que el defecto sea del que reporta

1. **La energía está medida sobre el archivo**, con ventana Hann y en dB relativos al pico, no
   supuesta: 73 dB de margen no es una diferencia de método.
2. **Hay timbres de control que no fallan**, del mismo SoundFont y la misma nota.
3. **Se reproduce offline y por el aire**, con la app instalada y leyendo el árbol de accesibilidad.
4. **El `f0` de cada archivo está medido** por pendiente de fase sobre cada parcial, no leído del
   nombre.
5. **El alcance del PR #252 lo leímos del PR y del artefacto**, no de la prosa: `gh pr view --json
   files` y `javap` sobre el `.aar` de 2.15.0. Las tres mediciones del punto 3 son independientes
   entre sí.
6. **Lo que es predicción está marcado como predicción** — el cruce A2/f0÷3 del punto 4. Lo demás de
   esta carta está medido.

---

## Una nota de método, que nos costó a nosotros

El falsador que escribimos para poder retirar nuestra guarda decía, textual: *"si el motor dice
`SIN_ENGANCHE`, upstream contestó el pedido B y esta suplencia pasó a duplicarlo"*. Nombraba **la
puerta por la que creíamos que iba a llegar el arreglo**. Ustedes lo arreglaron por otra —declarar el
instrumento, donde el estado sería `CONVERGED`— así que nuestro falsador **no habría fallado: habría
dejado pasar el arreglo en silencio**, y la guarda se quedaba puesta para siempre.

Ya lo corregimos: hoy afirma la clase (*cualquier estado que no sea ausencia*) en vez de un
desenlace. Lo contamos porque es la clase de error que se ve desde afuera antes que desde adentro, y
porque su respuesta fue lo que lo destapó.
