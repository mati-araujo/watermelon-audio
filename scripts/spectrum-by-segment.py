#!/usr/bin/env python3
"""
spectrum-by-segment.py — REQ-032 S3. La tabla de dB POR TRAMOS de un WAV: f0, 2f0, 3f0 (y f0/3,
f0/5) relativos al pico del tramo, con ventana de Hann de 250 ms cada 250 ms desde el ataque.

Es la tabla que el consumidor produjo el 07/09 sobre `guitarra-limpia_E4` (nota 07/09 b, §3) y que
separo las tres hipotesis de la carta del 07/09. Versionada para que las dos partes corran LA MISMA
sobre cualquier archivo. Solo stdlib.

    python3 scripts/spectrum-by-segment.py archivo.wav --f0 329.634 [--win 0.25] [--step 0.25]
"""
import argparse, math, struct, wave

def read_mono(path):
    with wave.open(path, 'rb') as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    vals = struct.unpack('<%dh' % (n * ch), raw)
    mono = [v / 32768.0 for v in vals] if ch == 1 else \
           [(vals[i] + vals[i + 1]) / 65536.0 for i in range(0, n * ch, ch)]
    return mono, sr

def goertzel(x, sr, hz):
    w = 2.0 * math.pi * hz / sr; c = 2.0 * math.cos(w); s1 = s2 = 0.0
    for v in x:
        s = v + c * s1 - s2; s2 = s1; s1 = s
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav"); ap.add_argument("--f0", type=float, required=True)
    ap.add_argument("--win", type=float, default=0.25); ap.add_argument("--step", type=float, default=0.25)
    ap.add_argument("--harmonics", type=int, default=8)
    a = ap.parse_args()
    mono, sr = read_mono(a.wav)
    n = int(a.win * sr)
    hann = [0.5 * (1.0 - math.cos(2.0 * math.pi * (i + 0.5) / n)) for i in range(n)]
    cols = [("f0/5", a.f0 / 5), ("f0/3", a.f0 / 3)] + [(f"{k}f0" if k > 1 else "f0", k * a.f0) for k in range(1, a.harmonics + 1)]
    print("t_s\trms\t" + "\t".join(c for c, _ in cols) + "\t(dB rel. al pico del tramo entre f0..%df0)" % a.harmonics)
    t = 0.0
    while int(t * sr) + n <= len(mono):
        a0 = int(t * sr)
        x = [mono[a0 + i] * hann[i] for i in range(n)]
        rms = math.sqrt(sum(v * v for v in mono[a0:a0 + n]) / n)
        mags = {c: goertzel(x, sr, f) for c, f in cols}
        peak = max(mags[c] for c, _ in cols if not c.startswith("f0/")) or 1e-12
        row = "\t".join(f"{20*math.log10(mags[c]/peak):+.1f}" if mags[c] > 0 else "-inf" for c, _ in cols)
        print(f"{t:.2f}\t{rms:.4f}\t{row}")
        t += a.step

if __name__ == "__main__":
    main()
