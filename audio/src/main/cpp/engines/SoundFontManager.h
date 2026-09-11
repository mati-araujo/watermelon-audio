#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../platform/Logger.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "tsf.h"
#include "tsf_ext.h"
#include "SoundFontFdRegion.h"
#include "SoundFontModulatorTable.h"

#ifndef SFM_LOG_TAG
#define SFM_LOG_TAG "SF8.Manager"
#endif
#define SFM_LOGI(...) wma::logMessage(wma::LogLevel::INFO, SFM_LOG_TAG, __VA_ARGS__)
#define SFM_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, SFM_LOG_TAG, __VA_ARGS__)

// Darwin has no mmap64/off64_t: there off_t is unconditionally 64-bit, so plain
// mmap already gives what the *64 variants give on the 32-bit Android ABIs.
// See loadFromFd() for why a 64-bit offset is required in the first place.
#if defined(__APPLE__)
#define WMA_MMAP ::mmap
using WmaMapOffset = ::off_t;
#else
#define WMA_MMAP ::mmap64
using WmaMapOffset = ::off64_t;
#endif

/**
 * Lo que el thread de audio toma de una vez: el `tsf` y SU tabla de moduladores
 * (REQ-039 S2). Van juntos bajo el mismo hazard pointer a propósito — dos atómicos
 * separados dejarían una ventana, en cada carga, en la que el audio evalúa los
 * moduladores de OTRO font. Rarísimo, y aun así incorrecto por construcción.
 *
 * Se aloca en el thread de control (carga o re-rate) y se libera en
 * [SoundFontManager::reclaimRetired] cuando el audio ya no lo sostiene.
 */
struct ActiveFont {
    tsf* sf = nullptr;
    wma::sfmod::ModulatorTable modulators;
};

/**
 * @class SoundFontManager
 * @brief Manages SoundFont loading/unloading lifecycle with RT-safe pointer swap
 *
 * Supports three loading methods:
 *   - loadFromPath(): mmap-based, zero-copy — preferred for large files
 *   - loadFromFd():   mmap a sub-region of an fd — for bundled assets (PAD)
 *   - loadFromMemory(): buffer-based — fallback for JNI byte arrays
 *
 * Thread model:
 *   - loadFromPath() / loadFromFd() / loadFromMemory() / unload(): JNI thread (mutex-protected)
 *   - getActiveSF(): audio thread (lock-free atomic load)
 *   - cleanupPending(): JNI thread after audio is paused
 */
class SoundFontManager {
public:
    /**
     * @brief Cached, immutable metadata for a single preset (AUD-4).
     *
     * Populated once at load time from the tsf instance. Reads on the JNI
     * thread hit the cache and never re-scan tsf internals.
     */
    struct PresetInfo {
        std::string name;
        int minKey;
        int maxKey;
        int bank = -1;     // SF2 bank (128 = GM percussion kit)
        int program = -1;  // GM program number (0-127)
    };

    SoundFontManager() = default;

    /**
     * Destruir el manager exige que el hilo de audio YA no esté corriendo — es
     * el mismo contrato de siempre, y el motor lo cumple parando el stream
     * antes. Por eso acá se libera todo lo retirado **sin mirar el hazard
     * pointer**: si quedara algo marcado en uso a esta altura, respetarlo sería
     * filtrarlo para siempre, y el problema real estaría en el orden de apagado.
     */
    ~SoundFontManager() {
        unload();
        std::lock_guard<std::mutex> lock(mLoadMutex);
        for (ActiveFont* p : mRetired) { tsf_close(p->sf); delete p; }
        mRetired.clear();
    }

