/**
 * UsbClockGraph.h
 *
 * Minimal UAC 2.0 clock-topology resolver.
 *
 * Terminals point at a bCSourceID, which may be a Clock Source directly or a
 * Clock Selector / Clock Multiplier that must be followed to reach the final
 * source that accepts CS_SAM_FREQ_CONTROL.
 */

#pragma once

#include "UsbAudioTypes.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace watermelon_audio::usb {

class UsbClockGraph {
public:
    struct ActivePath {
        uint8_t terminalId = 0;
        const UsbClockSelector* selector = nullptr;
        uint8_t selectorPin = 0;  // 1-based UAC2 pin index when selector != nullptr
        const UsbClockMultiplier* multiplier = nullptr;
        const UsbClockSource* source = nullptr;
    };

    explicit UsbClockGraph(const UsbAudioDevice& topology)
        : mTopology(topology) {
        buildGraph();
    }

    std::vector<const UsbClockSource*> reachableSourcesFor(uint8_t terminalId) const {
        std::vector<const UsbClockSource*> sources;
        std::unordered_set<uint8_t> visited;
        collectSources(clockNodeForTerminal(terminalId), visited, sources);
        return sources;
    }

    std::optional<ActivePath> resolvePath(uint8_t terminalId) const {
        const uint8_t start = clockNodeForTerminal(terminalId);
        if (start == 0) return std::nullopt;

        std::unordered_set<uint8_t> visited;
        std::vector<ActivePath> paths;
        ActivePath current;
        current.terminalId = terminalId;
        collectPaths(start, visited, current, paths);

        if (paths.size() != 1 || paths[0].source == nullptr) {
            return std::nullopt;
        }
        return paths[0];
    }

    std::optional<ActivePath> pathToSource(uint8_t terminalId, uint8_t clockSourceId) const {
        const uint8_t start = clockNodeForTerminal(terminalId);
        if (start == 0) return std::nullopt;

        std::unordered_set<uint8_t> visited;
        std::vector<ActivePath> paths;
        ActivePath current;
        current.terminalId = terminalId;
        collectPaths(start, visited, current, paths);

        auto it = std::find_if(paths.begin(), paths.end(),
            [clockSourceId](const ActivePath& path) {
                return path.source && path.source->clockId == clockSourceId;
            });
        if (it == paths.end()) {
            return std::nullopt;
        }
        return *it;
    }

    bool requiresSelectorConfiguration(uint8_t terminalId) const {
        auto sources = reachableSourcesFor(terminalId);
        return sources.size() > 1 && firstSelectorFor(clockNodeForTerminal(terminalId)) != nullptr;
    }

    const UsbClockSource* pickDefaultSource(uint8_t terminalId) const {
        auto sources = reachableSourcesFor(terminalId);
        if (sources.empty()) return nullptr;

        auto score = [](const UsbClockSource* source) {
            if (!source) return -1;
            switch (source->type) {
                case ClockSourceType::INTERNAL_PROGRAMMABLE: return 40;
                case ClockSourceType::INTERNAL_VARIABLE: return 30;
                case ClockSourceType::INTERNAL_FIXED: return 20;
                case ClockSourceType::EXTERNAL: return 10;
            }
            return 0;
        };

        return *std::max_element(sources.begin(), sources.end(),
            [&](const UsbClockSource* lhs, const UsbClockSource* rhs) {
                const int lhsScore = score(lhs);
                const int rhsScore = score(rhs);
                if (lhsScore != rhsScore) return lhsScore < rhsScore;
                return lhs->clockId > rhs->clockId;  // deterministic lower-id tie break
            });
    }

private:
    const UsbAudioDevice& mTopology;
    std::unordered_map<uint8_t, const UsbClockSource*> mSourceById;
    std::unordered_map<uint8_t, const UsbClockSelector*> mSelectorById;
    std::unordered_map<uint8_t, const UsbClockMultiplier*> mMultiplierById;

    void buildGraph() {
        for (const auto& source : mTopology.clockSources) {
            mSourceById[source.clockId] = &source;
        }
        for (const auto& selector : mTopology.clockSelectors) {
            mSelectorById[selector.clockId] = &selector;
        }
        for (const auto& multiplier : mTopology.clockMultipliers) {
            mMultiplierById[multiplier.clockId] = &multiplier;
        }
    }

    uint8_t clockNodeForTerminal(uint8_t terminalId) const {
        return mTopology.resolveClockSourceId(terminalId);
    }

    void collectSources(
        uint8_t nodeId,
        std::unordered_set<uint8_t>& visited,
        std::vector<const UsbClockSource*>& out) const {
        if (nodeId == 0 || visited.count(nodeId) != 0) return;
        visited.insert(nodeId);

        auto sourceIt = mSourceById.find(nodeId);
        if (sourceIt != mSourceById.end()) {
            if (std::find(out.begin(), out.end(), sourceIt->second) == out.end()) {
                out.push_back(sourceIt->second);
            }
            return;
        }

        auto selectorIt = mSelectorById.find(nodeId);
        if (selectorIt != mSelectorById.end()) {
            for (uint8_t sourceId : selectorIt->second->sourceIds) {
                collectSources(sourceId, visited, out);
            }
            return;
        }

        auto multiplierIt = mMultiplierById.find(nodeId);
        if (multiplierIt != mMultiplierById.end()) {
            collectSources(multiplierIt->second->sourceId, visited, out);
        }
    }

    void collectPaths(
        uint8_t nodeId,
        std::unordered_set<uint8_t> visited,
        ActivePath current,
        std::vector<ActivePath>& out) const {
        if (nodeId == 0 || visited.count(nodeId) != 0) return;
        visited.insert(nodeId);

        auto sourceIt = mSourceById.find(nodeId);
        if (sourceIt != mSourceById.end()) {
            current.source = sourceIt->second;
            out.push_back(current);
            return;
        }

        auto multiplierIt = mMultiplierById.find(nodeId);
        if (multiplierIt != mMultiplierById.end()) {
            ActivePath next = current;
            if (!next.multiplier) next.multiplier = multiplierIt->second;
            collectPaths(multiplierIt->second->sourceId, visited, next, out);
            return;
        }

        auto selectorIt = mSelectorById.find(nodeId);
        if (selectorIt != mSelectorById.end()) {
            const UsbClockSelector* selector = selectorIt->second;
            for (size_t i = 0; i < selector->sourceIds.size(); ++i) {
                ActivePath next = current;
                if (!next.selector) {
                    next.selector = selector;
                    next.selectorPin = static_cast<uint8_t>(i + 1);
                }
                collectPaths(selector->sourceIds[i], visited, next, out);
            }
        }
    }

    const UsbClockSelector* firstSelectorFor(uint8_t nodeId) const {
        std::unordered_set<uint8_t> visited;
        return firstSelectorFor(nodeId, visited);
    }

    const UsbClockSelector* firstSelectorFor(
        uint8_t nodeId,
        std::unordered_set<uint8_t>& visited) const {
        if (nodeId == 0 || visited.count(nodeId) != 0) return nullptr;
        visited.insert(nodeId);

        auto selectorIt = mSelectorById.find(nodeId);
        if (selectorIt != mSelectorById.end()) return selectorIt->second;

        auto multiplierIt = mMultiplierById.find(nodeId);
        if (multiplierIt != mMultiplierById.end()) {
            return firstSelectorFor(multiplierIt->second->sourceId, visited);
        }
        return nullptr;
    }
};

}  // namespace watermelon_audio::usb
