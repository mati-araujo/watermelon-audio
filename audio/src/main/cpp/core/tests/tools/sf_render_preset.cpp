// sf_render_preset — REQ-040, criterio de muerte, instrumento I-2.
//
// Rinde un .mid contra un font con EL MISMO arnes que la conformidad
// (support/MidiSpecHarness.h: tsf + la tabla de moduladores + wma::SoundFontSendBus,
// el objeto que produccion usa en SoundFontEngine::render) y escribe un WAV estereo
// float32. `--sends 1` es la perilla de la ambiencia en 1/1; `--sends 0`, en 0/0 —
// que AC-042.1 afirma muestra a muestra igual al seco de produccion. La diferencia
// entre los dos renders es el Δ_host que el criterio de muerte de REQ-040 compara
// contra el Δ_device de NoisyPad.
//
// Vivio como `wmarender.cpp` en un scratchpad hasta el 2026-09-16; se versiono porque
// un umbral cuyo instrumento no esta en el arbol no es reproducible. No es un test:
// es un target EXCLUDE_FROM_ALL que `scripts/sf-delta-host.py` construye a pedido.
//
//   sf_render_preset FONT MID OUT.wav [--sends 0|1] [--block N] [--rate R]
#include "support/MidiSpecHarness.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void writeLe32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
void writeLe16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

// WAV float32 estereo (fmt tag 3). El lector de scripts/sf-delta-host.py lo entiende;
// el modulo `wave` de Python no, y por eso ese script trae su lector RIFF propio.
bool writeWavFloat32Stereo(const char* path, const std::vector<float>& interleaved, int rate) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const auto dataBytes = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    fwrite("RIFF", 1, 4, f);
    writeLe32(f, 36 + dataBytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    writeLe32(f, 16);
    writeLe16(f, 3);                                   // IEEE float
    writeLe16(f, 2);                                   // canales
    writeLe32(f, static_cast<uint32_t>(rate));
    writeLe32(f, static_cast<uint32_t>(rate) * 8);     // bytes por segundo
    writeLe16(f, 8);                                   // bytes por frame
    writeLe16(f, 32);                                  // bits por muestra
    fwrite("data", 1, 4, f);
    writeLe32(f, dataBytes);
    fwrite(interleaved.data(), sizeof(float), interleaved.size(), f);
    fclose(f);
    return true;
}

int usage(const char* argv0) {
    fprintf(stderr, "uso: %s FONT MID OUT.wav [--sends 0|1] [--block N] [--rate R]\n", argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return usage(argv[0]);
    const std::string font = argv[1], mid = argv[2], out = argv[3];
    bool withSends = true;
    int block = 512;
    int rate = 44100;
    for (int i = 4; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--sends") == 0) withSends = atoi(argv[i + 1]) != 0;
        else if (strcmp(argv[i], "--block") == 0) block = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--rate") == 0) rate = atoi(argv[i + 1]);
        else return usage(argv[0]);
    }
    if (block <= 0 || block % 64 != 0 || rate <= 0) return usage(argv[0]);

    const auto r = wma_test::specmidi::render(font, mid, rate, 0.0f, block, withSends);
    if (!r.valid) {
        fprintf(stderr, "render invalido: font o mid ilegible (%s, %s)\n", font.c_str(), mid.c_str());
        return 1;
    }
    if (!writeWavFloat32Stereo(out.c_str(), r.stereo, rate)) {
        fprintf(stderr, "no se pudo escribir %s\n", out.c_str());
        return 1;
    }
    // Una linea por evento, parseable: el script la usa para ubicar el note-off sin
    // re-parsear el .mid que el mismo escribio.
    printf("rate %d frames %d sends %d\n", r.sampleRate, r.frames, withSends ? 1 : 0);
    for (const auto& n : r.noteOns) printf("note-on %.6f ch %d key %d vel %d\n", n.sec, n.channel, n.key, n.velocity);
    for (const auto& n : r.noteOffs) printf("note-off %.6f ch %d key %d\n", n.sec, n.channel, n.key);
    return 0;
}
