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
        //  The algorithm specified in [rand.util.seedseq] of the C++ standard, so the
        //  output matches std::seed_seq for the same seeds.
        template <typename RandomAccessIterator>
        void generate(RandomAccessIterator first, RandomAccessIterator last) noexcept
        {
            if (first == last)
                return;

            //  [rand.util.seedseq]: all arithmetic is modulo 2^32.
            using T = uint32_t;

            const size_t n = static_cast<size_t>(last - first);
            const size_t s = n_;

            auto at = [&first](size_t index) -> T
            {
                return static_cast<T>(first[index]);
            };

            auto mix = [](T x) -> T
            {
                return x ^ (x >> 27);
            };

            for (size_t i = 0u; i < n; ++i)
            {
                first[i] = T(0x8b8b8b8bu);
            }

            const size_t t = (n >= 623u) ? 11u
                             : (n >= 68u) ? 7u
                             : (n >= 39u) ? 5u
                             : (n >= 7u)  ? 3u
                                          : (n - 1u) / 2u;
            const size_t p = (n - t) / 2u;
            const size_t q = p + t;
            const size_t m = ((s + 1u) > n) ? (s + 1u) : n; //  max(s + 1, n)

            for (size_t k = 0u; k < m; ++k)
            {
                const size_t kn = k % n;
                const size_t kpn = (k + p) % n;
                const size_t kqn = (k + q) % n;
                const size_t km1n = (k + n - 1u) % n;

                const T r1 = T(1664525u) * mix(at(kn) ^ at(kpn) ^ at(km1n));
                const T r2 = (k == 0u)  ? T(r1 + T(s))
                             : (k <= s) ? T(r1 + T(kn) + T(v_[k - 1u]))
                                        : T(r1 + T(kn));

                first[kpn] = T(at(kpn) + r1);
                first[kqn] = T(at(kqn) + r2);
                first[kn] = r2;
            }

            for (size_t k = m; k < m + n; ++k)
            {
                const size_t kn = k % n;
                const size_t kpn = (k + p) % n;
                const size_t kqn = (k + q) % n;
                const size_t km1n = (k + n - 1u) % n;

                const T r3 = T(1566083941u) * mix(T(at(kn) + at(kpn) + at(km1n)));
                const T r4 = T(r3 - T(kn));

                first[kpn] = T(at(kpn) ^ r3);
                first[kqn] = T(at(kqn) ^ r4);
                first[kn] = r4;
            }
        }

    private:
        uint_least32_t v_[MAX_VALUES];
        size_t         n_;
    };

} // namespace MINIMAL_STD_NAMESPACE
