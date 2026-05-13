// Copyright 2025 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <limits>

namespace MINIMAL_STD_NAMESPACE
{
    //
    //  Abstract base class for non-deterministic hardware entropy sources.
    //
    //  Satisfies uniform_random_bit_generator by providing result_type, static min(),
    //  static max(), and operator()().
    //
    //  The OS platform provides a concrete subclass backed by the hardware RNG.
    //  Non-copyable and non-movable, per the C++ standard.
    //

    class random_device
    {
    public:
        using result_type = uint32_t;

        random_device() = default;
        virtual ~random_device() = default;

        random_device(const random_device &) = delete;
        random_device &operator=(const random_device &) = delete;
        random_device(random_device &&) = delete;
        random_device &operator=(random_device &&) = delete;

        static constexpr result_type min() noexcept
        {
            return numeric_limits<result_type>::min();
        }

        static constexpr result_type max() noexcept
        {
            return numeric_limits<result_type>::max();
        }

        virtual result_type operator()() = 0;

        //  Returns an estimate of the entropy of the source in bits.
        //  Returns 0.0 when the entropy cannot be determined or the source
        //  is deterministic.  Concrete subclasses override when hardware
        //  can report a meaningful estimate.

        virtual double entropy() const noexcept
        {
            return 0.0;
        }
    };

} // namespace MINIMAL_STD_NAMESPACE
