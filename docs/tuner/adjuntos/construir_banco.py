#!/usr/bin/env python3
"""
Renderiza un banco de notas de instrumento REAL (muestras) desde un SoundFont, para probar el
afinador sin depender de un instrumento físico.

🔴 **La verdad NO sale del nombre del archivo.** El renderizador produce el audio; el f0 real de
cada archivo lo mide `calibrar_f0.py` después, y ESO es la verdad conocida. Así el error de
afinación propio del SoundFont deja de importar: pasa a ser parte del dato.

Sin reverb ni chorus (`-R 0 -C 0`): los dos emborronan el pitch y son justo lo que no se quiere
medir.
"""
import os, struct, subprocess, sys

def midi_nota(path, prog, bank, nota, dur_s=5.0, vel=100, bend=None):
    div = 480; us_por_negra = 500000; tpq_s = us_por_negra/1e6
    ticks = int(dur_s/tpq_s*div)
    ev = bytearray()
    def vlq(n):
        b=[n & 0x7F]; n >>= 7
        while n: b.append((n & 0x7F)|0x80); n >>= 7
        return bytes(reversed(b))
    ev += vlq(0) + bytes([0xB0, 0x00, (bank>>7)&0x7F])      # CC0  bank MSB
    ev += vlq(0) + bytes([0xB0, 0x20, bank&0x7F])           # CC32 bank LSB
    ev += vlq(0) + bytes([0xC0, prog & 0x7F])
    if bend is not None:
        v = max(0, min(16383, int(8192 + bend)))
        ev += vlq(0) + bytes([0xE0, v & 0x7F, (v>>7)&0x7F])
    ev += vlq(0) + bytes([0x90, nota, vel])
    ev += vlq(ticks) + bytes([0x80, nota, 0])
    ev += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    trk = b'MTrk' + struct.pack('>I', len(ev)) + bytes(ev)
    hdr = b'MThd' + struct.pack('>IHHH', 6, 0, 1, div)
    open(path,'wb').write(hdr+trk)

def render(sf2, mid, wav, sr=44100):
    subprocess.run(["fluidsynth","-ni","-R","0","-C","0","-g","0.9","-r",str(sr),
                    "-F",wav,sf2,mid], capture_output=True, check=True)

INSTRUMENTOS = [
    ("guitarra-nylon",  0, 24), ("guitarra-acero", 0, 25),
    ("guitarra-jazz",   0, 26), ("guitarra-limpia",0, 27),
    ("bajo-acustico",   0, 32), ("bajo-dedos",     0, 33),
    ("bajo-pua",        0, 34), ("bajo-fretless",  0, 35),
    ("ukelele",         8, 24),
]
CUERDAS = {
    "guitarra": [("E2",40),("A2",45),("D3",50),("G3",55),("B3",59),("E4",64)],
    "bajo":     [("E1",28),("A1",33),("D2",38),("G2",43)],
    "ukelele":  [("G4",67),("C4",60),("E4",64),("A4",69)],
}
def familia(n): return "bajo" if n.startswith("bajo") else ("ukelele" if n=="ukelele" else "guitarra")

if __name__ == "__main__":
    sf2, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True); tmp=os.path.join(out,"_mid"); os.makedirs(tmp, exist_ok=True)
    hechos=0
    for nom,bank,pc in INSTRUMENTOS:
        for cuerda,nota in CUERDAS[familia(nom)]:
            base=f"{nom}_{cuerda}"
            m=os.path.join(tmp,base+".mid"); w=os.path.join(out,base+".wav")
            midi_nota(m, pc, bank, nota)
            render(sf2, m, w); hechos+=1
    print(f"{hechos} archivos renderizados en {out}")
