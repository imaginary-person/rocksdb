//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>

#include "cache/cache_helpers.h"
#include "port/lang.h"
#include "rocksdb/cache.h"
#include "rocksdb/status.h"
#include "rocksdb/system_clock.h"
#include "util/coding_lean.h"

namespace ROCKSDB_NAMESPACE {

// A generic helper object for gathering stats about cache entries by
// iterating over them with ApplyToAllEntries. This class essentially
// solves the problem of slowing down a Cache with too many stats
// collectors that could be sharing stat results, such as from multiple
// column families or multiple DBs sharing a Cache. We employ a few
// mitigations:
// * Only one collector for a particular kind of Stats is alive
// for each Cache. This is guaranteed using the Cache itself to hold
// the collector.
// * A mutex ensures only one thread is gathering stats for this
// collector.
// * The most recent gathered stats are saved and simply copied to
// satisfy requests within a time window (default: 3 minutes) of
// completion of the most recent stat gathering.
//
// Template parameter Stats must be copyable and trivially constructable,
// as well as...
// concept Stats {
//   // Notification before applying callback to all entries
//   void BeginCollection(Cache*, SystemClock*, uint64_t start_time_micros);
//   // Get the callback to apply to all entries. `callback`
//   // type must be compatible with Cache::ApplyToAllEntries
//   callback GetEntryCallback();
//   // Notification after applying callback to all entries
//   void EndCollection(Cache*, SystemClock*, uint64_t end_time_micros);
//   // Notification that a collection was skipped because of
//   // sufficiently recent saved results.
//   void SkippedCollection();
// }
template <class Stats>
class CacheEntryStatsCollector {
 public:
  // Gathers stats and saves results into `stats`
  void GetStats(Stats *stats, int maximum_age_in_seconds = 180) {
    // Waits for any pending reader or writer (collector)
    std::lock_guard<std::mutex> lock(mutex_);

    // Maximum allowed age is nominally given by the parameter
    uint64_t max_age_micros =
        static_cast<uint64_t>(std::min(maximum_age_in_seconds, 0)) * 1000000U;
    // But we will re-scan more frequently if it means scanning < 1%
    // of the time and no more than once per second.
    max_age_micros = std::min(
        max_age_micros,
        std::max(uint64_t{1000000},
                 100U * (last_end_time_micros_ - last_start_time_micros_)));

    uint64_t start_time_micros = clock_->NowMicros();
    if ((start_time_micros - last_end_time_micros_) > max_age_micros) {
      last_start_time_micros_ = start_time_micros;
      saved_stats_.BeginCollection(cache_, clock_, start_time_micros);

      cache_->ApplyToAllEntries(saved_stats_.GetEntryCallback(), {});

      uint64_t end_time_micros = clock_->NowMicros();
      last_end_time_micros_ = end_time_micros;
      saved_stats_.EndCollection(cache_, clock_, end_time_micros);
    } else {
      saved_stats_.SkippedCollection();
    }
    // Copy to caller
    *stats = saved_stats_;
  }

  Cache *GetCache() const { return cache_; }

  // Gets or creates a shared instance of CacheEntryStatsCollector in the
  // cache itself, and saves into `ptr`. This shared_ptr will hold the
  // entry in cache until all refs are destroyed.
  static Status GetShared(Cache *cache, SystemClock *clock,
                          std::shared_ptr<CacheEntryStatsCollector> *ptr) {
    std::array<uint64_t, 3> cache_key_data{
        {// First 16 bytes == md5 of class name
         0x7eba5a8fb5437c90U, 0x8ca68c9b11655855U,
         // Last 8 bytes based on a function pointer to make unique for each
         // template instantiation
         reinterpret_cast<uint64_t>(&CacheEntryStatsCollector::GetShared)}};
    Slice cache_key = GetSlice(&cache_key_data);

    Cache::Handle *h = cache->Lookup(cache_key);
    if (h == nullptr) {
      // Not yet in cache, but Cache doesn't provide a built-in way to
      // avoid racing insert. So we double-check under a shared mutex,
      // inspired by TableCache.
      STATIC_AVOID_DESTRUCTION(std::mutex, static_mutex);
      std::lock_guard<std::mutex> lock(static_mutex);

      h = cache->Lookup(cache_key);
      if (h == nullptr) {
        auto new_ptr = new CacheEntryStatsCollector(cache, clock);
        // TODO: non-zero charge causes some tests that count block cache
        // usage to go flaky. Fix the problem somehow so we can use an
        // accurate charge.
        size_t charge = 0;
        Status s = cache->Insert(cache_key, new_ptr, charge, Deleter, &h);
        if (!s.ok()) {
          assert(h == nullptr);
          return s;
        }
      }
    }
    // If we reach here, shared entry is in cache with handle `h`.
    assert(cache->GetDeleter(h) == Deleter);

    // Build an aliasing shared_ptr that keeps `ptr` in cache while there
    // are references.
    *ptr = MakeSharedCacheHandleGuard<CacheEntryStatsCollector>(cache, h);
    return Status::OK();
  }

 private:
  explicit CacheEntryStatsCollector(Cache *cache, SystemClock *clock)
      : saved_stats_(),
        last_start_time_micros_(0),
        last_end_time_micros_(/*pessimistic*/ 10000000),
        cache_(cache),
        clock_(clock) {}

  static void Deleter(const Slice &, void *value) {
    delete static_cast<CacheEntryStatsCollector *>(value);
  }

  std::mutex mutex_;
  Stats saved_stats_;
  uint64_t last_start_time_micros_;
  uint64_t last_end_time_micros_;
  Cache *const cache_;
  SystemClock *const clock_;
};

}  // namespace ROCKSDB_NAMESPACE
