#!/usr/bin/env python3
"""
corpus-reference-pitch.py — REQ-032 S1. EL ORACULO del corpus: la altura verdadera de cada WAV.

🔴 INDEPENDIENTE DEL MOTOR, A PROPOSITO. Un `hz_verdadero` que saliera del propio afinador no seria
un oraculo (R-API-48): el barrido probaria que el motor es igual a si mismo. Y NO es el nominal: el
G2 del bajo acustico viene 21 cents desafinado en el propio SoundFont, y el f0 medido que el
consumidor mando fallo justo en la E4 (calidad 0,017). Esto mide el FUNDAMENTAL sobre el archivo,
con un metodo distinto del que usa el motor (NSDF + strobe de fase): pico espectral por Goertzel con
ventana de Hann e interpolacion parabolica, sobre varios tramos, y reporta cuanto discrepan.

Solo stdlib: no le agrega una dependencia al repo por un script que corre una vez por archivo.

    python3 scripts/corpus-reference-pitch.py archivo.wav [--nominal HZ]
    python3 scripts/corpus-reference-pitch.py --dir DIR         # todos los .wav, nominal por nombre
    python3 scripts/corpus-reference-pitch.py --dir DIR --partials 4   # REQ-035: H1..H4 por archivo
    python3 scripts/corpus-reference-pitch.py --self-test      # el instrumento contra una serie conocida

Salida por archivo (una linea, tab-separada):
    nombre  hz_mediana  spread_cents  n_tramos  veredicto(ok|inestable)

`spread_cents` es la diferencia max-min entre tramos: la CALIDAD del oraculo. Por encima de
`--max-spread` (1 cent por defecto) el archivo no tiene una altura estable que declarar y el
manifiesto lo lista sin hz — no se le inventa una.

EL ORACULO POR PARCIAL (REQ-035 S1, `--partials N`)
---------------------------------------------------
El strobe del motor ajusta `cents_n = C + 600·log2(1 + B·n²)` sobre los parciales 1..4 y publica C.
Sobre el corpus real esa C se aparta del H1 hasta +5,4 cents, y para saber DONDE nace el error hay
que medir cada parcial del archivo con un metodo que no sea el del motor. Con `--partials N` se
mide, en los mismos tramos, el pico de cada parcial n alrededor de `n·hz_1` y se imprime:

    cents_n   la desviacion del parcial n respecto de `n·hz_1`, en cents (n = 1 vale 0 por
              construccion: hz_1 ES la referencia)
    dB_n      su nivel respecto de H1 (un parcial a −40 dB no es un parcial: es ruido con pico)
    B_fit     el B que mejor explica cents_1..N con el MISMO modelo que usa el motor, ajustado
              aca en Python (busqueda por seccion aurea, C libre): independiente del strobe
    C_fit     la C de ese ajuste — lo que un ajuste no ponderado publicaria sobre ESTOS parciales
    res_max   el residuo maximo del ajuste, en cents: cuanto NO sigue la serie estirada el sample

Si `C_fit` reproduce el error de la fina del motor, el error nace en los parciales del sample y en
el modelo, no en el rastreo de fase. Si no, hay que mirar el rastreo. La linea sigue siendo
tab-separada; con `--partials 1` (el default) la salida es la de siempre, byte a byte.

🔴 `C` NO ES H1, Y LA DIFERENCIA ES `600·log2(1+B)`. En una cuerda con rigidez el parcial 1 TAMBIEN
esta estirado: `f_1 = f0·√(1+B)`. El motor publica C = la desviacion del fundamental IDEAL f0; este
oraculo toma como referencia el H1 que suena. Sobre una serie perfecta, entonces, `C_fit` vale
`−600·log2(1+B)`: −0,09 c con B = 1e-4 y −0,43 c con 5e-4. Lo delato el propio self-test (la
primera version esperaba 0 y salio rojo con 5e-4). Es fisica, no error, y esta por debajo de lo
que REQ-035 persigue (+5,4 c) — pero un lector que compare C con H1 tiene que restarlo.

🔴 EL INSTRUMENTO SE VERIFICA ANTES DE CREERLE (`--self-test`): sobre un WAV sintetico con B
conocido devuelve `600·log2(1+B·n²)` a 0,1 c y recupera B; y sobre uno con un parcial corrido
+10 c el residuo lo delata (> 2 c). Un oraculo que no puede fallar no es un oraculo.
"""
import argparse, math, os, struct, sys, wave

# Nominales por nombre de cuerda: SOLO para centrar la busqueda (±100 cents). El resultado sale
# del archivo. Tabla publicada (A4 = 440), no derivada del motor.
NOMINAL = {"E1": 41.20344, "A1": 55.0, "D2": 73.41619, "G2": 97.99886,
           "E2": 82.40689, "A2": 110.0, "D3": 146.83238, "G3": 195.99772, "B3": 246.94165,
           "E4": 329.62756, "C4": 261.62557, "G4": 391.99544, "A4": 440.0}

