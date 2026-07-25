#include "Rules.h"

#include <cmath>
#include <unordered_map>

namespace antifraud {

// ---------------------------------------------------------------------
// HighAmountRule
// ---------------------------------------------------------------------
bool HighAmountRule::evaluate(const Transaction& tx, CardCache& /*cache*/, int& out_reason) const {
    if (tx.amount > threshold_) {
        out_reason = ReasonCode::kHighAmount;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// VelocityRule
// ---------------------------------------------------------------------
bool VelocityRule::evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const {
    bool flagged = false;
    cache.update(tx.card_id, [&](CardState& state) {
        // The physical buffer (velocity_window) is sized to 24h in
        // CardState's constructor; we evict against that bound for
        // long-term memory hygiene, then push, then count only entries
        // that fall within *this rule's* configured window_seconds_.
        // This lets multiple VelocityRule instances with different
        // durations coexist safely if the pipeline is customized.
        state.velocity_window.push(tx.timestamp, tx.amount);
        const std::size_t count_in_window =
            state.velocity_window.countWithin(tx.timestamp, window_seconds_);
        if (count_in_window > max_count_) {
            flagged = true;
        }
    });
    if (flagged) {
        out_reason = ReasonCode::kVelocityExceeded;
    }
    return flagged;
}

// ---------------------------------------------------------------------
// VolumeRule
// ---------------------------------------------------------------------
bool VolumeRule::evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const {
    bool flagged = false;
    cache.update(tx.card_id, [&](CardState& state) {
        state.volume_window.push(tx.timestamp, tx.amount);
        const double total_in_window =
            state.volume_window.sumWithin(tx.timestamp, window_seconds_);
        if (total_in_window > max_volume_) {
            flagged = true;
        }
    });
    if (flagged) {
        out_reason = ReasonCode::kVolumeExceeded;
    }
    return flagged;
}

// ---------------------------------------------------------------------
// GeoSpeedRule
// ---------------------------------------------------------------------

namespace {
struct LatLon { double lat; double lon; };

// Small built-in table of approximate country centroids (degrees).
// Not geographically precise -- good enough to detect "two purchases on
// opposite sides of the planet minutes apart" style fraud patterns.
const std::unordered_map<std::string, LatLon>& countryCentroids() {
    static const std::unordered_map<std::string, LatLon> table = {
        {"US", {37.09, -95.71}},
        {"DE", {51.16, 10.45}},
        {"FR", {46.23, 2.21}},
        {"GB", {55.38, -3.44}},
        {"RU", {61.52, 105.32}},
        {"CN", {35.86, 104.20}},
        {"JP", {36.20, 138.25}},
        {"BR", {-14.24, -51.93}},
        {"AU", {-25.27, 133.78}},
        {"IN", {20.59, 78.96}},
        {"CA", {56.13, -106.35}},
        {"ZA", {-30.56, 22.94}},
        {"AE", {23.42, 53.85}},
        {"SG", {1.35, 103.82}},
        {"NG", {9.08, 8.68}},
        {"MX", {23.63, -102.55}},
        {"IT", {41.87, 12.57}},
        {"ES", {40.46, -3.75}},
        {"NL", {52.13, 5.29}},
        {"PL", {51.92, 19.15}},
    };
    return table;
}

double toRadians(double deg) { return deg * M_PI / 180.0; }
} // namespace

double GeoSpeedRule::distanceKm(const std::string& country_a, const std::string& country_b) {
    if (country_a == country_b) {
        return 0.0;
    }
    const auto& table = countryCentroids();
    auto ita = table.find(country_a);
    auto itb = table.find(country_b);
    if (ita == table.end() || itb == table.end()) {
        return -1.0;
    }

    constexpr double kEarthRadiusKm = 6371.0;
    const double lat1 = toRadians(ita->second.lat);
    const double lon1 = toRadians(ita->second.lon);
    const double lat2 = toRadians(itb->second.lat);
    const double lon2 = toRadians(itb->second.lon);

    const double dlat = lat2 - lat1;
    const double dlon = lon2 - lon1;

    const double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                      std::cos(lat1) * std::cos(lat2) *
                      std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return kEarthRadiusKm * c;
}

bool GeoSpeedRule::evaluate(const Transaction& tx, CardCache& cache, int& out_reason) const {
    bool flagged = false;
    cache.update(tx.card_id, [&](CardState& state) {
        if (state.has_history && tx.timestamp >= state.last_timestamp) {
            const double dist_km = distanceKm(state.last_country, tx.country);
            const uint64_t dt_seconds = tx.timestamp - state.last_timestamp;

            if (dist_km >= 0.0 && dt_seconds > 0) {
                const double hours = static_cast<double>(dt_seconds) / 3600.0;
                const double speed_kmh = dist_km / hours;
                if (speed_kmh > max_speed_kmh_) {
                    flagged = true;
                }
            } else if (dist_km >= 0.0 && dt_seconds == 0 && dist_km > 0.0) {
                // Two different countries at the exact same timestamp:
                // infinite implied speed.
                flagged = true;
            }
        }

        state.last_country = tx.country;
        state.last_timestamp = tx.timestamp;
        state.has_history = true;
    });

    if (flagged) {
        out_reason = ReasonCode::kImpossibleGeoSpeed;
    }
    return flagged;
}

} // namespace antifraud
