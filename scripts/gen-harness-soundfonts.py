#!/usr/bin/env python3
"""
gen-harness-soundfonts.py — MINI-038: los fixtures `.sf2` y `.sf3` del harness, GENERADOS.

No se versiona ningún binario (memoria `fixture-sf2-generado`): un archivo opaco en el repo no dice
por qué tiene la forma que tiene ni cómo regenerarlo. Esta receta sí, y el build de `:harness` la
corre para empaquetarlos (assets en Android, recursos en iOS).

Qué genera — mínimos pero LEGÍTIMOS (no "lo más chico que tsf acepta", que es otra cosa):

- **Un preset** (bank 0 / program 0, "WMA fixture") → **un instrumento** → **una zona** con
  `sampleModes = 1` (loop continuo) → **una muestra**: 1 s de senoide de 441 Hz a 44 100 Hz
  (período exacto de 100 muestras, así el loop cae en un número entero de períodos), raíz 69 con
  `pitchCorrection = -4` cents (441 Hz ≈ A4 + 3,9 c). Una muestra en ceros daría un fixture MUDO,
  y un test de "suena" mediría silencio tanto si el motor anda como si no (memoria citada).
- Los **nueve** chunks de la hydra con su registro terminal (EOP/EOI/EOS), `ifil`, `isng` e `INAM`.
- `.sf2`: la muestra en PCM de 16 bits, seguida de los **46 puntos en cero** que exige el spec
  (§6.1). Sin ellos, la interpolación lee afuera del buffer (ASan lo vio en su momento).
- `.sf3`: la MISMA muestra comprimida en **Ogg Vorbis**, con el bit de compresión (0x10) en
  `sampleType`, `start`/`end` en BYTES dentro de `smpl` y los loops relativos a la muestra
  decodificada — la convención de los `.sf3` de MuseScore, que es la que lee tsf.

Determinismo: el `.sf2` es Python puro, byte a byte igual en cualquier máquina. El `.sf3` depende
del encoder (ffmpeg con `-bitexact`): es determinista EN UN ENTORNO (dos corridas, mismo sha), pero
libvorbis y el encoder nativo de ffmpeg dan bytes distintos. Por eso el sha256 se IMPRIME y no se
fija: es la huella de lo que se empaquetó, para comparar corridas.

Sin encoder Vorbis la receta FALLA con un mensaje claro (exit 3). No se versiona un binario para
esquivarlo. El build de `:harness` corre `--only sf2` y `--only sf3` por separado y, por D10 de
MINI-038, trata SOLO ese exit 3 como WARNING: empaqueta el `.sf2` y la app reporta el `.sf3` como no
empaquetado (FAIL del smoke). Cualquier otro fallo rompe el build.

Uso:
    python3 scripts/gen-harness-soundfonts.py --out DIR            # los dos
    python3 scripts/gen-harness-soundfonts.py --out DIR --only sf2 # sólo el .sf2 (sin dependencias)
    python3 scripts/gen-harness-soundfonts.py --out DIR --only sf3

Python stdlib + ffmpeg, como el resto de scripts/.
"""
from __future__ import annotations

import argparse
import hashlib
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

SAMPLE_RATE = 44100
PERIOD = 100                      # muestras por ciclo → 441 Hz exactos
N_SAMPLES = SAMPLE_RATE           # 1 s
AMPLITUDE = 0.5
LOOP_START = 44 * PERIOD          # 4400: múltiplos enteros del período
LOOP_END = 396 * PERIOD           # 39600
ROOT_KEY = 69
PITCH_CORRECTION = -4             # cents: 441 Hz → ≈ 440 Hz

GEN_INSTRUMENT = 41
GEN_SAMPLE_ID = 53
GEN_SAMPLE_MODES = 54

SAMPLE_TYPE_MONO = 1
SAMPLE_TYPE_VORBIS = 0x10         # el flag de compresión de los .sf3 (tsf mira `& 0x30`)

SF2_NAME = "wma-fixture.sf2"
SF3_NAME = "wma-fixture.sf3"


def chunk(fourcc: str, data: bytes) -> bytes:
    pad = b"\0" if len(data) % 2 else b""
    return fourcc.encode("ascii") + struct.pack("<I", len(data)) + data + pad


def zstr(s: str, size: int) -> bytes:
    b = s.encode("ascii")[: size - 1]
    return b + b"\0" * (size - len(b))


def info_list(major: int, minor: int) -> bytes:
    name = zstr("WMA harness fixture", 20)  # par y terminado en NUL (INAM impar rompe FluidSynth)
    return b"INFO" + chunk("ifil", struct.pack("<HH", major, minor)) + chunk("isng", zstr("EMU8000", 8)) \
        + chunk("INAM", name)


