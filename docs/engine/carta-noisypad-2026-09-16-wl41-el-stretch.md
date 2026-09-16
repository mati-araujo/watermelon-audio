---
title: "NoisyPad → watermelon-audio · WL-4.1: el stretch, y lo que la performance visual necesita saber de él"
type: reference
status: current
created: 2026-09-16
---

# Carta de NoisyPad: WL-4.1, el contrato del stretch

**Recibida el 2026-09-16 a la noche**, independiente de la de WV-3. No pide API nueva (WL-4.1/4.2 están en
`docs/looper/looper_evolution_requirements.md`): pide **tres cosas del contrato** de `stretchTrack` para
que la performance visual (PV-4.4) no re-derive el ratio. Prioridad declarada: **la voz (WV-3.2) antes que
el stretch**. Texto íntegro, como llegó (copia en `specs/referencias/inbox-noisypad-2026-09-16-carta-wl41.md`).

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo verificado está al final.

---

# NoisyPad → watermelon-audio · WL-4.1: el stretch, y lo que la performance visual necesita saber de él

**2026-09-16, noche.** Segunda carta, independiente de la de WV-3. Contexto: desde la Fase 17 cada toma
del pad lleva una performance visual (muestras del cursor y golpes de grid con `frameOffset` en frames
del buffer de la pista) que el video y el cursor fantasma reproducen. PV-4.4 de nuestra spec dice que la
performance tiene que seguir a cualquier transformación del audio: trim/región (ya lo hace: el video
wrapea en `[loopStart, loopEnd)`), stretch y nudge/rotate. Hoy PV-4.4 está bloqueado en NoisyPad porque
el motor no tiene stretch ni nudge — y la mitad que sí existe (varispeed por `getTrackSpeed`) ya la
consumimos: el cursor recorre la vuelta en `frames × speed`.

Su spec ya lo tiene escrito (`docs/looper/looper_evolution_requirements.md` WL-4.1 / WL-4.2): no pedimos
otra API. Pedimos tres cosas del contrato que, si faltan, nos obligan a re-derivar el ratio del lado de
la UI —y ahí es donde hoy se nos escapan los desfasajes que crecen vuelta a vuelta.

## 1 · Lo que pedimos del contrato de `stretchTrack(track, ratio, semitones)`

1. Que devuelva (o exponga) el ratio EFECTIVO aplicado y el largo nuevo del buffer en frames, no sólo
   `ok`. Si el algoritmo redondea a bloques o ajusta el ratio para cerrar en barra, nuestro
   `frameOffset × ratio` va a estar mal por ese redondeo, y acumulado. Preferimos
   `{ratioEffective, newLengthFrames, newLoopStart, newLoopEnd}`.
2. Que la región se preserve proporcionalmente (ya lo dice la spec) y que el origen del buffer no se
   mueva: `frame 0` antes = `frame 0` después. Si el stretch introduce latencia/pre-roll, que la absorba
   adentro (o la reporte como `offsetFrames`): un corrimiento constante de 20 ms en una performance de
   cursor se ve como "el dedo llega tarde" en cada vuelta.
3. Que `contentVersion`-equivalente cambie (o un callback "buffer swapped"): nuestra invalidación de
   performances y del caché de waveform va por versión. Hoy `importTrack` nos enseñó que bumpear la
   versión no alcanza si el consumidor no sabe qué cambió; para stretch queremos saber que fue un
   stretch (para transformar la performance) y no un reemplazo (para descartarla).

Para nudge/rotate (si entra en WL-4.x): `rotateTrack(track, frames)` con el mismo trío —frames
efectivos, región, versión— y rotación modular sobre la REGIÓN, no sobre el buffer (es el eje que suena).

## 2 · Precedente que ya funciona y queremos repetir

`armInFrames` / `armSyncedToLoop` devuelven el frame absoluto de disparo (WV-4.1): fue lo que hizo que
las performances queden sincronizadas al sample sin adivinar. El mismo principio acá: el motor devuelve
el número que aplicó, la UI no lo re-deriva.

## 3 · Cómo lo vamos a verificar

- Toma XY de 4 s con un gesto conocido (barrido lineal en X) → `stretchTrack(ratio 1,25)` → esperamos
  `newLengthFrames = round(length × 1,25)` ± 1 bloque y que el cursor del video llegue al mismo X en el
  mismo onset (los onsets de `detectOnsets` pre y post stretch, escalados por `ratioEffective`, tienen que
  coincidir ± 1 hop). Umbral de hallazgo: un corrimiento constante > 10 ms o uno que crezca con las
  vueltas.
- Control negativo: `ratio = 1,0` deja el buffer, la región y la performance byte a byte iguales.
- Todo en el Moto G42 con nuestro canal de debug, tabla en el formato acordado.

## 4 · Prioridad, honestamente

Para nosotros WV-3.2 (voz) vale más que WL-4.1: destraba una fuente entera del video. WL-4.1 destraba
PV-4.4, que es corrección de algo que hoy el usuario no puede hacer (no hay stretch). Si tienen que
elegir, primero la voz; esta carta existe para que, cuando el stretch llegue, llegue con los tres
números de arriba y no tengamos que pedirlos en un patch.

---

## Lo que verificamos de nuestro lado (2026-09-16)

- **WL-4.1 no está empezado**: no hay `signalsmith-stretch` en `thirdparty/`, ni `wma_looper_stretch_track`
  en la C API. La spec (`looper_evolution_requirements.md:89`) lo tiene como P1/L, detrás de WL-2.x y
  WL-5.x en la ruta crítica. Lo que piden es **contrato**, y va a la spec de WL-4.1 ahora, cueste lo que
  cueste después: `{ratioEffective, newLengthFrames, newLoopStart, newLoopEnd, offsetFrames}` de
  retorno; origen del buffer invariante; y el versionado con **causa** (stretch vs reemplazo). Ese
  último punto ya nos lo enseñó `importTrack`, y la memoria del repo lo tiene como clase ("un bump de
  versión sin decir qué cambió").
- **El precedente que citan es real**: `armInFrames`/`armSyncedToLoop` devuelven el frame absoluto
  (WV-4.1, hecho). El principio "el motor devuelve el número que aplicó" ya está en R-API-48/46 (el
  rango de teclas sale del nombre, no del sistema bajo prueba) y en el diseño de `sf-delta-host.py`.
- **Decisión pendiente del humano**: anotar los tres puntos como AC de WL-4.1 en su doc (cambio de
  spec, sin código), y que el REQ del stretch, cuando se abra, los herede como deltas declarados. No se
  abre ahora: la carta misma lo pone detrás de la voz.
