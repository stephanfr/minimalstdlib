// Copyright 2025 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <limits>

namespace MINIMAL_STD_NAMESPACE
{
    //
    //  Single-threaded low-quality XOR-shift PRNG.
    //
    //  Satisfies uniform_random_bit_generator.  Not thread-safe — intended for
    //  per-object or per-thread use where low overhead matters more than statistical
    //  quality (e.g. backoff randomisation, lock-free elimination arrays).
    //

    class fast_single_threaded_low_quality_rng
    {
    public:
        using result_type = uint64_t;

        static constexpr result_type min() noexcept
        {
            return numeric_limits<result_type>::min();
        }

        static constexpr result_type max() noexcept
        {
            return numeric_limits<result_type>::max();
        }

        //  Default constructor — uses built-in default seed.
        fast_single_threaded_low_quality_rng() noexcept
            : rng_state_(DEFAULT_SEED)
        {
        }

        explicit fast_single_threaded_low_quality_rng(uint64_t seed) noexcept
            : rng_state_(seed == 0 ? DEFAULT_SEED : seed)
        {
        }

        //  Re-seed.  A zero seed is replaced by the default.
        void seed(uint64_t s) noexcept
        {
            rng_state_ = (s == 0 ? DEFAULT_SEED : s);
        }

        result_type operator()() noexcept
        {
            rng_state_ ^= rng_state_ << 13;
            rng_state_ ^= rng_state_ >> 7;
            rng_state_ ^= rng_state_ << 17;
            return rng_state_;
        }

        void discard(unsigned long long z) noexcept
        {
            for (unsigned long long i = 0; i < z; ++i)
            {
                operator()();
            }
        }

        bool operator==(const fast_single_threaded_low_quality_rng &other) const noexcept
        {
            return rng_state_ == other.rng_state_;
        }

        bool operator!=(const fast_single_threaded_low_quality_rng &other) const noexcept
        {
            return !(*this == other);
        }

    private:
        static constexpr uint64_t DEFAULT_SEED = 88172645463325252ULL;

        uint64_t rng_state_;
    };
}
