#!/usr/bin/env python3
"""
check-ituner-implementations — el criterio de muerte de MINI-004, hecho codigo.

QUE VIGILA
----------
Que `TunerContractTest` ejerza TODA implementacion de `ITuner` del modulo. Un test
parametrico protege exactamente a las implementaciones que alguien se acordo de registrar,
y el olvido NO SE VE: la suite sigue verde y el archivo se sigue llamando "contrato".

Compara dos conjuntos, EN LAS DOS DIRECCIONES:

  fuente     las clases/objetos que declaran `: ITuner` bajo audio/src
  registro   la lista `IMPLEMENTACIONES_EJERCIDAS` de TunerSubjects.kt

POR QUE UN SCRIPT Y NO UN TEST
------------------------------
Kotlin/Native NO TIENE REFLECTION, asi que ningun test en runtime puede enumerar las
implementaciones del modulo. Se invierte la pregunta: en vez de descubrirlas ejecutando, se
las descubre LEYENDO EL FUENTE. Es el mismo movimiento que el inventario de bindings del
grafo en el otro repo, y por el mismo motivo.

La mitad de runtime la cubre `elContratoEjerceExactamenteLasImplementacionesDeclaradas`,
que ata la lista a los sujetos que de verdad se construyen. Las dos mitades hacen falta: sin
el test, la lista podria nombrar tres y el contrato correr dos; sin el script, el tercer
implementador entra al arbol y nadie lo suma a la lista.

LA TRAMPA QUE ESTE SCRIPT TIENE QUE ESQUIVAR
--------------------------------------------
`ITunerBridge` CONTIENE la cadena `ITuner`. Un patron ingenuo lo matchea y reporta como
implementaciones del afinador a los dos puentes de plataforma, que no lo son. El self-test
lo prueba explicitamente: si alguien "simplifica" el regex, ese control se pone rojo.
"""

import argparse
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
KOTLIN_ROOT = os.path.join(REPO, 'audio', 'src')
REGISTRY = os.path.join(
    REPO, 'audio', 'src', 'commonTest', 'kotlin', 'com', 'watermellonstudios',
    'audio', 'api', 'TunerSubjects.kt',
)

# Una declaracion. Se recuerda la ULTIMA vista para poder nombrar al implementador cuando la
# lista de supertipos aparece varias lineas mas abajo:
#
#     internal class TunerImpl(
#         private val bridge: ITunerBridge,
#     ) : ITuner {
DECL = re.compile(r'\b(?:class|object)\s+([A-Za-z_][A-Za-z0-9_]*)')

# `ITuner` detras de `:` o `,`, y seguido de algo que NO continua el identificador. Ese
# `(?![A-Za-z0-9_])` es lo unico que separa `ITuner` de `ITunerBridge`, y quitarlo hace que
# los dos puentes de plataforma se reporten como afinadores.
#
# 🔴 NO ALCANZA POR SI SOLO, y esto se descubrio corriendo el guard contra el arbol real:
# un PARAMETRO de constructor tipado `ITuner` —`class TunerSubject(val tuner: ITuner, ...)`—
# matchea igual, y el guard reportaba como implementacion a una clase que solo lo recibe.
# La posicion de supertipo se distingue por la PROFUNDIDAD DE PARENTESIS: un supertipo esta
# a profundidad 0 (`) : ITuner {`), un parametro esta adentro de los parentesis del
# constructor. Por eso el escaneo lleva contador y no es linea por linea.
SUPERTYPE = re.compile(r'[:,]\s*ITuner(?![A-Za-z0-9_])')

LINE_COMMENT = re.compile(r'//.*$')


def _strip_block_comments(text):
    """Los KDoc nombran `[ITuner]` todo el tiempo; sin sacarlos, todo archivo matchea."""
    return re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)


def scan_implementations(root):
    """Nombres de las clases/objetos que declaran `: ITuner` bajo `root`."""
    found = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            if not name.endswith('.kt'):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding='utf-8') as fh:
                text = _strip_block_comments(fh.read())
            last_decl = None
            depth = 0
            for lineno, raw in enumerate(text.splitlines(), start=1):
                line = LINE_COMMENT.sub('', raw)
                decl = DECL.search(line)
                if decl:
                    last_decl = decl.group(1)
                # Se recorre la linea llevando la profundidad, porque `) : ITuner {` cierra y
                # declara en el mismo renglon: mirar la profundidad al final de la linea
                # perderia ese caso, que es justo la forma que usan TunerImpl y FakeTuner.
                for ch in line:
                    if ch == '(':
                        depth += 1
                    elif ch == ')':
                        depth = max(0, depth - 1)
                if depth == 0 and SUPERTYPE.search(line) and last_decl:
                    found[last_decl] = (os.path.relpath(path, root), lineno)
    return found


def read_registry(path):
    """La lista `IMPLEMENTACIONES_EJERCIDAS` de TunerSubjects.kt, en orden."""
    with open(path, encoding='utf-8') as fh:
        text = _strip_block_comments(fh.read())
    m = re.search(
        r'IMPLEMENTACIONES_EJERCIDAS\s*(?::[^=]+)?=\s*listOf\(([^)]*)\)', text, re.DOTALL)
    if not m:
        return None
    return re.findall(r'"([^"]+)"', m.group(1))


