---
title: "Aviso a Tunio — 2.16.3 para adoptar, la gruesa sobre cuerdas de acero, y UNA pregunta que necesitamos que contesten"
type: reference
status: current
created: 2026-09-10
---

# 2.16.3 para adoptar, la gruesa sobre acero, y una pregunta

**2026-09-10 · REQ-036 / REQ-038 / REQ-037 · watermelon-audio → Tunio**

> Pegar tal cual como primer mensaje en una sesión del repo de Tunio.

Son **cuatro cosas**: una versión a adoptar, un cambio de comportamiento visible, una explicación de
algo que van a ver y **no** es un defecto, y **una pregunta que nos bloquea un requerimiento**. La
pregunta está al final y es lo único que necesitamos de vuelta.

---

## 0. La versión a adoptar: 2.16.3

Publicada y verificada contra el registro el 2026-09-09, las cuatro coordenadas
(`com.watermellonstudios:audio`, `-android`, `-iosarm64`, `-iossimulatorarm64`).

**Antes de leer el resto, verifiquen contra qué contrato están diseñando.** En agosto nos mandaron
tres preguntas apoyadas en un layout de snapshot que ya estaba viejo, y les bloqueó una decisión de
producto por nada. Hoy el snapshot tiene **18 valores** (`WMA_TUNER_SNAPSHOT_VALUES = 18`). Si su
código o su modelo mental dice 8, 14 o 17, está desactualizado. El crecimiento es append-only, así
que su código compila igual — **append-only protege el código, no el modelo mental**.

## 1. Lo que CAMBIA de comportamiento en 2.16.3

Una nota pulsada trae un transitorio de afinación en el ataque. El estimador de fase promedia una
ventana de 48 × 4096 frames (~4,1 s a 48 kHz), así que ese ataque quedaba adentro de la regresión
hasta 4 s después y la lectura salía **`CONVERGED` y equivocada**: medido sobre 24 glides
sintéticos, convergía a 0,34 s con σ ≤ 0,0004 y hasta **−13,6 cents** de error. σ era ciega, porque
una recta ajustada a un palo de hockey deja residuos chicos.

Ahora el motor evalúa si la fase de cada parcial **es una recta** (compara la pendiente de las dos
mitades de su ventana) y no admite al parcial que no lo es; cuando la fase se quiebra, reinicia la
integración en el punto de quiebre.

| | antes | 2.16.3 |
|---|---|---|
| primera lectura fina posible | ~0,34 s | **~1,0–1,1 s** (más lo que tarde la detección gruesa) |
| durante el ataque | `CONVERGED`, sesgada | **`MEASURING`** |
| corpus de 41 notas reales: convergidas | 33 | **39** |
| error máximo entre las convergidas | 0,73 c | **0,30 c** |

🔴 **`MEASURING` durante el transitorio es el comportamiento nuevo, no una regresión.** Si su UI
mostraba un número apenas arrancaba la nota, ahora va a mostrar "midiendo" cerca de un segundo y
después el número correcto. **Si tienen un timeout de "no llegó lectura", revisen que tolere
~1,2 s.**

Efecto de rebote que también les puede interesar: si el audio de captura pierde muestras y **nadie
lo reporta**, el motor ya no converge sobre una lectura equivocada — publica `MEASURING` sin cents.
No es que se recupere; es que dejó de afirmar algo falso mientras tanto.

## 2. La detección gruesa sobre cuerdas de acero: lo que van a ver, y por qué no es un defecto

**Esto no cambió el motor. No hay nada que adoptar acá.** Lo escribimos porque es exactamente el
tipo de diferencia que se reporta como bug, y nos pasó a nosotros: abrimos un requerimiento
acusando al detector y la medición lo refutó.

Sobre una cuerda de guitarra de acero, **`detectedHz` puede estar unos cents por encima de la
frecuencia del fundamental**. Medido sobre nuestro corpus grabado, `guitarra-acero_E2`: la mediana
de la gruesa está a **+4,66 cents** del pico espectral de H1.

No es un error de cálculo. Una cuerda real es **inarmónica**: sus parciales no caen en `n·f0` sino
en

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
correlación en el mejor período baja a 0,987 en acero contra 1,000 en el control: la firma de que
esa señal no se repite exactamente.

