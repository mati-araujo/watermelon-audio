---
title: "Nota a upstream — sobre el PR #255, y el control negativo que le falta a AC-030.1"
type: reference
created: 2026-09-07
---

# Sobre el PR #255 — y cómo reproducir nuestro archivo, ahora sí

**2026-09-07 · Tunio → watermelon-audio · corta, a propósito**

## 1. El hilo era del motor, y levanta el bloqueante que pusimos

Leímos el PR #255 entero. El hilo que en su mensaje del 09-04 quedó como *"puede ser de nuestro
arnés o del motor"* era del motor —dos escritores de `mAppliedTarget`, 27 y 26 re-aplicaciones
contra 1 y 0— y está cerrado por AC-030.1 con el oráculo al lado, que es la forma que no se puede
aflojar sin que se note.

Nosotros habíamos escrito que ese hilo era **precondición de nuestro pedido de API**: un modo rápido
que reengancha y después no converge no sirve en el producto, y abrirlo a la API pública no
destrabaría nada. **Con #255 esa precondición queda cumplida.** El pedido —`setTunerCandidates` /
`lockTunerString` alcanzables sin `@InternalWatermelonApi`— sigue en pie y ya no tiene nada nuestro
adelante; lo que decidan como producto, lo tomamos.

## 2. 🔴 AC-030.1 prueba el reenganche a la cuerda CORRECTA. El caso que falta es el reenganche a la equivocada

```cpp
const auto reenganchado = analizar(cuerda(kA2), kE4, guitarraHz());   // suena un A2 real
```

Ese test converge en A2 porque **suena un A2**. El caso que ninguno de sus tests ve es el nuestro:
**suena una E4** cuyo timbre el detector lee como 109,87 Hz (f0/3), y el modo rápido —que hace lo
que tiene que hacer con lo que le dan— reengancha a A2. Ustedes ya midieron esa mitad el 09-04:

```
objetivo=E4, candidatos=guitarra, altura 109,873 Hz  ->  enganchó a A2
```

Hasta #255 eso terminaba en `MEASURING` para siempre, que era feo pero honesto. **Con #255
termina en `CONVERGED`, A2, cents plausibles, sobre una grabación que toca E4.** Es la segunda mitad
de nuestra predicción, y ahora tiene todo a favor: el reenganche está medido por ustedes y la
convergencia tras reenganche la garantiza AC-030.1.

No pedimos que #255 no se mergee — arregla un defecto real y lo arregla bien. Pedimos dos cosas:

- **Que ese archivo entre como control negativo** de la suite de wiring, aunque nazca rojo: *"con
  `guitarra-limpia_E4` sonando y E4 elegida, el motor NO converge en A2"*. Hoy es imposible
  defender el rojo sin material; abajo está el material.
- **Que A′ (el subarmónico) pase a leerse como precondición del modo rápido**, no como un defecto
  de exactitud aparte. El mismo defecto que hoy produce un `NO_SIGNAL` honesto produce, con
  candidatos y con #255, un dato plausible y falso.

Y un límite que vale marcar en el KDoc del puerto offline: `OfflineAnalysis.h:69` dice *"con
candidatos ese caso NO EXISTE"*. Es cierto para una cuerda ajena que suena; **no** lo es para una
cuerda correcta que el detector lee en un subarmónico — ahí el caso no desaparece, cambia de
estado.

## 3. Su bloqueo 1 se disuelve: `main` del SoundFont no se movió desde febrero

Consultamos la API de GitHub, sin bajar nada:

| | |
|---|---|
| head de `main` el **2026-08-25** (lo que nuestro banco usó) | `684543d5e5efaef08d02be50dcda8d552478fa60` |
| head de `main` **hoy** | `684543d5e5efaef08d02be50dcda8d552478fa60` |
| commits entre las dos fechas | **0** (el último es del 2026-02-23) |
| `GeneralUser-GS.sf2` en ese commit | blob sha1 `298b552d2e9d1307e03e5c5c99d2c046aaed9ec3` · **32 319 396 bytes** |

O sea que bajar `main` hoy da **exactamente los bytes** que nuestro banco renderizó en agosto. Y para
que no dependa de que siga sin moverse, la URL inmutable:

```bash
curl -sL -o gu.zip https://github.com/mrbumpy409/GeneralUser-GS/archive/684543d5e5efaef08d02be50dcda8d552478fa60.zip
unzip -q gu.zip
git hash-object GeneralUser-GS-*/GeneralUser-GS.sf2    # tiene que dar 298b552d2e9d…
```

Tres cosas que salen de esto:

1. **Es el `.sf2`, no el `.sf3`.** Su `rc=139` es del `.sf3` (Ogg comprimido); con este archivo
   nuestro FluidSynth 2.6.0 —el mismo que el suyo— renderizó los 44 sin un fallo.
2. **El 1.471 que shippea NoisyPad no es este archivo**, y no hay forma de que lo sea: blob
   distinto. No lo usen para comparar contra nuestros números.
3. Con el `.mid` adjunto del 09-03 y este `.sf2`, **el WAV se regenera bit a bit** salvo por la
   versión de FluidSynth. Si quieren cerrar también esa variable, mándennos el sha256 del suyo y lo
   cruzamos contra una regeneración nuestra.

El manifiesto que les mandamos sigue valiendo tal cual: es sobre este mismo blob.

---

Lo demás de la carta del 09-04 —el `f0` nominal, el `usable = no`, el A2 real a 0,28 cents— no
cambia. Esto es un agregado, no una revisión.
