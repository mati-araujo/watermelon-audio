#!/usr/bin/env python3
"""
gen-glide-fixture.py — el fixture del glide para WV-3.2 (REQ-043, decisión 8 de la amplificación
del 2026-09-17). Regenerar es una TAREA EXPLÍCITA, nunca un efecto de correr los tests (misma regla
que regen-golden.sh): si el fixture cambia, cambia su sha256 en MANIFEST.txt y ESE diff es la revisión.

Escribe `<out>.wav` (48 kHz, estéreo, 16 bit, −20 dBFS) y `<out>.truth.txt`, EL ORÁCULO: la
frecuencia instantánea por hop, integrada por acumulador de fase — exacta y de afuera del sistema
bajo prueba (R-API-48: un valor que sale del motor no es oráculo). Incluye el vibrato, porque es la
f0 real que la cinta va a ver.

Tramos (carta de NoisyPad del 16/09 + el de 65 Hz que agregó la amplificación):
  [0,0 · 2,0 s)   glide 110 → 440 Hz, lineal en OCTAVAS (1 oct/s): el error relativo por ventana es
                  constante; en Hz el movimiento se concentra en el primer tercio
  [2,0 · 2,5 s)   silencio en CERO EXACTO, para que el 0/0 sea del detector y no del piso
  [2,5 · 4,0 s)   220 Hz estable
  [4,0 · 4,5 s)   65 Hz (C2): el único punto que mide 40 vs 30 ms de ventana (AC-043.2). Sin él,
                  "60–1200 Hz" sería una afirmación
Timbre: f0 + 6 armónicos a −6 dB/oct + vibrato 5 Hz ±15 c. Un seno puro no ejerce la elección de
τ del MPM (un solo pico por período: cualquier detector acierta) ni el soporte espectral.
`--vibrato 0` es el eje diagnóstico: si el p95 de AC-043.1 se acerca al techo, separa el vibrato
del detector. Fades de 5 ms en los bordes de cada tramo tonal para no meter un click que sea onset.

Uso:  python3 scripts/gen-glide-fixture.py --out audio/src/main/cpp/looper/tests/testdata/glide-voz
"""
import argparse, math, struct, wave

ap = argparse.ArgumentParser()
ap.add_argument("--out", required=True, help="prefijo de salida, sin extensión")
ap.add_argument("--vibrato", type=float, default=15.0, help="profundidad del vibrato en cents (0 = sin)")
ap.add_argument("--harmonics", type=int, default=6, help="armónicos además del f0 (0 = seno puro)")
ap.add_argument("--peak", type=float, default=-20.0, help="pico en dBFS")
ap.add_argument("--hop-ms", type=float, default=10.0, help="hop del oráculo (el del motor lo devuelve el motor)")
a = ap.parse_args()

SR = 48000
SEGMENTS = [("glide", 2.0), ("silence", 0.5), ("hold220", 1.5), ("hold65", 0.5)]
F0, F1 = 110.0, 440.0
EDGES = []
t = 0.0
for _, d in SEGMENTS:
    EDGES.append(t); t += d
EDGES.append(t)
n = int(round(SR * t))

def f_nominal(t):
    if t < EDGES[1]:
        return F0 * (F1 / F0) ** (t / (EDGES[1] - EDGES[0]))
    if t < EDGES[2]:
        return 0.0
    if t < EDGES[3]:
        return 220.0
    return 65.0

def f_inst(t):
    f = f_nominal(t)
    if f > 0 and a.vibrato > 0:
        f *= 2 ** (a.vibrato / 1200 * math.sin(2 * math.pi * 5.0 * t))
    return f

phase, samples, truth = 0.0, [], []
hop = int(round(SR * a.hop_ms / 1000))
for i in range(n):
    t = i / SR
    f = f_inst(t)
    if i % hop == 0:
        truth.append((i, f))
    if f == 0.0:
        samples.append(0.0)
        continue
    phase += 2 * math.pi * f / SR
    s = sum(math.sin(k * phase) / k for k in range(1, a.harmonics + 2))
    for edge in EDGES:
        d = abs(t - edge)
        if d < 0.005:
            s *= d / 0.005
    samples.append(s)

pk = max(abs(x) for x in samples)
g = 10 ** (a.peak / 20) / pk
pcm = [max(-32768, min(32767, int(round(x * g * 32767)))) for x in samples]
with wave.open(a.out + ".wav", "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes(struct.pack("<%dh" % (2 * n), *[v for v in pcm for _ in (0, 1)]))
with open(a.out + ".truth.txt", "w") as f:
    f.write("# frame  hz_verdadero  (0 = silencio exacto) · sr=%d hop=%d vibrato=%.1fc harmonics=%d peak=%.1f dBFS\n"
            % (SR, hop, a.vibrato, a.harmonics, a.peak))
    f.write("# fronteras de tramo (frames): %s\n" % " ".join(str(int(round(e * SR))) for e in EDGES))
    for fr, hz in truth:
        f.write("%d %.4f\n" % (fr, hz))
print("%s.wav: %d frames, %.3f s, pico %.1f dBFS, %d puntos de verdad (hop %d), %d en silencio"
      % (a.out, n, n / SR, a.peak, len(truth), hop, sum(1 for _, hz in truth if hz == 0.0)))
