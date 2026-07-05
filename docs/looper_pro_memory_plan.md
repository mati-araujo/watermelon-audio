# Looper Pro — Plan de memoria, CPU y estructuras de datos (watermelon-audio)

**Objetivo:** sentar la base técnica del motor de loops para mezcla profesional en vivo:
pistas largas con silencio (inicio/medio/final) sin costo de memoria, loops repetibles X veces,
escalado por tier de dispositivo (más pistas / más duración / más budget en gama alta),
y CPU predecible con 8–16 pistas activas.

**Alcance:** `audio/src/main/cpp/looper/` (AudioLooper.h, TrackBuffer.h, WavFile.h, Transport.h)
y el bridge JNI correspondiente. El plan hermano en NoisyPad consume estas APIs:
`NoisyPad/docs/korg-req/phase15_looper_pro_memory.md`.

---

## 1. Diagnóstico (estado actual)

### Memoria

| Hecho | Consecuencia |
|---|---|
| `TrackBuffer` = `std::vector<float>` denso, estéreo interleaved | 60 s @48kHz = **23.04 MB por pista**. El silencio ocupa lo mismo que el audio. |
| Budget global fijo `MEMORY_BUDGET_BYTES = 48 MB` (AudioLooper.h:41) | ~131 s de audio total. **2 takes libres de 60 s ≈ budget completo.** Igual en un Galaxy A14 que en un S24 Ultra. |
| `MAX_TRACKS = 8` compile-time | No escalable por dispositivo sin recompilar. |
| Free take pre-dimensiona a 60 s y luego `trimToLength()` | Pico transitorio de 23 MB + **realloc/copy de hasta 23 MB** en UI/IO thread con spin-wait (`waitForRenderIdle`) bloqueando la liberación. |
| `saveUndoSnapshot()` copia el buffer entero | Undo de una pista de 60 s = **+23 MB** (duplica), contado contra el budget. |
| `finalizeFreeLoop()` pad con silencio = alloc nuevo + copy completo | Otra copia O(n) para agregar ceros al final. |
| Alloc contiguo grande (`vector::resize`) | Riesgo de fallo por fragmentación del heap en sesiones largas, incluso con budget disponible. |

**Conclusión estructural:** el buffer denso contiguo es la causa raíz de 5 de los 7 problemas.
La respuesta correcta no es subir el budget: es cambiar la estructura de datos.

### CPU (audio thread)

- `mixInto()` (TrackBuffer.h:239): por **cada sample** de **cada pista**: `std::fmod`, interpolación
  Catmull-Rom 4-tap, 3 smoothers one-pole (vol/mute/pan) y lookup de pan. A speed==1.0 (el caso
  dominante en vivo) la interpolación y el fmod son puro desperdicio.
- Overdub en `process()` (AudioLooper.h:119): `%` por frame + `tanh` por sample.
- `mLooperMixBuf.resize()` puede correr en el audio thread (alloc en RT path, AudioLooper.h:213).
- `emitStateEvents()` ya es push-based (bien), pero `recordProgress`/`progress` global siguen
  siendo polled desde Kotlin a 33/50 ms.

### Diseño / SRP

- **AudioLooper.h = 1659 líneas, header-only, god class:** máquina de estados de grabación,
  mezcla, click de metrónomo, export/import/resample, caché de waveform, telemetría y emisión
  de eventos en una sola clase. Cada TU que lo incluye recompila todo.
- Export/import/resample (~500 líneas) no tocan el RT path: no tienen por qué vivir en el header.

### Tests

- `looper/tests/` cubre TrackBuffer, Transport, Limiter, PanLUT, PreRollRing, WavFile,
  free-loop autosync. **No existe `test_audio_looper.cpp`**: budget, FSM de grabación,
  wrap-mix tail, armed trigger, import+resample y export snapshot no tienen cobertura.

---

## 2. Quick wins (sin cambio estructural, 1–2 días)

