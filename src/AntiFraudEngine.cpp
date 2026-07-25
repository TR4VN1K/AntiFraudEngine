#include "AntiFraudEngine.h"

#include <chrono>

namespace antifraud {

AntiFraudEngine::AntiFraudEngine()
    : AntiFraudEngine(/*high_amount_threshold=*/10000.0,
                       /*velocity_max_count=*/5,
                       /*velocity_window_seconds=*/60,
                       /*volume_max_amount=*/20000.0,
                       /*volume_window_seconds=*/3600,
                       /*geo_max_speed_kmh=*/900.0) {}

AntiFraudEngine::AntiFraudEngine(double high_amount_threshold,
                                   std::size_t velocity_max_count,
                                   uint64_t velocity_window_seconds,
                                   double volume_max_amount,
                                   uint64_t volume_window_seconds,
                                   double geo_max_speed_kmh) {
    rules_.reserve(4);
    rules_.push_back(std::make_unique<HighAmountRule>(high_amount_threshold));
    rules_.push_back(std::make_unique<VelocityRule>(velocity_max_count, velocity_window_seconds));
    rules_.push_back(std::make_unique<VolumeRule>(volume_max_amount, volume_window_seconds));
    rules_.push_back(std::make_unique<GeoSpeedRule>(geo_max_speed_kmh));
}

CheckResult AntiFraudEngine::check(const Transaction& tx) {
    const auto start = std::chrono::steady_clock::now();

    CheckResult result;
    for (const auto& rule : rules_) {
        int reason = ReasonCode::kOk;
        if (rule->evaluate(tx, cache_, reason)) {
            result.is_fraud = true;
            result.reason_code = reason;
            break;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    result.processing_time_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    return result;
}

void AntiFraudEngine::addRule(std::unique_ptr<IFraudRule> rule) {
    rules_.push_back(std::move(rule));
}

void AntiFraudEngine::resetState() {
    cache_.clear();
}

std::size_t AntiFraudEngine::cachedCardCount() const {
    return cache_.size();
}

} // namespace antifraud
