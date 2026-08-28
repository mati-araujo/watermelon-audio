// Validates Transport state: BPM/beats-per-bar clamping, frames-per-beat math,
// metronome scheduling arming, play-frame counter accounting, and the
// nextBarBoundary quantizer used by armed recording.
//
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <gtest/gtest.h>
#include "tests/support/TestWait.h"
#include "Transport.h"
#include "AudioLooper.h"

TEST(Transport, BpmClampedToRange) {
    Transport t;
    t.setBpm(5.0f);
    EXPECT_FLOAT_EQ(t.getBpm(), Transport::MIN_BPM);
    t.setBpm(1000.0f);
    EXPECT_FLOAT_EQ(t.getBpm(), Transport::MAX_BPM);
    t.setBpm(140.0f);
    EXPECT_FLOAT_EQ(t.getBpm(), 140.0f);
}

TEST(Transport, BeatsPerBarClampedToRange) {
    Transport t;
    t.setBeatsPerBar(0);
    EXPECT_EQ(t.getBeatsPerBar(), Transport::MIN_BEATS_PER_BAR);
    t.setBeatsPerBar(99);
    EXPECT_EQ(t.getBeatsPerBar(), Transport::MAX_BEATS_PER_BAR);
    t.setBeatsPerBar(3);
    EXPECT_EQ(t.getBeatsPerBar(), 3);
}

TEST(Transport, FramesPerBeatMathAtCommonRates) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    // 60/120 * 48000 = 24000 frames per beat
    EXPECT_EQ(t.framesPerBeat(), 24000);

    t.setBpm(60.0f);
    EXPECT_EQ(t.framesPerBeat(), 48000);

    t.setSampleRate(44100);
    t.setBpm(120.0f);
    // 60/120 * 44100 = 22050
    EXPECT_EQ(t.framesPerBeat(), 22050);
}

TEST(Transport, FramesPerBarHonoursBeatsPerBar) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    t.setBeatsPerBar(4);
    EXPECT_EQ(t.framesPerBar(1), 24000 * 4);
    EXPECT_EQ(t.framesPerBar(2), 24000 * 4 * 2);
    EXPECT_EQ(t.framesPerBar(0), 0);

    t.setBeatsPerBar(3);
    EXPECT_EQ(t.framesPerBar(1), 24000 * 3);
}

TEST(Transport, MetronomeArmsAndStops) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);

    EXPECT_FALSE(t.isMetronomeRunning());
    t.startMetronome(4);
    EXPECT_TRUE(t.isMetronomeRunning());
    EXPECT_EQ(t.getRemainingBeats(), 4);

    t.stopMetronome();
    EXPECT_FALSE(t.isMetronomeRunning());
    EXPECT_EQ(t.getRemainingBeats(), 0);
}

TEST(Transport, MetronomeIgnoresNonPositiveBeats) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    t.startMetronome(0);
    EXPECT_FALSE(t.isMetronomeRunning());
    t.startMetronome(-3);
    EXPECT_FALSE(t.isMetronomeRunning());
}

TEST(Transport, PlayFrameAdvancesOnTick) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    AudioLooper looper;  // tick requires a real looper for triggerClick

    EXPECT_EQ(t.getPlayFrame(), 0);
    t.tick(480, looper);
    EXPECT_EQ(t.getPlayFrame(), 480);
    t.tick(192, looper);
    EXPECT_EQ(t.getPlayFrame(), 672);

    t.resetPlayPosition();
    EXPECT_EQ(t.getPlayFrame(), 0);
}

TEST(Transport, ScheduledMetronomeRendersClickWhenLooperDisabled) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);

    AudioLooper looper;
    looper.setSampleRate(48000);
    looper.setEnabled(false);

    std::array<float, 256> buffer{};
    t.startMetronome(4);
    t.tick(128, looper);
    looper.process(buffer.data(), 128);

    EXPECT_TRUE(std::any_of(buffer.begin(), buffer.end(), [](float sample) {
        return sample != 0.0f;
    }));
    EXPECT_EQ(t.getRemainingBeats(), 3);
}

