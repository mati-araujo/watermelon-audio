#!/usr/bin/env python3
"""
check-test-waits.py — que una espera cruda en un test no pueda entrar sin clasificar.

REQ-002 · S4. Tres de cinco pushes a master fallaron el 2026-08-20 por tests que
sincronizaban con el reloj de pared. S2 y S3 pagaron los sitios; esto existe para
que la clase no vuelva.

POR QUE ATAJA EL CAMINO Y NO LA FORMA
--------------------------------------
Un lint que buscara "esperas sospechosas" por su forma se evade sin querer: basta
escribir el sleep distinto. Lo que se pide aca es otra cosa — que toda espera
cruda en un test este CLASIFICADA, porque la clasificacion es justamente el
trabajo que S3 midio como el dificil. De 28 esperas, la mayoria NO eran defectos:

    polling (bucle con deadline) ....... 13   ya es la forma correcta
    estimulo (la duracion ES el test) ..  8   jitter, intervalos, forzar un orden
    presencia (espera y afirma) ........  4   -> wma_test::waitUntil
    ausencia (espera a que NO pase) ....  4   -> wma_test::sleepFixed

Las dos ultimas ya no son crudas: pasan por el helper. Este lint cubre las dos
primeras, que son las que siguen siendo `sleep_for` legitimo, y obliga a que
cada una diga cual es.

EL POLLING SE RECONOCE SOLO; EL RESTO SE DECLARA
-------------------------------------------------
Un sleep dentro de un bucle con deadline es polling por construccion y no hace
falta anotarlo. Cualquier otro lleva

    // WAIT-OK: <razon>

y ante la duda el lint EXIGE la marca: es preferible una anotacion de mas que un
sitio sin clasificar.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

SLEEP = re.compile(r'this_thread::sleep_for')
MARK = re.compile(r'WAIT-OK:\s*\S')
LOOP = re.compile(r'\b(while|for)\s*\(')
DEADLINE = re.compile(r'deadline|until|steady_clock::now|elapsed')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP = os.path.join(ROOT, 'audio', 'src', 'main', 'cpp')
# El helper ES la implementacion de las esperas; no se audita a si mismo.
EXEMPT = {os.path.join('tests', 'support', 'TestWait.h')}


def test_files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ('build', 'build-san', '.deps')]
        if not re.search(r'[/\\]tests?([/\\]|$)', dirpath):
            continue
        for name in filenames:
            if name.endswith(('.cpp', '.h')):
                path = os.path.join(dirpath, name)
                if any(path.endswith(e) for e in EXEMPT):
                    continue
                yield path


def is_polling(lines, idx):
    """Un sleep dentro de un bucle gobernado por un deadline es polling."""
    back = lines[max(0, idx - 12):idx]
    return any(LOOP.search(l) for l in back) and any(DEADLINE.search(l) for l in back)


def marked(lines, idx):
    return any(MARK.search(l) for l in lines[max(0, idx - 6):idx + 1])


def scan(root):
    findings = []
    for path in sorted(test_files(root)):
        with open(path, encoding='utf-8', errors='replace') as fh:
            lines = fh.read().splitlines()
        for i, line in enumerate(lines):
            if not SLEEP.search(line) or line.lstrip().startswith('*'):
                continue
            if is_polling(lines, i) or marked(lines, i):
                continue
            findings.append((os.path.relpath(path, ROOT), i + 1, line.strip()))
    return findings


def self_test():
    """El lint tiene que poder FALLAR. Un parser roto queda en verde para siempre."""
    with tempfile.TemporaryDirectory() as tmp:
        d = os.path.join(tmp, 'tests')
        os.makedirs(d)
        # Positivo: una espera cruda sin clasificar TIENE que salir.
        with open(os.path.join(d, 'bad.cpp'), 'w') as fh:
            fh.write('void f() {\n    std::this_thread::sleep_for(ms(120));\n}\n')
        if not scan(tmp):
            print('  ROTO: no detecto una espera cruda sin clasificar.', file=sys.stderr)
            return 3
        print('  detecta una espera cruda sin clasificar                    OK')

        # Negativo 1: con la marca, no se reporta.
        with open(os.path.join(d, 'bad.cpp'), 'w') as fh:
            fh.write('void f() {\n    // WAIT-OK: estimulo\n'
                     '    std::this_thread::sleep_for(ms(120));\n}\n')
        if scan(tmp):
            print('  ROTO: reporto una espera YA marcada.', file=sys.stderr)
            return 3
        print('  respeta la marca WAIT-OK                                   OK')

        # Negativo 2: el polling se reconoce solo.
        with open(os.path.join(d, 'bad.cpp'), 'w') as fh:
            fh.write('void f() {\n    auto deadline = now() + ms(10);\n'
                     '    while (steady_clock::now() < deadline) {\n'
                     '        std::this_thread::sleep_for(ms(1));\n    }\n}\n')
        if scan(tmp):
            print('  ROTO: reporto polling dentro de un bucle con deadline.', file=sys.stderr)
            return 3
        print('  reconoce el polling sin que haya que anotarlo              OK')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()

    if args.self_test:
        print('== self-test del lint de esperas ==')
        rc = self_test()
        print('self-test OK.' if rc == 0 else 'self-test ROTO.')
        return rc

    findings = scan(CPP)
    if not findings:
        return 0

    print('check-test-waits: esperas crudas sin clasificar\n', file=sys.stderr)
    for path, line, text in findings:
        print('  {}:{}\n      {}'.format(path, line, text[:90]), file=sys.stderr)
    print("""
Cada `sleep_for` en un test es una de cuatro cosas, y hay que decir cual:

  polling    dentro de un bucle con deadline. El lint lo reconoce SOLO.
  estimulo   la duracion ES el experimento (jitter, intervalo entre callbacks,
             forzar un orden). Se deja, con `// WAIT-OK: <razon>`.
  presencia  se espera a que algo ocurra y despues se afirma. NO va aca:
             va `wma_test::waitUntil(pred, techo)`.
  ausencia   se espera a que algo NO ocurra. Va `wma_test::sleepFixed`, para que
             `check-time-dependence.sh` la vea en vez de que quede escondida.

Si es presencia, agrandar el sleep no lo arregla: alcanza en tu maquina y se
queda corto en el runner. Eso fue REQ-002.""", file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
