// Copyright 2025 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <atomic>
#include <limits>

namespace MINIMAL_STD_NAMESPACE
{
    //
    //  Lock-free low-quality XOR-shift PRNG.
    //
    //  Satisfies uniform_random_bit_generator.  Thread-safe via an atomic CAS loop.
    //  Suitable for use cases that require low overhead and can tolerate reduced
    //  statistical quality (e.g. backoff jitter, load-balancing, non-cryptographic
    //  sampling).  Not suitable where high statistical quality is required.
    //

    class fast_lockfree_low_quality_rng
    {
    public:
        using result_type = uint64_t;

        static constexpr result_type min() noexcept
        {
            return std::numeric_limits<result_type>::min();
        }

        static constexpr result_type max() noexcept
        {
            return std::numeric_limits<result_type>::max();
        }

        //  Default constructor — uses built-in default seed.
        fast_lockfree_low_quality_rng() noexcept
            : rng_state_(DEFAULT_SEED)
        {
        }

        explicit fast_lockfree_low_quality_rng(uint64_t seed) noexcept
            : rng_state_(seed == 0 ? DEFAULT_SEED : seed)
        {
        }

        //  Re-seed.  A zero seed is replaced by the default.
        void seed(uint64_t s) noexcept
        {
            rng_state_.store(s == 0 ? DEFAULT_SEED : s, memory_order_relaxed);
        }

        result_type operator()()
        {
            uint64_t initial_state = rng_state_.load(memory_order_relaxed);
            uint64_t next_value;

            size_t retries = 0;

            do
            {
                if (retries != 0)
                {
                    for (size_t i = 0; i < 500 * retries; ++i)
                    {
                        __asm__ __volatile__("" ::: "memory");
                    }
                }

                retries++;

                next_value = initial_state;
                next_value ^= next_value << 13;
                next_value ^= next_value >> 7;
                next_value ^= next_value << 17;
            } while (!rng_state_.compare_exchange_weak(initial_state, next_value, memory_order_acq_rel, memory_order_acquire));

            return next_value;
        }

        void discard(unsigned long long z)
        {
            for (unsigned long long i = 0; i < z; ++i)
            {
                operator()();
            }
        }

        bool operator==(const fast_lockfree_low_quality_rng &other) const noexcept
        {
            return rng_state_.load(memory_order_relaxed) == other.rng_state_.load(memory_order_relaxed);
        }

        bool operator!=(const fast_lockfree_low_quality_rng &other) const noexcept
        {
            return !(*this == other);
        }

    private:
        static constexpr uint64_t DEFAULT_SEED = 88172645463325252ULL;

        atomic<uint64_t> rng_state_;
    };
}
