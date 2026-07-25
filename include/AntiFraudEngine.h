#pragma once

#include <memory>
#include <vector>
#include <string>

#include "Rules.h"

namespace antifraud {

// Main fraud detection engine. Owns the rule pipeline and per-card state cache.
// Thread-safe for concurrent calls to check().
class AntiFraudEngine {
public:
    // Creates an engine with the default rule set.
    AntiFraudEngine();

    // Creates an engine with custom rule thresholds.
    AntiFraudEngine(double high_amount_threshold,
                    std::size_t velocity_max_count,
                    uint64_t velocity_window_seconds,
                    double volume_max_amount,
                    uint64_t volume_window_seconds,
                    double geo_max_speed_kmh);

    // Processes a transaction and returns the first matching fraud result.
    CheckResult check(const Transaction& tx);

    // Adds a custom rule to the pipeline.
    void addRule(std::unique_ptr<IFraudRule> rule);

    // Clears all cached per-card state.
    void resetState();

    std::size_t cachedCardCount() const;
    std::size_t ruleCount() const noexcept { return rules_.size(); }

private:
    std::vector<std::unique_ptr<IFraudRule>> rules_;
    CardCache cache_;
};

} // namespace antifraud
