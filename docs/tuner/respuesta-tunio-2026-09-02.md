# Respuesta a Tunio — la nota ajena y la octava

**2026-09-02 · sobre 2.15.0 · watermelon-audio → Tunio**

Respuesta a los dos defectos que midieron. Uno tiene arreglo y llega por un camino distinto al que
sugería el reporte; el otro sigue abierto, y acá está exactamente qué lo bloquea.

---

## Primero, gracias por confirmar REQ-027

La red offline que dejaron fijada poniéndose **roja en el único test que afirmaba el defecto** es la
mejor confirmación posible: el test hizo justo aquello para lo que existía. De 38,70 cents en el peor
caso a 0,00005, sin regresiones en latch, signo ni invariante de ausencia.

Y su forma de reportar ahorró una ronda entera. El aviso de que *«no aparece con señal sintética»*
resultó **literalmente cierto de nuestro lado también**, y se midió antes de intentar nada.

---

## Defecto 2 — `NO_SIGNAL` sobre una nota que no es la seleccionada

**Estado: resuelto, en PR** ([#252](https://github.com/mati-araujo/watermelon-audio/pull/252)).

La causa estaba donde la señalaron. El arreglo **no** es el que el reporte sugería, y la diferencia
importa.

### Qué hay que cambiar

Una sola llamada. El puerto offline ahora acepta **los candidatos del instrumento**:

```kotlin
// Las seis de guitarra, en orden de cuerda
val guitarra = floatArrayOf(82.407f, 110.000f, 146.832f, 195.998f, 246.942f, 329.628f)

val snapshot = OfflineTuner.analyze(
    samples      = buffer,
    sampleRate   = 44_100,
    targetHz     = 329.628f,     // E4 seleccionada
    candidatesHz = guitarra,     // ← lo nuevo
)
```

Con eso, la matriz de 36 deja de tener 30 ausencias. El parámetro tiene default vacío, así que **las
llamadas de hoy siguen compilando**: es append-only en las cuatro capas (C API → JNI →
`AudioNativeBridge` → `OfflineTuner` → cinterop) y el punto de entrada anterior sigue existiendo.

### Por qué el arreglo va por ahí y no por donde parecía

Con el instrumento declarado, el motor **reengancha el objetivo a la cuerda que suena** y la mide:
el modo rápido ya hacía lo correcto, y el caso reportado no se da. Lo que faltaba no era una decisión
nueva sobre el estado — era que el puerto offline no tenía forma de expresar el instrumento, y ése es
el único camino donde ustedes miden.

### Se intentó lo que el reporte pedía, y se revirtió

Pidieron que una nota ajena saliera `NO_LOCK` en vez de `NO_SIGNAL`. Se implementó exactamente eso
para el caso sin instrumento declarado, y se midió lo que costaba:

| qué se rompía | medición |
|---|---|
| Tests de ausencia sobre ruido de sala (REQ-014) | **6 en rojo** |
| Altura que el detector grueso ve en un zumbido de red | **48,45 Hz** |
| Garantía de ausencia verificada en hardware por ustedes | **perdida** |

Sin instrumento declarado, **una cuerda ajena y el ruido de una habitación son indistinguibles para
el motor**: el detector ve una altura clara en los dos. Tratarlos distinto significaba devolver la
ausencia a «nunca», que es de donde la sacó 2.10.0 → 2.15.0 y que ustedes pusieron explícitamente en
su lista de *no tocar*.

Quedó escrito como contrato (`R-PITCH-56`) y fijado por un test que compara los dos estímulos en las
dos configuraciones — para que el próximo que lea el reporte no lo «arregle» igual.

---

## Defecto 1 — error de octava con timbres reales en G3

**Estado: abierto, bloqueado por el corpus.**

El detector es MPM, y su defensa contra la octava es elegir el **primer** pico sobre `0,9 · máximo`
en vez del más alto. La documentación del propio detector describe el caso de ustedes antes de que lo
midieran: *«en una bordona grave el fundamental puede estar 20 dB por debajo del segundo parcial, así
que el caso patológico es también el caso normal»*. Con H2 sostenido +4,3 a +10,7 dB, esa defensa no
alcanza. No falta la defensa: es **insuficiente para ese régimen**.

### La advertencia de ustedes, medida de este lado

| estímulo sobre G3 | hz detectada | razón |
|---|---|---|
| Serie completa desde H2 *(el generador que ya teníamos)* | 39,20 | **0,200** |
| H2 domina, H3 −12 dB *(nylon real)* | 195,998 | **1,000** |
| H2 +4,3 dB sobre H1 *(acero real)* | 195,998 | **1,000** |

**Los dos casos realistas enganchan bien.** El falso negativo de ustedes es también el nuestro: el
generador que el repo tiene produce exactamente la serie completa desde H2 que describen, y su
espaciado sigue dando f0. Sin material grabado no se puede escribir un test que detecte este defecto
— y sin test, cualquier arreglo no se puede defender.

### Qué sigue, y qué no se promete

- El próximo paso es **construir el corpus** con la receta de fluidsynth. Sirve tal cual: es
  determinista y regenerable, que es mejor que grabaciones para un test de CI.
- 🔴 **La visibilidad NO mejoró con este cambio, y no queremos que lo supongan.** Un G3 detectado en
  392 Hz queda a ~300 cents de E4, fuera del enganche, así que sigue saliendo `NO_SIGNAL` aun con
  candidatos declarados. La razón `detectedHz/targetHz ≈ 2` que ustedes calculan **sigue siendo la
  única señal**, y `detectionClarity` sigue sin delatarlo.
- No se va a tocar `kPeakThreshold` a ciegas: tiene un test de mutación que lo vigila, y moverlo sin
  material que reproduzca el defecto sería cambiar un número contra la nada.

---

## Lo que necesitamos de ustedes

- **Confirmen la receta**: GeneralUser GS 2.0.3, banco 0, PC 24 y 25, nota MIDI 55, 5 s,
  velocity 100. Si tienen el `.mid` exacto que usaron, ahorra una fuente de divergencia.
- **Digan si les sirve el arreglo del defecto 2 tal como está**, antes del merge. Está en PR, no
  publicado — si `candidatesHz` no encaja con cómo arman sus tandas, es el momento de decirlo.

Y una nota de método: su carta trae los controles adelante —dos versiones, nivel descartado, f0
medido, versión confirmada por sha256— y eso hizo que las dos primeras hipótesis de este lado se
pudieran descartar sin escribirles. Sigan haciéndolo así.

---

Contrato tocado: `R-API-49` (el puerto acepta el instrumento) y `R-PITCH-56` (el instrumento es lo
que hace distinguible una nota de la sala).
