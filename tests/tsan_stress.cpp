// Standalone stress test (not part of the pybind11 module) that hammers
// AntiFraudEngine::check() from many concurrent std::threads across a
// shared pool of card_ids. Intended to be compiled with
// -fsanitize=thread to catch any data races in ConcurrentMap's striped
// locking or SlidingWindow usage. Not linked into the Python extension.
#include "AntiFraudEngine.h"

#include <atomic>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

int main() {
    using namespace antifraud;

    AntiFraudEngine engine;
    constexpr int kThreads = 16;
    constexpr int kOpsPerThread = 20000;
    constexpr int kCards = 64; // deliberately small to force lock contention

    std::atomic<uint64_t> flagged{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            std::mt19937 rng(1000 + t);
            std::uniform_int_distribution<int> card_dist(0, kCards - 1);
            std::uniform_real_distribution<double> amount_dist(1.0, 300.0);
            uint64_t ts = 1'700'000'000ULL + static_cast<uint64_t>(t) * 100000ULL;

            for (int i = 0; i < kOpsPerThread; ++i) {
                Transaction tx;
                tx.id = static_cast<uint64_t>(t) * kOpsPerThread + i;
                tx.card_id = "card_" + std::to_string(card_dist(rng));
                tx.amount = amount_dist(rng);
                tx.timestamp = ts++;
                tx.country = "DE";

                CheckResult r = engine.check(tx);
                if (r.is_fraud) {
                    flagged.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& w : workers) w.join();

    std::cout << "Completed " << (kThreads * kOpsPerThread) << " concurrent checks, "
              << "flagged=" << flagged.load() << ", "
              << "cached_card_count=" << engine.cachedCardCount() << std::endl;
    return 0;
}
