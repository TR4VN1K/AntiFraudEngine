#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>

#include "SlidingWindow.h"
#include "ConcurrentMap.h"

namespace antifraud {

// Plain data describing a single card transaction.
struct Transaction {
    uint64_t id = 0;
    std::string card_id;
    double amount = 0.0;
    uint64_t timestamp = 0;   // Unix time, seconds
    std::string country;      // ISO country code, e.g. "DE", "US"
};

// Result of running all rules against a transaction.
struct CheckResult {
    bool is_fraud = false;
    int reason_code = 0;               // see ReasonCode below
    uint64_t processing_time_ns = 0;   // wall-clock time spent in Engine::check()
};

// Stable reason codes returned in CheckResult::reason_code so Python callers
// can branch on them without string comparisons.
enum ReasonCode : int {
    kOk = 0,
    kHighAmount = 1,
    kVelocityExceeded = 2,
    kVolumeExceeded = 3,
    kImpossibleGeoSpeed = 4,
};

// Per-card mutable state that rules read/write. One instance lives inside
// the engine's ConcurrentMap, keyed by card_id.
//
// VelocityRule and VolumeRule intentionally use *separate* sliding windows
// (velocity_window / volume_window) even though both simply record amounts
// over time: the two rules are typically configured with different window
// durations (e.g. "5 tx / 60s" vs "20000 spent / 1h"), and keeping them
// independent means either rule can be registered, removed, or reconfigured
// without silently affecting the other.
struct CardState {
    SlidingWindow<double> velocity_window;
    SlidingWindow<double> volume_window;
    std::string last_country;
    uint64_t last_timestamp = 0;
    bool has_history = false;

    // Sliding windows are constructed with a size, so CardState needs an
    // explicit constructor rather than aggregate initialization. The
    // windows are sized generously here (24h) and each rule further
    // restricts what it actually reads via its own configured duration;
    // see VelocityRule::evaluate / VolumeRule::evaluate.
    CardState() : velocity_window(24 * 3600), volume_window(24 * 3600) {}
};

using CardCache = ConcurrentMap<std::string, CardState, 32>;

// Base interface every fraud rule implements. Rules are stateless with
// respect to C++ member data describing *decisions* (thresholds only);
// all mutable per-card state lives in CardCache so that a single IFraudRule
// instance can be safely shared and invoked concurrently for different
// cards.
class IFraudRule {
public:
    virtual ~IFraudRule() = default;

    // Evaluates the rule for `tx`, given (and possibly updating) the
    // shared per-card cache. Returns true if the transaction is flagged
    // by this rule; on a positive result, `out_reason` is set to this
    // rule's ReasonCode.
    virtual bool evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const = 0;

    virtual const char* name() const noexcept = 0;
};

// Flags any single transaction whose amount exceeds a fixed threshold.
class HighAmountRule final : public IFraudRule {
public:
    explicit HighAmountRule(double threshold) : threshold_(threshold) {}

    bool evaluate(const Transaction& tx, CardCache& /*cache*/, int& out_reason) const override;
    const char* name() const noexcept override { return "HighAmountRule"; }

private:
    double threshold_;
};

// Flags a card that makes more than `max_count` transactions within a
// rolling `window_seconds` window ("velocity" / transaction-spam check).
class VelocityRule final : public IFraudRule {
public:
    VelocityRule(std::size_t max_count, uint64_t window_seconds)
        : max_count_(max_count), window_seconds_(window_seconds) {}

    bool evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const override;
    const char* name() const noexcept override { return "VelocityRule"; }

private:
    std::size_t max_count_;
    uint64_t window_seconds_;
};

// Flags a card whose cumulative spend within a rolling `window_seconds`
// window exceeds `max_volume`.
class VolumeRule final : public IFraudRule {
public:
    VolumeRule(double max_volume, uint64_t window_seconds)
        : max_volume_(max_volume), window_seconds_(window_seconds) {}

    bool evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const override;
    const char* name() const noexcept override { return "VolumeRule"; }

private:
    double max_volume_;
    uint64_t window_seconds_;
};

// Flags a transaction if the implied travel speed between the country of
// the previous transaction and this one's country exceeds what's
// physically plausible (e.g. two purchases in different continents a few
// minutes apart). Distances are looked up from a small built-in table of
// approximate country centroids; unknown countries are conservatively
// never flagged.
class GeoSpeedRule final : public IFraudRule {
public:
    explicit GeoSpeedRule(double max_speed_kmh) : max_speed_kmh_(max_speed_kmh) {}

    bool evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const override;
    const char* name() const noexcept override { return "GeoSpeedRule"; }

    // Great-circle distance in kilometers between two country centroids.
    // Returns -1.0 if either country is not in the lookup table.
    static double distanceKm(const std::string& country_a, const std::string& country_b);

private:
    double max_speed_kmh_;
};

} // namespace antifraud
