#!/usr/bin/env bash
# render-corpus.sh — REQ-032 S1. LA RECETA del corpus grabado, entera y versionada.
#
# Renderiza con FluidSynth las notas de instrumento REAL (muestras de un SoundFont)
# que forman el corpus de robustez del afinador. Es la misma receta que el
# consumidor documento el 2026-09-03 (`docs/tuner/adjuntos/construir_banco.py`):
# banco/programa por instrumento, una nota de 5 s a velocity 100, sin reverb ni
# chorus, 44,1 kHz. Con el mismo `.sf2` y el mismo FluidSynth, el WAV sale igual.
#
# 🔴 EL SOUNDFONT SE FIJA POR ARTEFACTO, NO POR NOMBRE NI POR RAMA. El repo de
# GeneralUser GS no tiene tags: `main` es una rama movil. Lo que se fija aca es el
# COMMIT del que se baja, el sha256 del archivo y su blob sha1 de git, y la version
# se LEE del artefacto (chunk INAM) — no se afirma. Si `main` cambia manana, este
# script sigue bajando exactamente los mismos bytes.
#
# 🔴 LOS WAV NO VIVEN EN EL REPO. Este script los produce; el corpus que los tests
# usan se baja de los assets del release `corpus-vN` con `scripts/fetch-corpus.sh`
# y se verifica por sha256 contra `analysis/tests/corpus-manifest.txt`. Regenerar
# es para RECONSTRUIR o EXTENDER el corpus, no para correr los tests: FluidSynth no
# promete el mismo byte en otra maquina, y por eso lo que se verifica es el
# artefacto publicado, no la regeneracion.
#
#   bash scripts/render-corpus.sh [DIR_SALIDA]     # default: build/corpus-render
#
# Requiere `fluidsynth` (2.6.0, la version de la receta) y `python3` (stdlib).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO_ROOT/build/corpus-render}"

# --- el artefacto, fijado ----------------------------------------------------
SF2_COMMIT="684543d5e5efaef08d02be50dcda8d552478fa60"     # head de main del 2026-02-23
SF2_URL="https://github.com/mrbumpy409/GeneralUser-GS/raw/${SF2_COMMIT}/GeneralUser-GS.sf2"
SF2_SHA256="9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe"
SF2_BLOB_SHA1="298b552d2e9d1307e03e5c5c99d2c046aaed9ec3"   # el que declaro el consumidor
FLUIDSYNTH_VERSION="2.6.0"

mkdir -p "$OUT/_sf2" "$OUT/_mid"
SF2="$OUT/_sf2/GeneralUser-GS.sf2"

command -v fluidsynth >/dev/null || { echo "falta fluidsynth" >&2; exit 1; }
have="$(fluidsynth --version 2>&1 | head -1 | sed -E 's/.*version ([0-9.]+).*/\1/')"
if [ "$have" != "$FLUIDSYNTH_VERSION" ]; then
    # Se para en vez de avisar: un render con otra version es OTRO corpus, y un
    # corpus que no coincide con el manifiesto produce un rojo que parece del motor.
    echo "fluidsynth $have, la receta fija $FLUIDSYNTH_VERSION. Poné WMA_RENDER_ANY_FLUIDSYNTH=1 para renderizar igual (no va a coincidir con el manifiesto)." >&2
    [ "${WMA_RENDER_ANY_FLUIDSYNTH:-0}" = "1" ] || exit 1
fi

if [ ! -f "$SF2" ]; then
    echo "bajando GeneralUser-GS.sf2 del commit ${SF2_COMMIT:0:7} ..."
    curl -sSL -o "$SF2" "$SF2_URL"
fi
got="$(shasum -a 256 "$SF2" | cut -d' ' -f1)"
if [ "$got" != "$SF2_SHA256" ]; then
    printf '🔴 el .sf2 NO es el artefacto fijado\n   esperado %s\n   obtenido %s\n' "$SF2_SHA256" "$got" >&2
    exit 1
fi
blob="$(git hash-object "$SF2")"
[ "$blob" = "$SF2_BLOB_SHA1" ] || { printf '🔴 blob sha1 distinto: %s\n' "$blob" >&2; exit 1; }

