// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <stdint.h>
#include <initializer_list>

namespace MINIMAL_STD_NAMESPACE
{
    //
    //  seed_seq — implements the C++ SeedSequence concept.
    //
    //  Produces a deterministic, well-distributed sequence of uint_least32_t
    //  values from an arbitrary collection of integer seed values.  Used to
    //  initialise one or more PRNG engines from a small amount of entropy,
    //  spreading and mixing the input bits so that even low-quality seeds
    //  (e.g. a single 32-bit timestamp) yield well-distributed engine states.
    //
    //  Conforms to the C++ standard SeedSequence named requirement:
    //    - result_type = uint_least32_t
    //    - Constructor from initializer_list<uint_least32_t>
    //    - Constructor from an iterator range
    //    - generate(first, last)  — fills [first,last) via the standard mixing
    //    - size()                 — number of stored values
    //    - param(dest)            — copies stored values to output iterator
    //
    //  Non-copyable and non-assignable per the C++ standard.
    //
    //  Storage is fixed at MAX_VALUES words.  Values beyond that limit are
    //  silently discarded.  For typical seeding scenarios (1-8 values) this
    //  is never reached.
    //

    class seed_seq
    {
    public:
        using result_type = uint_least32_t;

        //  Maximum number of 32-bit seed values that can be stored.
        static constexpr size_t MAX_VALUES = 16u;

        seed_seq() noexcept : n_(0u) {}

        seed_seq(std::initializer_list<uint_least32_t> il) noexcept : n_(0u)
        {
            for (auto v : il)
            {
                if (n_ >= MAX_VALUES)
                    break;
                v_[n_++] = v;
            }
        }

        template <typename InputIterator>
        seed_seq(InputIterator first, InputIterator last) noexcept : n_(0u)
        {
            for (; first != last && n_ < MAX_VALUES; ++first)
                v_[n_++] = uint_least32_t(*first);
        }

        seed_seq(const seed_seq &) = delete;
        seed_seq &operator=(const seed_seq &) = delete;

        //  Returns the number of stored seed values.
        size_t size() const noexcept { return n_; }

        //  Copies stored seed values to the output iterator dest.
        template <typename OutputIterator>
        void param(OutputIterator dest) const
        {
            for (size_t i = 0u; i < n_; ++i)
                *dest++ = v_[i];
        }

        //  Fills [first, last) with values derived from the stored seeds.
        //
        //  Implements the mixing algorithm specified in [rand.util.seedseq.generate]
        //  of the C++ standard.  Two passes (forward and backward) over the output
        //  range ensure every output word depends on every stored seed value.
        template <typename RandomAccessIterator>
        void generate(RandomAccessIterator first, RandomAccessIterator last) noexcept
        {
            if (first == last)
                return;

            using T = uint_least32_t;

            const size_t m = static_cast<size_t>(last - first);
            const size_t s = n_;

            //  Initialise output to a non-zero constant.
            for (size_t i = 0u; i < m; ++i)
                first[i] = T(0x8b8b8b8bu);

            //  Layout parameters derived from output-range size.
            const size_t t = (m >= 623u) ? 11u
                           : (m >= 68u)  ? 7u
                           : (m >= 39u)  ? 5u
                           : (m >= 7u)   ? 3u
                           : (m - 1u) / 2u;
            const size_t p = (m - t) / 2u;
            const size_t q = p + t;
            const size_t S = (s > m) ? s : m;   // max(s, m)

            //  Forward pass — k = 0 .. S (inclusive).
            //  Mixes stored seed values into the output array.
            for (size_t k = 0u; k <= S; ++k)
            {
                const size_t km  = k % m;
                const size_t km1 = (k + m - 1u) % m;   // (k-1) mod m, avoids underflow
                const T r1 = T(1664525u) * (first[km] ^ first[(k + p) % m] ^ first[km1]);
                const T r2 = (k == 0u)  ? r1 + T(s) :
                             (k <= s)   ? r1 + T(km) + v_[k - 1u] :
                                          r1 + T(km);
                first[(k + p) % m] += r1;
                first[(k + q) % m] += r2;
                first[km] = r2;
            }

            //  Backward pass — k = S+1 .. S+m (inclusive).
            //  A second mixing sweep using XOR so that the forward values
            //  cannot be trivially recovered.
            for (size_t k = S + 1u; k <= S + m; ++k)
            {
                const size_t km  = k % m;
                const size_t km1 = (k + m - 1u) % m;
                const T r1 = T(1664525u) * (first[km] + first[(k + p) % m] + first[km1]);
                //  v_ index for this iteration is k-m-1; valid when k <= s+m.
                const T r2 = (k <= s + m) ? r1 - v_[k - m - 1u] - T(km)
                                           : r1 - T(km);
                first[(k + p) % m] ^= r1;
                first[(k + q) % m] ^= r2;
                first[km] = r2;
            }
        }

    private:
        uint_least32_t v_[MAX_VALUES];
        size_t         n_;
    };

} // namespace MINIMAL_STD_NAMESPACE
