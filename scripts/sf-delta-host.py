#!/usr/bin/env python3
"""
sf-delta-host.py — REQ-040, criterio de muerte, instrumento I-2: la cola del wet de un preset en host.

Rinde UN preset/tecla/velocity del font con la ambiencia en 1/1 y en 0/0 (el mismo arnés que
la conformidad, `sf_render_preset`) y mide, en los dos, el nivel a +T s del note-off RELATIVO
al régimen de la nota ("cola − régimen"). El umbral del criterio compara la de 1/1 con la que
NoisyPad mide en device con `tanda-fx.sh`:

    |(cola−régimen)_device − (cola−régimen)_host| > 3 dB, ambiencia 1/1, a +0,3 s del note-off
        ⇒ el wet no es el del font (muerto)

y el 0/0 es el CONTROL DE OPORTUNIDAD: el preset cuenta en N sólo si su (cola−régimen)_0/0
queda ≥ 10 dB por debajo de la de 1/1. Si no, la release del dry tapa el wet a +T s y ese
preset no puede fallar aunque el wet estuviera mal — es "sin oportunidad", no verde.

🔴 Por qué NO es Δ = nivel(1/1) − nivel(0/0), que fue la primera formulación (2026-09-16):
con una release corta (Trumpet) el 0/0 ya es cero digital a +0,3 s en host y el piso de ruido
de la medición en device — el Δ resta dos pisos distintos y da cualquier cosa (+186 dB en
host). Y con una release larga (String Ensemble) el dry tapa el wet y el Δ da 0,00 genuino
sin decir nada del wet. Las dos las mostró este mismo script el día que se versionó.

    python3 scripts/sf-delta-host.py --preset 81 --key 60 --vel 100          # Saw Lead
    python3 scripts/sf-delta-host.py --preset 56 --key 67 --vel 100          # Trumpet
    python3 scripts/sf-delta-host.py --font otro.sf2 --preset 0 --key 60 --vel 44 --at 0.3

Régimen = RMS 0,1–0,4 s tras el note-on; cola = RMS en una ventana de `--win` s centrada en
note-off + T. "cola − régimen" es lo que NoisyPad tabula (la que separó 28 dB entre seco y
cola a +0,3 s).

Límites, declarados: bank 0 solamente (el arnés hace program change sin bank select: las
baterías de bank 128 y los presets de bank > 0 no se pueden rendir acá); la expresión por toque
en 1,0; una sola nota. Python stdlib: sin numpy, como el resto de scripts/.
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(REPO, "audio", "src", "main", "cpp", "tests")
DEFAULT_FONT = os.path.normpath(os.path.join(
    REPO, "..", "NoisyPad", "core-soundfont-bundle", "src", "main", "assets", "soundfonts",
    "GeneralUser_GS.sf3"))


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def write_midi(path, program, key, vel, dur_s, tail_s, ppq=480, bpm=120):
    """Un track: program change, note-on en t=0, note-off en dur_s, fin en dur_s + tail_s.
    A 120 bpm un negra = 0,5 s = ppq ticks."""
    ticks = lambda s: int(round(s * bpm / 60.0 * ppq))
    trk = (vlq(0) + bytes([0xC0, program & 0x7F])
           + vlq(0) + bytes([0x90, key & 0x7F, vel & 0x7F])
           + vlq(ticks(dur_s)) + bytes([0x80, key & 0x7F, 0])
           + vlq(ticks(tail_s)) + b"\xff\x2f\x00")
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppq)
                + b"MTrk" + struct.pack(">I", len(trk)) + trk)


def read_wav_float32_stereo(path):
    """Lector RIFF mínimo para fmt 3 (float32): `wave` lo rechaza."""
    d = open(path, "rb").read()
    off, fmt, raw = 12, None, None
    while off + 8 <= len(d):
        cid, size = d[off:off + 4], struct.unpack_from("<I", d, off + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", d, off + 8)
        elif cid == b"data":
            raw = d[off + 8:off + 8 + size]
        off += 8 + size + (size & 1)
    tag, ch, sr, _, _, bits = fmt
    if tag != 3 or bits != 32 or ch != 2:
        sys.exit(f"{path}: se esperaba float32 estéreo, hay tag {tag} bits {bits} ch {ch}")
    n = len(raw) // 4
    x = struct.unpack(f"<{n}f", raw)
    # mono = media de los dos canales (lo que mide un RMS mono, como el de NoisyPad)
    return [(x[i] + x[i + 1]) * 0.5 for i in range(0, n, 2)], sr


def rms_db(x, a, b):
    seg = x[a:b]
    if not seg:
        return float("-inf")
    return 20.0 * math.log10(math.sqrt(sum(v * v for v in seg) / len(seg)) + 1e-12)


def build_tool(build_dir):
    exe = os.path.join(build_dir, "core_tests", "sf_render_preset")
    if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        gen = ["-G", "Ninja"] if subprocess.call(["which", "ninja"], stdout=subprocess.DEVNULL) == 0 else []
        subprocess.check_call(["cmake", "-S", TESTS, "-B", build_dir, *gen, "-DCMAKE_BUILD_TYPE=Debug",
                               "-DFETCHCONTENT_BASE_DIR=" + os.path.join(TESTS, ".deps")])
    subprocess.check_call(["cmake", "--build", build_dir, "--target", "sf_render_preset"],
                          stdout=sys.stderr)
    return exe


def render(exe, font, mid, out, sends, block):
    res = subprocess.run([exe, font, mid, out, "--sends", str(sends), "--block", str(block)],
                         capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"sf_render_preset falló ({res.returncode}): {res.stderr.strip()}")
    offs = [float(l.split()[1]) for l in res.stdout.splitlines() if l.startswith("note-off ")]
    ons = [float(l.split()[1]) for l in res.stdout.splitlines() if l.startswith("note-on ")]
    if not ons or not offs:
        sys.exit("el render no reportó note-on/note-off: " + res.stdout)
    return ons[0], offs[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font", default=DEFAULT_FONT)
    ap.add_argument("--preset", type=int, required=True, help="program number, bank 0")
    ap.add_argument("--key", type=int, required=True)
    ap.add_argument("--vel", type=int, required=True)
    ap.add_argument("--dur", type=float, default=1.0, help="segundos de nota (default 1,0)")
    ap.add_argument("--at", type=float, default=0.3, help="segundos tras el note-off donde se mide (default 0,3)")
    ap.add_argument("--win", type=float, default=0.05, help="ventana RMS en segundos (default 0,05)")
    ap.add_argument("--block", type=int, default=512)
    ap.add_argument("--build-dir", default=os.path.join(TESTS, "build"))
    ap.add_argument("--keep", metavar="DIR", help="dejar los .wav y el .mid acá en vez de borrarlos")
    a = ap.parse_args()
    if not os.path.exists(a.font):
        sys.exit(f"font no encontrado: {a.font} (pasá --font)")

    exe = build_tool(a.build_dir)
    work = a.keep or tempfile.mkdtemp(prefix="sf-delta-")
    os.makedirs(work, exist_ok=True)
    tag = f"p{a.preset}_k{a.key}_v{a.vel}"
    mid = os.path.join(work, tag + ".mid")
    write_midi(mid, a.preset, a.key, a.vel, a.dur, tail_s=max(2.0, a.at + 1.0))

    print(f"font {os.path.basename(a.font)} · preset {a.preset} (bank 0) · key {a.key} · vel {a.vel}"
          f" · nota {a.dur:.2f} s · medición a +{a.at:.2f} s del note-off, ventana {a.win * 1000:.0f} ms")
    print(f"{'ambiencia':>10} {'régimen':>9} {'cola':>9} {'cola−rég':>9}   (dBFS RMS mono)")
    levels = {}
    for sends in (1, 0):
        wav = os.path.join(work, f"{tag}_{'11' if sends else '00'}.wav")
        on, off = render(exe, a.font, mid, wav, sends, a.block)
        x, sr = read_wav_float32_stereo(wav)
        i = lambda s: int(round(s * sr))
        reg = rms_db(x, i(on + 0.1), i(on + 0.4))
        tail = rms_db(x, i(off + a.at - a.win / 2), i(off + a.at + a.win / 2))
        levels[sends] = tail - reg
        print(f"{'1/1' if sends else '0/0':>10} {reg:9.1f} {tail:9.1f} {tail - reg:9.1f}")
    wet_rel, dry_rel = levels[1], levels[0]
    margin = wet_rel - dry_rel
    print(f"\n(cola−régimen)_host con 1/1 a +{a.at:.2f} s = {wet_rel:+.1f} dB  ← comparar con el device: |Δ| > 3 dB = muerto")
    if margin >= 10.0:
        print(f"control 0/0: {margin:.1f} dB por debajo ⇒ el preset ES oportunidad (cuenta en N)")
    else:
        print(f"control 0/0: sólo {margin:.1f} dB por debajo (< 10) ⇒ la release del dry tapa el wet a +{a.at:.2f} s:"
              f" SIN OPORTUNIDAD, no cuenta en N")
    if not a.keep:
        for f in os.listdir(work):
            os.remove(os.path.join(work, f))
        os.rmdir(work)
    else:
        print(f"renders en {work}")


if __name__ == "__main__":
    main()
