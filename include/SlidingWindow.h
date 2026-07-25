#pragma once

#include <cstdint>
#include <deque>
#include <numeric>
#include <algorithm>

namespace antifraud {

// SlidingWindow<T, DurationExtractor>
//
// A generic time-based sliding window built on top of std::deque.
//
// The window stores items of type T together with a timestamp (uint64_t,
// typically epoch milliseconds/seconds — the unit is up to the caller as
// long as it is used consistently). On every `push`, all items whose
// timestamp is older than `now - window_size` are evicted from the front
// of the deque. Because a transaction can only expire once, the amortized
// cost of eviction across a sequence of N pushes is O(1) per push, even
// though a single push can in theory evict many stale elements.
//
// The class is intentionally not thread-safe: callers that need concurrent
// access should guard a SlidingWindow with an external lock (see
// ConcurrentMap.h, which stripes locks around per-key windows).
template <typename T>
class SlidingWindow {
public:
    struct Entry {
        uint64_t timestamp;
        T value;
    };

    explicit SlidingWindow(uint64_t window_size) noexcept
        : window_size_(window_size) {}

    // Adds a new entry with the given timestamp, evicting everything that
    // has fallen out of the window. Returns the number of evicted entries.
    std::size_t push(uint64_t timestamp, T value) {
        std::size_t evicted = evict(timestamp);
        entries_.push_back(Entry{timestamp, std::move(value)});
        return evicted;
    }

    // Drops stale entries relative to `now` without inserting anything.
    std::size_t evict(uint64_t now) {
        return evictWithWindow(now, window_size_);
    }

    // Same as evict(), but uses `custom_window_size` instead of the
    // instance's own window_size_. Useful when a single physical
    // SlidingWindow (sized to the largest duration any caller needs) is
    // shared by multiple logical consumers that each care about a
    // shorter, independently-configured horizon (e.g. a 60s velocity
    // check and a 1h volume check both reading off the same 24h buffer).
    // The underlying storage always keeps entries up to `window_size_`;
    // callers needing a shorter effective window should additionally
    // filter with `sum`/count helpers below rather than relying on this
    // to trim below `window_size_`.
    std::size_t evictWithWindow(uint64_t now, uint64_t custom_window_size) {
        std::size_t evicted = 0;
        const uint64_t cutoff = (now > custom_window_size) ? (now - custom_window_size) : 0;
        while (!entries_.empty() && entries_.front().timestamp < cutoff) {
            entries_.pop_front();
            ++evicted;
        }
        return evicted;
    }

    // Adds a new entry, evicting relative to `custom_window_size` first.
    std::size_t pushWithWindow(uint64_t timestamp, T value, uint64_t custom_window_size) {
        std::size_t evicted = evictWithWindow(timestamp, custom_window_size);
        entries_.push_back(Entry{timestamp, std::move(value)});
        return evicted;
    }

    // Count of entries with timestamp >= (now - custom_window_size),
    // without mutating the deque. Used when the physical buffer retains
    // a longer history than the logical window a particular rule cares
    // about (see evictWithWindow doc above).
    std::size_t countWithin(uint64_t now, uint64_t custom_window_size) const {
        const uint64_t cutoff = (now > custom_window_size) ? (now - custom_window_size) : 0;
        std::size_t count = 0;
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            if (it->timestamp < cutoff) break;
            ++count;
        }
        return count;
    }

    // Sum of values with timestamp >= (now - custom_window_size).
    template <typename Proj = std::identity>
    auto sumWithin(uint64_t now, uint64_t custom_window_size, Proj proj = {}) const {
        using ResultT = decltype(proj(std::declval<T>()));
        const uint64_t cutoff = (now > custom_window_size) ? (now - custom_window_size) : 0;
        ResultT total{};
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            if (it->timestamp < cutoff) break;
            total += proj(it->value);
        }
        return total;
    }

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }

    void clear() noexcept { entries_.clear(); }

    // Sum of values currently held in the window, using the supplied
    // projection function (defaults to identity for arithmetic T).
    template <typename Proj = std::identity>
    auto sum(Proj proj = {}) const {
        using ResultT = decltype(proj(std::declval<T>()));
        ResultT total{};
        for (const auto& e : entries_) {
            total += proj(e.value);
        }
        return total;
    }

    const std::deque<Entry>& entries() const noexcept { return entries_; }
    uint64_t window_size() const noexcept { return window_size_; }

private:
    uint64_t window_size_;
    std::deque<Entry> entries_;
};

} // namespace antifraud
