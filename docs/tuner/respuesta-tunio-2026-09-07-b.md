---
title: "Respuesta a Tunio — su archivo ahora es nuestro corpus, y lo que midió sobre él"
type: reference
status: current
created: 2026-09-07
---

# Su archivo ya es nuestro corpus, y confirma su hipótesis 3 desde este lado

**2026-09-07 · sobre su nota de hoy (b) · watermelon-audio → Tunio**

Cuatro cosas: reproducimos su banco byte a byte y lo versionamos como corpus (REQ-032); su tabla
por tramos sale idéntica de este lado; el motor se midió sobre los 44 archivos con la bandera a la
vista y dejó tres hallazgos, dos de ellos los que ustedes reportaron; y la línea de contrato de su
§1 ya está escrita.

---

## 1. `corpus-v1`: su banco, fijado por artefacto

Bajamos el `.sf2` del commit `684543d` que ustedes nombraron: **32 319 396 bytes, blob sha1
`298b552d…` — el mismo**, sha256 `9575028c…`, y el chunk `INAM` dice `GeneralUser GS 2.0.3 BETA`
(leído del artefacto, no afirmado). Con FluidSynth 2.6.0 y su receta (`construir_banco.py`, tal
cual), los 44 WAV. Viven como assets del release
[`corpus-v1`](https://github.com/mati-araujo/watermelon-audio/releases/tag/corpus-v1) de este repo,
y `scripts/fetch-corpus.sh` los baja y verifica por sha256 contra el manifiesto.

**La verdad de cada archivo NO sale del motor ni del nominal**: `scripts/corpus-reference-pitch.py`
(Goertzel + Hann + interpolación, cuatro tramos de 0,75 s desde 2,0 s, dispersión como calidad).
Dos cosas que cambian su manifiesto:

- **El preset de guitarra limpia trae un glide de afinación de más de un segundo en el ataque**:
  E4 = −2,28 c a 0,5 s, −0,97 c a 1,0 s, +0,03 c desde 2,0 s. Por eso se mide desde 2,0 s. Su
  `f0_medido = 329,49964` con `calidad 0,017` era eso: la ventana pisando el glide. El archivo está
  a **+0,034 c del nominal** (329,63401), como el motor infería.
- **Donde nuestro oráculo y su `f0_medido` discrepan por más de 6 c, nuestros cuatro tramos
  coinciden a la milésima**: `bajo-acustico_G2` (+21,2 c, su calidad 0,26 no lo delataba),
  `guitarra-acero_G3` (+8,4), `guitarra-nylon_B3` (−6,8). Los valores están en
  `audio/src/main/cpp/analysis/tests/corpus-manifest.txt`, con la discrepancia anotada por línea.
  41 de 44 tienen altura estable (≤ 1 c entre tramos); tres bajos derivan y van sin hz.

## 2. Su tabla por tramos, de este lado: idéntica

`scripts/spectrum-by-segment.py` (versionado, para que corramos la misma) sobre
`guitarra-limpia_E4`, Hann 250 ms cada 250 ms, dB relativos al pico del tramo:

```
t_s   f0     2f0    3f0    f0/3
0.50  −7.2   −2.4   +0.0   −58.6
1.00  −7.0   −4.0   +0.0   −47.0
1.25  −8.7   −4.4   −0.4   −57.3     (el pico pasa a 4f0)
1.50… −9.0   −4.6   −0.5   −48.1     (constante hasta el final)
```

Al décimo de la suya. O sea que el render es el mismo, la tabla es la misma, y **su hipótesis 3
queda confirmada desde acá**: f0 y 2f0 están siempre, el espectro no se mueve, y lo que la síntesis
de REQ-031 no tenía era el resto. Su reproductor de siete senos (H7 a −9,6 dB) se reproduce
también, idéntico: seis convergen en 329,618; con el séptimo, 109,874 con bandera 0 — `NO_LOCK`
con candidatos, `NO_SIGNAL` sin ellos.

## 3. El motor sobre los 44, con la bandera a la vista

El barrido drena tick a tick con el mismo análisis del puerto, declara el instrumento por familia,
y se queda con **la última lectura fina que el afinador mostró mientras la nota sonaba** (la nota:
hasta el último bloque con rms ≥ 0,002; la más corta, `ukelele_A4`, 1,49 s). Verifica además
*"nunca convergido con bandera 0"* sobre todas las publicaciones: **0 de 2 000+**, igual que sus
1 320.

**Tres casos conocidos, declarados como trinquete** (si uno se arregla, el test se pone rojo):

| archivo | qué hace hoy |
|---|---|
| `guitarra-limpia_E4` | f0/3 (109,874), bandera 0, `NO_LOCK`, nunca convergida — REQ-031 sobre su archivo real |
| `guitarra-limpia_G3` | f0/5 (39,176), bandera 0 — lo mismo que midieron |
| `guitarra-acero_E4` | **su `false` en el ataque**, y peor de lo que se veía por prefijos: la gruesa lee f0/3 de 0,33 a 1,30 s, vuelve a E4, cae otra vez a 1,58 y vuelve; con instrumento declarado el modo rápido reengancha **seis** veces siguiéndola, y el strobe no junta sus 0,5 s antes de que la nota muera (2,1 s). Sin candidatos converge a 1,44 s a −0,40 c. |

Los tres son la clase de su §3 (timbre de ataque / H7). **El REQ sobre el detector grueso está
propuesto de este lado**, con su reproductor como control negativo ya en la suite — detrás de una
guarda que lo nombra y que imprime la medición (`SKIPPED`, nunca `PASSED`, hasta que arranque). Es
decisión de producto nuestra abrirlo; no está tomada todavía.

🔴 **Y un hallazgo que no esperábamos, y que es de nosotros**: el presupuesto de exactitud sobre
material real **no es el 0,1 c del contrato**. Con la lectura tomada mientras la nota suena, la
lectura **fina** se aparta del oráculo hasta **+5,40 c** (`ukelele_C4`; +4,28 `nylon_E4`, +2,99
`jazz_E4`), mientras la detección **gruesa** y el oráculo coinciden entre sí a ~0,1 c
(`ukelele_C4`: oráculo 262,500 · gruesa 262,483 · fina 263,32). Dos métodos independientes contra
uno: **el que se aparta es el strobe**, sobre cuerdas sampleadas inarmónicas y decayendo. No
concluimos la causa; el techo (6 c) quedó pinneado para que una regresión se vea, y es un REQ
propio. Si en su banco ven lo mismo con `cents` contra su `f0_medido` corregido, ése es el dato.

## 4. Su §1, escrito

Tienen razón y era nuestro. Ya está en el KDoc de `TunerSnapshot.spectralSupport`, en la C API
(`[17]`) y en la spec viva (R-API-52/26): **se lee la bandera antes que el estado**; sin candidatos
una altura ajena cae en `NO_SIGNAL` (R-PITCH-56) y la bandera viaja igual; `NO_LOCK` con `false`
sólo aparece con instrumento declarado.

---

## Estado de lo suyo

| pedido | dónde quedó |
|---|---|
| la tabla por tramos | **cerrada**: idéntica de este lado, script versionado; hipótesis 3 confirmada |
| el reproductor de siete senos | **en la suite**, guardado con nombre hasta que el REQ del detector se abra (decisión nuestra, pendiente) |
| sus dos `false` en el ataque | **reproducidos y explicados** (§3, `acero_E4`): seis reenganches en 2 s |
| §1 (`false` sobre `NO_SIGNAL`) | **documentado** en las tres capas |
| `setTunerCandidates` / `lockTunerString` públicas | sigue siendo nuestra; la evidencia nueva (§3) pesa: con candidatos el ataque cuesta lecturas, sin ellos no |
| el corpus | **ya no hace falta que manden nada**: `corpus-v1` es su banco |
