#pragma once

/// @file
/// Thread-safe in-process key/value cache.
/// @ingroup cache

#include "caelitus/cache/WriterPreferringMutex.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace caelitus::cache {

/// Thread-safe in-process key/value cache for read-heavy data.
///
/// Lookups take a shared lock and run in parallel; writes take an exclusive
/// one and are not starved by a continuous stream of reads
/// (WriterPreferringMutex). No expiry or size bound: whoever owns it decides
/// what is in it (see catalog::BookCache).
///
/// @tparam K     Key type.
/// @tparam V     Value type; get() returns copies, with() reads in place.
/// @tparam Hash  Hash function for K.
template <typename K, typename V, typename Hash = std::hash<K>>
class LocalCache {
public:
    /// Counters for monitoring.
    struct Stats {
        std::size_t size = 0;      ///< Entries currently cached.
        std::uint64_t hits = 0;    ///< Lookups that found their key, since construction.
        std::uint64_t misses = 0;  ///< Lookups that did not.
    };

    /// A copy of the value for `key`, or std::nullopt.
    std::optional<V> get(const K& key) const {
        std::shared_lock<WriterPreferringMutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second;
    }

    /// Reads a value in place, without copying it.
    ///
    /// `f(const V&)` runs under the shared lock, so keep it trivial (read a
    /// field or two) and never call back into the cache from it.
    /// @return What `f` returned, or std::nullopt on a miss.
    template <typename F>
    auto with(const K& key, F&& f) const -> std::optional<decltype(f(std::declval<const V&>()))> {
        std::shared_lock<WriterPreferringMutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        hits_.fetch_add(1, std::memory_order_relaxed);
        return f(it->second);
    }

    /// Inserts or replaces the value for `key`.
    void put(const K& key, V value) {
        std::unique_lock<WriterPreferringMutex> lock(mutex_);
        map_.insert_or_assign(key, std::move(value));
    }

    /// Removes `key`, if present.
    void erase(const K& key) {
        std::unique_lock<WriterPreferringMutex> lock(mutex_);
        map_.erase(key);
    }

    /// Swaps in a complete new content, built by the caller without the lock.
    /// Readers see either the old content or the new one, never a mix.
    void replaceAll(std::unordered_map<K, V, Hash> content) {
        std::unique_lock<WriterPreferringMutex> lock(mutex_);
        map_.swap(content);
    }  // the old content (now in `content`) is freed after the lock is released

    /// Current size and hit/miss counters.
    Stats stats() const {
        std::shared_lock<WriterPreferringMutex> lock(mutex_);
        return {map_.size(), hits_.load(std::memory_order_relaxed), misses_.load(std::memory_order_relaxed)};
    }

private:
    mutable WriterPreferringMutex mutex_;
    std::unordered_map<K, V, Hash> map_;
    mutable std::atomic<std::uint64_t> hits_{0}, misses_{0};
};

}  // namespace caelitus::cache