def read_mono(path):
    with wave.open(path, 'rb') as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise SystemExit(f"{path}: solo PCM de 16 bits (tiene {sw*8})")
    vals = struct.unpack('<%dh' % (n * ch), raw)
    if ch == 1:
        mono = [v / 32768.0 for v in vals]
    else:
        mono = [(vals[i] + vals[i + 1]) / 65536.0 for i in range(0, n * ch, ch)]
    return mono, sr

def goertzel(x, sr, hz):
    """Magnitud en `hz`. `x` ya viene con la ventana aplicada."""
    w = 2.0 * math.pi * hz / sr
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s = v + c * s1 - s2
        s2 = s1
        s1 = s
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2))

def cents(f, ref): return 1200.0 * math.log2(f / ref)
def detune(ref, c): return ref * 2.0 ** (c / 1200.0)

def peak_in(x, sr, center, span_cents, step_cents):
    """Barre `center` ± span en pasos y devuelve (hz, mag) del maximo, con interpolacion parabolica."""
    grid = []
    c = -span_cents
    while c <= span_cents + 1e-9:
        grid.append(c); c += step_cents
    mags = [goertzel(x, sr, detune(center, g)) for g in grid]
    i = max(range(len(mags)), key=lambda k: mags[k])
    if 0 < i < len(mags) - 1:
        a, b, cc = mags[i - 1], mags[i], mags[i + 1]
        den = a - 2 * b + cc
        off = 0.5 * (a - cc) / den if den != 0 else 0.0
    else:
        off = 0.0
    return detune(center, grid[i] + off * step_cents), mags[i]