def self_test():
    """El guard tiene que poder FALLAR, y tiene que poder NO fallar de mas."""
    rc = 0
    with tempfile.TemporaryDirectory() as tmp:
        # Positivo 1: la forma de una sola linea.
        with open(os.path.join(tmp, 'a.kt'), 'w', encoding='utf-8') as fh:
            fh.write('class AfinadorDeUnaLinea(c: C) : ITuner {\n}\n')
        # Positivo 2: la forma multilinea, que es la que usan TunerImpl y FakeTuner.
        with open(os.path.join(tmp, 'b.kt'), 'w', encoding='utf-8') as fh:
            fh.write('internal class AfinadorMultilinea(\n'
                     '    private val bridge: ITunerBridge,\n'
                     ') : ITuner {\n}\n')
        # Negativo 1: ITunerBridge NO es ITuner. Es la trampa del prefijo.
        with open(os.path.join(tmp, 'c.kt'), 'w', encoding='utf-8') as fh:
            fh.write('class PuenteDePlataforma : ITunerBridge {\n}\n')
        # Negativo 2: un KDoc que NOMBRA la interfaz no la implementa.
        with open(os.path.join(tmp, 'd.kt'), 'w', encoding='utf-8') as fh:
            fh.write('/**\n * Ver [ITuner]: esto NO es un afinador.\n */\n'
                     'class SoloLaMenciona {\n}\n')
        # 🔴 Negativo 3: RECIBIR un ITuner no es SER un ITuner.
        #
        # Este control se agrego porque el guard fallo por acá contra el arbol real —
        # reportaba `TunerSubject`, que solo lo toma por constructor— y el self-test estaba
        # en verde. O sea: el control positivo tenia un hueco del tamano exacto del bug.
        # Queda escrito para que la proxima "simplificacion" del escaneo lo choque.
        with open(os.path.join(tmp, 'e.kt'), 'w', encoding='utf-8') as fh:
            fh.write('class SoloLoRecibe(\n'
                     '    val tuner: ITuner,\n'
                     '    val otro: Int,\n'
                     ') {\n}\n')

        found = scan_implementations(tmp)

        for esperado in ('AfinadorDeUnaLinea', 'AfinadorMultilinea'):
            if esperado in found:
                print('  detecta {:<40} OK'.format(esperado))
            else:
                print('  ROTO: no detecto {}'.format(esperado), file=sys.stderr)
                rc = 3
        for prohibido, motivo in (
            ('PuenteDePlataforma', 'ITunerBridge contiene la cadena ITuner'),
            ('SoloLaMenciona', 'un KDoc que la nombra no la implementa'),
            ('SoloLoRecibe', 'un parametro tipado ITuner no es un supertipo'),
        ):
            if prohibido in found:
                print('  ROTO: reporto {} ({})'.format(prohibido, motivo), file=sys.stderr)
                rc = 3
            else:
                print('  NO confunde {:<38} OK'.format(prohibido))

        # Y el parser del registro tiene que leer lo que escribe el Kotlin de verdad.
        with open(os.path.join(tmp, 'TunerSubjects.kt'), 'w', encoding='utf-8') as fh:
            fh.write('internal val IMPLEMENTACIONES_EJERCIDAS = listOf("Uno", "Dos")\n')
        leido = read_registry(os.path.join(tmp, 'TunerSubjects.kt'))
        if leido == ['Uno', 'Dos']:
            print('  lee el registro                                 OK')
        else:
            print('  ROTO: lei el registro como {}'.format(leido), file=sys.stderr)
            rc = 3
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()

    if args.self_test:
        print('== self-test del guard de implementaciones de ITuner ==')
        rc = self_test()
        print('self-test OK.' if rc == 0 else 'self-test ROTO.')
        return rc

    found = scan_implementations(KOTLIN_ROOT)
    registry = read_registry(REGISTRY)

    if registry is None:
        print('check-ituner-implementations: no encontre IMPLEMENTACIONES_EJERCIDAS en\n'
              '  {}\n'
              'Si el registro se movio o cambio de forma, este guard quedo ciego — que es\n'
              'peor que no tenerlo, porque sigue saliendo verde.'.format(REGISTRY),
              file=sys.stderr)
        return 1

    en_fuente = set(found)
    registradas = set(registry)

    sin_registrar = sorted(en_fuente - registradas)
    sin_existir = sorted(registradas - en_fuente)

    if not sin_registrar and not sin_existir:
        print('check-ituner-implementations: {} implementacion(es), todas ejercidas por el '
              'contrato: {}'.format(len(en_fuente), ', '.join(sorted(en_fuente))))
        return 0

    print('check-ituner-implementations: el contrato de ITuner y el arbol se separaron\n',
          file=sys.stderr)
    for name in sin_registrar:
        path, lineno = found[name]
        print('  NO EJERCIDA  {}\n      audio/src/{}:{}'.format(name, path, lineno),
              file=sys.stderr)
    for name in sin_existir:
        print('  REGISTRADA PERO NO EXISTE  {}'.format(name), file=sys.stderr)
    print("""
Una implementacion de ITuner que el contrato no ejerce NO ESTA CUBIERTA, y el olvido no se
ve: la suite sigue verde con el mismo nombre de "contrato".

  falta ejercer una   agregala a IMPLEMENTACIONES_EJERCIDAS y dale su sujeto en
                      TunerSubjects.kt. Si no se puede construir sin hardware, ESO es el
                      hallazgo: decilo por escrito, no la saques de la lista en silencio.

  sobra una           si el registro nombra algo que ya no existe, el contrato esta
                      ejerciendo menos de lo que declara desde el commit que la borro.
""", file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
