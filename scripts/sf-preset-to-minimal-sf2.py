#!/usr/bin/env python3
"""
sf-preset-to-minimal-sf2.py — el oráculo FluidSynth para el font que FluidSynth no abre.

FluidSynth 2.6.0 segfaultea con `GeneralUser_GS.sf3` (exit 139). Este script escribe un `.sf2`
MÍNIMO con las zonas EXACTAS (las cuatro capas: global y local de preset e instrumento, generadores
y moduladores tal cual) y los samples REALES (el OGG del `sdta` decodificado con ffmpeg, o el PCM
si el font ya es .sf2) de las zonas de `bank:prog` que cubren una tecla (y una velocity, si se
pasa). El preset queda en bank 0 / prog 0. Es la receta del 2026-09-15 (`zone863.py` + `mksb4.py`,
que refutó la hipótesis de NoisyPad sobre el fix 2 de 2.17.4) generalizada el 16/09, cuando decidió
la pregunta de Warm Pad (I-2 de REQ-041) y aisló MINI-031.

🔴 El control de identidad se corre CADA vez, y es lo que vuelve oráculo a FluidSynth: nuestro motor
(`sf_render_preset`) tiene que rendir el `.sf2` mínimo y el `.sf3` real iguales (el 16/09: 0,01 dB en
el régimen). Si no coinciden, el mínimo no es el preset, y FluidSynth sobre él no dice nada.

🔴 Sin `CC7` en el `.mid`, FluidSynth arranca con el default GM CC7 = 100 y su modulador #3 lo aplica
como −4,15 dB; nuestro arnés arranca en 127. Toda comparación ABSOLUTA lleva `CC7 = 127` explícito
en el `.mid` (costó 4 dB de "hallazgo" el 16/09 antes de verlo).

    python3 scripts/sf-preset-to-minimal-sf2.py GeneralUser_GS.sf3 0 89 60 warmpad60.sf2
    fluidsynth -ni -R 0 -C 0 -g 1 -r 44100 -O float -F fs.wav warmpad60.sf2 nota.mid

Mutaciones (para aislar un mecanismo contra la referencia, como en MINI-031):
    --pgen G=V        pone el generador G en V en las zonas LOCALES del preset que cubren
    --igen-global G=V idem en la global de cada instrumento alcanzado
    --pmod-global-drop SRC   borra de la global del preset los moduladores con esa fuente (hex ok)
    --imod-global-drop SRC   idem en la global del instrumento

Trampas conocidas: `INAM` de tamaño impar hace que FluidSynth rechace el archivo y caiga al loader
DLS; tsf con CC0 y CC32 arma bank (MSB<<7)|LSB, así que el bank select va sólo por el MSB. Python
stdlib + ffmpeg, como el resto de scripts/.
"""
import argparse
import importlib.util
import os
import struct
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("rd", os.path.join(_HERE, "read-sf2-modulators.py"))
rd = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rd)
u16, s16 = rd._u16, rd._s16

GEN_KEYRANGE, GEN_VELRANGE, GEN_INSTRUMENT, GEN_SAMPLEID = 43, 44, 41, 53


def _kv(items):
    out = []
    for it in items or []:
        g, v = it.split("=")
        out.append((int(g), int(v)))
    return out