TEST(AudioLooper, DirectClickRendersWhenDisabled) {
    AudioLooper looper;
    looper.setSampleRate(48000);
    looper.setEnabled(false);

    std::array<float, 256> buffer{};
    looper.triggerClick(true);
    looper.process(buffer.data(), 128);

    EXPECT_TRUE(std::any_of(buffer.begin(), buffer.end(), [](float sample) {
        return sample != 0.0f;
    }));
}

TEST(AudioLooper, ClickIsRenderedAfterRecordingTapWhenEnabled) {
    AudioLooper looper;
    looper.setSampleRate(48000);
    ASSERT_TRUE(looper.prepareTrack(0, 512, 48000));
    looper.startRecording(0);

    std::array<float, 256> buffer{};
    looper.triggerClick(true);
    looper.process(buffer.data(), 128);

    EXPECT_TRUE(std::any_of(buffer.begin(), buffer.end(), [](float sample) {
        return sample != 0.0f;
    }));

    // The click is rendered to the OUTPUT but never captured into the track — the
    // recorded buffer stays silent. Read via sampleAt() so this holds for both the
    // dense and the paged backend (buffer.size() floats = buffer.size()/2 frames).
    const TrackBuffer& recorded = looper.getTrack(0);
    for (int f = 0; f < static_cast<int>(buffer.size()) / 2; ++f) {
        EXPECT_FLOAT_EQ(recorded.sampleAt(f, 0), 0.0f);
        EXPECT_FLOAT_EQ(recorded.sampleAt(f, 1), 0.0f);
    }
}

TEST(Transport, NextBarBoundaryQuantizes) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    t.setBeatsPerBar(4);
    const int64_t fpb = t.framesPerBar(1);  // 96000

    // Already on a boundary → returns same frame.
    EXPECT_EQ(t.nextBarBoundary(0), 0);
    EXPECT_EQ(t.nextBarBoundary(fpb), fpb);
    EXPECT_EQ(t.nextBarBoundary(fpb * 2), fpb * 2);

    // Inside a bar → next multiple.
    EXPECT_EQ(t.nextBarBoundary(1), fpb);
    EXPECT_EQ(t.nextBarBoundary(fpb - 1), fpb);
    EXPECT_EQ(t.nextBarBoundary(fpb + 1), fpb * 2);
}

// ===========================================================================
// REQ-017 — el beat co-emitido con el click
//
// POR QUE EL AC ES CO-EMISION Y NO "EL FRAME EXACTO"
// --------------------------------------------------
// El click MISMO esta cuantizado al bloque, por diseño y documentado arriba en
// `tick()`. Un evento de beat sample-exacto seria MAS preciso que el click al
// que tiene que estar clavado: pediria una precision que el referente no tiene.
// Lo que se verifica entonces es que el evento sale de la MISMA decision que el
// click — con eso es exactamente tan preciso como el, por construccion.
// ===========================================================================

namespace {

/** Junta los eventos de beat que el worker despacha, en orden. */
struct BeatCollector {
    wm::LooperEventDispatcher dispatcher;
    std::mutex mutex;
    std::vector<wm::LooperEvent> beats;

    void attachTo(AudioLooper& looper) {
        dispatcher.setSink([this](const wm::LooperEvent& ev) {
            if (ev.type == wm::LooperEvent::Type::TrackCompleted) {
                sentinel.fetch_add(1, std::memory_order_release);
                return;
            }
            if (ev.type != wm::LooperEvent::Type::Beat) return;
            std::lock_guard<std::mutex> lk(mutex);
            beats.push_back(ev);
        });
        dispatcher.start();
        looper.setEventDispatcher(&dispatcher);
    }

    size_t count() {
        std::lock_guard<std::mutex> lk(mutex);
        return beats.size();
    }

    /** Espera POR CONDICION (nunca por reloj de pared) y despega el sink. */
    bool settle(AudioLooper& looper, size_t expected) {
        const bool arrived = wma_test::waitUntil([this, expected] {
            return count() >= expected;
        });
        looper.setEventDispatcher(nullptr);
        dispatcher.stop();
        return arrived;
    }

