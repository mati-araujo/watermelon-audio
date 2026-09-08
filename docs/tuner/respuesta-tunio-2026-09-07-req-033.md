---
title: "Respuesta a Tunio — su reproductor de siete senos ya lee E4, y la causa no era el umbral"
type: reference
status: current
created: 2026-09-07
---

# Su reproductor de siete senos ya lee E4 — y la causa no era el umbral

**2026-09-07 · sobre su nota de hoy (b), §3 · watermelon-audio → Tunio**

El REQ sobre el detector grueso se abrió, se ejecutó y está en PR (REQ-033). Tres cosas: la causa
medida, qué cambió sobre sus archivos, y qué cambia para ustedes en el contrato (nada de forma; una
cosa de contenido que conviene saber).

---

## 1. La causa: el barrido muestreaba el pico de τ a 1,9 muestras, y el umbral se aplicaba ahí

Instrumentamos el detector real para ver **qué lags evaluó y con qué valor** sobre su reproductor.
La NSDF en τ vale 0,999 —la regla del primer pico la elegiría—, pero el barrido proporcional
(`τ/12`) la muestreó en el lag 65, a 1,9 muestras del pico, donde vale **0,816**; a 3τ la muestreó
a 1,3 muestras, donde vale 0,908. El umbral `0,9 · máximo` daba **0,817** y decidía por **0,001**
a favor de 3τ. Con seis armónicos el margen era +0,01; el séptimo angosta el lóbulo y lo da vuelta.

Y a **48 kHz** —el rate de sus dispositivos— era peor: la grilla cae a 2,2 muestras de τ y a 0,4
de 3τ, y hasta los **seis** armónicos limpios leían f0/3. Su render y nuestro corpus están a
44,1 kHz; por eso ahí hacía falta el séptimo y en device no.

No era `kPeakThreshold` (moverlo desplaza el defecto a picos espurios) ni el antialias ni la
interpolación. El arreglo refina **cada** candidato a su pico real antes de aplicar el umbral, que
es lo que antes se hacía sólo para el ya elegido. El umbral queda en 0,9.

## 2. Sobre sus archivos

| archivo | antes (nota b, §3) | ahora |
|---|---|---|
| reproductor de siete senos | 109,874, bandera 0 | **E4** con soporte, convergido, a 44,1 y 48 kHz |
| `guitarra-limpia_E4` | f0/3, `NO_LOCK` | convergida **−0,66 c**, soporte 1 |
| `guitarra-limpia_G3` | f0/5, `NO_LOCK` | convergida **−1,76 c**, soporte 1 |
| `guitarra-acero_E4` | seis reenganches, sin lectura | convergida **+0,92 c** a 2,09 s |

El ataque de la acero era la misma clase, no otro timbre: el pico de τ mal muestreado. Los 38 que
ya leían bien no se movieron, ni la octava, ni el umbral fijado, ni la bordona a −40 dB.

## 3. Lo que cambia para ustedes

**Nada de forma**: el snapshot, el índice 17 y los estados son los mismos. **Una cosa de
contenido**: el caso de REQ-031 —f0 a −20 dB, sin H2, con H3 y H5— ahora **converge sobre E4** con
soporte, porque E4 es el período de H3 + H5 y su f0 está dentro del umbral de soporte (−25 dB). Ya
no es `NO_LOCK` en 109,87: es la lectura correcta. La bandera sigue haciendo su trabajo como red:
con f0 a −30 dB o menos, E4 sale publicada con bandera 0 y `NO_LOCK`.

Si tienen un test que fije *"la E4 con f0 −20 dB queda en `NO_LOCK` a 109,87"*, se pone rojo con la
próxima versión, y es lo que tiene que pasar. La lectura fina sobre cuerdas inarmónicas (+5,4 c en
`ukelele_C4`) sigue siendo un REQ aparte.
