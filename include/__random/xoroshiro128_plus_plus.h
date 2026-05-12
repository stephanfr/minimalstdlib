// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <limits>

namespace MINIMAL_STD_NAMESPACE
{
    //
    //  xoroshiro128++ PRNG engine.
    //
    //  Satisfies uniform_random_bit_generator.  A concrete value type — no virtual
    //  inheritance.  Algorithm by David Blackman and Sebastiano Vigna (2019).
    //
    //  Non-standard extensions:
    //    - seed_type struct for explicit 128-bit initialisation
    //    - fork(): advances state by 2^64 steps and returns an independent instance,
    //      suitable for creating non-overlapping parallel streams.
    //

    class xoroshiro128_plus_plus
    {
    public:
        using result_type = uint64_t;

        struct seed_type
        {
            constexpr seed_type(uint64_t low, uint64_t high) noexcept
                : low(low), high(high)
            {
            }

            uint64_t low;
            uint64_t high;
        };

        static constexpr result_type min() noexcept
        {
            return std::numeric_limits<result_type>::min();
        }

        static constexpr result_type max() noexcept
        {
            return std::numeric_limits<result_type>::max();
        }

        //  Default constructor — uses built-in default seed.
        xoroshiro128_plus_plus() noexcept
            : state_(DEFAULT_SEED_LOW, DEFAULT_SEED_HIGH)
        {
        }

        //  seed_type from a single 64-bit value.  A zero seed is replaced by the default.
        explicit xoroshiro128_plus_plus(uint64_t seed) noexcept
            : state_(seed == 0 ? DEFAULT_SEED_LOW : seed, DEFAULT_SEED_HIGH)
        {
        }

        //  seed_type from a full 128-bit seed_type struct.
        explicit xoroshiro128_plus_plus(const seed_type &seed) noexcept
            : state_(seed)
        {
        }

        xoroshiro128_plus_plus(const xoroshiro128_plus_plus &) = default;
        xoroshiro128_plus_plus &operator=(const xoroshiro128_plus_plus &) = default;

        //  Re-seed from a single 64-bit value.  A zero seed is replaced by the default.
        void seed(uint64_t s) noexcept
        {
            state_ = seed_type(s == 0 ? DEFAULT_SEED_LOW : s, DEFAULT_SEED_HIGH);
        }

        //  Re-seed from a full 128-bit seed_type struct.
        void seed(const seed_type &s) noexcept
        {
            state_ = s;
        }

        result_type operator()() noexcept
        {
            return next_value_internal();
        }

        void discard(unsigned long long z) noexcept
        {
            for (unsigned long long i = 0; i < z; ++i)
            {
                next_value_internal();
            }
        }

        bool operator==(const xoroshiro128_plus_plus &other) const noexcept
        {
            return state_.low == other.state_.low && state_.high == other.state_.high;
        }

        bool operator!=(const xoroshiro128_plus_plus &other) const noexcept
        {
            return !(*this == other);
        }

        //  Non-standard extension.  Returns an independent engine whose state is
        //  2^64 output values ahead of the current state, ensuring non-overlapping
        //  streams when used in parallel.
        xoroshiro128_plus_plus fork() noexcept
        {
            static constexpr uint64_t JUMP[] = {0x2bd7a6a6e99c2ddc, 0x0992ccaf6a6fca05};

            uint64_t s0 = 0;
            uint64_t s1 = 0;

            for (uint64_t i = 0; i < sizeof JUMP / sizeof *JUMP; i++)
            {
                for (int b = 0; b < 64; b++)
                {
                    if (JUMP[i] & UINT64_C(1) << b)
                    {
                        s0 ^= state_.low;
                        s1 ^= state_.high;
                    }

                    next_value_internal();
                }
            }

            return xoroshiro128_plus_plus(seed_type(s0, s1));
        }

    private:
        static constexpr uint64_t DEFAULT_SEED_LOW  = 88172645463325252ULL;
        static constexpr uint64_t DEFAULT_SEED_HIGH = 6364136223846793005ULL;

        seed_type state_;

        static constexpr uint64_t rotl(uint64_t x, int k) noexcept
        {
            return (x << k) | (x >> (64 - k));
        }

        uint64_t next_value_internal() noexcept
        {
            const uint64_t s0 = state_.low;
            uint64_t s1 = state_.high;
            const uint64_t result = rotl(s0 + s1, 17) + s0;

            s1 ^= s0;
            state_.low  = rotl(s0, 49) ^ s1 ^ (s1 << 21); // a, b
            state_.high = rotl(s1, 28);                    // c

            return result;
        }
    };

} // namespace MINIMAL_STD_NAMESPACE
