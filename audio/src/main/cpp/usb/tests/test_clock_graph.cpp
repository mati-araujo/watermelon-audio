// Stage 3 - UsbClockGraph unit tests
//
// Synthetic UAC2 topologies for selector/multiplier clock navigation.

#include <gtest/gtest.h>

#include "../UsbClockGraph.h"
#include "../UsbConstants.h"

using namespace watermelon_audio::usb;

namespace {

UsbClockSource makeSource(uint8_t id, ClockSourceType type) {
    UsbClockSource source;
    source.clockId = id;
    source.type = type;
    source.canControlFrequency = true;
    return source;
}

UsbOutputTerminal makeOutputTerminal(uint8_t terminalId, uint8_t clockNodeId) {
    UsbOutputTerminal terminal;
    terminal.terminalId = terminalId;
    terminal.terminalType = UAC_TERMINAL_SPEAKER;
    terminal.clockSourceId = clockNodeId;
    return terminal;
}

UsbInputTerminal makeInputTerminal(uint8_t terminalId, uint8_t clockNodeId) {
    UsbInputTerminal terminal;
    terminal.terminalId = terminalId;
    terminal.terminalType = UAC_TERMINAL_MICROPHONE;
    terminal.clockSourceId = clockNodeId;
    return terminal;
}

UsbClockSelector makeSelector(uint8_t id, std::vector<uint8_t> sourceIds) {
    UsbClockSelector selector;
    selector.clockId = id;
    selector.sourceIds = std::move(sourceIds);
    selector.canControlSelector = true;
    return selector;
}

UsbClockMultiplier makeMultiplier(uint8_t id, uint8_t sourceId) {
    UsbClockMultiplier multiplier;
    multiplier.clockId = id;
    multiplier.sourceId = sourceId;
    return multiplier;
}

UsbAudioDevice makeUac2Device() {
    UsbAudioDevice device;
    device.uacVersion = 2;
    return device;
}

}  // namespace

TEST(UsbClockGraph, ReachableSourcesReturnsAllSelectorInputs) {
    auto device = makeUac2Device();
    device.clockSources.push_back(makeSource(10, ClockSourceType::INTERNAL_FIXED));
    device.clockSources.push_back(makeSource(11, ClockSourceType::EXTERNAL));
    device.clockSelectors.push_back(makeSelector(20, {10, 11}));
    device.outputTerminals.push_back(makeOutputTerminal(3, 20));

    UsbClockGraph graph(device);
    auto sources = graph.reachableSourcesFor(3);

    ASSERT_EQ(sources.size(), 2u);
    EXPECT_EQ(sources[0]->clockId, 10);
    EXPECT_EQ(sources[1]->clockId, 11);
    EXPECT_TRUE(graph.requiresSelectorConfiguration(3));
}

TEST(UsbClockGraph, ResolvePathReturnsDirectSource) {
    auto device = makeUac2Device();
    device.clockSources.push_back(makeSource(30, ClockSourceType::INTERNAL_PROGRAMMABLE));
    device.outputTerminals.push_back(makeOutputTerminal(4, 30));

    UsbClockGraph graph(device);
    auto path = graph.resolvePath(4);

    ASSERT_TRUE(path.has_value());
    ASSERT_NE(path->source, nullptr);
    EXPECT_EQ(path->source->clockId, 30);
    EXPECT_EQ(path->selector, nullptr);
    EXPECT_EQ(path->multiplier, nullptr);
    EXPECT_FALSE(graph.requiresSelectorConfiguration(4));
}

TEST(UsbClockGraph, ResolvePathFollowsMultiplier) {
    auto device = makeUac2Device();
    device.clockSources.push_back(makeSource(40, ClockSourceType::INTERNAL_VARIABLE));
    device.clockMultipliers.push_back(makeMultiplier(41, 40));
    device.inputTerminals.push_back(makeInputTerminal(5, 41));

    UsbClockGraph graph(device);
    auto path = graph.resolvePath(5);

    ASSERT_TRUE(path.has_value());
    ASSERT_NE(path->source, nullptr);
    ASSERT_NE(path->multiplier, nullptr);
    EXPECT_EQ(path->source->clockId, 40);
    EXPECT_EQ(path->multiplier->clockId, 41);
}

TEST(UsbClockGraph, PathToSourceTracksSelectorPinThroughMultiplier) {
    auto device = makeUac2Device();
    device.clockSources.push_back(makeSource(50, ClockSourceType::EXTERNAL));
    device.clockSources.push_back(makeSource(51, ClockSourceType::INTERNAL_PROGRAMMABLE));
    device.clockSelectors.push_back(makeSelector(52, {50, 51}));
    device.clockMultipliers.push_back(makeMultiplier(53, 52));
    device.outputTerminals.push_back(makeOutputTerminal(6, 53));

    UsbClockGraph graph(device);
    auto path = graph.pathToSource(6, 51);

    ASSERT_TRUE(path.has_value());
    ASSERT_NE(path->source, nullptr);
    ASSERT_NE(path->selector, nullptr);
    ASSERT_NE(path->multiplier, nullptr);
    EXPECT_EQ(path->source->clockId, 51);
    EXPECT_EQ(path->selector->clockId, 52);
    EXPECT_EQ(path->selectorPin, 2);
    EXPECT_EQ(path->multiplier->clockId, 53);
}

TEST(UsbClockGraph, PickDefaultSourcePrefersInternalProgrammable) {
    auto device = makeUac2Device();
    device.clockSources.push_back(makeSource(60, ClockSourceType::EXTERNAL));
    device.clockSources.push_back(makeSource(61, ClockSourceType::INTERNAL_FIXED));
    device.clockSources.push_back(makeSource(62, ClockSourceType::INTERNAL_PROGRAMMABLE));
    device.clockSources.push_back(makeSource(63, ClockSourceType::INTERNAL_VARIABLE));
    device.clockSelectors.push_back(makeSelector(64, {60, 61, 62, 63}));
    device.outputTerminals.push_back(makeOutputTerminal(7, 64));

    UsbClockGraph graph(device);
    const auto* source = graph.pickDefaultSource(7);

    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->clockId, 62);
}

TEST(UsbClockGraph, ResolvePathIsAmbiguousForMultiSourceSelector) {
    auto device = makeUac2Device();
    device.clockSources.push_back(makeSource(70, ClockSourceType::INTERNAL_FIXED));
    device.clockSources.push_back(makeSource(71, ClockSourceType::INTERNAL_VARIABLE));
    device.clockSelectors.push_back(makeSelector(72, {70, 71}));
    device.outputTerminals.push_back(makeOutputTerminal(8, 72));

    UsbClockGraph graph(device);
    EXPECT_FALSE(graph.resolvePath(8).has_value());
}
