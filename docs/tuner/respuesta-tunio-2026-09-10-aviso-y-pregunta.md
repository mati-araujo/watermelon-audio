---
title: "Aviso a Tunio — 2.16.3 para adoptar, la gruesa sobre acero, y UNA pregunta que necesitamos que contesten"
type: reference
status: current
created: 2026-09-10
---

# Aviso a Tunio — 2026-09-10

**watermelon-audio → Tunio · REQ-036 / REQ-038 / REQ-037**

> **Pegar tal cual como primer mensaje en una sesión del repo de Tunio.**

---

Llega un aviso del motor del afinador (`watermelon-audio`). Son **cuatro cosas**, y la última es
la única que necesitamos de vuelta:

0. Una versión a adoptar: **2.16.3** (ustedes están en 2.16.2).
1. Un **cambio de comportamiento visible** que puede sorprender en la UI.
2. Una **explicación** de algo que van a ver y que **no es un defecto**.
3. 🔴 Una **pregunta** que nos tiene un requerimiento parado, y que no podemos contestar nosotros.

Empezá por 1 y 3. El resto es contexto.

---

## 0. La versión: 2.16.3

Publicada y verificada contra el registro el 2026-09-09, las cuatro coordenadas
(`com.watermellonstudios:audio`, `-android`, `-iosarm64`, `-iossimulatorarm64`).
En `gradle/libs.versions.toml` hoy dice `audio = "2.16.2"`.

**Antes de leer el resto, verificá contra qué contrato está diseñado el código de acá.** En agosto
nos llegaron tres preguntas apoyadas en un layout de snapshot que ya estaba viejo, y les bloqueó una
decisión de producto por nada. Hoy el snapshot tiene **18 valores**
(`WMA_TUNER_SNAPSHOT_VALUES = 18`). Si el código o el modelo mental dice 8, 14 o 17, está
desactualizado — y como el crecimiento es **append-only**, **compila igual**: append-only protege el
código, no el modelo mental.

## 1. Lo que CAMBIA de comportamiento en 2.16.3

Una nota pulsada trae un transitorio de afinación en el ataque. El estimador de fase promedia una
ventana de 48 × 4096 frames (~4,1 s a 48 kHz), así que ese ataque quedaba **adentro** de la
regresión hasta 4 s después y la lectura salía **`CONVERGED` y equivocada**: medido sobre 24 glides
sintéticos, convergía a 0,34 s con σ ≤ 0,0004 y hasta **−13,6 cents** de error. σ era ciega, porque
una recta ajustada a un palo de hockey deja residuos chicos.

Ahora el motor evalúa si la fase de cada parcial **es una recta** (compara la pendiente de las dos
mitades de su ventana) y no admite al parcial que no lo es; cuando la fase se quiebra, reinicia la
integración en el punto de quiebre.

| | antes | 2.16.3 |
|---|---|---|
| primera lectura fina posible | ~0,34 s | **~1,0–1,1 s** (más lo que tarde la detección gruesa, ~0,1 s) |
| durante el ataque | `CONVERGED`, sesgada | **`MEASURING`** |
| corpus de 41 notas reales: convergidas | 33 | **39** |
| error máximo entre las convergidas | 0,73 c | **0,30 c** |

🔴 **`MEASURING` durante el transitorio es el comportamiento nuevo, no una regresión.** Si la UI
mostraba un número apenas arrancaba la nota, ahora va a mostrar "midiendo" cerca de un segundo y
después el número correcto.

**Qué revisar de este lado, concreto:**

- **Cualquier timeout de "no llegó lectura"**: que tolere **~1,2 s**. Si hay un timeout de 1 s, se
  va a disparar en notas que antes leían.
- **Estados de la UI**: que exista y se vea un estado "midiendo" que no sea el mismo que "no hay
  señal". Con el número llegando un segundo después, ese estado ahora dura lo suficiente para que
  el usuario lo lea.
- **Tests que afirmen que hay lectura antes de ~1 s**: se ponen rojos, y con razón.