| # | Cambio | Archivo | Valor |
|---|---|---|---|
| QW-1 | **Fast path speed==1.0 en `mixInto()`**: fuera de la ventana de crossfade del seam, copiar por bloques con ganancia en rampa lineal (SIMD, reutilizar `simd::applyStereoGainRamp`). Catmull-Rom + fmod solo cuando `speed != 1.0` o dentro del crossfade. | TrackBuffer.h | ~3–5× menos CPU por pista en el caso común. Habilita 16 pistas. |
| QW-2 | **Smoothers por bloque, no por sample**: vol/mute/pan como rampa lineal start→end por bloque (forma cerrada del one-pole, mismo patrón que ya usa el master volume en AudioLooper.h:238). | TrackBuffer.h | Elimina 3 multiplicaciones+3 sumas por sample por pista. |
| QW-3 | **Overdub sin `%` por frame**: partir el bloque en 2 segmentos lineales (hasta el wrap, después del wrap). Reemplazar `tanh` por aproximación polinómica (o LUT) — ya hay precedente con PanLUT. | AudioLooper.h `process()` | Menos branching y transcendentales en RT. |
| QW-4 | **Pre-alloc de `mLooperMixBuf` fuera del RT**: dimensionar al `framesPerBurst` máximo reportado por Oboe en `setSampleRate()`/prepare (UI thread). El grow en audio thread queda solo como fallback con log de error. | AudioLooper.h | Elimina alloc en RT path. |
| QW-5 | **Evento `RecordProgress` en el dispatcher**: emitir desde `process()` con el mismo threshold que `Progress`. Kotlin puede matar su polling loop de 33 ms. | AudioLooper.h + LooperEventDispatcher.h | ~30 llamadas JNI/s menos; una sola fuente de verdad push. |
| QW-6 | **Waveform incremental durante grabación**: acumular el peak del bin actual en `writeFrame()`/overdub (ya se conoce la posición), en vez de escanear O(n) el buffer en `getTrackWaveform()`. | TrackBuffer.h / AudioLooper.h | Waveform en vivo sin escaneo; el caché por overdub deja de servir datos viejos. |
| QW-7 | **`waitForRenderIdle()` con `std::this_thread::yield()`** en el spin y contador de iteraciones con log si excede ~2 callbacks. | TrackBuffer.h | Menos contención; diagnóstico si el audio thread se cuelga. |

---

## 3. Refactor de alto valor

### 3.1 `ChunkedAudioBuffer` — la pieza central (P0)

Reemplazar el `std::vector<float>` denso por un buffer paginado:

```
ChunkedAudioBuffer
├─ PageTable: std::vector<Chunk*> (indexada por frame >> kChunkShift)
├─ Chunk: bloque fijo de 32768 frames estéreo (256 KB), alineado, del ChunkPool
├─ nullptr en la PageTable = página de silencio (no ocupa memoria)
└─ ChunkPool: free-list lock-free; UI/IO thread aloca y libera, audio thread solo toma/lee
```

**Parámetros:** `kChunkFrames = 32768` (~0.68 s @48k, 256 KB). Grande para amortizar el branch
por página en `mixInto`, chico para granularidad de silencio y undo.

**Qué resuelve, punto por punto:**

| Problema actual | Con ChunkedAudioBuffer |
|---|---|
| Silencio ocupa memoria | Páginas silenciosas = `nullptr`. Una pista de 60 s con 10 s de contenido ocupa ~4 MB, no 23 MB. El lector emite ceros por bloque (branch por página, no por sample). |
| Pre-size de 60 s del free take | No hay pre-size: la grabación toma chunks del pool a demanda (el pool se mantiene con N chunks libres; un hilo IO lo rellena — el audio thread nunca aloca). |
| `trimToLength()` realloc+copy 23 MB | Trim = devolver páginas al pool + truncar PageTable. **O(páginas), sin copias.** |
| `finalizeFreeLoop()` pad con silencio | Pad = agregar entradas `nullptr` a la PageTable. **O(1) por página.** |
| Undo duplica el buffer | **Copy-on-write:** `startOverdub()` (UI thread) retiene la PageTable actual como snapshot de undo y marca las páginas de la región como COW; la primera escritura por página materializa una copia (fuera de RT o pre-materializada en startOverdub para la región del loop). Undo = swap de PageTables. Costo = solo las páginas tocadas, y las de silencio nunca se materializan. |
| Fragmentación por allocs contiguos | Todos los allocs son de 256 KB uniformes → el heap no se fragmenta. |
| Budget accounting | `allocatedBytes()` = `chunksEnUso × kChunkBytes` exacto y honesto por construcción. |

**RT-safety del diseño:** la PageTable activa se publica por puntero atómico (patrón RCU):
el audio thread carga el puntero al inicio de `mixInto()` (dentro del `RenderScope` existente);
UI/IO thread nunca libera una PageTable/chunk hasta `waitForRenderIdle()`. Es el mismo contrato
que hoy protege `mBuffer`, formalizado.

**Migración incremental:** `TrackBuffer` conserva su API pública (`writeFrame`, `mixInto`,
`overdubFrame`, `data()` desaparece → ver 3.3). Los tests existentes de TrackBuffer deben pasar
sin cambios de semántica.