def pdta_list(sample_start: int, sample_end: int, loop_start: int, loop_end: int, sample_type: int) -> bytes:
    phdr = struct.pack("<20sHHHIII", zstr("WMA fixture", 20), 0, 0, 0, 0, 0, 0) \
        + struct.pack("<20sHHHIII", zstr("EOP", 20), 0, 0, 1, 0, 0, 0)
    pbag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 1, 0)
    pmod = b"\0" * 10
    pgen = struct.pack("<HH", GEN_INSTRUMENT, 0) + struct.pack("<HH", 0, 0)
    inst = struct.pack("<20sH", zstr("WMA fixture sine", 20), 0) + struct.pack("<20sH", zstr("EOI", 20), 1)
    ibag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 2, 0)
    imod = b"\0" * 10
    igen = struct.pack("<HH", GEN_SAMPLE_MODES, 1) + struct.pack("<HH", GEN_SAMPLE_ID, 0) + struct.pack("<HH", 0, 0)
    shdr = struct.pack("<20sIIIIIBbHH", zstr("sine441", 20), sample_start, sample_end, loop_start, loop_end,
                       SAMPLE_RATE, ROOT_KEY, PITCH_CORRECTION, 0, sample_type) \
        + struct.pack("<20sIIIIIBbHH", zstr("EOS", 20), 0, 0, 0, 0, 0, 0, 0, 0, 0)
    return b"pdta" + b"".join(chunk(n, d) for n, d in (
        ("phdr", phdr), ("pbag", pbag), ("pmod", pmod), ("pgen", pgen),
        ("inst", inst), ("ibag", ibag), ("imod", imod), ("igen", igen), ("shdr", shdr)))


def sine_pcm16() -> bytes:
    peak = int(AMPLITUDE * 32767)
    return b"".join(struct.pack("<h", int(round(peak * math.sin(2 * math.pi * i / PERIOD))))
                    for i in range(N_SAMPLES))


def build_sf2(pcm: bytes) -> bytes:
    smpl = pcm + b"\0" * (46 * 2)  # los 46 puntos en cero del spec (§6.1)
    body = b"sfbk" + chunk("LIST", info_list(2, 1)) + chunk("LIST", b"sdta" + chunk("smpl", smpl)) \
        + chunk("LIST", pdta_list(0, N_SAMPLES, LOOP_START, LOOP_END, SAMPLE_TYPE_MONO))
    return chunk("RIFF", body)


def vorbis_encoder(ffmpeg: str) -> str | None:
    out = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
    names = {line.split()[1] for line in out.splitlines() if len(line.split()) > 1 and line.lstrip().startswith("A")}
    if "libvorbis" in names:
        return "libvorbis"
    if "vorbis" in names:
        return "vorbis"
    return None


def fail_no_encoder(why: str) -> None:
    sys.stderr.write(
        "gen-harness-soundfonts: FAIL — no hay encoder Vorbis (%s).\n"
        "  El .sf3 del harness lleva sus muestras en Ogg Vorbis y se GENERA: no se versiona un\n"
        "  binario para esquivar esto. Instalá ffmpeg con un encoder Vorbis (libvorbis o el\n"
        "  nativo `vorbis`), p. ej. `brew install ffmpeg` / `apt-get install ffmpeg`.\n" % why)
    sys.exit(3)


def encode_ogg(pcm: bytes) -> tuple[bytes, str]:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        fail_no_encoder("ffmpeg no está en el PATH")
    enc = vorbis_encoder(ffmpeg)
    if enc is None:
        fail_no_encoder("ffmpeg no lista ni libvorbis ni vorbis")
    with tempfile.TemporaryDirectory() as tmp:
        wav_path = os.path.join(tmp, "in.wav")
        # La muestra entra en MONO. El encoder nativo de ffmpeg sólo acepta estéreo, así que a ése
        # se le pide `-ac 2` (el upmix de ffmpeg) y no un WAV intercalado a mano: con el MISMO PCM,
        # el intercalado a mano dio un Ogg que tsf+stb_vorbis RECHAZA y el upmix uno que carga
        # (medido el 2026-09-30 con ffmpeg 9.0.1; ver las notas de MINI-038). tsf decodifica el
        # canal 0, así que las dos formas dan la misma muestra.
        with wave.open(wav_path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(pcm)
        ogg_path = os.path.join(tmp, "out.ogg")
        cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", wav_path,
               "-c:a", enc, "-fflags", "+bitexact", "-flags:a", "+bitexact",
               "-map_metadata", "-1", ogg_path]
        if enc == "vorbis":
            cmd[cmd.index("-c:a"):cmd.index("-c:a")] = ["-strict", "-2", "-ac", "2"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(ogg_path):
            sys.stderr.write("gen-harness-soundfonts: FAIL — ffmpeg (%s) no pudo codificar:\n%s\n" % (enc, r.stderr))
            sys.exit(1)
        with open(ogg_path, "rb") as f:
            ogg = f.read()
    if not ogg.startswith(b"OggS"):
        sys.stderr.write("gen-harness-soundfonts: FAIL — la salida de ffmpeg no empieza con OggS\n")
        sys.exit(1)
    return ogg, enc


def build_sf3(ogg: bytes) -> bytes:
    body = b"sfbk" + chunk("LIST", info_list(3, 0)) + chunk("LIST", b"sdta" + chunk("smpl", ogg)) \
        + chunk("LIST", pdta_list(0, len(ogg), LOOP_START, LOOP_END, SAMPLE_TYPE_MONO | SAMPLE_TYPE_VORBIS))
    return chunk("RIFF", body)


def write(out_dir: str, name: str, data: bytes, note: str) -> None:
    path = os.path.join(out_dir, name)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    print("%s  %s  %d bytes  (%s)" % (hashlib.sha256(data).hexdigest(), name, len(data), note))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True, help="directorio de salida")
    ap.add_argument("--only", choices=("sf2", "sf3"), help="generar uno solo")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    pcm = sine_pcm16()
    if args.only in (None, "sf2"):
        write(args.out, SF2_NAME, build_sf2(pcm), "PCM 16 bits")
    if args.only in (None, "sf3"):
        ogg, enc = encode_ogg(pcm)
        write(args.out, SF3_NAME, build_sf3(ogg), "Ogg Vorbis, encoder=%s" % enc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
