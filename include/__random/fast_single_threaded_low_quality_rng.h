// Copyright 2025 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <limits>
#include "__random/engine.h"

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

        //  SeedSequence constructor — seeds the engine from a SeedSequence.
        template <seed_sequence SeedSeq>
        explicit fast_single_threaded_low_quality_rng(SeedSeq &seq) noexcept
        {
            seed(seq);
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

        //  Re-seed from a SeedSequence.  Generates two 32-bit words and assembles
        //  them into the 64-bit state.  Guards against the all-zero state.
        template <seed_sequence SeedSeq>
        void seed(SeedSeq &seq) noexcept
        {
            uint_least32_t s[2];
            seq.generate(s, s + 2);
            rng_state_ = (uint64_t(s[0]) << 32) | uint64_t(s[1]);
            if (rng_state_ == 0u)
                rng_state_ = DEFAULT_SEED;
        }

        //  Re-seed.  A zero seed is replaced by the default.
        void seed(uint64_t s) noexcept
        {
            rng_state_ = (s == 0 ? DEFAULT_SEED : s);
        }

        //  Returns the current engine state.  Combined with seed(uint64_t) or the
        //  uint64_t constructor, this provides full state save/restore.
        uint64_t state() const noexcept { return rng_state_; }

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
