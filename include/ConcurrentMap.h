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

// Thread-safe hash map using striped locking. Keys are distributed across
// independent shards, each protected by its own shared_mutex.
template <typename Key, typename Value, std::size_t Stripes = 32>
class ConcurrentMap {
    static_assert(Stripes > 0, "ConcurrentMap requires at least one stripe");

public:
    ConcurrentMap() : shards_(Stripes) {}

    // Returns the value for key or std::nullopt if absent.
    std::optional<Value> get(const Key& key) const {
        const Shard& shard = shard_for(key);
        std::shared_lock lock(shard.mutex);
        auto it = shard.data.find(key);
        if (it == shard.data.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Inserts or replaces the value.
    void put(const Key& key, Value value) {
        Shard& shard = shard_for(key);
        std::unique_lock lock(shard.mutex);
        shard.data[key] = std::move(value);
    }

    // Applies fn(value) under an exclusive lock.
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

    // Returns the current number of stored entries.
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

    // Shards are allocated once and never resized.
    std::vector<Shard> shards_;
};

} // namespace antifraud