Efecto de rebote que también puede interesar: si el audio de captura pierde muestras y **nadie lo
reporta**, el motor ya no converge sobre una lectura equivocada — publica `MEASURING` sin cents. No
es que se recupere; es que dejó de afirmar algo falso mientras tanto.

## 2. La detección gruesa sobre cuerdas de acero: lo que van a ver, y por qué NO es un defecto

**Esto no cambió el motor. No hay nada que adoptar acá.** Lo escribimos porque es exactamente el
tipo de diferencia que se reporta como bug — nos pasó a nosotros: abrimos un requerimiento acusando
al detector y la medición lo refutó.

Sobre una cuerda de guitarra de acero, **`detectedHz` puede estar unos cents por encima de la
frecuencia del fundamental**. Medido sobre nuestro corpus grabado, `guitarra-acero_E2`: la mediana
de la gruesa está a **+4,66 cents** del pico espectral de H1.

No es un error de cálculo. Una cuerda real es **inarmónica**: sus parciales no caen en `n·f0` sino en

```
f_n = n · f0 · sqrt(1 + B·n²)
```

así que **la señal no tiene un período exacto**, y "la altura" deja de ser un solo número. El pico
espectral del fundamental dice una cosa y el mejor período de la forma de onda dice otra, y
**ninguna de las dos está mal**. Lo comprobamos con un tercer método independiente:

| método | qué mide | `guitarra-acero_E2` | control `guitarra-jazz_E2` |
|---|---|---|---|
| el detector del motor (NSDF) | período de la forma de onda | **+6,32 c** | −0,09 c |
| autocorrelación cruda (independiente) | período de la forma de onda | **+6,71 c** | −0,20 c |
| oráculo espectral (Goertzel sobre H1) | pico de H1 | 0,00 c | 0,00 c |

**Dos métodos temporales independientes coinciden entre sí y se apartan del espectral.** El desvío
de los parciales altos con energía coincide con el sesgo en signo y magnitud (H9 a +7,38 c), y la
correlación en el mejor período baja a **0,987** en acero contra **1,000** en el control: la firma de
que esa señal no se repite exactamente.

Dos hipótesis nuestras se cayeron por el camino, por si les ahorran el rodeo: **no** lo causa un
segundo parcial dominante (`guitarra-jazz_E2` tiene H2 a **+14,3 dB** y no se sesga;
`guitarra-acero_A2` lo tiene a **−17,8 dB** y sí se sesga) ni una deriva de la nota (H1 estable a
±0,3 c por tramos).

**Qué hacer con esto, concreto:**

- **`detectedHz` no es la nota que un afinador espectral mostraría.** Su trabajo es traer el objetivo
  cerca. **Lo que se muestra como afinación es la lectura fina**, que sí es espectral y está a
  **≤ 0,30 c** del oráculo sobre material real.
- Si comparan `detectedHz` contra una referencia espectral y ven unos cents sobre cuerdas graves de
  acero, **es esperable**. Sobre nylon, jazz, limpia y bajo el corpus queda dentro de **±0,2 c**.
- Si lo usan para **elegir la cuerda** del catálogo, unos cents no cambian nada: la distancia entre
  cuerdas es de semitonos.
- Recordatorio del cierre anterior, que sigue vigente: para llevar la lectura fina a Hz absolutos,
  `hz_medido = objetivo_del_strobe · 2^(cents / 1200)`. Los cents publicados son **relativos al
  objetivo**, no a una referencia absoluta.

Nuestro presupuesto interno para la gruesa **bajó de 6 c a 2,3 c** con el criterio corregido, así que
la vigilancia se apretó, no se aflojó.

## 3. Lo que ya estaba en 2.16.1 / 2.16.2 y quizá no adoptaron

- **La nota dependía del rate**: con un séptimo armónico fuerte el detector grueso podía leer
  **f0/3** (a 48 kHz llegaba a pasarle con seis armónicos limpios). Arreglado: el umbral del primer
  pico se aplica sobre el **pico real** de cada candidato, no sobre la muestra del barrido.
- Una mejora de costo del detector, sin cambio de exactitud (bit a bit sobre 12 000 ventanas).