    /**
     * @brief Load a SoundFont from a file path using mmap (zero-copy)
     * @param path Absolute path to .sf2 file
     * @param sampleRate Audio output sample rate
     * @return true if loading succeeded
     *
     * Uses mmap to map the file into virtual memory without copying.
     * The kernel pages data lazily — only accessed regions use physical RAM.
     * After tsf parses the file, the mmap is released (tsf owns parsed data).
     *
     * Memory savings vs loadFromMemory:
     *   - No Kotlin ByteArray allocation (~30-148 MB saved on JVM heap)
     *   - No JNI byte array copy
     *   - mmap'd region paged lazily by kernel (not all in RAM at once)
     *
     * NOT RT-safe — call from background/JNI thread.
     */
    bool loadFromPath(const char* path, int32_t sampleRate) {
        std::lock_guard<std::mutex> lock(mLoadMutex);

        // Open file
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            SFM_LOGE("[SF8] loadFromPath: failed to open %s (errno=%d)", path, errno);
            return false;
        }

        // Get file size
        struct stat st{};
        if (fstat(fd, &st) < 0) {
            SFM_LOGE("[SF8] loadFromPath: fstat failed (errno=%d)", errno);
            close(fd);
            return false;
        }
        size_t fileSize = static_cast<size_t>(st.st_size);

