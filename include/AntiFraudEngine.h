#pragma once

#include <memory>
#include <vector>
#include <string>

#include "Rules.h"

namespace antifraud {

// AntiFraudEngine ties together the per-card state cache (CardCache) and a
// configurable pipeline of IFraudRule instances. It is the single
// entry point exposed to Python via pybind11.
//
// Thread-safety: `check()` is safe to call concurrently from multiple
// threads. Rules only mutate state through CardCache, which internally
// uses striped locking, so throughput scales with the number of distinct
// cards being processed concurrently.
class AntiFraudEngine {
public:
    // Builds an engine with a sensible default rule set:
    //   - HighAmountRule(threshold = 10000.0)
    //   - VelocityRule(max_count = 5, window = 60s)
    //   - VolumeRule(max_volume = 20000.0, window = 3600s)
    //   - GeoSpeedRule(max_speed_kmh = 900.0)
    AntiFraudEngine();

    // Builds an engine with explicit thresholds, useful for tests/tuning
    // from Python without touching C++.
    AntiFraudEngine(double high_amount_threshold,
                     std::size_t velocity_max_count,
                     uint64_t velocity_window_seconds,
                     double volume_max_amount,
                     uint64_t volume_window_seconds,
                     double geo_max_speed_kmh);

    // Runs every registered rule (in registration order) against `tx`,
    // updating per-card state along the way, and returns on the first
    // rule that flags the transaction (short-circuit evaluation). Always
    // records CheckResult::processing_time_ns for the whole call,
    // including cache updates.
    CheckResult check(const Transaction& tx);

    // Registers an additional custom rule at the end of the pipeline.
    // Ownership is transferred to the engine.
    void addRule(std::unique_ptr<IFraudRule> rule);

    // Clears all per-card cached state (sliding windows, last-seen
    // country/time). Rule configuration itself is untouched.
    void resetState();

    std::size_t cachedCardCount() const;
    std::size_t ruleCount() const noexcept { return rules_.size(); }

private:
    std::vector<std::unique_ptr<IFraudRule>> rules_;
    CardCache cache_;
};

} // namespace antifraud