    /**
     * Cierra probando una AUSENCIA, sin esperar al techo. Empuja un centinela
     * DESPUES de todo lo que el test produjo y espera a que ese llegue: la cola
     * es FIFO y la drena un solo worker, asi que si el centinela salio, todo lo
     * anterior ya se despacho. Esperar el timeout completo tambien seria
     * correcto, pero paga 2 s por test para probar que no paso nada.
     */
    bool settleEmpty(AudioLooper& looper) {
        dispatcher.pushFromRT(
            wm::LooperEvent{wm::LooperEvent::Type::TrackCompleted, 0, 0.0f});
        const bool arrived = wma_test::waitUntil([this] {
            return sentinel.load(std::memory_order_acquire) > 0;
        });
        looper.setEventDispatcher(nullptr);
        dispatcher.stop();
        return arrived;
    }

    std::atomic<int> sentinel{0};
};

}  // namespace

// AC-017.1 + AC-017.3 + AC-017.5 — un evento por click, indice monotono y sin
// huecos, y NINGUN descarte (el contador se assertea, no se supone).
TEST(Transport, BeatEventCoEmittedOncePerClickWithMonotonicIndex) {
    constexpr int kBlock = 512;
    constexpr int kBeats = 4;

    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);            // framesPerBeat = 24000
    AudioLooper looper;
    looper.setSampleRate(48000);

    BeatCollector collector;
    collector.attachTo(looper);

    t.startMetronome(kBeats);
    // 4 beats a 24000 frames = 72000 frames; con margen de sobra.
    for (int i = 0; i < 200; ++i) t.tick(kBlock, looper);

    ASSERT_TRUE(collector.settle(looper, kBeats))
        << "los eventos de beat nunca llegaron al worker";

    ASSERT_EQ(collector.beats.size(), static_cast<size_t>(kBeats))
        << "un evento por click: ni de mas (el bucle emitiria dos veces) "
           "ni de menos (la emision quedaria afuera del bucle)";
    for (int k = 0; k < kBeats; ++k) {
        EXPECT_FLOAT_EQ(collector.beats[static_cast<size_t>(k)].value,
                        static_cast<float>(k))
            << "indice de beat monotono y sin huecos, en k=" << k;
    }
    EXPECT_EQ(collector.dispatcher.getDroppedEvents(), 0);
}

// AC-017.4 — el ancla apunta al PROXIMO beat, verificado contra un oraculo
// INDEPENDIENTE de la emision: `getRemainingBeats()` es una via de PULL que ya
// existia y que baja cuando el scheduler dispara un click. Si el ancla se
// derivara del mismo calculo que se quiere probar, el test seria un espejo.
TEST(Transport, BeatAnchorLandsInTheBlockWhereTheNextClickFires) {
    constexpr int kBlock = 512;
    constexpr int kBeats = 4;

    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    AudioLooper looper;
    looper.setSampleRate(48000);

    BeatCollector collector;
    collector.attachTo(looper);

    t.startMetronome(kBeats);

    // Frame de FIN del bloque en el que cada click disparo de verdad.
    std::vector<int64_t> firedBlockEnd;
    int prevRemaining = t.getRemainingBeats();
    for (int i = 0; i < 300 && firedBlockEnd.size() < static_cast<size_t>(kBeats); ++i) {
        t.tick(kBlock, looper);
        const int now = t.getRemainingBeats();
        if (now < prevRemaining) firedBlockEnd.push_back(t.getPlayFrame());
        prevRemaining = now;
    }
    ASSERT_EQ(firedBlockEnd.size(), static_cast<size_t>(kBeats));

    ASSERT_TRUE(collector.settle(looper, kBeats));
    ASSERT_EQ(collector.beats.size(), static_cast<size_t>(kBeats));
    EXPECT_EQ(collector.dispatcher.getDroppedEvents(), 0);

    for (int k = 0; k + 1 < kBeats; ++k) {
        const int64_t anchor = collector.beats[static_cast<size_t>(k)].trackIndex;
        const int64_t nextEnd = firedBlockEnd[static_cast<size_t>(k) + 1];
        EXPECT_GE(anchor, nextEnd - kBlock)
            << "el ancla del beat " << k << " cae ANTES del bloque donde disparo "
               "el beat " << (k + 1);
        EXPECT_LT(anchor, nextEnd)
            << "el ancla del beat " << k << " cae DESPUES del bloque donde disparo "
               "el beat " << (k + 1);
    }

    // Y la forma cerrada: el beat 0 dispara en el frame 0 (startMetronome pone
    // framesUntilNextClick=0), asi que el ancla del beat k es (k+1)*framesPerBeat.
    for (int k = 0; k < kBeats; ++k) {
        EXPECT_EQ(collector.beats[static_cast<size_t>(k)].trackIndex,
                  static_cast<int32_t>((k + 1) * 24000))
            << "ancla exacta del beat " << k;
    }
}

