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

Salida por archivo (una linea, tab-separada):
    nombre  hz_mediana  spread_cents  n_tramos  veredicto(ok|inestable)

`spread_cents` es la diferencia max-min entre tramos: la CALIDAD del oraculo. Por encima de
`--max-spread` (1 cent por defecto) el archivo no tiene una altura estable que declarar y el
manifiesto lo lista sin hz — no se le inventa una.
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
    a = ap.parse_args()
    files = sorted(os.path.join(a.dir, f) for f in os.listdir(a.dir) if f.endswith(".wav")) \
            if a.dir else [a.wav]
    if not files or files == [None]:
        ap.error("falta el .wav o --dir")
    for path in files:
        name = os.path.basename(path)
        nominal = a.nominal or nominal_for(name)
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
