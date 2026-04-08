#pragma once

/**
 * @file EffectRegistry.h
 * @brief Dynamic registry for audio effect factories.
 *
 * Replaces the hardcoded switch statement in EffectChain::addEffect().
 * New effects can be registered at runtime without modifying EffectChain.
 *
 * Phase 1F — Audio Library Extraction.
 */

#include "Effect.h"
#include "EffectTypes.h"
#include <functional>
#include <unordered_map>
#include <memory>
#include <vector>

class EffectRegistry {
public:
    using EffectFactory = std::function<std::unique_ptr<Effect>()>;

    /**
     * Register a factory for an effect type.
     * @param type Effect type ID
     * @param name Human-readable name
     * @param numParams Number of parameters
     * @param factory Function that creates a new instance
     */
    void registerEffect(EffectType type, const char* name, int numParams, EffectFactory factory) {
        mFactories[type] = {name, numParams, std::move(factory)};
    }

    /**
     * Create an effect instance by type.
     * @return New effect or nullptr if type is not registered.
     */
    std::unique_ptr<Effect> createEffect(EffectType type) const {
        auto it = mFactories.find(type);
        if (it == mFactories.end()) return nullptr;
        return it->second.factory();
    }

    /**
     * Check if an effect type is registered.
     */
    bool hasEffect(EffectType type) const {
        return mFactories.count(type) > 0;
    }

    /**
     * Get number of parameters for an effect type.
     */
    int getNumParams(EffectType type) const {
        auto it = mFactories.find(type);
        if (it == mFactories.end()) return 0;
        return it->second.numParams;
    }

    /**
     * Get all registered effect types.
     */
    std::vector<EffectType> registeredTypes() const {
        std::vector<EffectType> types;
        types.reserve(mFactories.size());
        for (const auto& [type, _] : mFactories) {
            types.push_back(type);
        }
        return types;
    }

    /**
     * Get the number of registered effects.
     */
    size_t size() const { return mFactories.size(); }

private:
    struct Entry {
        const char* name;
        int numParams;
        EffectFactory factory;
    };

    std::unordered_map<EffectType, Entry> mFactories;
};

/**
 * Register all 20 built-in effects in the registry.
 * Called by EffectChain constructor. Defined in EffectRegistry.cpp.
 */
void registerBuiltinEffects(EffectRegistry& registry);