// AC-017.2 — con el metronomo detenido no hay beat, aunque el contador de play
// frames siga avanzando. Es una decision declarada: manda el tren de clicks.
// Derivar de playFrame/framesPerBeat daria pulso siempre, pero dejaria de ser el
// beat que se escucha en el primer cambio de BPM en vuelo.
TEST(Transport, NoBeatEventsWhileMetronomeIsStopped) {
    constexpr int kBlock = 512;

    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    AudioLooper looper;
    looper.setSampleRate(48000);

    BeatCollector collector;
    collector.attachTo(looper);

    // Sin startMetronome: el scheduler nunca se arma.
    for (int i = 0; i < 200; ++i) t.tick(kBlock, looper);

    EXPECT_GT(t.getPlayFrame(), 0) << "el contador de play frames SI tiene que avanzar";
    ASSERT_TRUE(collector.settleEmpty(looper)) << "el centinela nunca llego";
    EXPECT_EQ(collector.beats.size(), 0u);
    EXPECT_EQ(collector.dispatcher.getDroppedEvents(), 0);
}

// AC-017.1, el borde que ya mordio una vez. Con framesPerBeat multiplo exacto
// del bloque (120 BPM @48 kHz = 24000; 24000/192 = 125) el tren de clicks ya se
// corrio un bloque entero una vez — es lo que documenta el comentario de
// `next < 0` en tick(). El beat no puede reintroducir esa clase.
TEST(Transport, BeatSurvivesFramesPerBeatBeingAMultipleOfTheBlock) {
    constexpr int kBlock = 192;    // 24000 / 192 = 125, exacto
    constexpr int kBeats = 3;

    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);
    AudioLooper looper;
    looper.setSampleRate(48000);

    BeatCollector collector;
    collector.attachTo(looper);

    t.startMetronome(kBeats);
    for (int i = 0; i < 400; ++i) t.tick(kBlock, looper);

    ASSERT_TRUE(collector.settle(looper, kBeats));
    ASSERT_EQ(collector.beats.size(), static_cast<size_t>(kBeats));
    for (int k = 0; k < kBeats; ++k) {
        EXPECT_FLOAT_EQ(collector.beats[static_cast<size_t>(k)].value,
                        static_cast<float>(k));
        EXPECT_EQ(collector.beats[static_cast<size_t>(k)].trackIndex,
                  static_cast<int32_t>((k + 1) * 24000))
            << "con fpb multiplo del bloque el ancla se corre, en k=" << k;
    }
    EXPECT_EQ(collector.dispatcher.getDroppedEvents(), 0);
}

// H5 — `triggerClick()` esta declarado invocable desde el thread de UI y tiene un
// llamador que NO viene de la grilla (`wma_looper_trigger_click`). Si la emision
// estuviera plegada adentro de triggerClick, ese camino emitiria un beat FANTASMA,
// sin indice valido y sin ancla.
TEST(AudioLooper, DirectClickFromUiThreadEmitsNoBeat) {
    AudioLooper looper;
    looper.setSampleRate(48000);

    BeatCollector collector;
    collector.attachTo(looper);

    for (int i = 0; i < 8; ++i) looper.triggerClick(i == 0);

    ASSERT_TRUE(collector.settleEmpty(looper)) << "el centinela nunca llego";
    EXPECT_EQ(collector.beats.size(), 0u)
        << "un click suelto de UI no es un beat de la grilla";
}

// ============================================================================
// REQ-020 — el observable que distingue ARMADO de SONANDO.
//
// Los issues #228 y #229 se veian identicos desde afuera porque ninguna de las
// tres vias de PULL que ya existian contesta "?la grilla esta avanzando?":
// `isMetronomeRunning()` dice armado, `getRemainingBeats()` es un centinela fijo
// en modo continuo, y `getPlayFrame()` avanza aunque el metronomo este parado.
// ============================================================================