## Lo que NO cambió

El layout del snapshot (**18 valores**), la C API, el JNI, la superficie Kotlin, y el contrato de
exactitud sobre material sintético (**0,1 cents a 3 s**).

---

# 🔴 4. La pregunta

Tenemos un requerimiento **parado esperando este dato**, y no lo podemos contestar nosotros: sería
diseñar API mirándonos el ombligo, que es justo lo que nuestras propias reglas prohíben.

## El contexto, en dos párrafos

Hoy, para que el motor **elija la cuerda solo** —instrumento declarado, el usuario no toca nada, y
el afinador engancha la cuerda que suena— hay que llamar `setTunerCandidates`, que vive detrás de
`@InternalWatermelonApi`.

**Todo lo demás ya está en la puerta pública**: declarar el instrumento
(`TunerFactory.create(configuration)`), listar sus cuerdas (`targets`), enganchar una a mano
(`selectedString`) y leer (`reading()`). **Falta sólo el modo automático.** Podemos abrirlo — pero
superficie pública no tiene vuelta atrás barata, y sólo la abrimos si un consumidor real la
necesita.

## Lo que ya medimos de este lado, para que no contesten la pregunta equivocada

Corrimos un grep sobre el árbol de Tunio el 2026-09-10:

- **Cero** `@OptIn(InternalWatermelonApi)`. Los dos únicos hits de `setTunerCandidates` /
  `getAudioBridge` son **comentarios** (uno en `core-domain/.../AudioCapture.kt`, otro en
  `core-audio/.../AfinadorDelMotor.kt`).
- De `ITuner` se usa: `selectedString` (5), `targets` (4), `configuration` (2), `reading` (1).

O sea que **hoy la cuerda la elige el usuario**. Eso ya lo sabemos. Lo que **no** podemos saber desde
acá es si el producto lo necesita.

## Las tres preguntas

Contestalas **mirando el código y el diseño de producto de Tunio**, no por opinión. Los archivos que
tocan esto son `feature-afinador/.../AfinadorViewModel.kt`, `core-audio/.../AfinadorDelMotor.kt`,
`core-audio/.../MapeoDeCuerdas.kt` y `core-domain/.../PoliticaDeSonido.kt`.

1. **¿El producto necesita que el motor elija la cuerda solo?** ¿O el usuario siempre elige la
   cuerda, o usan modo cromático y no hay catálogo? — *Con un "no" acá, ya alcanza: pueden saltear
   las otras dos.*

2. Si lo necesitan: **¿hoy cómo lo resuelven?** Tres opciones y cuál es:
   - llaman `setTunerCandidates` con el opt-in interno (el grep dice que no, pero puede haber
     cambiado);
   - lo resuelven ustedes, eligiendo la cuerda más cercana por su cuenta y empujando
     `setTunerTargetHz` — **si es esto, decinos en qué archivo y con qué criterio elige**;
   - directamente no lo hacen y la cuerda la elige siempre el usuario.

3. Si lo resuelven por su cuenta: **¿les sirve como está, o preferirían que lo haga el motor?** Y si
   preferirían que lo haga el motor, **¿con qué forma?** — un flag en `TuningConfiguration`, un
   método en `ITuner`, un modo explícito. **Mostranos dónde lo llamarían**: un lugar concreto en el
   código de Tunio donde iría esa llamada vale más que una descripción.

🔴 **Un "no lo necesitamos" es una respuesta completa, y la más útil que nos pueden dar.** Cierra el
requerimiento sin implementar y no abrimos superficie que nadie pidió, que es el mejor resultado
posible. No hace falta que investiguen nada para eso.

## Lo que necesitamos de vuelta

Un mensaje corto con:

- **la respuesta a (1)**, y a (2)/(3) sólo si aplica;
- **si adoptan 2.16.3**: si la espera de ~1 s del §1 molesta en la UI, **díganlo con un número** —
  cuánto tarda hoy la primera aguja y cuánto toleran. Ese dato entra directo en cómo se dimensiona;
- **cualquier cosa del §2** que ya estuvieran tratando como bug, para que la cerremos.
