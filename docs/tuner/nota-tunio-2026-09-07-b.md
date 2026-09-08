---
title: "Nota a upstream — 2.16.0 adoptada, y las dos cosas que pidieron"
type: reference
created: 2026-09-07
---

# 2.16.0 adoptada, y las dos cosas que pidieron

**2026-09-07 · Tunio → watermelon-audio · sobre su respuesta a REQ-031**

Adoptamos 2.16.0 (Tunio REQ-019). `spectralSupport` cruza el seam como `soporteEspectral`, la UI
deja de pedir objetivo sobre una altura sin soporte —el hueco del header dice `A2?` y el nodo
accesible "suena algo cerca de A2, no confiable"—, y `detectionClarity` no se tocó. Abajo va lo que
pidieron, y una cosa que encontramos en el camino.

## 1. Sin candidatos, la bandera en `false` viaja sobre `NO_SIGNAL`

Ustedes escribieron *"`NO_LOCK` ahora cubre dos situaciones y la bandera las separa"*. Es cierto
**con** candidatos. Sin ellos —que es lo que nuestra app declara hoy, y lo que su R-PITCH-56
prescribe para una altura ajena— el estado sigue siendo `NO_SIGNAL` y la bandera viene igual:

```
E4 sintética, f0 −20 dB, sin H2, H3 y H5 (su falso)
  sin candidatos   NO_SIGNAL   detectedHz=109.874   cents=null   spectralSupport=false
  con las seis     NO_LOCK     detectedHz=109.874   cents=null   spectralSupport=false
```

No es un defecto: es el contrato. Lo decimos porque un consumidor que lea *"mirá la bandera cuando
veas `NO_LOCK`"* al pie de la letra no la miraría nunca sin candidatos. Nosotros la miramos antes que
el estado, y nuestro discriminador de cuerda ajena —que disparaba justo sobre `NO_SIGNAL` +
claridad 0,9946— calla cuando ella dice `false`.

## 2. El banco contra 2.16.0, con `spectralSupport` a la vista

44 archivos × prefijos de 0,5 a 4 s × sin/con candidatos. Lo que importa:

- **`guitarra-limpia_E4`: 14 de 14 ventanas `false`**, altura 109,873, **cero convergidas en A2**, en
  los dos modos. **`guitarra-limpia_G3`: 15 de 15 `false`** sobre 39,176 (f0/5). Cerrado.
- 40 archivos sin cambio de estado ni de altura respecto de 2.15.0, bandera `true` en toda ventana
  con altura.
- **Ninguna ventana `CONVERGED` con `false`** sobre 1 320.

🔴 **`false` sobre dos cuerdas reales, en el ataque** — el dato que pidieron, tal cual:

```
guitarra-acero_E4   0,50–1,00 s   NO_SIGNAL  109,9 Hz (f0/3)  claridad 0,96–0,99  false ×3
                    1,25 s →      CONVERGED  329,9            cents +0,75         true
                    (y un NO_LOCK con 109,8 false suelto a 1,75 s)
guitarra-acero_G3   0,50–2,50 s   NO_SIGNAL   39,2 Hz (f0/5)  claridad 0,95–0,996 false ×9
                    2,75 s →      MEASURING/CONVERGED 196,0   cents −0,4…−0,9     true
```

La bandera tiene razón las dos veces —esa altura no suena— y el archivo se mide bien después. Lo
que muestra es que **el subarmónico existe en más timbres que limpia**, sólo que ahí se le pasa.
Durante ese tramo la cuerda no se afina; antes de 2.16.0 tampoco (era `NO_SIGNAL` sin marca). No
pedimos bajar ningún umbral.

## 3. La tabla por tramos — y es la hipótesis 3, con nombre y apellido

Su pedido, exacto: `guitarra-limpia_E4`, Hann de 250 ms cada 250 ms, dB relativos al pico del tramo
(control: la ventana de 1 s desde 0,3 s reproduce su −4,8 / −1,8 / 0,0 al décimo):

```
t_s   f0     2f0    3f0   f0/3
0.00  −11.3  −2.6   0.0   −58.5
0.25   −8.8  −2.4   0.0   −66.0
0.50   −7.2  −2.4   0.0   −58.6
0.75   −6.8  −3.4   0.0   −57.7
1.00   −7.1  −4.0   0.0   −47.0
1.25…  −8.5  −4.0   0.0   −47…−48     (constante hasta el final)
```

f0 y 2f0 están en todos los tramos, ataque incluido, y el espectro no se mueve. Las hipótesis 1 y 2
se caen; y del otro lado, el motor lee 109,87 con `false` arrancando el archivo en 0, 0,25, 0,5 o
1,0 s, a ganancia ×1, ×4 y ×16. **El error vive en el régimen y no depende del nivel.**

Lo que su síntesis no tenía es **el resto del espectro**. FFT del tramo estable:

```
H1 −7,2 · H2 −3,2 · H3 −0,6 · H4 0,0 · H5 −15,1 · H6 −15,0 · H7 −9,6 · H8 −20,9 ·
H13 −17,6 · H14 −13,2 · H16 −18,7 · H17 −14,5 · H18 −17,3     (y H13–H18 estirados +0,1 %…+0,12 %)
```

Y las sondas sobre 2.16.0, sin y con candidatos, todas iguales en los dos modos:

| síntesis de E4 | resultado |
|---|---|
| sus tres parciales (−4,8 / −1,8 / 0) | converge |
| seis parciales medidos, a cualquier nivel, fases al azar | converge |
| **H1..H7 armónicos, con H7 a −9,6 dB** | **109,87 Hz, `false`** |
| 18 parciales medidos, armónicos exactos | 109,87, `false` |
| seis parciales con las frecuencias medidas (estiradas) | 109,78, `false` |

**Reproductor mínimo: siete senos armónicos con H7 a −9,6 dB.** Con seis converge; con el séptimo,
no. Y hay un **segundo disparador independiente: el estiramiento de los parciales**, que con los
mismos seis niveles que convergen armónicos deja de converger. Los dos son timbre real de guitarra
sampleada.

## Estado de lo suyo, visto desde acá

| | |
|---|---|
| 2.16.0 | adoptada; ci-l2 y los tres smokes en verde; medida en el aparato |
| control negativo de su suite | verde de su lado; nuestra red offline lo fija también, con y sin candidatos, más el contrato "nunca convergido con `false`" sobre toda publicación |
| el banco con la bandera | §2, con los dos `false` reales |
| la tabla por tramos | §3: hipótesis 3, reproductor de siete senos |
| `setTunerCandidates` / `lockTunerString` públicas | sigue siendo suya; ahora sabemos que sin candidatos la bandera viaja sobre `NO_SIGNAL` (§1) |

---

*La evidencia cruda vive en el sidecar de Tunio, en `archive/2026-09-REQ-019-…/mediciones/`
(`sonda-banco-216-2026-09-07.tsv`, `tramos-250ms-guitarra-limpia_E4.tsv`,
`picos-fft-guitarra-limpia_E4.tsv`, y las sondas `Sonda*.kt.txt` que corren sobre `OfflineTuner`).*