def _set_gen(gens, g, v):
    return [(gg, v if gg == g else aa) for gg, aa in gens] + ([] if g in dict(gens) else [(g, v)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("font")
    ap.add_argument("bank", type=int)
    ap.add_argument("prog", type=int)
    ap.add_argument("key", type=int)
    ap.add_argument("out")
    ap.add_argument("--vel", type=int, help="quedarse sólo con las capas que cubren esta velocity")
    ap.add_argument("--pgen", action="append", metavar="G=V")
    ap.add_argument("--igen-global", action="append", metavar="G=V")
    ap.add_argument("--pmod-global-drop", action="append", metavar="SRC")
    ap.add_argument("--imod-global-drop", action="append", metavar="SRC")
    a = ap.parse_args()

    data, ch = rd._read_font(a.font)

    def recs(name, size):
        b, e = ch[("pdta", name)]
        return [b + i * size for i in range((e - b) // size)]

    phdr, pbag, pgen, pmod = recs("phdr", 38), recs("pbag", 4), recs("pgen", 4), recs("pmod", 10)
    inst, ibag, igen, imod = recs("inst", 22), recs("ibag", 4), recs("igen", 4), recs("imod", 10)
    shdr = recs("shdr", 46)

    def zgens(bags, tab, z):
        return [(u16(data, tab[g]), s16(data, tab[g] + 2))
                for g in range(u16(data, bags[z]), u16(data, bags[z + 1]))]

    def zmods(bags, tab, z):
        return [struct.unpack_from("<HHhHH", data, tab[m])
                for m in range(u16(data, bags[z] + 2), u16(data, bags[z + 1] + 2))]

    def covers(gens):
        d = dict(gens)
        if GEN_KEYRANGE in d and not (d[GEN_KEYRANGE] & 0xFF) <= a.key <= (d[GEN_KEYRANGE] >> 8):
            return False
        if a.vel is not None and GEN_VELRANGE in d and not (d[GEN_VELRANGE] & 0xFF) <= a.vel <= (d[GEN_VELRANGE] >> 8):
            return False
        return True

    try:
        p = next(p for p in range(len(phdr) - 1)
                 if (u16(data, phdr[p] + 20), u16(data, phdr[p] + 22)) == (a.prog, a.bank))
    except StopIteration:
        sys.exit(f"no hay preset {a.bank}:{a.prog} en {a.font}")
    pname = data[phdr[p]:phdr[p] + 20].split(b"\0")[0].decode("latin1")
    z0, z1 = u16(data, phdr[p] + 24), u16(data, phdr[p + 1] + 24)
    pglob, plocal = ([], []), []          # plocal: (gens, mods, inst_index)
    for z in range(z0, z1):
        pg, pm = zgens(pbag, pgen, z), zmods(pbag, pmod, z)
        if GEN_INSTRUMENT not in dict(pg):
            if z == z0:
                pglob = (pg, pm)
            continue
        if covers(pg):
            plocal.append((pg, pm, dict(pg)[GEN_INSTRUMENT]))
    if not plocal:
        sys.exit(f"ninguna zona de {a.bank}:{a.prog} cubre la tecla {a.key}" + (f" a velocity {a.vel}" if a.vel is not None else ""))
    for g, v in _kv(a.pgen):
        plocal = [(_set_gen(pg, g, v), pm, ii) for pg, pm, ii in plocal]
    for src in a.pmod_global_drop or []:
        s = int(src, 0)
        pglob = (pglob[0], [m for m in pglob[1] if m[0] != s])

    # Instrumentos: sólo las zonas que cubren. Samples: los referenciados, más su par estéreo.
    insts, samples = [], {}

    def sample_new_index(si):
        if si not in samples:
            samples[si] = len(samples)
            link, typ = struct.unpack_from("<HH", data, shdr[si] + 42)
            if (typ & 0x0F) in (2, 4) and link != si:
                sample_new_index(link)
        return samples[si]

    for pg, pm, ii in plocal:
        iname = data[inst[ii]:inst[ii] + 20].split(b"\0")[0].decode("latin1")
        iz0, iz1 = u16(data, inst[ii] + 20), u16(data, inst[ii + 1] + 20)
        iglob, zones = ([], []), []
        for iz in range(iz0, iz1):
            ig, im = zgens(ibag, igen, iz), zmods(ibag, imod, iz)
            if GEN_SAMPLEID not in dict(ig):
                if iz == iz0:
                    iglob = (ig, im)
                continue
            if covers(ig):
                zones.append(([(g, sample_new_index(v) if g == GEN_SAMPLEID else v) for g, v in ig], im))
        if not zones:
            sys.exit(f"el instrumento {iname!r} no tiene zonas para la tecla {a.key}")
        for src in a.imod_global_drop or []:
            s = int(src, 0)
            iglob = (iglob[0], [m for m in iglob[1] if m[0] != s])
        for g, v in _kv(a.igen_global):
            iglob = (_set_gen(iglob[0], g, v), iglob[1])
        insts.append((iname, iglob, zones))

    sb, _ = ch[("sdta", "smpl")]
    pcm_all, shdr_out = b"", b""
    for si in sorted(samples, key=samples.get):
        s = shdr[si]
        sname = data[s:s + 20].split(b"\0")[0].decode("latin1")
        st, en, ls, le, rate, root, corr, link, typ = struct.unpack_from("<IIIIIBbHH", data, s + 20)
        if typ & 0x10:                      # sf3: start/end son OFFSETS EN BYTES del OGG; el loop es del PCM
            blob = data[sb + st:sb + en]
            pcm = subprocess.run(["ffmpeg", "-v", "error", "-i", "pipe:0", "-f", "s16le", "-ac", "1", "pipe:1"],
                                 input=blob, capture_output=True, check=True).stdout
            n, nls, nle = len(pcm) // 2, ls, le
        else:
            pcm = data[sb + st * 2:sb + en * 2]
            n, nls, nle = en - st, ls - st, le - st
        base = len(pcm_all) // 2
        pcm_all += pcm + b"\0\0" * 46       # los 46 ceros de guarda que el spec pide tras cada sample
        nlink = samples.get(link, 0) if (typ & 0x0F) in (2, 4) else 0
        shdr_out += (sname.encode("latin1").ljust(20, b"\0")[:20]
                     + struct.pack("<IIIII", base, base + n, base + nls, base + nle, rate)
                     + struct.pack("<Bb", root, corr) + struct.pack("<HH", nlink, typ & 0x0F))
        print(f"sample {si} -> {samples[si]} {sname!r}: {n} muestras, {rate} Hz, root {root}, loop {nls}..{nle}, type {typ & 0x0F}")
    shdr_out += b"EOS".ljust(20, b"\0") + b"\0" * 26

    def chunk(cid, body):
        b = cid.encode() + struct.pack("<I", len(body)) + body
        return b + (b"\0" if len(body) & 1 else b"")

    def name20(s):
        return s.encode("latin1").ljust(20, b"\0")[:20]

    def gens(lst):
        return b"".join(struct.pack("<Hh", g, v) for g, v in lst)

    def mods(lst):
        return b"".join(struct.pack("<HHhHH", *m) for m in lst)

    pzones = ([pglob] if (pglob[0] or pglob[1]) else []) + \
             [([(g, k if g == GEN_INSTRUMENT else v) for g, v in pg], pm) for k, (pg, pm, _) in enumerate(plocal)]
    phdr_o = name20(pname) + struct.pack("<HHH", 0, 0, 0) + struct.pack("<III", 0, 0, 0)
    phdr_o += name20("EOP") + struct.pack("<HHH", 0, 0, len(pzones)) + struct.pack("<III", 0, 0, 0)
    pbag_o = pgen_o = pmod_o = b""
    gi = mi = 0
    for g, m in pzones:
        pbag_o += struct.pack("<HH", gi, mi)
        pgen_o += gens(g)
        pmod_o += mods(m)
        gi += len(g)
        mi += len(m)
    pbag_o += struct.pack("<HH", gi, mi)
    pgen_o += struct.pack("<HH", 0, 0)
    pmod_o += b"\0" * 10

    inst_o = ibag_o = igen_o = imod_o = b""
    gi = mi = bi = 0
    for iname, iglob, zones in insts:
        inst_o += name20(iname) + struct.pack("<H", bi)
        for g, m in ([iglob] if (iglob[0] or iglob[1]) else []) + zones:
            ibag_o += struct.pack("<HH", gi, mi)
            igen_o += gens(g)
            imod_o += mods(m)
            gi += len(g)
            mi += len(m)
            bi += 1
    inst_o += name20("EOI") + struct.pack("<H", bi)
    ibag_o += struct.pack("<HH", gi, mi)
    igen_o += struct.pack("<HH", 0, 0)
    imod_o += b"\0" * 10

    pdta = b"pdta" + b"".join(chunk(c, b) for c, b in [
        ("phdr", phdr_o), ("pbag", pbag_o), ("pmod", pmod_o), ("pgen", pgen_o),
        ("inst", inst_o), ("ibag", ibag_o), ("imod", imod_o), ("igen", igen_o), ("shdr", shdr_out)])
    sdta = b"sdta" + chunk("smpl", pcm_all)
    inam = (pname + " minimo").encode("latin1") + b"\0"
    if len(inam) & 1:
        inam += b"\0"
    info = b"INFO" + chunk("ifil", struct.pack("<HH", 2, 1)) + chunk("isng", b"E-mu 10K2\0") + chunk("INAM", inam)
    with open(a.out, "wb") as f:
        f.write(chunk("RIFF", b"sfbk" + chunk("LIST", info) + chunk("LIST", sdta) + chunk("LIST", pdta)))
    print(f"ok {a.out}: {a.bank}:{a.prog} {pname!r} -> 0:0 · {len(plocal)} zona(s) de preset · "
          f"{sum(len(z) for _, _, z in insts)} zona(s) de instrumento · {len(samples)} sample(s)")


if __name__ == "__main__":
    main()
