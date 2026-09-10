---
title: "Tunio → watermelon-audio · respuesta al aviso del 2026-09-10"
type: reference
status: current
created: 2026-09-10
---

# Respuesta de Tunio al aviso de 2.16.3

**Recibida el 2026-09-10.** Contesta el aviso de
`respuesta-tunio-2026-09-10-aviso-y-pregunta.md`. Texto íntegro, como llegó.

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo que sí se verificó contra
> nuestro árbol está anotado al final, separado a propósito.

---

## 0 · Contrato verificado antes de contestar

`javap` sobre `audio-release.aar`: `VALUE_COUNT = 18`. Gracias por el aviso — fue el chequeo
correcto, y encontró algo, aunque no lo que temían. Nuestro código no dependía del largo: la
traducción es por tipos, campo por campo desde `TunerSnapshot` (12 campos consumidos), y el único
camino que indexa el arreglo crudo exige `size >= 8`, acepta más largo, y hoy sólo lo usan tests.
Lo que sí estaba vencido era la prosa: tres KDoc decían 15, 16 y 8. Ya corregidos, citando el
artefacto.

## 1 · La espera de ~1 s: adoptamos 2.16.3 y no molesta

No pedimos que la achiquen.

- No tenemos ningún timeout de "no llegó lectura" que tolerar. Preguntamos cada 16 ms y todo el
  camino avanza por evento (el contador `framesAnalyzed`), sin un solo reloj de pared. Los únicos
  temporizadores son los del gesto (250 / 400 / 400 ms), los tres relativos a lecturas que llegaron.
- El estado "midiendo" ya existe y ya es distinto de "sin señal", por construcción. `MEASURING` con
  `cents = null` cae en nuestra zona fuera de rango y se rinde como `LISTENING`: cara despierta,
  medidor vacío, `FREQ`/`CENTS` en `–––`. `NO_SIGNAL` cae en `SIGNAL_LOST`. 2.16.3 nos rinde
  exactamente lo que describen sin tocar una línea. Lo único que agregamos fue un test que lo
  nombre, porque salía bien por construcción y nada lo fijaba.
- **Los números nuestros**: sobre lo que publican agregamos 250 ms de permanencia de zona y 400 ms
  de estabilidad antes de la sonrisa de afinado. Con sus cifras: hoy ~0,34 + 0,4 ≈ 0,75 s; con
  2.16.3 ~1,1 + 0,4 ≈ 1,5 s. Lo toleramos sin discusión — una cuerda pulsada suena varios segundos,
  y el tramo que se pierde es justo el que salía sesgado hasta −13,6 cents.
- 🔴 **La única alarma que nos dejamos puesta**: nuestro diagnóstico de cuerda ajena ("SUENA A2")
  exige `NO_SIGNAL`. Si 2.16.3 corría alguna de esas situaciones a `MEASURING`, ese mensaje
  desaparecía en silencio. Ya lo medimos y no pasó: `DiagnosticoDeCuerdaAjenaTest` pasa 11/11 sin
  tocar un test, o sea que las 425 combinaciones ajenas siguen dando `NO_SIGNAL`.

## 2 · La detección gruesa sobre acero: no lo estábamos tratando como bug

Nada que cerrar. `detectedHz` lo consumimos en dos funciones y **sólo para nombrar la nota** que
suena cuando ustedes declaran ausencia: semitono más cercano, al header. Nunca como lectura de
afinación, nunca contra una referencia absoluta, nunca para elegir la cuerda por proximidad. Unos
cents no mueven una decisión cuya resolución es el semitono.

## 3 · 🔴 La pregunta: sí, lo necesitamos — y es la funcionalidad central, hoy bloqueada

**(1) ¿El producto necesita que el motor elija la cuerda solo?** Sí. El handoff de diseño aprobado
por negocio vende un afinador **cromático**: el usuario toca una cuerda cualquiera y el aparato le
dice qué nota es. Su propio `tuning-engine.md` lo dice del otro lado: *"the string row is an aid,
not a filter"*. La pantalla mono-objetivo que tenemos es **el mundo de repuesto**: la construimos en
agosto porque su estimador afina alrededor de un objetivo y no lo busca, lo dejamos escrito entonces
como bloqueado por terceros, y sigue así. No es una idea que se nos ocurrió leyendo su aviso: es el
requisito que quedó esperando.

