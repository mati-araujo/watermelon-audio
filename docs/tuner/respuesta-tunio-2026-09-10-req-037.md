---
title: "Respuesta a Tunio — el modo automático está abierto, y no en el lugar que pidieron"
type: reference
status: superseded
created: 2026-09-10
---

# Respuesta a Tunio — 2026-09-10 · REQ-037

> 🔴 **SUPERSEDED, y nunca se envió.** Describe `ITuner.automaticStringSelection`, que se mergeó a
> `master` el 10/09 a las 20:35 y se revirtió el 11/09 antes de cualquier release. Cinco minutos
> antes de ese merge, Tunio había commiteado su propio automático (`EleccionDeCuerda.kt`) y su
> segunda carta retiró el pedido. Se conserva como la traza de esos cinco minutos; lo vigente es
> `respuesta-tunio-2026-09-11-req-037-cerrado.md`.

**watermelon-audio → Tunio.** Contesta la respuesta al aviso del 2026-09-10.

Gracias por contestar en dos días, y por contestar la pregunta que hacía falta: *"es la
funcionalidad central y está bloqueada desde agosto"* es exactamente el dato que nos faltaba para
abrir superficie pública. Sin él no lo hubiéramos hecho.

**Está implementado.** Y no está donde lo pidieron. Abajo el qué, el por qué, y una cosa que les
puede morder.

---

## 1 · Qué quedó

```kotlin
interface ITuner {
    /** Que el motor elija la cuerda solo, con histéresis. Nace en `false`. */
    var automaticStringSelection: Boolean
}
```

Es la **política de fallback de `selectedString`**, no un eje aparte: gobierna qué pasa cuando **no
hay cuerda elegida**. Con `selectedString` puesto **manda el consumidor**, siempre — que es
literalmente lo que pidieron (*"en manual se queda"*). Así que "automático encendido y cuerda
elegida" no es un estado ambiguo que haya que resolver leyendo documentación: es el modo manual.

Nace apagado. `setTunerCandidates` **no** se abrió, y `lockTunerString` tampoco: `selectedString`
ya es el enganche manual, y los candidatos los deriva el afinador de la configuración que ustedes
ya empujan — tal cual lo leyeron.

**Y `TunerReading.target` ahora contesta contra qué objetivo se publicó la lectura**, lo haya
elegido el consumidor o el motor. Es la mitad que faltaba para que los cents del modo automático se
puedan llevar a Hz absolutos sin adivinar la cuerda.

## 2 · 🔴 Lo que les puede morder: las dos bases no coinciden

Escribieron que con `lockedString` les alcanza *"para prender el cap correcto"*. Cuidado:

| | base |
|---|---|
| `ITuner.selectedString` | **1-based** — numera cuerdas como el músico |
| `TunerSnapshot.lockedString` | **0-based** — indexa el arreglo de candidatos que el motor recibió |

No es un descuido que vayamos a corregir: `lockedString` sale de `FastModeTracker`, que lo usa para
indexar candidatos (`mCandidates[mLocked]`), y cambiarlo ahora rompería a quien ya lo lee. Lo que
hicimos fue **documentarlo en ambos lados** y darles una salida que no obliga a pensarlo:
**usen `TunerReading.target`**, que ya resuelve la base. Si necesitan el número,
`selectedString = lockedString + 1`.

Lo decimos con énfasis porque el modo de falla es silencioso: confundirlas devuelve la cuerda de al
lado con cara de lectura perfectamente válida.

## 3 · Por qué no es un flag en `TuningConfiguration`

Pidieron eso, con una razón buena —viaja con lo que ya empujan y no toca el ciclo de vida— y la
descartamos. Se lo debemos explicar, no anunciar.

**La razón corta**: `TuningConfiguration` describe *qué debería sonar*; `TunerSnapshot`, *qué está
sonando*. Elegir **cómo se elige la cuerda** no es ninguna de las dos. `ITuner` es donde las dos
mitades se juntan, y el KDoc de `selectedString` ya tenía escrito este contrato desde antes de que
existiera la pregunta:

> *"No tiene default automático a propósito: elegir la cuerda por proximidad es detección gruesa, y
> ésa es otra capa. **Una implementación que la agregue lo hace explícito**."*

**La razón que apareció al mirar su código, y que es la que de verdad pesa**: su
`cambiarAfinacion()` empuja un `TuningConfiguration` nuevo al mismo `ITuner`. Con el flag adentro de
esa clase, **cada cambio de afinación reescribiría el modo**: pasar de estándar a Drop D apagaría el
automático salvo que se acuerden de propagar el flag en cada construcción — un olvido que no da
error de compilación, sólo apaga la funcionalidad central sin que nadie se entere. Con el flag en
`ITuner`, el modo sobrevive a los cambios de afinación porque vive en el objeto que sobrevive.

**Y el precedente de la forma ya está en el artefacto que tienen**: `api/OfflineTuner.kt` (2.16.x)
es una puerta pública que absorbe el `@OptIn` adentro para que ustedes no lo escriban. Es la misma
forma, aplicada al puerto offline.

## 4 · Qué les cuesta esto — lo medimos antes de decidir

Un `var` nuevo en una interfaz **rompe a compilar a quien la implemente**. Es real: nos rompió
nuestro propio doble de tests. El flag en `TuningConfiguration` que pidieron **no** habría tenido
ese costo, así que es consecuencia directa de haber elegido otra forma y no lo queremos disimular.

**Verificamos si les cobra a ustedes, y no**: en su árbol **nadie implementa `ITuner`** — lo obtienen
de `TunerFactory.create(configuracion)` y lo usan. Cero clases con `: ITuner`. Si en algún módulo que
no vimos tienen un doble propio de la interfaz, ése sí va a pedir el miembro nuevo, y la corrección
es una línea.

El otro costo es el que nombraron: una llamada más, fuera de `configuracionDe()`. En su código es
**una línea al lado de una que ya existe** — en `arrancar()`, donde ya escriben
`tuner.selectedString = indice`.

## 5 · Lo que necesitamos de ustedes

**¿Dónde iría la llamada, bajo esta forma?** El call site que nos dieron (`configuracionDe()`) era
para el flag en `TuningConfiguration`, y elegimos otra cosa — así que ese dato quedó sin equivalente.

No es curiosidad: nuestro criterio para saber si nos equivocamos al abrir esta puerta es
**si alguien la cruza**, y para eso necesitamos saber dónde debería aparecer la llamada. Si dentro
de unas versiones no está en ningún lado, la conclusión que queremos poder sacar es *"la abrimos y
no hacía falta"* — y no *"no supimos dónde mirar"*.

## 6 · Una nota sobre su argumento para no encenderlo por default

Dijeron que auto-por-default *"le saca el mando a `selectedString` a sus otros consumidores"*.
Estamos de acuerdo con la conclusión —nace apagado— pero la premisa no se sostiene: **esos otros
consumidores no existen hoy**. El único otro consumidor del motor no usa el afinador (cero
referencias a `ITuner`, `TunerFactory` o `TunerReading`). Lo decimos porque protegerse de un
consumidor que no existe puede llevarlos a pedir menos de lo que necesitan; acá coincidió con lo
correcto, y en la próxima puede no coincidir.

## 7 · Estado

Implementado contra la superficie interna que ya existía. Sin C API ni JNI nuevos, sin cambios en el
layout del snapshot (siguen siendo 18 valores) y sin tocar el DSP: el golden del modo rápido queda
bit a bit. Va en la próxima release.
