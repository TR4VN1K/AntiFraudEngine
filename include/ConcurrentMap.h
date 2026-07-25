#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <optional>

namespace antifraud {

// ConcurrentMap<Key, Value, Stripes>
//
// A thread-safe hash map implemented with striped locking: instead of a
// single global mutex (which would serialize every access) the key space
// is partitioned into `Stripes` independent shards, each protected by its
// own std::shared_mutex. The shard for a given key is chosen deterministically
// via hash(key) % Stripes, so all operations on the same key always land on
// the same shard/mutex, while operations on different keys spread across
// stripes can proceed truly in parallel.
//
// Readers use a shared (read) lock via std::shared_lock, allowing multiple
// concurrent readers per shard; writers take an exclusive lock.
//
// This is a good fit for a fraud engine's per-card state cache: most
// accesses are keyed by card_id, and traffic for different cards is
// naturally independent.
template <typename Key, typename Value, std::size_t Stripes = 32>
class ConcurrentMap {
    static_assert(Stripes > 0, "ConcurrentMap requires at least one stripe");

public:
    ConcurrentMap() : shards_(Stripes) {}

    // Returns a copy of the value for `key`, or std::nullopt if absent.
    std::optional<Value> get(const Key& key) const {
        const Shard& shard = shard_for(key);
        std::shared_lock lock(shard.mutex);
        auto it = shard.data.find(key);
        if (it == shard.data.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Inserts or overwrites the value for `key`.
    void put(const Key& key, Value value) {
        Shard& shard = shard_for(key);
        std::unique_lock lock(shard.mutex);
        shard.data[key] = std::move(value);
    }

    // Atomically applies `fn` to the value stored at `key` (default-
    // constructing it first if absent), under an exclusive lock. This is
    // the primary primitive fraud rules use to update per-card state
    // (e.g. append a transaction to a sliding window) without a
    // read-modify-write race between threads.
    template <typename Fn>
    void update(const Key& key, Fn&& fn) {
        Shard& shard = shard_for(key);
        std::unique_lock lock(shard.mutex);
        fn(shard.data[key]);
    }

    bool erase(const Key& key) {
        Shard& shard = shard_for(key);
        std::unique_lock lock(shard.mutex);
        return shard.data.erase(key) > 0;
    }

    bool contains(const Key& key) const {
        const Shard& shard = shard_for(key);
        std::shared_lock lock(shard.mutex);
        return shard.data.find(key) != shard.data.end();
    }

    // Approximate total size across all shards. Since each shard is locked
    // independently and briefly, this is a best-effort snapshot, not an
    // atomic global count.
    std::size_t size() const {
        std::size_t total = 0;
        for (const auto& shard : shards_) {
            std::shared_lock lock(shard.mutex);
            total += shard.data.size();
        }
        return total;
    }

    void clear() {
        for (auto& shard : shards_) {
            std::unique_lock lock(shard.mutex);
            shard.data.clear();
        }
    }

private:
    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<Key, Value> data;
    };

    Shard& shard_for(const Key& key) {
        return shards_[std::hash<Key>{}(key) % Stripes];
    }
    const Shard& shard_for(const Key& key) const {
        return shards_[std::hash<Key>{}(key) % Stripes];
    }

    // std::shared_mutex is not movable/copyable, so shards live in a
    // std::vector<Shard> allocated once at construction time and never
    // resized (vector's element addresses stay stable as long as we never
    // push_back/reallocate after construction).
    std::vector<Shard> shards_;
};

} // namespace antifraud