### 3.2 Capacidades runtime — budget, pistas y duraciones por tier (P0)

```cpp
struct LooperCapabilities {
    size_t memoryBudgetBytes;   // default 48 MB (comportamiento actual)
    int    maxActiveTracks;     // default 8, techo compile-time MAX_TRACKS_HW = 16
    int    maxFreeSeconds;      // default 60
    int    chunkPoolPrefill;    // chunks pre-alocados al habilitar el looper
};
void AudioLooper::setCapabilities(const LooperCapabilities&);  // UI thread, antes/entre sesiones
```

- Arrays internos dimensionados a `MAX_TRACKS_HW = 16` compile-time; `mMaxActiveTracks`
  runtime limita `prepareTrack`/`startRecording`/loops de mezcla (iterar solo hasta el límite
  activo — también ahorra CPU en gama baja).
- Exponer por JNI: `setLooperCapabilities(budgetBytes, maxTracks, maxFreeSeconds)`.
  NoisyPad decide el tier (RAM del dispositivo + Remote Config) — el motor no conoce Android.
- Reglas de reducción segura: bajar el budget con pistas cargadas nunca libera contenido;
  solo afecta allocs futuros. Bajar maxActiveTracks no desactiva pistas ya activas.

### 3.3 Descomposición de AudioLooper (SRP) (P1)

```
looper/
├─ AudioLooper.h        → fachada delgada + process() (orquestación RT)
├─ ChunkedAudioBuffer.h → estructura de datos (3.1)
├─ TrackBuffer.h        → estado por pista + mixInto (usa ChunkedAudioBuffer)
├─ LooperRecorder.h     → FSM de grabación: armed/recording/overdub/wrap-mix tail/finalize
├─ LooperExporter.{h,cpp} → export mix/stems/track, import, resample, snapshot (fuera del header)
├─ MetronomeClick.h     → triggerClick/processClick (hoy mezclado en AudioLooper)
└─ WaveformCache.h      → caché + bins incrementales (QW-6)
```

- **LooperExporter a .cpp:** no es RT, ~500 líneas de header que recompilan a todos los
  consumidores. El snapshot de export deja de apuntar a `data()` crudo: con chunks, el exporter
  itera la PageTable retenida (mismo contrato RCU), o materializa una copia densa por pista
  bajo `ExportGuard` (23 MB transitorios máx., en IO thread — aceptable).
- `emitStateEvents()` + estado last-emitted → clase propia (`LooperStateEmitter`) o dentro del
  dispatcher; AudioLooper::process queda legible: capture → mix → master → click → emit.

### 3.4 Loop X veces + modos de reproducción por pista (P1)

Para mezcla en vivo: una pista debe poder reproducirse N veces y detenerse/mutearse sola.

```cpp
// TrackBuffer
std::atomic<int> mRemainingPlays{-1};   // -1 = infinito (comportamiento actual)
enum class EndBehavior : uint8_t { Stop, Mute, NextQueued };  // fase 2: NextQueued
```

- En el wrap de `mixInto()`: si `mRemainingPlays > 0`, decrementar; al llegar a 0 →
  `setPlaying(false)` + evento `TrackCompleted` por el dispatcher (la UI decide qué sigue).
- JNI: `setTrackPlayCount(track, n)`, evento nuevo en `LooperEventDispatcher`.
- El conteo respeta la loop region (un "play" = una pasada de la región).

### 3.5 Frames a 64 bits en el contrato público (P2)

`int` frames @48k satura a ~12.4 h — no urgente, pero el contrato JNI nuevo
(capabilities, play counts, regiones) debe usar `int64_t`/`jlong` desde el día 1 para no
re-romper la ABI cuando haya pistas largas en gama alta.

---

## 4. Tests

### Nuevos (bloquean el refactor — escribirlos ANTES de 3.1)

1. **`test_audio_looper.cpp`** (gap actual):
   - Budget: prepare hasta agotar → falla limpia; clear → budget recuperado (regresión del bug
     "2º free take falla"); prepare que reemplaza pista existente no cuenta doble.
   - FSM de grabación: armed→trigger→recording→loop-boundary→tail→finalize; abort en cada estado;
     stopRecording durante tail; free-at-cap termina y limpia `mRecordingTrack`.
   - Import: resample 44.1→48 con longitud correcta; budget respetado; pista importada suena
     (regresión del bug `mEnabled`).
   - Export: snapshot inmutable durante overdub (guard); repeatLoops/countIn producen las
     longitudes exactas; cancel a mitad.
