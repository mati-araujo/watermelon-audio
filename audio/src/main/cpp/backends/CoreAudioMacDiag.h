/**
 * CoreAudioMacDiag.h — diagnostico del backend CoreAudio en macOS (REQ-055 S3, espiga).
 *
 * C puro y SOLO macOS: lo exporta `libwatermelon_audio.dylib` del build de desktop
 * (`desktop/CMakeLists.txt`) y lo consume la espiga de Compose Desktop a traves de su
 * propio shim JNI (`studio-spike/src/main/native/`). NO es parte de la C API `wma_*`
 * (`api/watermelon_audio.h`, sin WMA_API): no lo ve el cinterop de iOS ni la JNI de
 * Android, y el build de iOS no lo compila (`#if TARGET_OS_OSX` en CoreAudioBackend.mm).
 *
 * De donde sale cada numero:
 *
 * - Lo de ADENTRO del render block (bloques, sobre-presupuesto, discontinuidades del
 *   reloj de muestras, duracion, toques) lo escribe SOLO el thread RT, con atomics
 *   (`CoreAudioMacDiag::noteRenderEnter/Exit`, raices de scripts/check-rt-safety.py).
 * - Los overloads del HAL (`kAudioDeviceProcessorOverload`) los cuenta un listener de
 *   propiedad, que CoreAudio despacha en su thread de notificaciones (no en el de IO).
 * - Los datos del dispositivo (tasa, buffer pedido/efectivo, latencias) se leen al abrir
 *   el stream, en el thread de control.
 *
 * Un solo backend vivo por proceso (el motor global): el estado es estatico.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WmaMacRenderStats {
    uint64_t blocks;             /* render blocks que llamaron al motor */
    uint64_t overBudget;         /* bloques cuyo render duro mas que frames/tasa */
    uint64_t discontinuities;    /* mSampleTime != anterior + frames */
    uint64_t sampleTimeInvalid;  /* bloques sin kAudioTimeStampSampleTimeValid */
    uint64_t halOverloads;       /* notificaciones kAudioDeviceProcessorOverload */
    uint64_t maxRenderNs;        /* duracion maxima del render block */
    uint64_t sumRenderNs;        /* suma, para la media */
    uint64_t lastFrames;         /* frames del ultimo bloque */
    uint64_t minFrames;          /* bloque mas chico visto (0 si ninguno) */
    uint64_t maxFrames;          /* bloque mas grande visto */
    uint64_t touchMarks;         /* toques estampados desde el reset (registrados o pisados) */
} WmaMacRenderStats;

typedef struct WmaMacDeviceInfo {
    uint32_t deviceId;
    double   nominalSampleRate;       /* kAudioDevicePropertyNominalSampleRate del que suena (0 si fallo) */
    double   streamSampleRate;        /* tasa con la que abrio el grafo (la del motor) */
    uint32_t bufferFramesRequested;   /* lo que pidio el backend */
    uint32_t bufferFramesActual;      /* kAudioDevicePropertyBufferFrameSize despues de start */
    uint32_t bufferFramesMin;         /* kAudioDevicePropertyBufferFrameSizeRange */
    uint32_t bufferFramesMax;
    uint32_t deviceLatencyFrames;     /* kAudioDevicePropertyLatency (salida) */
    uint32_t safetyOffsetFrames;      /* kAudioDevicePropertySafetyOffset (salida) */
    uint32_t streamLatencyFrames;     /* kAudioStreamPropertyLatency, primer stream de salida */
    double   presentationLatencySec;  /* AVAudioOutputNode.presentationLatency (control cruzado) */
    int32_t  bufferSetStatus;         /* OSStatus de fijar el buffer (0 = ok) */
    int32_t  overloadListenerStatus;  /* 0 = HAY listener de overload sobre deviceId */
} WmaMacDeviceInfo;

typedef struct WmaMacTouchSample {
    uint64_t touchNs;        /* wma_macos_diag_mark_touch(), reloj mach en ns */
    uint64_t renderEntryNs;  /* entrada al primer render block que lo levanto */
    uint64_t bufferHostNs;   /* AudioTimeStamp.mHostTime de ese bloque (0 si no era valido) */
    uint32_t frames;         /* frames de ese bloque */
} WmaMacTouchSample;

/** 1 si hay datos (backend macOS compilado), 0 si no. */
int wma_macos_diag_render_stats(WmaMacRenderStats* out);
int wma_macos_diag_device_info(WmaMacDeviceInfo* out);

/** Pide poner a cero contadores y toques; lo aplica el thread RT en su proximo bloque. */
void wma_macos_diag_reset(void);

/** Estampa un toque con el reloj mach; el proximo render block lo registra. */
void wma_macos_diag_mark_touch(void);

/**
 * Copia hasta `max` toques registrados desde el ultimo reset ya aplicado; devuelve
 * cuantos. Devuelve -1 si hay un reset pedido que el RT todavia no aplico.
 */
int wma_macos_diag_touch_samples(WmaMacTouchSample* out, int max);

/** Reloj mach en ns (el mismo de los toques). */
uint64_t wma_macos_diag_now_ns(void);

#ifdef __cplusplus
}
#endif
