#!/usr/bin/env python3
"""Lee los moduladores de un SoundFont y reporta su reparto — REQ-039.

Un numero que exige CORRER algo no se afirma en CLAUDE.md ni en una spec: lo imprime
el comando que lo produce (R-MOT-33). Este es ese comando para los moduladores.

    python3 scripts/read-sf2-modulators.py <font.sf2|sf3>

Lee el chunk `pdta` directamente del archivo: no depende del motor, asi que puede
contradecirlo. Descuenta la entrada TERMINADORA que la spec SF2 exige al final de
`pmod` y de `imod` (por eso 2814 entradas en el archivo son 2812 moduladores).

🔴 EL AMBITO ES PARTE DEL REPARTO, Y NO ES DECORATIVO (REQ-039 S1 1.6). Un modulador
en la zona GLOBAL de un instrumento no pertenece a ninguna region: aplica a todas las
zonas de ese instrumento. Medido sobre GeneralUser_GS.sf3, el 40% de los moduladores
vive ahi, y sobre el SoundFont-Spec-Test el 84% — o sea que un modelo de
"modulador -> region" deja afuera justo a los que programan el defecto que S1 midio.
"""
import collections
import struct
import sys

GEN = {0: 'startAddrsOffset', 5: 'modEnvToPitch', 6: 'modLfoToPitch', 7: 'vibLfoToPitch',
       8: 'initialFilterFc', 9: 'initialFilterQ', 10: 'modLfoToFilterFc', 11: 'modLfoToVolume',
       15: 'chorusEffectsSend', 16: 'reverbEffectsSend', 17: 'pan', 21: 'delayModLFO',
       22: 'freqModLFO', 23: 'delayVibLFO', 24: 'freqVibLFO', 26: 'holdVolEnv',
       28: 'decayVolEnv', 33: 'holdModEnv', 34: 'decayModEnv', 36: 'sustainVolEnv',
       37: 'releaseVolEnv', 38: 'keynumToVolEnvHold', 48: 'initialAttenuation',
       51: 'coarseTune', 52: 'fineTune'}
CTRL = {0: 'NoController', 2: 'NoteOnVelocity', 3: 'NoteOnKeyNumber', 10: 'PolyPressure',
        13: 'ChannelPressure', 14: 'PitchWheel', 16: 'PitchWheelSens'}
CURVE = {0: 'lineal', 1: 'concava', 2: 'convexa', 3: 'switch'}

GEN_INSTRUMENT = 41
GEN_SAMPLE_ID = 53


def _chunks(buf, off, end):
    while off + 8 <= end:
        cid = buf[off:off + 4]
        size = struct.unpack_from('<I', buf, off + 4)[0]
        body = off + 8
        yield cid, body, body + size
        off = body + size + (size & 1)


def _read_font(path):
    data = open(path, 'rb').read()
    if data[:4] != b'RIFF' or data[8:12] != b'sfbk':
        sys.exit('no es un SoundFont RIFF/sfbk: %s' % path)
    out = {}
    for cid, b, e in _chunks(data, 12, struct.unpack_from('<I', data, 4)[0] + 8):
        if cid == b'LIST':
            tag = data[b:b + 4].decode()
            for c2, b2, e2 in _chunks(data, b + 4, e):
                out[(tag, c2.decode())] = (b2, e2)
    return data, out


def _src(oper):
    idx, cc = oper & 0x7F, (oper >> 7) & 1
    decreasing, bipolar, curve = (oper >> 8) & 1, (oper >> 9) & 1, (oper >> 10) & 0x3F
    name = ('CC%d' % idx) if cc else CTRL.get(idx, 'ctrl%d' % idx)
    return (name, CURVE.get(curve, 'curva%d' % curve),
            'bipolar' if bipolar else 'unipolar',
            'decreciente' if decreasing else 'creciente')