2. **`test_chunked_buffer.cpp`** (junto con 3.1):
   - Lectura sobre páginas de silencio = ceros exactos; escritura materializa página.
   - Trim/pad O(páginas) sin mover contenido (verificar punteros de chunks estables).
   - COW undo: overdub toca K páginas → undo restaura bit-exacto; memoria extra = K chunks.
   - Pool: agotamiento del pool durante grabación → frames dropped contados, sin crash ni alloc en RT.
   - Accounting: `allocatedBytes()` == chunks × tamaño, tras cada operación.
3. **Stress RT** (ThreadSanitizer, ya que los tests corren host-side):
   - Hilo "audio" haciendo `mixInto`/`process` en loop vs hilo UI haciendo
     clear/trim/finalizeFreeLoop/import/undo aleatorios. Sin data races ni use-after-free.
4. **Benchmark harness** (no gate de CI, sí métrica de PR):
   - `mixInto` × 8 y × 16 pistas, speed 1.0 y 1.5, con/sin QW-1/QW-2. Presupuesto objetivo:
     16 pistas < 25% del callback de 10 ms en un core mid-range (proxy: host x86 con factor).

### Existentes
- Los tests de TrackBuffer actuales (21 casos) son el contrato de no-regresión del refactor 3.1:
  deben pasar sin modificar aserciones (solo setup si cambia la construcción).

---

## 5. Lineamientos (para todo código nuevo del looper)

1. **Audio thread: cero allocs, cero locks, cero syscalls, cero transcendentales evitables.**
   Toda memoria que el RT path necesite se pre-aloca desde UI/IO (el ChunkPool existe para esto).
2. **Un solo patrón de reclamo de memoria:** publicar por puntero atómico, `mPlaying=false` +
   fence + `waitForRenderIdle()` antes de liberar. Nada de patrones ad-hoc nuevos por método.
3. **Per-sample solo lo que debe ser per-sample.** Ganancias, pan y mute son rampas por bloque.
   Si un PR agrega un cálculo por sample, necesita justificación en el PR.
4. **Todo estado observable sale por el dispatcher (push).** No agregar getters nuevos pensados
   para polling desde Kotlin; agregar el evento.
5. **Header-only solo para código RT o templates.** Export, import, análisis (onsets, bounds) y
   cualquier cosa con IO van a `.cpp`.
6. **Budget accounting exacto:** cualquier alloc/free de audio pasa por el ChunkPool; ninguna
   estructura paralela guarda audio fuera del accounting (el undo COW cuenta sus páginas).
7. **Nuevas APIs JNI del looper usan `int64_t` para frames** y devuelven códigos de error
   distinguibles (budget vs. estado inválido) — NoisyPad necesita mensajes distintos para
   "sin memoria" vs. "pista ocupada".
8. **Cada fix de bug RT trae su test de regresión** en `looper/tests/` (patrón ya establecido
   por test_free_loop_autosync).

---

## 6. Plan de implementación (fases)

| Fase | Contenido | Dep. | Estimación |
|---|---|---|---|
| **F0** | QW-1…QW-7 + benchmark harness (medir antes/después) | — | 2–3 días |
| **F1** | `test_audio_looper.cpp` (red de seguridad) + extracción `LooperExporter.cpp`, `MetronomeClick.h`, `LooperStateEmitter` (3.3 parcial: solo mover, no rediseñar) | F0 | 2–3 días |
| **F2** | `ChunkedAudioBuffer` + integración en TrackBuffer + COW undo + `test_chunked_buffer.cpp` + stress TSan (3.1) | F1 | 5–8 días |
| **F3** | `setCapabilities` runtime (budget/tracks/free-seconds) + `MAX_TRACKS_HW=16` + JNI + evento `RecordProgress`/`TrackCompleted` + `setTrackPlayCount` (3.2, 3.4, QW-5) | F2 | 3–4 días |
| **F4** | Benchmarks finales, CHANGELOG, publish `1.1.0` a GitHub Packages, actualizar NoisyPad | F3 | 1 día |

**Contrato de release:** F0–F1 pueden salir como `1.0.x` (sin cambio de API). F2–F3 son `1.1.0`
(API nueva aditiva; los defaults reproducen el comportamiento actual, así NoisyPad puede
actualizar la dependencia sin adoptar nada todavía).

**Riesgos:**
- F2 es el cambio de mayor riesgo (RT). Mitigación: contrato de tests F1 primero, TSan, y un
  flag de compilación `WM_LOOPER_DENSE_BUFFER` para volver al buffer denso durante la transición.
- El entorno CLI sandbox no puede correr gradlew (loopback); los tests C++ host-side corren con
  CMake/ctest directo — verificar builds nativos desde Android Studio o CI.
