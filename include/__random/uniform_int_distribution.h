// Copyright 2025 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include "__random/engine.h"

#include <__type_traits/is_integral.h>
#include <__type_traits/make_unsigned.h>
#include <limits>

namespace MINIMAL_STD_NAMESPACE
{
    //
    //  Produces random integers uniformly distributed on the closed interval [a, b].
    //
    //  Satisfies the C++ standard UniformRandomBitDistribution requirements.
    //  Uses rejection sampling to eliminate modulo bias.
    //

    template <typename IntType = int>
        requires is_integral_v<IntType>
    class uniform_int_distribution
    {
    public:
        using result_type = IntType;

        class param_type
        {
        public:
            using distribution_type = uniform_int_distribution;

            explicit param_type(result_type a = 0,
                                result_type b = numeric_limits<result_type>::max()) noexcept
                : a_(a), b_(b)
            {
            }

            result_type a() const noexcept { return a_; }
            result_type b() const noexcept { return b_; }

            bool operator==(const param_type &other) const noexcept
            {
                return a_ == other.a_ && b_ == other.b_;
            }

            bool operator!=(const param_type &other) const noexcept
            {
                return !(*this == other);
            }

        private:
            result_type a_;
            result_type b_;
        };

        uniform_int_distribution() noexcept
            : param_(0, numeric_limits<result_type>::max())
        {
        }

        explicit uniform_int_distribution(result_type a,
                                          result_type b = numeric_limits<result_type>::max()) noexcept
            : param_(a, b)
        {
        }

        explicit uniform_int_distribution(const param_type &p) noexcept
            : param_(p)
        {
        }

        //  No internal state to reset for this distribution.
        void reset() noexcept {}

        param_type param() const noexcept { return param_; }
        void param(const param_type &p) noexcept { param_ = p; }

        result_type a() const noexcept { return param_.a(); }
        result_type b() const noexcept { return param_.b(); }

        result_type min() const noexcept { return a(); }
        result_type max() const noexcept { return b(); }

        template <typename URBG>
            requires uniform_random_bit_generator<URBG>
        result_type operator()(URBG &g)
        {
            return generate(g, param_);
        }

        template <typename URBG>
            requires uniform_random_bit_generator<URBG>
        result_type operator()(URBG &g, const param_type &p)
        {
            return generate(g, p);
        }

        bool operator==(const uniform_int_distribution &other) const noexcept
        {
            return param_ == other.param_;
        }

        bool operator!=(const uniform_int_distribution &other) const noexcept
        {
            return !(*this == other);
        }

    private:
        param_type param_;

        using unsigned_result_type = __make_unsigned_t<result_type>;
        using engine_result_type   = uint64_t;

        template <typename URBG>
        result_type generate(URBG &g, const param_type &p)
        {
            const unsigned_result_type a = static_cast<unsigned_result_type>(p.a());
            const unsigned_result_type b = static_cast<unsigned_result_type>(p.b());
            const unsigned_result_type range = b - a;

            if (range == 0)
            {
                return p.a();
            }

            //  Add in the unsigned type: a signed add overflows when the interval is wider than result_type's max.
            return static_cast<result_type>(static_cast<unsigned_result_type>(draw(g, static_cast<engine_result_type>(range))) + a);
        }

        //  A uniform value in [0, range].
        //
        //  An engine at least as wide as the range: rejection sampling on one output.  Raw values below
        //      threshold = (engine values) mod (range + 1) are rejected, leaving a whole number of copies of each
        //      result.
        //
        //  An engine narrower than the range (e.g. a 32-bit engine and a 64-bit range): combine outputs.  A uniform
        //      high part in [0, range / engine values], scaled by the number of engine values, plus one engine
        //      output is uniform over a superset of [0, range]; reject results above range or that wrapped.  This
        //      is the method libstdc++ uses.
        template <typename URBG>
        static engine_result_type draw(URBG &g, engine_result_type range)
        {
            constexpr engine_result_type engine_range =
                static_cast<engine_result_type>(URBG::max()) -
                static_cast<engine_result_type>(URBG::min());

            auto raw = [&g]() -> engine_result_type
            {
                return static_cast<engine_result_type>(g()) - static_cast<engine_result_type>(URBG::min());
            };

            if (range == engine_range)
            {
                return raw();
            }

            if (range < engine_range)
            {
                const engine_result_type range_plus_one = range + 1;
                const engine_result_type threshold = (engine_range - range) % range_plus_one;

                engine_result_type value;
                do
                {
                    value = raw();
                } while (value < threshold);

                return value % range_plus_one;
            }

            //  Only an engine narrower than engine_result_type gets here: a full-width one has range <= engine_range,
            //      handled above (and engine_values would be 0).
            if constexpr (engine_range == static_cast<engine_result_type>(~engine_result_type{0}))
            {
                return raw();
            }
            else
            {
                constexpr engine_result_type engine_values = engine_range + 1;

                engine_result_type high;
                engine_result_type value;
                do
                {
                    high = engine_values * draw(g, range / engine_values);
                    value = high + raw();
                } while ((value > range) || (value < high));

                return value;
            }
        }
    };

} // namespace MINIMAL_STD_NAMESPACE