def _scope_ranges(data, ch):
    """Devuelve (pmod_global, imod_global): sets de indices en zona GLOBAL.

    La zona global es la PRIMERA bag de un preset/instrumento que no declara
    GenInstrument / GenSampleID. Es la misma regla que aplica `tsf_load_presets`.
    """
    def table(name, fmt, size):
        if ('pdta', name) not in ch:
            return []
        b, e = ch[('pdta', name)]
        return [struct.unpack_from(fmt, data, b + i * size) for i in range((e - b) // size)]

    phdrs = table('phdr', '<20sHHHIII', 38)
    pbags = table('pbag', '<HH', 4)
    pgens = table('pgen', '<HH', 4)
    insts = table('inst', '<20sH', 22)
    ibags = table('ibag', '<HH', 4)
    igens = table('igen', '<HH', 4)

    pmod_global, imod_global = set(), set()
    for pi in range(len(phdrs) - 1):
        first = phdrs[pi][3]
        for bi in range(first, phdrs[pi + 1][3]):
            if bi + 1 >= len(pbags):
                break
            has_inst = any(pgens[gi][0] == GEN_INSTRUMENT
                           for gi in range(pbags[bi][0], min(pbags[bi + 1][0], len(pgens))))
            if bi == first and not has_inst:
                pmod_global.update(range(pbags[bi][1], pbags[bi + 1][1]))
    for ii in range(len(insts) - 1):
        first = insts[ii][1]
        for bi in range(first, insts[ii + 1][1]):
            if bi + 1 >= len(ibags):
                break
            has_sample = any(igens[gi][0] == GEN_SAMPLE_ID
                             for gi in range(ibags[bi][0], min(ibags[bi + 1][0], len(igens))))
            if bi == first and not has_sample:
                imod_global.update(range(ibags[bi][1], ibags[bi + 1][1]))
    return pmod_global, imod_global


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    data, ch = _read_font(path)
    pmod_global, imod_global = _scope_ranges(data, ch)

    by_pair = collections.Counter()
    curves = collections.Counter()
    amt_src = collections.Counter()
    by_dest = collections.Counter()
    by_scope = collections.Counter()
    linked = 0
    total = 0

    for chunk, globals_ in (('pmod', pmod_global), ('imod', imod_global)):
        if ('pdta', chunk) not in ch:
            continue
        b, e = ch[('pdta', chunk)]
        n = (e - b) // 10
        for i in range(n - 1):  # la ultima entrada es la TERMINADORA
            oper, dest, amount, amtsrc, _trans = struct.unpack_from('<HHhHH', data, b + i * 10)
            name, curve, polarity, direction = _src(oper)
            is_link = bool(dest & 0x8000)
            dname = 'LINK' if is_link else GEN.get(dest, 'gen%d' % dest)
            linked += is_link
            total += 1
            by_pair[(name, dname)] += 1
            by_dest[dname] += 1
            curves[(curve, polarity, direction)] += 1
            by_scope[('GLOBAL' if i in globals_ else 'zona', chunk)] += 1
            if amtsrc != 0:
                amt_src[(_src(amtsrc)[0], dname)] += 1

    glob = sum(v for k, v in by_scope.items() if k[0] == 'GLOBAL')
    print('font                        :', path)
    print('TOTAL moduladores           :', total, '(sin las 2 entradas terminadoras)')
    print('destinos distintos          :', len(by_dest))
    print('con modAmtSrcOper           :', sum(amt_src.values()))
    print('con bit de link en destino  :', linked)
    print()
    print('=== AMBITO (REQ-039 S1 1.6) ===')
    for scope in ('GLOBAL', 'zona'):
        for chunk in ('pmod', 'imod'):
            if by_scope[(scope, chunk)]:
                print('%-8s %-6s : %d' % (scope, chunk, by_scope[(scope, chunk)]))
    print('en zona GLOBAL              : %d de %d (%.1f%%) — NO pertenecen a una region'
          % (glob, total, 100.0 * glob / total if total else 0.0))
    print()
    print('=== por destino ===')
    for k, v in by_dest.most_common():
        print('%6d  %s' % (v, k))
    print()
    print('=== por (fuente, destino) — top 20 ===')
    for k, v in by_pair.most_common(20):
        print('%6d  %18s -> %s' % (v, k[0], k[1]))
    print()
    print('=== curvas (curva, polaridad, direccion) ===')
    for k, v in curves.most_common():
        print('%6d  %s' % (v, k))
    print()
    print('=== modAmtSrcOper != NoController ===')
    for k, v in amt_src.most_common():
        print('%6d  amt-src %18s sobre %s' % (v, k[0], k[1]))


if __name__ == '__main__':
    main()
