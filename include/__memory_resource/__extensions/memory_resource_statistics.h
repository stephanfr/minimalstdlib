// Copyright 2025 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <__fwd/memory_resource.h>

#include "algorithm"
#include "atomic"

#include "__platform/cpu_platform_abstractions.h"

namespace MINIMAL_STD_NAMESPACE
{
    namespace pmr
    {
        namespace extensions
        {
            class memory_resource_statistics
            {
            public:
                memory_resource_statistics() = default;

                memory_resource_statistics(const memory_resource_statistics &) = delete;
                memory_resource_statistics(memory_resource_statistics &&) = delete;

                virtual ~memory_resource_statistics() = default;

                memory_resource_statistics &operator=(const memory_resource_statistics &) = delete;
                memory_resource_statistics &operator=(memory_resource_statistics &&) = delete;

                size_t total_allocations() const
                {
                    size_t total = 0;
                    for (const shard_counters &shard : shards_)
                    {
                        total += shard.allocations_.load(memory_order_acquire);
                    }
                    return total;
                }

                size_t total_deallocations() const
                {
                    size_t total = 0;
                    for (const shard_counters &shard : shards_)
                    {
                        total += shard.deallocations_.load(memory_order_acquire);
                    }
                    return total;
                }

                size_t current_allocated() const
                {
                    //  Deallocations first: an allocation counted on another shard may not be visible yet, but each
                    //      deallocation follows its allocation, so reading deallocations first errs high, not low.
                    const size_t deallocations = total_deallocations();
                    const size_t allocations = total_allocations();
                    return (allocations > deallocations) ? (allocations - deallocations) : 0;
                }

                size_t peak_allocated() const
                {
                    return peak_allocated_.load(memory_order_acquire);
                }

                size_t current_bytes_allocated() const
                {
                    size_t deallocated = 0;
                    for (const shard_counters &shard : shards_)
                    {
                        deallocated += shard.bytes_deallocated_.load(memory_order_acquire);
                    }

                    size_t allocated = 0;
                    for (const shard_counters &shard : shards_)
                    {
                        allocated += shard.bytes_allocated_.load(memory_order_acquire);
                    }

                    return (allocated > deallocated) ? (allocated - deallocated) : 0;
                }

                size_t aborted_deallocations() const
                {
                    return aborted_deallocations_.load(memory_order_acquire);
                }

                size_t aborted_deallocations_bad_index() const
                {
                    return aborted_deallocations_bad_index_.load(memory_order_acquire);
                }

                size_t aborted_deallocations_state_mismatch() const
                {
                    return aborted_deallocations_state_mismatch_.load(memory_order_acquire);
                }

                size_t aborted_deallocations_cas_race() const
                {
                    return aborted_deallocations_cas_race_.load(memory_order_acquire);
                }

            protected:
                //  Counters are sharded by CPU id: each allocation and deallocation updates only its own CPU's cache
                //      line, so CPUs do not contend on shared counters.  The totals are sums over the shards.  The peak
                //      is exact for a single context (also one that migrates between CPUs): every allocation computes
                //      the current count from all shards.  That read touches every shard, so with several CPUs
                //      allocating at once statistics still cost noticeable throughput; they are off by default in the
                //      lock-free resources.
                static constexpr size_t STATISTICS_SHARDS = 16;

                struct alignas(64) shard_counters
                {
                    atomic<size_t> allocations_{0};
                    atomic<size_t> deallocations_{0};
                    atomic<size_t> bytes_allocated_{0};
                    atomic<size_t> bytes_deallocated_{0};
                };

                shard_counters shards_[STATISTICS_SHARDS];

                alignas(64) atomic<size_t> peak_allocated_{0};
                alignas(64) atomic<size_t> aborted_deallocations_{0};
                atomic<size_t> aborted_deallocations_bad_index_{0};
                atomic<size_t> aborted_deallocations_state_mismatch_{0};
                atomic<size_t> aborted_deallocations_cas_race_{0};

                shard_counters &local_shard()
                {
                    return shards_[platform::get_cpu_id() & (STATISTICS_SHARDS - 1)];
                }

                void allocation_made(size_t size)
                {
                    shard_counters &shard = local_shard();
                    shard.allocations_.fetch_add(1, memory_order_relaxed);
                    shard.bytes_allocated_.fetch_add(size, memory_order_relaxed);

                    const size_t current = current_allocated();
                    size_t peak = peak_allocated_.load(memory_order_relaxed);

                    while ((current > peak) && !peak_allocated_.compare_exchange_weak(peak, current, memory_order_relaxed, memory_order_relaxed))
                    {
                    }
                }

                void deallocation_made(size_t size)
                {
                    shard_counters &shard = local_shard();
                    shard.deallocations_.fetch_add(1, memory_order_relaxed);
                    shard.bytes_deallocated_.fetch_add(size, memory_order_relaxed);
                }

                void deallocation_aborted_bad_index()
                {
                    aborted_deallocations_.fetch_add(1, memory_order_relaxed);
                    aborted_deallocations_bad_index_.fetch_add(1, memory_order_relaxed);
                }

                void deallocation_aborted_state_mismatch()
                {
                    aborted_deallocations_.fetch_add(1, memory_order_relaxed);
                    aborted_deallocations_state_mismatch_.fetch_add(1, memory_order_relaxed);
                }

                void deallocation_aborted_cas_race()
                {
                    aborted_deallocations_.fetch_add(1, memory_order_relaxed);
                    aborted_deallocations_cas_race_.fetch_add(1, memory_order_relaxed);
                }
            };

            class null_memory_resource_statistics
            {
            public:
                null_memory_resource_statistics() = default;

                null_memory_resource_statistics(const null_memory_resource_statistics &) = delete;
                null_memory_resource_statistics(null_memory_resource_statistics &&) = delete;

                virtual ~null_memory_resource_statistics() = default;

                null_memory_resource_statistics &operator=(const null_memory_resource_statistics &) = delete;
                null_memory_resource_statistics &operator=(null_memory_resource_statistics &&) = delete;

            protected:
                size_t aborted_deallocations() const
                {
                    return 0;
                }

                size_t aborted_deallocations_bad_index() const
                {
                    return 0;
                }

                size_t aborted_deallocations_state_mismatch() const
                {
                    return 0;
                }

                size_t aborted_deallocations_cas_race() const
                {
                    return 0;
                }

                void allocation_made(size_t size)
                {
                }

                void deallocation_made(size_t size)
                {
                }

                void deallocation_aborted_bad_index()
                {
                }

                void deallocation_aborted_state_mismatch()
                {
                }

                void deallocation_aborted_cas_race()
                {
                }
            };

        } // namespace extensions
    } // namespace pmr
} // namespace MINIMAL_STD_NAMESPACE
