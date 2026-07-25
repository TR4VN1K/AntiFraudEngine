#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <numeric>

namespace antifraud {

// Generic time-based sliding window.
// Expired entries are evicted in O(1) amortized time.
// Not thread-safe.
template <typename T>
class SlidingWindow {
public:
    struct Entry {
        uint64_t timestamp;
        T value;
    };

    explicit SlidingWindow(uint64_t window_size) noexcept
        : window_size_(window_size) {}

    // Inserts a new entry.
    std::size_t push(uint64_t timestamp, T value) {
        std::size_t evicted = evict(timestamp);
        entries_.push_back(Entry{timestamp, std::move(value)});
        return evicted;
    }

    // Evicts expired entries.
    std::size_t evict(uint64_t now) {
        return evictWithWindow(now, window_size_);
    }

    // Evicts entries using a custom window.
    std::size_t evictWithWindow(uint64_t now, uint64_t custom_window_size) {
        std::size_t evicted = 0;

        const uint64_t cutoff =
            (now > custom_window_size)
                ? (now - custom_window_size)
                : 0;

        while (!entries_.empty() &&
               entries_.front().timestamp < cutoff) {
            entries_.pop_front();
            ++evicted;
        }

        return evicted;
    }

    // Inserts an entry using a custom window.
    std::size_t pushWithWindow(
        uint64_t timestamp,
        T value,
        uint64_t custom_window_size) {

        std::size_t evicted =
            evictWithWindow(timestamp, custom_window_size);

        entries_.push_back(
            Entry{
                timestamp,
                std::move(value)
            });

        return evicted;
    }

    // Returns the number of entries within a custom window.
    std::size_t countWithin(
        uint64_t now,
        uint64_t custom_window_size) const {

        const uint64_t cutoff =
            (now > custom_window_size)
                ? (now - custom_window_size)
                : 0;

        std::size_t count = 0;

        for (auto it = entries_.rbegin();
             it != entries_.rend();
             ++it) {

            if (it->timestamp < cutoff)
                break;

            ++count;
        }

        return count;
    }

    // Returns the projected sum within a custom window.
    template <typename Proj = std::identity>
    auto sumWithin(
        uint64_t now,
        uint64_t custom_window_size,
        Proj proj = {}) const {

        using ResultT = decltype(proj(std::declval<T>()));

        const uint64_t cutoff =
            (now > custom_window_size)
                ? (now - custom_window_size)
                : 0;

        ResultT total{};

        for (auto it = entries_.rbegin();
             it != entries_.rend();
             ++it) {

            if (it->timestamp < cutoff)
                break;

            total += proj(it->value);
        }

        return total;
    }

    std::size_t size() const noexcept {
        return entries_.size();
    }

    bool empty() const noexcept {
        return entries_.empty();
    }

    void clear() noexcept {
        entries_.clear();
    }

    // Returns the projected sum of all stored entries.
    template <typename Proj = std::identity>
    auto sum(Proj proj = {}) const {

        using ResultT = decltype(proj(std::declval<T>()));

        ResultT total{};

        for (const auto& e : entries_) {
            total += proj(e.value);
        }

        return total;
    }

    const std::deque<Entry>& entries() const noexcept {
        return entries_;
    }

    uint64_t window_size() const noexcept {
        return window_size_;
    }

private:
    uint64_t window_size_;
    std::deque<Entry> entries_;
};

} // namespace antifraud
