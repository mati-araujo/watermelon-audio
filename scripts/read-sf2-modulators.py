#!/usr/bin/env python3
"""Lee los moduladores de un SoundFont y reporta su reparto — REQ-039.

Un numero que exige CORRER algo no se afirma en CLAUDE.md ni en una spec: lo imprime
el comando que lo produce (R-MOT-33). Este es ese comando para los moduladores.

    python3 scripts/read-sf2-modulators.py <font.sf2|sf3>
    python3 scripts/read-sf2-modulators.py <font.sf2|sf3> --presets   # la tabla POR PRESET
                                                                     # de la nota de bump (S3 3.6)
    python3 scripts/read-sf2-modulators.py <font> --generators        # que GENERADORES usa el font
                                                                     # (I-2 de MINI-024: la ceguera
                                                                     # del spec-test se mide asi)
    python3 scripts/read-sf2-modulators.py <font> --attenuation       # initialAttenuation POR PRESET
                                                                     # y cuanto baja con el factor 0,4

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

# La tabla de generadores de SF2 2.04 §8.1.2, COMPLETA y por su numero.
#
# 🔴 Y despues de "COMPLETA" seguia faltando el 45 (`startloopAddrsCoarseOffset`, 523 usos
# en GeneralUser): lo encontro `--generators` el 2026-09-12 (MINI-024) al imprimir '?' en
# una fila. Una tabla escrita a mano se verifica CONTRA EL ARCHIVO, no releyendola.
#
# 🔴 Hasta REQ-039 S3 (2026-09-11) este mapa estaba CORRIDO en tres tramos (5-7,
# 11, 26-38): decia `modLfoToVolume` donde el 11 es `modEnvToFilterFc`, `decayModEnv`
# donde el 34 es `attackVolEnv`, `holdVolEnv` donde el 26 es `attackModEnv`... Los
# CONTEOS que produjo eran correctos (cuenta por numero); los NOMBRES no, y con esos
# nombres se escribieron la tabla "por destino" del spec del REQ y la de los 30 de
# S3. Se detecto porque el test de C++ (que usa los numeros del spec) no encontraba
# las filas que este script nombraba. La leccion: una tabla de nombres escrita a
# mano se verifica contra el spec ANTES de que un numero salga de ella.
GEN = {0: 'startAddrsOffset', 1: 'endAddrsOffset', 2: 'startloopAddrsOffset',
       3: 'endloopAddrsOffset', 4: 'startAddrsCoarseOffset', 5: 'modLfoToPitch',
       6: 'vibLfoToPitch', 7: 'modEnvToPitch', 8: 'initialFilterFc', 9: 'initialFilterQ',
       10: 'modLfoToFilterFc', 11: 'modEnvToFilterFc', 12: 'endAddrsCoarseOffset',
       13: 'modLfoToVolume', 15: 'chorusEffectsSend', 16: 'reverbEffectsSend', 17: 'pan',
       21: 'delayModLFO', 22: 'freqModLFO', 23: 'delayVibLFO', 24: 'freqVibLFO',
       25: 'delayModEnv', 26: 'attackModEnv', 27: 'holdModEnv', 28: 'decayModEnv',
       29: 'sustainModEnv', 30: 'releaseModEnv', 31: 'keynumToModEnvHold',
       32: 'keynumToModEnvDecay', 33: 'delayVolEnv', 34: 'attackVolEnv', 35: 'holdVolEnv',
       36: 'decayVolEnv', 37: 'sustainVolEnv', 38: 'releaseVolEnv', 39: 'keynumToVolEnvHold',
       40: 'keynumToVolEnvDecay', 41: 'instrument', 43: 'keyRange', 44: 'velRange',
       45: 'startloopAddrsCoarseOffset', 46: 'keynum', 47: 'velocity', 48: 'initialAttenuation', 50: 'endloopAddrsCoarseOffset',
       51: 'coarseTune', 52: 'fineTune', 53: 'sampleID', 54: 'sampleModes',
       56: 'scaleTuning', 57: 'exclusiveClass', 58: 'overridingRootKey'}
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


def _u16(data, off):
    return struct.unpack_from('<H', data, off)[0]


def _s16(data, off):
    return struct.unpack_from('<h', data, off)[0]


def presets(path):
    """La tabla POR PRESET que sostiene la nota de bump de REQ-039 (S3 3.6, 2026-09-11):
    que hace cada preset con velocity -> initialAttenuation (el default #1, identidad
    `0x0502 -> 48` sin amtSrc ni transform) y cuantos velocity -> initialFilterFc declara.

    🔴 HASTA EL 2026-09-14 ESTA TABLA SOLO RECORRIA `imod`, Y CLASIFICO MAL 26 PRESETS.
    Decia "ANULADO" donde el instrumento pone el default #1 en 0 — y no miraba que el
    PRESET lo vuelve a declarar en su zona global (`pmod`, 500/800/840 cB), que por SF2
    §9.5 se SUMA al del instrumento. Los 26 "velocity -> nivel anulado" de la nota de
    v2.17.0 (pianos, organos, `0:81 Saw Lead`...) tienen TODOS una curva efectiva. Lo
    encontro NoisyPad midiendo `Saw Lead` en el dispositivo (-6,9 / -16,9 dB a velocity
    76 / 38: la curva de 800 cB, no "anulado"), y el motor ya lo hacia bien — el que
    estaba mal era este instrumento. Un lector que recorre un solo ambito describe otro
    archivo.

    Ahora resuelve los CUATRO ambitos, con la precedencia del spec y la misma que aplica
    `SoundFontModulatorTable::resolve()`: en el instrumento la zona local pisa a la global
    por identidad y el default #1 vale 960 si nadie lo pisa; en el preset la zona local
    pisa a la global; y el resultado del preset se SUMA al del instrumento. Imprime por
    preset la curva EFECTIVA (o el conjunto, si sus regiones difieren) en cB.
    """
    data, ch = _read_font(path)

    def recs(name, size):
        b, e = ch[('pdta', name)]
        return [b + i * size for i in range((e - b) // size)]

    phdr, pbag, pgen, pmod = recs('phdr', 38), recs('pbag', 4), recs('pgen', 4), recs('pmod', 10)
    inst, ibag, igen, imod = recs('inst', 22), recs('ibag', 4), recs('igen', 4), recs('imod', 10)
    DEF1 = (0x0502, 48, 0, 0)
    DEFAULT_1_CB = 960

    def zone_mods(bags, tab, z):
        out = {}
        for m in range(_u16(data, bags[z] + 2), _u16(data, bags[z + 1] + 2)):
            src, dest, amt, asrc, tr = struct.unpack_from('<HHhHH', data, tab[m])
            out[(src, dest, asrc, tr)] = amt   # la ultima identidad igual gana, como en el spec
        return out

    def zone_gens(bags, tab, z):
        return dict((_u16(data, tab[g]), _s16(data, tab[g] + 2))
                    for g in range(_u16(data, bags[z]), _u16(data, bags[z + 1])))

    def vel_fc(mods):
        return sum(1 for (src, dest, _a, _t), amt in mods.items()
                   if dest == 8 and (src & 0x7F) == 2 and not (src & 0x80) and amt != 0)

    # El default #2 (velocity -> initialFilterFc, -2400) tiene DOS identidades segun la
    # version del spec: 2.04 = `0x0102 -> 8` sin amtSrc (la del motor, R-MOT-38); 2.01 =
    # la misma con amtSrc `0x0D02` (velocity, switch). Un archivo que lo borra con la
    # identidad 2.01 NO lo anula en un synth 2.04 — el default sigue vivo y se SUMA a lo
    # que el archivo declare. GeneralUser lo borra asi en 1422 zonas de instrumento.
    DEF2_204 = (0x0102, 8, 0, 0)
    DEF2_201 = (0x0102, 8, 0x0D02, 0)

    def def2_state(mods):
        if mods.get(DEF2_204, None) == 0:
            return 'borrado 2.04'
        if DEF2_201 in mods:
            return 'borrado 2.01 (sigue vivo en 2.04)'
        return 'default'

    rows = []
    for p in range(len(phdr) - 1):
        name = data[phdr[p]:phdr[p] + 20].split(b'\0')[0].decode('latin1')
        prog, bank = _u16(data, phdr[p] + 20), _u16(data, phdr[p] + 22)
        z0, z1 = _u16(data, phdr[p] + 24), _u16(data, phdr[p + 1] + 24)
        pglobal, effective, fc, def2 = {}, collections.Counter(), 0, set()
        for z in range(z0, z1):
            pg = zone_gens(pbag, pgen, z)
            if GEN_INSTRUMENT not in pg:
                if z == z0:
                    pglobal = zone_mods(pbag, pmod, z)
                continue
            pz = dict(pglobal)
            pz.update(zone_mods(pbag, pmod, z))
            ii = pg[GEN_INSTRUMENT]
            iz0, iz1 = _u16(data, inst[ii] + 20), _u16(data, inst[ii + 1] + 20)
            iglobal = {}
            for iz in range(iz0, iz1):
                ig = zone_gens(ibag, igen, iz)
                if GEN_SAMPLE_ID not in ig:
                    if iz == iz0:
                        iglobal = zone_mods(ibag, imod, iz)
                    continue
                im = dict(iglobal)
                im.update(zone_mods(ibag, imod, iz))
                effective[im.get(DEF1, DEFAULT_1_CB) + pz.get(DEF1, 0)] += 1
                fc += vel_fc(im) + vel_fc(pz)
                def2.add(def2_state(im))
        amounts = sorted(effective)
        if amounts == [0]:
            curve = 'ANULADO'
        elif amounts == [DEFAULT_1_CB]:
            curve = 'default (960 cB)'
        elif len(amounts) == 1:
            curve = '%d cB' % amounts[0]
        else:
            curve = 'mezcla %s' % dict((k, effective[k]) for k in amounts)
        rows.append((bank, prog, name, fc, curve, ' / '.join(sorted(def2))))

    print('%-5s %-3s %-22s %8s  %-34s %s' % ('banco', 'prog', 'preset', 'vel->FC',
                                             'vel -> nivel, curva EFECTIVA', 'default #2 (vel -> filtro)'))
    for bank, prog, name, fc, curve, def2 in rows:
        print('%5d %3d  %-22s %8d  %-34s %s' % (bank, prog, name, fc, curve, def2))
    resumen = collections.Counter(r[4] if not r[4].startswith('mezcla') else 'mezcla' for r in rows)
    print()
    print('presets: %d; con velocity -> FC declarado (amount != 0): %d; curva efectiva de velocity -> nivel: %s'
          % (len(rows), sum(1 for r in rows if r[3] > 0), dict(resumen.most_common())))
    print('default #2 por preset: %s' % dict(collections.Counter(r[5] for r in rows).most_common()))


STRUCTURAL_GENS = {GEN_INSTRUMENT, 43, 44, GEN_SAMPLE_ID}  # instrument, keyRange, velRange, sampleID


def generators(path):
    """Que generadores del SF2 §8.1.2 usa el font, por numero y con cuantos usos (pgen + igen,
    sin los cuatro estructurales). Es el denominador del normalizador de MINI-024 y, corrido
    sobre dos fonts, la CEGUERA del spec-test: lo que GeneralUser usa y sf_spec_test.sf2 no."""
    data, ch = _read_font(path)

    def gens(name):
        b, e = ch[('pdta', name)]
        return [_u16(data, b + i * 4) for i in range((e - b) // 4)]

    used = collections.Counter(g for g in gens('pgen') + gens('igen') if g not in STRUCTURAL_GENS)
    print('generadores distintos usados (sin instrument/keyRange/velRange/sampleID): %d' % len(used))
    for g, n in sorted(used.items(), key=lambda kv: (-kv[1], kv[0])):
        print('  %3d %-26s %6d' % (g, GEN.get(g, '?'), n))
    return used


def attenuation(path, factor_old=0.1, factor_new=0.4):
    """`initialAttenuation` (gen 48) POR PRESET, y cuanto MAS baja cada preset cuando el
    generador entra a `factor_new` dB por dB declarado en vez de `factor_old` — la nota de bump
    de MINI-024 se escribe con esto. La regla del SF2 §8.5: el valor de la zona de instrumento
    (o de su zona global) es el absoluto, y el de la zona de preset (o su global) se SUMA.
    Se imprime por preset el rango [min, max] en cB entre sus zonas con atenuacion, cuantas
    zonas la declaran, y la atenuacion EXTRA en dB en la zona mas atenuada."""
    data, ch = _read_font(path)

    def recs(name, size):
        b, e = ch[('pdta', name)]
        return [b + i * size for i in range((e - b) // size)]

    phdr, pbag, pgen = recs('phdr', 38), recs('pbag', 4), recs('pgen', 4)
    inst, ibag, igen = recs('inst', 22), recs('ibag', 4), recs('igen', 4)

    def zone_gens(bags, gentab, z):
        return dict((_u16(data, gentab[g]), _s16(data, gentab[g] + 2))
                    for g in range(_u16(data, bags[z]), _u16(data, bags[z + 1])))

    rows = []
    for p in range(len(phdr) - 1):
        name = data[phdr[p]:phdr[p] + 20].split(b'\0')[0].decode('latin1')
        prog, bank = _u16(data, phdr[p] + 20), _u16(data, phdr[p] + 22)
        z0, z1 = _u16(data, phdr[p] + 24), _u16(data, phdr[p + 1] + 24)
        pglobal = 0
        values, zones = [], 0
        for z in range(z0, z1):
            pg = zone_gens(pbag, pgen, z)
            if 41 not in pg:
                if z == z0:
                    pglobal = pg.get(48, 0)
                continue
            padd = pglobal + pg.get(48, 0)
            ii = pg[41]
            iz0, iz1 = _u16(data, inst[ii] + 20), _u16(data, inst[ii + 1] + 20)
            iglobal = 0
            for iz in range(iz0, iz1):
                ig = zone_gens(ibag, igen, iz)
                if 53 not in ig:
                    if iz == iz0:
                        iglobal = ig.get(48, 0)
                    continue
                zones += 1
                total = ig.get(48, iglobal) + padd
                if total:
                    values.append(total)
        rows.append((bank, prog, name, zones, values))

    print('%-5s %-3s %-22s %6s %6s %8s %8s  %s' % ('banco', 'prog', 'preset', 'zonas', 'c/att',
                                                    'min cB', 'max cB', 'extra dB (0,%d->0,%d) en la mas atenuada'
                                                    % (factor_old * 10, factor_new * 10)))
    affected = 0
    for bank, prog, name, zones, values in rows:
        if values:
            affected += 1
            extra = (factor_new - factor_old) * max(values) / 10.0
            print('%5d %3d  %-22s %6d %6d %8d %8d  %6.1f' % (bank, prog, name, zones, len(values),
                                                            min(values), max(values), extra))
        else:
            print('%5d %3d  %-22s %6d %6d %8s %8s  %6s' % (bank, prog, name, zones, 0, '-', '-', '0.0'))
    print()
    print('presets: %d; con initialAttenuation declarada en alguna zona: %d' % (len(rows), affected))
    return rows


if __name__ == '__main__':
    mode = sys.argv[2] if len(sys.argv) >= 3 else ''
    if mode == '--presets':
        presets(sys.argv[1])
    elif mode == '--generators':
        generators(sys.argv[1])
    elif mode == '--attenuation':
        attenuation(sys.argv[1])
    else:
        main()