// AC-020.1 — `getBeatsElapsed()` cuenta los beats que la grilla EMITIO, y el
// oraculo es INDEPENDIENTE del contador: son los eventos `Beat` que llegaron al
// worker por el dispatcher. Si el getter leyera cualquier otro atomico —o una
// constante— los dos numeros dejarian de coincidir.
TEST(Transport, BeatsElapsedCountsExactlyTheBeatsTheGridEmitted) {
    constexpr int kBlock = 512;
    constexpr int kBeats = 4;

    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);            // framesPerBeat = 24000
    AudioLooper looper;
    looper.setSampleRate(48000);

    BeatCollector collector;
    collector.attachTo(looper);

    EXPECT_EQ(t.getBeatsElapsed(), 0) << "recien armado no hay pulso emitido";

    t.startMetronome(kBeats);
    for (int i = 0; i < 200; ++i) t.tick(kBlock, looper);

    ASSERT_TRUE(collector.settle(looper, kBeats));
    ASSERT_EQ(collector.beats.size(), static_cast<size_t>(kBeats));
    EXPECT_EQ(collector.dispatcher.getDroppedEvents(), 0);

    EXPECT_EQ(t.getBeatsElapsed(), kBeats)
        << "el contador tiene que valer lo MISMO que la cantidad de eventos Beat "
           "empujados: los dos salen de la misma iteracion del mismo bucle";
}

// AC-020.2 — EL GEMELO, y el que hace util al par. Sin este test, un getter que
// devolviera `mBeatsRemaining` pasaria el de arriba en modo finito y seguiria sin
// distinguir armado de sonando, que es el defecto entero de #229.
TEST(Transport, ArmedWithoutTickingIsRunningButNoBeatHasElapsed) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);

    t.startMetronome(4);

    // NADIE tickea: es exactamente la condicion de #229 — el render apagado.
    EXPECT_TRUE(t.isMetronomeRunning())
        << "control positivo: la query vieja SIGUE diciendo que corre";
    EXPECT_EQ(t.getBeatsElapsed(), 0)
        << "y la nueva dice la verdad: no sono un solo beat";

    // Y en modo continuo, donde `getRemainingBeats()` es el centinela fijo `1`,
    // el par es la UNICA via que discrimina.
    t.startMetronomeContinuous(true);
    EXPECT_TRUE(t.isMetronomeRunning());
    EXPECT_EQ(t.getRemainingBeats(), 1) << "el centinela, que no dice nada";
    EXPECT_EQ(t.getBeatsElapsed(), 0);
}

// AC-020.3 — el discriminador de #228, anclado. El argumento del issue era "el
// click suena, y triggerClick/emitBeat estan en la misma iteracion, asi que
// emitBeat se llamo". El eslabon es FALSO: `triggerClick` tiene un segundo
// llamador que no viene de la grilla (`wma_looper_trigger_click`, expuesto a
// Kotlin como `looperTriggerClick()`). Este test fija que ese camino deja el
// contador quieto, o sea que (click audible + elapsed == 0) es un veredicto.
TEST(Transport, DirectClickFromUiThreadDoesNotAdvanceBeatsElapsed) {
    Transport t;
    t.setSampleRate(48000);
    t.setBpm(120.0f);

    AudioLooper looper;
    looper.setSampleRate(48000);
    looper.setEnabled(false);

    std::array<float, 256> buffer{};
    for (int i = 0; i < 8; ++i) looper.triggerClick(i == 0);
    looper.process(buffer.data(), 128);

    // Control positivo: el click SUENA de verdad por este camino — sin esto, el
    // EXPECT_EQ de abajo seria verde por construccion (nada paso nunca).
    ASSERT_TRUE(std::any_of(buffer.begin(), buffer.end(), [](float sample) {
        return sample != 0.0f;
    })) << "el click suelto tiene que sonar: es la mitad audible del sintoma";

    EXPECT_EQ(t.getBeatsElapsed(), 0)
        << "un click que no viene de la grilla no es un beat, y el contador no miente";
}