**(2) ¿Hoy cómo lo resuelven?** La opción (c), con un matiz. La cuerda la elige siempre el usuario
(`AfinadorViewModel.kt:587` → `ITuner.selectedString`). No llamamos `setTunerCandidates` y no
empujamos un Hz crudo — su grep acertó. El matiz: sí resolvemos "qué cuerda suena" por nuestra
cuenta, pero **para nombrar, no para reenganchar**. En `core-domain/.../CuerdaAjena.kt`, cuando
ustedes dicen `NO_SIGNAL` con `detectionClarity ≥ 0,8` y `spectralSupport != false`, tomamos
`detectedHz`, buscamos el semitono más cercano y lo escribimos en el header. Calla en dos casos: a
menos de un semitono del objetivo, y a una octava por encima ±50 cents (2 de 18 timbres reales
enganchan `2·f0`). Nunca cambia el objetivo.

**(3) ¿Preferirían que lo haga el motor, y con qué forma?** Sí, y ya se lo habíamos pedido — es el
pedido del 03/09 §3, que sigue abierto de su lado.

🔴 **Lo que cambió es que su precondición se cerró.** El 04/09 les dijimos que el pedido era inútil
si tras un reenganche la integración no converge. Eso lo arreglaron en REQ-030
(`mati-araujo/tunio#255`, los dos escritores de `mAppliedTarget`), y REQ-031 volvió seguro el
reenganche a la cuerda equivocada. Nuestro bloqueo era ése y ya no está.

**Sobre la forma, tres cosas leídas del artefacto:**

- **No nos hace falta `setTunerCandidates`.** `ITuner` ya tiene los candidatos: los deriva de
  `configuration` vía `targets()`, y nosotros ya empujamos esa configuración entera en caliente.
  Falta sólo el interruptor. Nos serviría **un flag en `TuningConfiguration`**: viaja con lo que ya
  empujamos y no agrega un método al ciclo de vida.
- **Y la mitad de LECTURA ya es pública.** `TunerSnapshot` expone `getLockedString()` y
  `getFastModeState()`. Un consumidor puede leer la salida del modo rápido y no puede encenderlo:
  **la puerta está media abierta**. Con `lockedString` nos alcanza para prender el cap correcto.
- **Dónde iría la llamada, exacto**: `core-audio/src/mobileMain/.../AfinadorDelMotor.kt`, en
  `configuracionDe()` — la única función del árbol que arma un `TuningConfiguration`, y de la que ya
  salen `iniciar()` y `cambiarAfinacion()`.

**Y repetimos lo que NO pedimos**: que `TunerImpl` baje los candidatos solo. Auto por default
convierte el reenganche en el comportamiento de todos y le saca el mando a `selectedString` a sus
otros consumidores. **Pedimos que el consumidor pueda declararlo.**

**Qué les destraba de nuestro lado**: podemos pasar `candidatesHz` en nuestra red offline sin perder
fidelidad (hoy no lo hacemos justamente por eso), y `cuerdaAjena` deja de hacer falta en modo
automático — en manual se queda.

**Estado**: 2.16.3 adoptada y mergeada. Red offline entera verde contra el artefacto nuevo sin tocar
un test, `ci` verde en las tres plataformas, y los tres smokes `ok` — el de Android sobre un
moto g42 real.

---

## Lo que verificamos de este lado (2026-09-10)

Sus tres afirmaciones sobre **nuestra** API se sostienen, medidas en el árbol:

| lo que dicen | verificado |
|---|---|
| la lectura del modo rápido ya es pública | `TunerSnapshot` es un `data class` **sin opt-in**, con `lockedString` (`:73`) y `fastModeState` (`:75`) |
| `ITuner` ya tiene los candidatos vía `configuration` | el KDoc de `ITuner.configuration`: *"Cambiarla recalcula `targets`"* |
| un flag en `TuningConfiguration` no rompería a nadie | es un `data class` de 4 campos, **tres con default**: agregar uno más con default es append-only |

🔑 **"La puerta está media abierta" es literal y es nuestro**: se puede LEER `lockedString` y
`fastModeState` desde la superficie pública, y no hay forma pública de ENCENDER el modo que los
produce. Esa asimetría no la había registrado ningún REQ.