        // mmap the file — read-only, private (copy-on-write if needed)
        void* mapped = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd); // fd can be closed after mmap

        if (mapped == MAP_FAILED) {
            SFM_LOGE("[SF8] loadFromPath: mmap failed for %zu bytes (errno=%d)", fileSize, errno);
            return false;
        }

        // Advise kernel: we'll read sequentially (improves readahead)
        madvise(mapped, fileSize, MADV_SEQUENTIAL);

        SFM_LOGI("[SF8] loadFromPath: mmap'd %zu bytes from %s", fileSize, path);

        // Parse SF2 from mmap'd memory — tsf copies what it needs internally
        ActiveFont* font = parseFont(mapped, fileSize);

        // Release mmap — tsf has its own copy of parsed data, and so does the
        // modulator table (REQ-039: los bytes no sobreviven a la carga).
        munmap(mapped, fileSize);

        if (!font) {
            SFM_LOGE("[SF8] loadFromPath: tsf_load_memory failed");
            return false;
        }

        return configurAndSwap(font, sampleRate, fileSize);
    }

    /**
     * @brief Load a SoundFont from a sub-region [offset, offset+length) of a
     *        file descriptor, using mmap (zero-copy).
     * @param fd         Open, readable file descriptor. Owned by the CALLER.
     * @param offset     Byte offset of the SoundFont within the fd's file.
     * @param length     Length of the SoundFont region, in bytes (> 0).
     * @param sampleRate Audio output sample rate
     * @return true if loading succeeded
     *
     * Designed for assets shipped inside a Play Asset Delivery install-time
     * pack, which Android exposes only as an AssetFileDescriptor
     * (fd + startOffset + declaredLength) — never a plain path. This maps just
     * the declared region instead of forcing a copy-to-storage first.
     *
     * fd OWNERSHIP: the fd is NOT dup'd, closed, or retained. This call is
     * fully synchronous — it maps the region, lets tsf parse (tsf keeps its own
     * copy of everything it needs), then unmaps before returning. The caller
     * therefore keeps ownership and may close the fd any time after this
     * returns. The fd only needs to stay open for the duration of the call.
     *
     * mmap requires a page-aligned offset; AssetFileDescriptor offsets are not.
     * We align down and map a slightly larger region — see SoundFontFdRegion.h.
     *
     * NOT RT-safe — call from background/JNI thread.
     */
    bool loadFromFd(int fd, int64_t offset, int64_t length, int32_t sampleRate) {
        std::lock_guard<std::mutex> lock(mLoadMutex);

        if (fd < 0) {
            SFM_LOGE("[SF8] loadFromFd: invalid fd=%d", fd);
            return false;
        }

        // Validate the region against the actual file size.
        struct stat st{};
        if (fstat(fd, &st) < 0) {
            SFM_LOGE("[SF8] loadFromFd: fstat failed (fd=%d, errno=%d)", fd, errno);
            return false;
        }

        wma::MmapRegion region{};
        const long pageSize = sysconf(_SC_PAGE_SIZE);
        if (!wma::computeSoundFontMmapRegion(static_cast<int64_t>(st.st_size),
                                             offset, length,
                                             static_cast<int64_t>(pageSize),
                                             region)) {
            SFM_LOGE("[SF8] loadFromFd: region out of range "
                     "(offset=%lld, length=%lld, fileSize=%lld)",
                     static_cast<long long>(offset),
                     static_cast<long long>(length),
                     static_cast<long long>(st.st_size));
            return false;
        }

        // mmap the page-aligned region — read-only, private.
        // A 64-bit offset is mandatory here: off_t is 32-bit on the 32-bit
        // ABIs (armeabi-v7a, x86), which would truncate a large asset offset.
        // loadFromPath is immune (it always maps at offset 0); this path takes
        // an arbitrary offset. WMA_MMAP resolves to mmap64 where that matters
        // and to plain mmap on Darwin, where off_t is already 64-bit.
        void* mapped = WMA_MMAP(nullptr, static_cast<size_t>(region.mapLength),
                                PROT_READ, MAP_PRIVATE, fd,
                                static_cast<WmaMapOffset>(region.alignedOffset));
        if (mapped == MAP_FAILED) {
            SFM_LOGE("[SF8] loadFromFd: mmap failed for %lld bytes at offset %lld (errno=%d)",
                     static_cast<long long>(region.mapLength),
                     static_cast<long long>(region.alignedOffset), errno);
            return false;
        }

        // Advise kernel: we'll read sequentially (improves readahead)
        madvise(mapped, static_cast<size_t>(region.mapLength), MADV_SEQUENTIAL);

        // SF data starts `dataDelta` bytes into the (page-aligned) mapping.
        const uint8_t* sfData =
            static_cast<const uint8_t*>(mapped) + region.dataDelta;

        SFM_LOGI("[SF8] loadFromFd: mmap'd %lld bytes (fd=%d, offset=%lld)",
                 static_cast<long long>(length), fd,
                 static_cast<long long>(offset));

        // Parse SF2/SF3 from the mmap'd memory — tsf copies what it needs.
        ActiveFont* font = parseFont(sfData, static_cast<size_t>(length));

        // Release mmap — tsf has its own copy of parsed data, and so does the
        // modulator table.
        munmap(mapped, static_cast<size_t>(region.mapLength));

        if (!font) {
            SFM_LOGE("[SF8] loadFromFd: tsf_load_memory failed");
            return false;
        }

        return configurAndSwap(font, sampleRate, static_cast<size_t>(length));
    }

    /**
     * @brief Load a SoundFont from a memory buffer (legacy path)
     * @param data Raw .sf2 file data
     * @param size Size in bytes
     * @param sampleRate Audio output sample rate
     * @return true if loading succeeded
     *
     * NOT RT-safe — allocates memory. Call from background/JNI thread.
     */
    bool loadFromMemory(const void* data, int size, int32_t sampleRate) {
        std::lock_guard<std::mutex> lock(mLoadMutex);

        ActiveFont* font = parseFont(data, static_cast<size_t>(size));
        if (!font) {
            SFM_LOGE("[SF8] loadFromMemory: Failed to parse SF2 data (%d bytes)", size);
            return false;
        }

        return configurAndSwap(font, sampleRate, static_cast<size_t>(size));
    }

    /**
     * @brief Unload the current SoundFont
     */
    void unload() {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        mPresetCache.reset();
        publishAndRetire(nullptr);
    }

    /**
     * @brief Publica el `tsf` que se va a usar y lo devuelve. RT-safe.
     *
     * **Es lo que debe llamar el hilo de audio**, no [getActiveSF]. Devuelve el `tsf`
     * JUNTO con su tabla de moduladores (REQ-039): son una sola cosa bajo este hazard. La diferencia
     * no es cosmética: entre leer el puntero y usarlo, el hilo de control puede
     * retirarlo y liberarlo. Acá se publica en [mInUse] y se **re-verifica** que
     * siga siendo el activo; recién entonces el reclamador tiene prohibido
     * liberarlo.
     *
     * ## Por qué `seq_cst` y no acquire/release
     *
     * El reclamador hace lo simétrico: cambia [mActiveSF] y después lee
     * [mInUse]. Con acquire/release, el store de un lado y el load del otro se
     * pueden reordenar y **los dos se pierden mutuamente** (store buffering):
     * el lector no ve el swap y el reclamador no ve el hazard → libera algo en
     * uso. Es el mismo requisito que en el algoritmo de Peterson. Son dos
     * operaciones por bloque de audio, no por muestra.
     *
     * ## Reintentos acotados
     *
     * Un swap entre el load y el re-chequeo obliga a reintentar. Se acota a
     * [kAcquireAttempts] y ante el peor caso se devuelve `nullptr`, o sea **un
     * bloque de silencio**, en vez de spinear sin techo en el hilo de audio. Es
     * aceptable: para que haya swap tuvo que haber una carga o una reapertura de
     * stream, que ya cortan el sonido. En la práctica no reintenta nunca: los
     * swaps son cosa del hilo de control y son rarísimos.
     *
     * Emparejar SIEMPRE con [releaseActive].
     */
    const ActiveFont* acquireActiveFont() {
        for (int attempt = 0; attempt < kAcquireAttempts; ++attempt) {
            ActiveFont* p = mActiveSF.load(std::memory_order_seq_cst);
            mInUse.store(p, std::memory_order_seq_cst);
            if (!p) return nullptr;
            if (mActiveSF.load(std::memory_order_seq_cst) == p) return p;
        }
        mInUse.store(nullptr, std::memory_order_seq_cst);
        return nullptr;
    }


    /** Baja el hazard pointer. RT-safe. Ver [acquireActiveFont]. */
    void releaseActive() {
        mInUse.store(nullptr, std::memory_order_seq_cst);
    }

    /**
     * @brief El `tsf` activo, **para diagnóstico y tests**.
     *
     * NO usar desde el hilo de audio: devuelve un puntero sin protegerlo, así
     * que el reclamador puede liberarlo mientras se lo usa. Para renderizar está
     * [acquireActiveFont]. Sirve para observar *cuál* es el activo (por ejemplo, que
     * un re-rate haya swapeado), no para desreferenciarlo bajo concurrencia.
     */
    tsf* getActiveSF() const {
        const ActiveFont* f = mActiveSF.load(std::memory_order_acquire);
        return f ? f->sf : nullptr;
    }

    /**
     * @brief Libera los fonts retirados que ya nadie esté usando.
     *
     * Se puede llamar en cualquier momento desde el hilo de control; los que
     * sigan en uso quedan para la próxima. La reclamación también ocurre sola en
     * cada retiro, así que llamarla es una optimización, no una obligación.
     */
    void cleanupPending() {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        reclaimRetired();
    }

    bool isLoaded() const {
        return mActiveSF.load(std::memory_order_acquire) != nullptr;
    }

    /**
     * @brief Preset count from the cache (AUD-4).
     *
     * Thread model: read from JNI/main thread under [mLoadMutex]. Returns 0
     * when no SoundFont is loaded.
     */
    int getPresetCount() const {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        return mPresetCache ? static_cast<int>(mPresetCache->size()) : 0;
    }

    /**
     * @brief Preset name from the cache (AUD-4).
     *
     * Thread model: read from JNI/main thread under [mLoadMutex]. The returned
     * pointer is owned by the cache and is valid until the next load/unload.
     * Returns nullptr for out-of-range indices or when no SoundFont is loaded.
     */
    const char* getPresetName(int presetIndex) const {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        if (!mPresetCache || presetIndex < 0 ||
            presetIndex >= static_cast<int>(mPresetCache->size())) {
            return nullptr;
        }
        return (*mPresetCache)[presetIndex].name.c_str();
    }

    int32_t getSampleRate() const { return mSampleRate; }

    /**
     * @brief Re-configure the OUTPUT rate of the already-loaded SoundFont.
     *
     * La tasa de salida se fija con `tsf_set_output()`, y hasta 2026-07-28 eso
     * ocurría en un solo lugar: la carga. Si el stream abría —o REABRÍA— a otra
     * tasa, el font seguía renderizando a la vieja y sonaba desafinado **en
     * silencio**: ni error, ni log. Los dos caminos reales son
     * `AudioEngine::start()` cuando el device fuerza otra tasa que la pedida, y
     * en iOS la reapertura de sesión al pedir captura (que con un manos libres
     * Bluetooth puede caer a HFP, 16 kHz).
     *
     * ## Reemplaza, no muta — y no es una preferencia de estilo
     *
     * El hilo de audio hace `getActiveSF()` una vez por bloque y renderiza con
     * ese puntero hasta el final del bloque. Escribirle `outSampleRate` encima
     * mientras tanto es una carrera sobre un `float` que `tsf_render_float` lee
     * para el pitch de cada voz. Por eso esto usa **la misma disciplina que la
     * carga**: construir aparte, publicar con un swap atómico, y retirar el
     * viejo a `mPendingDelete`.
     *
     * `tsf_copy()` es barato y correcto para esto: comparte presets y muestras
     * por refcount —no re-parsea el .sf2— y `tsf_close()` sólo libera cuando el
     * último se va. Pero devuelve la copia **sin voces** (`voices = NULL`), así
     * que hay que rehacer `tsf_set_max_voices()`; olvidarlo deja un font que
     * carga, reporta presets y tasa correcta, y renderiza silencio.
     *
     * ## Consecuencia aceptada
     *
     * Un cambio de tasa **corta las notas que estuvieran sonando**: la copia
     * viene con las voces en reposo. Es inevitable y no molesta, porque para que
     * la tasa cambie el stream tuvo que reabrirse, cosa que ya interrumpe el
     * audio. Por eso mismo el caso "misma tasa" retorna temprano SIN swapear:
     * `prepare()` corre en cada `start()`, y swapear de gusto convertiría cada
     * arranque en un corte.
     *
     * @param sampleRate nueva tasa de salida, en Hz.
     * @return `false` sólo ante un error real (tasa inválida, fallo de
     *         asignación). Sin SoundFont cargado no hay nada que re-ratear y
     *         retorna `true`: la próxima carga fija la tasa explícitamente.
     *
     * NOT RT-safe — aloca. Llamar desde el hilo de control (lo hace
     * `SoundFontEngine::prepare()`), nunca desde el callback de audio.
     */
    bool setOutputSampleRate(int32_t sampleRate) {
        if (sampleRate < 1) {
            SFM_LOGE("[SF8] setOutputSampleRate: tasa invalida (%d)", sampleRate);
            return false;
        }

        std::lock_guard<std::mutex> lock(mLoadMutex);

        ActiveFont* active = mActiveSF.load(std::memory_order_acquire);
        if (!active) return true;              // nada cargado, nada que re-ratear
        if (mSampleRate == sampleRate) return true;  // idempotente: NO swapear

        tsf* reRated = tsf_copy(active->sf);
        if (!reRated) {
            SFM_LOGE("[SF8] setOutputSampleRate: tsf_copy fallo");
            return false;
        }
        tsf_set_output(reRated, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
        if (!tsf_set_max_voices(reRated, 64)) {
            SFM_LOGE("[SF8] setOutputSampleRate: tsf_set_max_voices fallo");
            tsf_close(reRated);
            return false;
        }

        // La tabla de moduladores es del ARCHIVO, no de la tasa: la copia viaja con
        // el tsf re-rateado. Copiar vectores acá es control, no audio.
        ActiveFont* reRatedFont = new ActiveFont{reRated, active->modulators};

        SFM_LOGI("[SF8] Re-rate del SoundFont: %d -> %d Hz", mSampleRate, sampleRate);
        publishAndRetire(reRatedFont);
        mSampleRate = sampleRate;
        return true;
    }

    /**
     * @brief MIDI key range que el preset DECLARA, del caché (AUD-4, MINI-017).
     *
     * Sale de las **regiones** del preset —el min de `lokey` y el max de `hikey`—,
     * calculado una vez al cargar. Ya NO se infiere del nombre: ver la nota al tope
     * de `SoundFontManager.cpp`.
     *
     * Thread model: se lee desde JNI/main bajo [mLoadMutex]; acá no se toca tsf.
     *
     * @return false —sin tocar los out-params— si el índice no existe **o si el
     *         preset no declara regiones**. Un preset sin regiones no suena en
     *         ninguna tecla: devolver un rango plausible sería indistinguible de
     *         un dato, que es justo el defecto que MINI-017 borró.
     */
    bool getPresetKeyRange(int presetIndex, int& outMinKey, int& outMaxKey) const {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        if (!mPresetCache || presetIndex < 0 ||
            presetIndex >= static_cast<int>(mPresetCache->size())) {
            return false;
        }
        const auto& info = (*mPresetCache)[presetIndex];
        if (info.minKey < 0 || info.maxKey < 0) {
            return false;  // el preset no declaró regiones
        }
        outMinKey = info.minKey;
        outMaxKey = info.maxKey;
        return true;
    }

    /**
     * @brief SF2 bank + GM program for a preset, from the cache.
     *
     * Used for instrument classification (bank 128 = percussion → DrumGrid).
     * Thread model: read from JNI/main thread under [mLoadMutex].
     * @return true if the preset exists and outBank/outProgram were populated.
     */
    bool getPresetBankProgram(int presetIndex, int& outBank, int& outProgram) const {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        if (!mPresetCache || presetIndex < 0 ||
            presetIndex >= static_cast<int>(mPresetCache->size())) {
            return false;
        }
        const auto& info = (*mPresetCache)[presetIndex];
        outBank = info.bank;
        outProgram = info.program;
        return true;
    }

private:
    /**
     * @brief Configure tsf instance and atomically swap to audio thread
     */
    /**
     * `tsf_load_memory` MÁS la tabla de moduladores, sobre los mismos bytes y ANTES de
     * que el llamador los suelte (los tres caminos de carga hacen `munmap` o pierden
     * el buffer justo después). Thread de control. `nullptr` si tsf no pudo parsear.
     */
    ActiveFont* parseFont(const void* data, size_t size) {
        tsf* sf = tsf_load_memory(data, static_cast<int>(size));
        if (!sf) return nullptr;
        ActiveFont* font = new ActiveFont{sf, {}};
        font->modulators.buildFromFontBytes(data, size);
        return font;
    }

    bool configurAndSwap(ActiveFont* font, int32_t sampleRate, size_t fileSize) {
        tsf* newSF = font->sf;
        tsf_set_output(newSF, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
        tsf_set_max_voices(newSF, 64);

        int presetCount = tsf_get_presetcount(newSF);
        SFM_LOGI("[SF8] Loaded SF2 (%zu bytes, %d presets, sr=%d)",
                 fileSize, presetCount, sampleRate);
        // REQ-039: el rechazo de moduladores deja rastro AL CARGAR, no solo en un
        // test. Las dos familias por separado: lo que el spec manda rechazar tiene
        // que ser 0 sobre un font real (AC-039.10); lo fuera de alcance no.
        const auto& rej = font->modulators.rejections();
        SFM_LOGI("[SF8] Moduladores: %u declarados en el archivo; rechazados por spec %u "
                 "(encadenados %u, transform no lineal %u); fuera de alcance de S2 %u "
                 "(fuente de canal %u, destino no aplicado %u)",
                 font->modulators.declaredInFile(), rej.specRejections(), rej.linked,
                 rej.nonLinearTransform, rej.outOfScope(), rej.sourceNotAtNoteOn,
                 rej.destinationUnsupported);

        // Build the immutable preset cache (AUD-4). buildPresetCache must run
        // before the atomic swap so consumers see metadata in lockstep with
        // the active SF — reads on the JNI thread can never observe a window
        // where the SF is published but the cache is still stale.
        auto cache = buildPresetCache(newSF, presetCount);

        // Atomic swap to audio thread
        mPresetCache = std::move(cache);
        publishAndRetire(font);

        mSampleRate = sampleRate;
        return true;
    }

    /**
     * @brief Publish [newSF] to the audio thread and retire the previous one.
     *
     * El swap atómico y el baile de `mPendingDelete`, en un solo lugar. Lo
     * comparten la carga y [setOutputSampleRate]: es la parte delicada y tener
     * dos copias es pedir que diverjan.
     *
     * El viejo NO se cierra acá — el hilo de audio puede estar renderizando el
     * bloque en curso con ese puntero. Se pasa a `mPendingDelete` y lo libera
     * `cleanupPending()` desde el hilo de control.
     *
     * Llamar SIEMPRE con [mLoadMutex] tomado.
     */
    void publishAndRetire(ActiveFont* newFont) {
        // seq_cst y no release: es la mitad escritora del hazard pointer. Ver
        // [acquireActiveFont] para por qué acquire/release no alcanza.
        ActiveFont* old = mActiveSF.exchange(newFont, std::memory_order_seq_cst);
        if (old) mRetired.push_back(old);
        reclaimRetired();
    }

    /**
     * @brief Cierra los retirados que el hilo de audio no esté usando.
     *
     * Llamar SIEMPRE con [mLoadMutex] tomado.
     *
     * Un solo hilo de audio ⇒ un solo hazard pointer que consultar. Si algún día
     * hubiera más de un lector, esto necesita un hazard por lector; está dicho
     * acá porque el error sería silencioso.
     */
    void reclaimRetired() {
        ActiveFont* held = mInUse.load(std::memory_order_seq_cst);
        auto it = mRetired.begin();
        while (it != mRetired.end()) {
            if (*it == held) {
                ++it;  // en uso: la próxima vez
            } else {
                tsf_close((*it)->sf);
                delete *it;
                it = mRetired.erase(it);
            }
        }
    }

    /**
     * @brief Build the immutable preset cache from a freshly parsed tsf.
     * Implemented in SoundFontManager.cpp where the heuristic lives.
     * Called only from configurAndSwap under mLoadMutex.
     */
    static std::shared_ptr<const std::vector<PresetInfo>> buildPresetCache(
        tsf* sf, int presetCount);

    /**
     * Techo de reintentos de [acquireActiveFont]. Un swap entre el load y el
     * re-chequeo obliga a reintentar; 4 es holgadísimo porque los swaps sólo los
     * produce el hilo de control (cargar, descargar, re-ratear) y son rarísimos.
     * Existe para que el hilo de audio no pueda spinear sin techo, nunca porque
     * se espere agotarlo.
     */
    static constexpr int kAcquireAttempts = 4;

    std::atomic<ActiveFont*> mActiveSF{nullptr};

    /**
     * @brief Hazard pointer: el `tsf` que el hilo de audio está usando AHORA.
     *
     * Lo publica [acquireActiveFont] antes de renderizar y lo limpia
     * [releaseActive]. El hilo de control no libera jamás un `tsf` que figure
     * acá. Es lo único que hace segura la reclamación sin que el hilo de audio
     * tome un lock.
     */
    std::atomic<ActiveFont*> mInUse{nullptr};

    /**
     * @brief Fonts retirados, esperando a que nadie los esté usando.
     *
     * Sustituye a la ranura única `mPendingDelete`, que era un
     * **use-after-free**: al tercer swap cerraba en el acto el font retirado dos
     * swaps atrás, sin ninguna garantía de que el hilo de audio ya lo hubiera
     * soltado. Lo destapó el TSan del CI (Linux) con
     * `ReRatingWhileTheAudioThreadRendersIsSafe`; el TSan de macOS no lo veía.
     *
     * Sólo lo tocan el hilo de control y siempre bajo [mLoadMutex], así que un
     * `std::vector` es adecuado — el `push_back` puede alocar y eso ahí está
     * permitido.
     */
    std::vector<ActiveFont*> mRetired;

    mutable std::mutex mLoadMutex;
    // Immutable post-load preset metadata. Replaced wholesale on load/unload
    // while [mLoadMutex] is held. Reads on JNI thread take the mutex briefly
    // to copy/inspect; the pointed-to vector is never mutated in place.
    std::shared_ptr<const std::vector<PresetInfo>> mPresetCache;
    int32_t mSampleRate = 48000;
};
