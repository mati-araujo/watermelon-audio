// PoC MINI-028: maneja el motor por la API C en macOS y reporta si suena y cómo
// le fue al render block (ver WMA_POC_RT_STATS en CoreAudioBackend.mm).
#include "api/watermelon_audio.h"

#include <CoreAudio/CoreAudio.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" void wma_poc_get_rt_stats(uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*);

static AudioObjectID defaultOutput() {
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDefaultOutputDevice,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &dev);
    return dev;
}

static UInt32 deviceBufferFrames(AudioObjectID dev) {
    UInt32 frames = 0, size = sizeof(frames);
    AudioObjectPropertyAddress a{kAudioDevicePropertyBufferFrameSize,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &frames);
    return frames;
}

static bool setDeviceBufferFrames(AudioObjectID dev, UInt32 frames) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyBufferFrameSize,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    return AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof(frames), &frames) == noErr;
}

static double deviceSampleRate(AudioObjectID dev) {
    Float64 sr = 0; UInt32 size = sizeof(sr);
    AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &sr);
    return sr;
}

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 10.0;
    const UInt32 frames = argc > 2 ? (UInt32)std::atoi(argv[2]) : 256;
    const int engines[] = {0, 1, 2, 3, 4, 5};  // CLASSIC..SUPERSAW; SOUNDFONT necesita un .sf2

    const AudioObjectID dev = defaultOutput();
    const bool bufOk = setDeviceBufferFrames(dev, frames);
    std::printf("device: id=%u sr=%.0f buffer pedido=%u (%s) efectivo=%u\n", dev,
                deviceSampleRate(dev), frames, bufOk ? "ok" : "rechazado", deviceBufferFrames(dev));

    WmaEngine* e = wma_engine_create();
    if (!e) { std::puts("FAIL: wma_engine_create"); return 2; }
    wma_set_master_volume(e, 0.5f);
    if (wma_engine_start(e, 20) != 0 || wma_has_init_failed(e)) {
        std::printf("FAIL: wma_engine_start (error %d)\n", wma_get_last_error_code(e));
        return 3;
    }
    int sr = 0, buf = 0, ch = 0, ll = 0; float lat = 0;
    wma_get_stream_info_ex(e, &sr, &buf, &lat, &ch, &ll);
    std::printf("stream: sr=%d buffer=%d ch=%d latencia=%.1f ms\n", sr, buf, ch, lat);

    const auto t0 = std::chrono::steady_clock::now();
    const double perEngine = seconds / (sizeof(engines) / sizeof(engines[0]));
    float peakMax = -100.f;
    int engineIdx = -1;
    for (;;) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t >= seconds) break;
        const int idx = (int)(t / perEngine);
        if (idx != engineIdx) {
            if (engineIdx >= 0) std::printf("  engine %d: pico %.1f dBFS\n", engines[engineIdx], peakMax);
            engineIdx = idx;
            peakMax = -100.f;
            wma_set_engine_type(e, engines[idx]);
        }
        const float x = 0.5f + 0.45f * (float)std::sin(t * 1.3);
        const float y = 0.5f + 0.45f * (float)std::cos(t * 0.7);
        wma_set_xy(e, x, y);
        peakMax = std::fmax(peakMax, std::fmax(wma_get_output_peak_db(e, 0), wma_get_output_peak_db(e, 1)));
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~un cuadro de gesto
    }
    std::printf("  engine %d: pico %.1f dBFS\n", engines[engineIdx], peakMax);

    // SOUNDFONT (6): sólo si se pasa un .sf2/.sf3 como tercer argumento.
    if (argc > 3) {
        wma_set_engine_type(e, 6);
        const bool loaded = wma_sf_load_path(e, argv[3]);
        std::printf("  engine 6: carga %s, %d presets\n", loaded ? "ok" : "FALLÓ", wma_sf_get_preset_count(e));
        if (loaded) {
            wma_sf_set_preset(e, 0);
            peakMax = -100.f;
            const int notes[] = {60, 64, 67, 72};
            for (int n : notes) {
                wma_sf_note_on(e, 0, n, 0.8f);
                for (int i = 0; i < 30; ++i) {
                    peakMax = std::fmax(peakMax, wma_get_output_peak_db(e, 0));
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                wma_sf_note_off(e, 0);
            }
            std::printf("  engine 6 (%s): pico %.1f dBFS\n", wma_sf_get_preset_name(e, 0), peakMax);
        }
    }

    uint64_t blocks, sumNs, maxNs, over, disc, last;
    wma_poc_get_rt_stats(&blocks, &sumNs, &maxNs, &over, &disc, &last);
    const double budgetUs = sr > 0 ? (double)last / sr * 1e6 : 0;
    std::printf("render: bloques=%llu frames/bloque=%llu presupuesto=%.0f us medio=%.1f us max=%.1f us "
                "sobre-presupuesto=%llu discontinuidades=%llu\n",
                blocks, last, budgetUs, blocks ? sumNs / 1e3 / blocks : 0, maxNs / 1e3, over, disc);

    wma_engine_stop(e, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    wma_engine_destroy(e);
    return (blocks > 0 && over == 0 && disc == 0) ? 0 : 1;
}
