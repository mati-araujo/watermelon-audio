#!/usr/bin/env python3
"""
corpus-oracle-sigma.py — REQ-058 S1 (D50). EL RUIDO PROPIO DEL ORACULO CORTO DE f0, por clase y fase.

AC-058.4 juzga la σ de las publicaciones MEASURING con |error| <= 2·√(σ² + σ_or²): el error es contra el
oraculo corto de f0 (`corpus-reference-pitch.py --short --partials 4`), y ese oraculo tiene su propia
dispersion. En los estratos donde la σ del motor es chica, «|err| <= 2σ» mediria el ruido del oraculo y no
la honestidad de σ (medido en REQ-058 S1: sobre sintesis el oraculo erra 0,002-0,09 c; sobre corpus-v1 su
dispersion llega a 2-3 c en acero, donde los parciales baten).

PROCEDIMIENTO (fijado ANTES de juzgar; se cambia con una decision, no para que pase un test):
  · por archivo, t0 = el PRIMER `# ataque` del oraculo; ventana = t_fin - t_ini de su primer tramo, hop =
    t_ini del segundo - t_ini del primero; k = ceil(ventana/hop): tramos a k de distancia NO se solapan;
  · para cada tramo b con vecinos a = b-k y c = b+k, los tres con hz y a empezando en t0 o despues:
    d = -(cents(a, b) + cents(c, b))/2 (segunda diferencia: quita la tendencia lineal) y |d|/√1,5 (la
    varianza de la segunda diferencia de ruido iid es 1,5·σ²);
  · fase de b: (0,25, 1] s desde t0 o asentada (> 1 s); los de menos de 0,25 s no entran;
  · σ_or(clase, fase) = 1,4826 × mediana de |d|/√1,5 (MAD robusto a las colas). Para (0, 0,25] se usa la
    de (0,25, 1]. La clase es el prefijo del nombre del archivo.
Es una COTA SUPERIOR del ruido del oraculo: tambien cuenta el movimiento real de la altura mas rapido que
lineal (batido, vibrato).

    python3 scripts/corpus-oracle-sigma.py ORACULO_F0.tsv > SIGMA.tsv
Lo verifica `EarlyReadingCorpus.TheOracleSigmaIsTheVersionedOne`, que recomputa el TSV con el mismo
procedimiento en C++ y lo compara con el versionado.
"""
import collections, math, statistics, sys

def main(path):
    rows = collections.defaultdict(list)
    att, rate, order = {}, {}, []
    for line in open(path):
        if line.startswith('# archivo '):
            p = line.split(); cur = p[2]; rate[cur] = int(p[6]); order.append(cur); continue
        if line.startswith('# ataque '):
            p = line.split(); att.setdefault(p[2], []).append(int(p[4])); continue
        if line.startswith('#') or not line.strip(): continue
        p = line.rstrip('\n').split('\t')
        rows[p[0]].append((float(p[1]), float(p[2]), float(p[3])))
    vals = collections.defaultdict(list)
    for f in order:
        rs = rows[f]
        if f not in att or len(rs) < 2: continue
        t0 = att[f][0] / rate[f]
        win, hop = rs[0][1] - rs[0][0], rs[1][0] - rs[0][0]
        k = max(1, math.ceil(win / hop - 1e-9))
        cls = f.split('_')[0]
        for i in range(k, len(rs) - k):
            a, b, c = rs[i - k], rs[i], rs[i + k]
            if any(x[2] != x[2] for x in (a, b, c)) or a[0] < t0: continue
            since = b[0] - t0
            if since < 0.25: continue
            ph = 'ataque' if since <= 1.0 else 'asentada'
            d = -(1200.0 * math.log2(a[2] / b[2]) + 1200.0 * math.log2(c[2] / b[2])) / 2.0
            vals[(cls, ph)].append(abs(d) / math.sqrt(1.5))
    print("# sigma del oraculo corto de f0 por clase y fase (REQ-058 S1, D50). Generado por")
    print("#   python3 scripts/corpus-oracle-sigma.py " + path.split('/')[-1])
    print("# procedimiento: ver el docstring del script. fase 'ataque' = (0,25, 1] s desde el primer ataque del")
    print("# oraculo, y vale tambien para (0, 0,25]; 'asentada' = > 1 s. n = segundas diferencias usadas.")
    print("# clase\tfase\tn\tsigma_or_c")
    for (cls, ph) in sorted(vals):
        v = vals[(cls, ph)]
        print(f"{cls}\t{ph}\t{len(v)}\t{1.4826 * statistics.median(v):.4f}")

if __name__ == '__main__':
    if len(sys.argv) != 2: sys.exit("uso: corpus-oracle-sigma.py ORACULO_F0.tsv")
    main(sys.argv[1])