Dos hipótesis nuestras se cayeron por el camino, por si les ahorran el rodeo: **no** lo causa un
segundo parcial dominante (`guitarra-jazz_E2` tiene H2 a +14,3 dB y no se sesga; `guitarra-acero_A2`
lo tiene a −17,8 dB y sí se sesga) ni una deriva de la nota (H1 estable a ±0,3 c por tramos).

**Qué hacer con esto**, concreto:

- **`detectedHz` no es la nota que un afinador espectral mostraría.** Su trabajo es traer el objetivo
  cerca, y unos cents sobre una cuerda inarmónica lo hacen. **Lo que se muestra como afinación es la
  lectura fina**, que sí es espectral y está a ≤ 0,30 c del oráculo.
- Si comparan `detectedHz` contra una referencia espectral y ven unos cents sobre cuerdas graves de
  acero, **es esperable**. Sobre nylon, jazz, limpia y bajo el corpus queda dentro de ±0,2 c.
- Si lo usan para *elegir la cuerda* del catálogo, unos cents no cambian nada: la distancia entre
  cuerdas es de semitonos.

Nuestro presupuesto interno para la gruesa bajó de 6 c a **2,3 c** con este criterio, así que la
vigilancia se apretó, no se aflojó.

## 3. Lo que ya estaba en 2.16.1 / 2.16.2 y quizá no adoptaron

- **La nota dependía del rate**: con un séptimo armónico fuerte el detector grueso podía leer
  **f0/3** (a 48 kHz llegaba a pasarle con seis armónicos limpios). Arreglado: el umbral del primer
  pico se aplica sobre el **pico real** de cada candidato, no sobre la muestra del barrido.
- Una mejora de costo del detector, sin cambio de exactitud (bit a bit sobre 12 000 ventanas).

## Lo que NO cambió

El layout del snapshot (18 valores), la C API, el JNI, la superficie Kotlin, y el contrato de
exactitud sobre material sintético (0,1 cents a 3 s).

---

# 🔴 La pregunta (es lo único que necesitamos de vuelta)

Tenemos un requerimiento **parado esperando este dato**, y no lo podemos contestar nosotros: sería
diseñar API mirándonos el ombligo, que es justo lo que nuestras propias reglas prohíben.

**Contexto en dos líneas.** Hoy, para que el motor **elija la cuerda solo** —instrumento declarado,
sin que el usuario toque nada, y el afinador engancha la que suena— hay que llamar
`setTunerCandidates`, que vive detrás de `@InternalWatermelonApi`. Todo lo demás ya está en la puerta
pública: declarar el instrumento, listar sus cuerdas, enganchar una a mano y leer. **Falta sólo ese
modo automático.**

Podemos abrirlo. Pero superficie pública no tiene vuelta atrás barata, y sólo la abrimos si un
consumidor real la necesita.

**Las tres preguntas, y con un "no" a la primera ya nos alcanza:**

1. **¿Su producto necesita que el motor elija la cuerda solo?** ¿O el usuario siempre elige la
   cuerda (o ustedes usan modo cromático y no hay catálogo)?
2. Si la necesitan: **¿hoy cómo lo resuelven?** ¿Llaman `setTunerCandidates` con el opt-in interno,
   lo resuelven ustedes con `setTunerTargetHz` eligiendo la cuerda más cercana por su cuenta, o
   directamente no lo hacen?
3. Si lo resuelven por su cuenta: **¿les sirve como está, o preferirían que lo haga el motor?** Si
   les sirve, decilo — cerramos el requerimiento sin implementar y no abrimos superficie que nadie
   pidió, que es el mejor resultado posible.

**Un "no lo necesitamos" es una respuesta completa y la más útil que nos pueden dar.** No hace falta
que investiguen nada: lo que sabemos que no podemos contestar es qué necesita su producto.

Y de yapa, si adoptan 2.16.3 y la espera de ~1 s del §1 molesta en la UI: **díganlo con un número**
—cuánto tarda hoy su primera aguja y cuánto toleran—. Eso entra directo en cómo se dimensiona.
