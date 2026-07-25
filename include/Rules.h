#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>

#include "SlidingWindow.h"
#include "ConcurrentMap.h"

namespace antifraud {

// Card transaction data.
struct Transaction {
    uint64_t id = 0;
    std::string card_id;
    double amount = 0.0;
    uint64_t timestamp = 0;   // Unix timestamp (seconds)
    std::string country;      // ISO country code (e.g. "DE", "US")
};

// Evaluation result of engine rules.
struct CheckResult {
    bool is_fraud = false;
    int reason_code = 0;               // ReasonCode enum
    uint64_t processing_time_ns = 0;   // Execution duration in nanoseconds
};

// Error / flagging reason codes.
enum ReasonCode : int {
    kOk = 0,
    kHighAmount = 1,
    kVelocityExceeded = 2,
    kVolumeExceeded = 3,
    kImpossibleGeoSpeed = 4,
};

// Stateful per-card data stored in ConcurrentMap.
struct CardState {
    SlidingWindow<double> velocity_window;
    SlidingWindow<double> volume_window;
    std::string last_country;
    uint64_t last_timestamp = 0;
    bool has_history = false;

    // Default 24h capacity for sliding windows
    CardState() : velocity_window(24 * 3600), volume_window(24 * 3600) {}
};

using CardCache = ConcurrentMap<std::string, CardState, 32>;

// Base interface for fraud rules.
class IFraudRule {
public:
    virtual ~IFraudRule() = default;

    // Evaluates transaction against card cache state. Returns true if flagged.
    virtual bool evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const = 0;
    virtual const char* name() const noexcept = 0;
};

// Rule: Single transaction amount threshold.
class HighAmountRule final : public IFraudRule {
public:
    explicit HighAmountRule(double threshold) : threshold_(threshold) {}

    bool evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const override;
    const char* name() const noexcept override { return "HighAmountRule"; }

private:
    double threshold_;
};

// Rule: Transaction count within a rolling time window.
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

// Rule: Total transaction volume/amount within a rolling time window.
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

// Rule: Physical travel speed between consecutive transactions in different countries.
class GeoSpeedRule final : public IFraudRule {
public:
    explicit GeoSpeedRule(double max_speed_kmh) : max_speed_kmh_(max_speed_kmh) {}

    bool evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const override;
    const char* name() const noexcept override { return "GeoSpeedRule"; }

    // Distance in km between country centroids (-1.0 if unknown).
    static double distanceKm(const std::string& country_a, const std::string& country_b);

private:
    double max_speed_kmh_;
};

} // namespace antifraud