def measure(path, nominal, win_s=0.75, starts=(2.0, 2.75, 3.5, 4.25)):
    mono, sr = read_mono(path)
    n = int(win_s * sr)
    hann = [0.5 * (1.0 - math.cos(2.0 * math.pi * (i + 0.5) / n)) for i in range(n)]
    # Desde 2,0 s, no desde el ataque: MEDIDO que el preset de guitarra limpia (PC 27) trae un
    # GLIDE de afinacion que dura mas de un segundo (E4: -2,28 c a 0,5 s, -0,97 c a 1,0 s, +0,03 c
    # de 2,0 s en adelante; G3: -3,9 c y despues -1,02 c estable). El afinador mide la nota
    # sostenida, no el transitorio, y el manifiesto declara lo sostenido. Cuatro tramos de 0,75 s
    # hasta el note-off (5,0 s); si un archivo no se estabiliza ni ahi, el spread lo dice.
    per_window = []
    for t0 in starts:
        a = int(t0 * sr)
        if a + n > len(mono): break
        x = [mono[a + i] * hann[i] for i in range(n)]
        coarse, _ = peak_in(x, sr, nominal, 100.0, 10.0)   # ±100 cents, de a 10
        fine, _ = peak_in(x, sr, coarse, 12.0, 0.25)       # ±12 cents, de a 0,25
        per_window.append(fine)
    if not per_window:
        return None, None, 0, []
    raw = list(per_window)
    per_window.sort()
    med = per_window[len(per_window) // 2] if len(per_window) % 2 else \
          0.5 * (per_window[len(per_window) // 2 - 1] + per_window[len(per_window) // 2])
    spread = cents(per_window[-1], per_window[0])
    return med, spread, len(per_window), raw

# --- REQ-035 S1: el oraculo POR PARCIAL ------------------------------------------------------

def stretch_cents(B, n):
    """El modelo del motor: `600·log2(1 + B·n²)`. Se reescribe aca a proposito (no se importa nada
    del motor): es la formula publicada de una cuerda con rigidez, no una decision del codigo."""
    return 600.0 * math.log2(1.0 + B * n * n)

def fit_stretched(cents_by_n, b_max=5e-3, iters=60):
    """Ajusta `cents_n = C + stretch(B, n)` con C libre y B en [0, b_max] (seccion aurea sobre B;
    para B fijo la C optima es la media de los residuos). Devuelve (B, C, residuos por n)."""
    ns = sorted(cents_by_n)
    def sse(B):
        C = sum(cents_by_n[n] - stretch_cents(B, n) for n in ns) / len(ns)
        return sum((cents_by_n[n] - C - stretch_cents(B, n)) ** 2 for n in ns), C
    phi = 0.6180339887498949
    lo, hi = 0.0, b_max
    b1, b2 = hi - phi * (hi - lo), lo + phi * (hi - lo)
    f1, f2 = sse(b1)[0], sse(b2)[0]
    for _ in range(iters):
        if f1 < f2:
            hi, b2, f2 = b2, b1, f1
            b1 = hi - phi * (hi - lo); f1 = sse(b1)[0]
        else:
            lo, b1, f1 = b1, b2, f2
            b2 = lo + phi * (hi - lo); f2 = sse(b2)[0]
    B = 0.5 * (lo + hi)
    _, C = sse(B)
    return B, C, {n: cents_by_n[n] - C - stretch_cents(B, n) for n in ns}

def measure_partials(path, nominal, partials, win_s=0.75, starts=(2.0, 2.75, 3.5, 4.25)):
    """Como `measure`, y ademas los parciales 2..`partials` alrededor de `n·hz_1` en cada tramo.

    Devuelve un dict por n con `hz` (mediana entre tramos), `cents` respecto de `n·hz_1`,
    `spread` entre tramos y `db` respecto de H1 (mediana). Para n = 1, `cents` vale 0.0.
    """
    mono, sr = read_mono(path)
    n_win = int(win_s * sr)
    hann = [0.5 * (1.0 - math.cos(2.0 * math.pi * (i + 0.5) / n_win)) for i in range(n_win)]
    per = {n: [] for n in range(1, partials + 1)}     # hz por tramo
    mag = {n: [] for n in range(1, partials + 1)}
    for t0 in starts:
        a = int(t0 * sr)
        if a + n_win > len(mono): break
        x = [mono[a + i] * hann[i] for i in range(n_win)]
        coarse, _ = peak_in(x, sr, nominal, 100.0, 10.0)
        f1, m1 = peak_in(x, sr, coarse, 12.0, 0.25)
        per[1].append(f1); mag[1].append(m1)
        for n in range(2, partials + 1):
            if n * f1 >= 0.5 * sr: break
            # El estiramiento de una cuerda real llega a ~14 c en el 4to parcial con B = 1e-3:
            # ±30 c de a 3 lo cubre con margen, y despues ±4 c de a 0,25 para interpolar.
            c, _ = peak_in(x, sr, n * f1, 30.0, 3.0)
            f, m = peak_in(x, sr, c, 4.0, 0.25)
            per[n].append(f); mag[n].append(m)
    out = {}
    if not per[1]:
        return out
    def median(v):
        v = sorted(v)
        return v[len(v) // 2] if len(v) % 2 else 0.5 * (v[len(v) // 2 - 1] + v[len(v) // 2])
    hz1 = median(per[1]); m1 = median(mag[1])
    for n in range(1, partials + 1):
        if not per[n]: continue
        hz = median(per[n])
        out[n] = {"hz": hz,
                  "cents": 0.0 if n == 1 else cents(hz, n * hz1),
                  "spread": cents(max(per[n]), min(per[n])),
                  "db": 20.0 * math.log10(max(median(mag[n]), 1e-12) / max(m1, 1e-12)),
                  "n_tramos": len(per[n])}
    return out

def format_partials(name, nominal, res, max_spread):
    if 1 not in res:
        return f"{name}\t-\t-\t0\tsin-tramos"
    hz1 = res[1]["hz"]
    verdict = "ok" if res[1]["spread"] <= max_spread else "inestable"
    cols = [name, f"{hz1:.5f}", f"{res[1]['spread']:.3f}", str(res[1]["n_tramos"]), verdict,
            f"vs-nominal={cents(hz1, nominal):+.3f}c"]
    for n in sorted(res):
        if n == 1: continue
        r = res[n]
        cols.append(f"p{n}={r['cents']:+.2f}c(±{r['spread']:.2f},{r['db']:+.0f}dB)")
    fit = {n: res[n]["cents"] for n in res}
    if len(fit) >= 3:
        B, C, resid = fit_stretched(fit)
        cols.append(f"B_fit={B:.2e}")
        cols.append(f"C_fit={C:+.2f}c")
        cols.append(f"res_max={max(abs(v) for v in resid.values()):.2f}c")
    return "\t".join(cols)

def self_test():
    """El instrumento contra una serie CONOCIDA, en las dos direcciones. Sale con 1 si falla."""
    import tempfile
    sr, secs, f0 = 44100, 6.0, detune(82.40689, 3.0)     # E2, +3 c: el error no puede ser 0
    def write_wav(path, partial_cents):
        n = int(secs * sr)
        buf = [0.0] * n
        for k, dc in partial_cents.items():
            fk = detune(k * f0, dc)
            w = 2.0 * math.pi * fk / sr
            a = 0.3 / k
            for i in range(n): buf[i] += a * math.sin(w * i)
        with wave.open(path, 'wb') as wv:
            wv.setnchannels(2); wv.setsampwidth(2); wv.setframerate(sr)
            wv.writeframes(b''.join(struct.pack('<hh', int(v * 32767), int(v * 32767)) for v in buf))
    failures = []
    with tempfile.TemporaryDirectory() as d:
        for B in (1e-4, 5e-4):
            p = os.path.join(d, f"serie_B{B:g}.wav")
            write_wav(p, {k: stretch_cents(B, k) for k in range(1, 7)})
            res = measure_partials(p, 82.40689, 4)
            # Respecto de H1 (que esta en f0·√(1+B)), no de f0: ver el bloque `C NO ES H1`.
            for n in range(1, 5):
                want = stretch_cents(B, n) - stretch_cents(B, 1)
                got = res[n]["cents"]
                if abs(got - want) > 0.1:
                    failures.append(f"B={B:g} parcial {n}: {got:+.3f} c contra {want:+.3f} esperado")
            Bf, Cf, resid = fit_stretched({n: res[n]["cents"] for n in res})
            if abs(Bf / B - 1.0) > 0.10: failures.append(f"B={B:g}: B_fit {Bf:.3e} fuera del 10 %")
            if abs(Cf + stretch_cents(B, 1)) > 0.1:
                failures.append(f"B={B:g}: C_fit {Cf:+.3f} c, tenia que ser {-stretch_cents(B, 1):+.3f} ± 0,1")
            h1 = cents(res[1]["hz"], 82.40689)
            if abs(h1 - 3.0 - stretch_cents(B, 1)) > 0.1:
                failures.append(f"B={B:g}: H1 leyo {h1:+.3f} c, tenia que ser {3.0 + stretch_cents(B, 1):+.3f}")
        # La otra direccion: un parcial que NO sigue la serie tiene que salir como residuo.
        p = os.path.join(d, "p3_corrido.wav")
        c = {k: stretch_cents(1e-4, k) for k in range(1, 7)}; c[3] += 10.0
        write_wav(p, c)
        res = measure_partials(p, 82.40689, 4)
        Bf, Cf, resid = fit_stretched({n: res[n]["cents"] for n in res})
        if max(abs(v) for v in resid.values()) <= 2.0:
            failures.append(f"p3 +10 c: res_max {max(abs(v) for v in resid.values()):.2f} c no delata al parcial corrido")
        if abs(res[3]["cents"] - (stretch_cents(1e-4, 3) - stretch_cents(1e-4, 1) + 10.0)) > 0.1:
            failures.append(f"p3 +10 c: el oraculo leyo {res[3]['cents']:+.3f} c en el parcial 3")
    for f in failures: print("SELF-TEST FALLO:", f)
    print("self-test:", "ROJO" if failures else "verde", f"({len(failures)} fallos)")
    return 1 if failures else 0

def nominal_for(name):
    cuerda = os.path.splitext(name)[0].rsplit('_', 1)[-1]
    if cuerda not in NOMINAL:
        raise SystemExit(f"{name}: no se que cuerda es '{cuerda}'; pasa --nominal")
    return NOMINAL[cuerda]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", nargs="?")
    ap.add_argument("--dir")
    ap.add_argument("--nominal", type=float)
    ap.add_argument("--max-spread", type=float, default=1.0)
    ap.add_argument("--verbose", action="store_true", help="imprime la altura de cada tramo")
    ap.add_argument("--from", dest="t0", type=float, default=2.0, help="segundo del primer tramo")
    ap.add_argument("--partials", type=int, default=1,
                    help="REQ-035: medir tambien los parciales 2..N alrededor de n·hz_1")
    ap.add_argument("--self-test", action="store_true",
                    help="verificar el oraculo por parcial contra una serie sintetica conocida")
    a = ap.parse_args()
    if a.self_test:
        sys.exit(self_test())
    files = sorted(os.path.join(a.dir, f) for f in os.listdir(a.dir) if f.endswith(".wav")) \
            if a.dir else [a.wav]
    if not files or files == [None]:
        ap.error("falta el .wav o --dir")
    for path in files:
        name = os.path.basename(path)
        nominal = a.nominal or nominal_for(name)
        if a.partials > 1:
            starts = tuple(a.t0 + 0.75 * k for k in range(4))
            print(format_partials(name, nominal, measure_partials(path, nominal, a.partials, starts=starts),
                                  a.max_spread))
            sys.stdout.flush()
            continue
        hz, spread, n, raw = measure(path, nominal, starts=tuple(a.t0 + 0.75 * k for k in range(4)))
        if a.verbose and raw:
            print(f"  tramos: " + "  ".join(f"{cents(r, nominal):+.3f}c" for r in raw))
        if hz is None:
            print(f"{name}\t-\t-\t0\tsin-tramos"); continue
        verdict = "ok" if spread <= a.max_spread else "inestable"
        print(f"{name}\t{hz:.5f}\t{spread:.3f}\t{n}\t{verdict}\tvs-nominal={cents(hz, nominal):+.3f}c")
        sys.stdout.flush()

if __name__ == "__main__":
    main()