# La version se LEE del artefacto (chunk INAM del INFO), no se escribe a mano.
python3 - "$SF2" <<'PY'
import sys, struct
d = open(sys.argv[1], 'rb').read(65536)
assert d[:4] == b'RIFF' and d[8:12] == b'sfbk', "no es un SoundFont 2"
off = 12
while off + 8 <= len(d):
    cid = d[off:off+4]; sz = struct.unpack('<I', d[off+4:off+8])[0]
    if cid == b'LIST' and d[off+8:off+12] == b'INFO':
        o = off + 12; end = off + 8 + sz
        while o + 8 <= end:
            c2 = d[o:o+4]; s2 = struct.unpack('<I', d[o+4:o+8])[0]
            if c2 == b'INAM':
                print("artefacto:", d[o+8:o+8+s2].rstrip(b'\0').decode('latin1'))
            o += 8 + s2 + (s2 & 1)
        break
    off += 8 + sz + (sz & 1)
PY
echo "sha256:    $SF2_SHA256"
echo "blob sha1: $SF2_BLOB_SHA1"

# --- la receta: los .mid los escribe python (stdlib), FluidSynth los toca ----
#
# Instrumentos y cuerdas son los del consumidor, tal cual: 9 presets x sus
# cuerdas = 44 archivos. Se renderizan TODOS aunque el manifiesto despues no
# declare hz para alguno — la receta es una y no se recorta a mano.
python3 - "$OUT/_mid" <<'PY'
import sys, os, struct
out = sys.argv[1]
def midi_nota(path, prog, bank, nota, dur_s=5.0, vel=100):
    div = 480; tpq_s = 0.5
    ticks = int(dur_s / tpq_s * div)
    ev = bytearray()
    def vlq(n):
        b = [n & 0x7F]; n >>= 7
        while n: b.append((n & 0x7F) | 0x80); n >>= 7
        return bytes(reversed(b))
    ev += vlq(0) + bytes([0xB0, 0x00, (bank >> 7) & 0x7F])   # CC0  bank MSB
    ev += vlq(0) + bytes([0xB0, 0x20, bank & 0x7F])          # CC32 bank LSB
    ev += vlq(0) + bytes([0xC0, prog & 0x7F])
    ev += vlq(0) + bytes([0x90, nota, vel])
    ev += vlq(ticks) + bytes([0x80, nota, 0])
    ev += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    trk = b'MTrk' + struct.pack('>I', len(ev)) + bytes(ev)
    hdr = b'MThd' + struct.pack('>IHHH', 6, 0, 1, div)
    open(path, 'wb').write(hdr + trk)
INSTRUMENTOS = [("guitarra-nylon", 0, 24), ("guitarra-acero", 0, 25), ("guitarra-jazz", 0, 26),
                ("guitarra-limpia", 0, 27), ("bajo-acustico", 0, 32), ("bajo-dedos", 0, 33),
                ("bajo-pua", 0, 34), ("bajo-fretless", 0, 35), ("ukelele", 8, 24)]
CUERDAS = {"guitarra": [("E2", 40), ("A2", 45), ("D3", 50), ("G3", 55), ("B3", 59), ("E4", 64)],
           "bajo":     [("E1", 28), ("A1", 33), ("D2", 38), ("G2", 43)],
           "ukelele":  [("G4", 67), ("C4", 60), ("E4", 64), ("A4", 69)]}
def familia(n): return "bajo" if n.startswith("bajo") else ("ukelele" if n == "ukelele" else "guitarra")
for nom, bank, pc in INSTRUMENTOS:
    for cuerda, nota in CUERDAS[familia(nom)]:
        midi_nota(os.path.join(out, f"{nom}_{cuerda}.mid"), pc, bank, nota)
PY

n=0
for mid in "$OUT"/_mid/*.mid; do
    base="$(basename "$mid" .mid)"
    fluidsynth -ni -R 0 -C 0 -g 0.9 -r 44100 -F "$OUT/$base.wav" "$SF2" "$mid" >/dev/null 2>&1
    n=$((n + 1))
done
echo "$n archivos renderizados en $OUT"
