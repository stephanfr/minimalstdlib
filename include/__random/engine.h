// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <stdint.h>
#include <__concepts/same_as.h>

namespace MINIMAL_STD_NAMESPACE
{
    //  Satisfies the C++ UniformRandomBitGenerator named requirement.
    template <typename Generator>
    concept uniform_random_bit_generator =
        requires(Generator gen) {
            typename Generator::result_type;
            { Generator::min() } -> same_as<typename Generator::result_type>;
            { Generator::max() } -> same_as<typename Generator::result_type>;
            { gen() } -> same_as<typename Generator::result_type>;
        } &&
        Generator::min() < Generator::max();

    //  Satisfies the C++ SeedSequence named requirement.
    //  A type models seed_sequence if it provides:
    //    - result_type = uint_least32_t
    //    - generate(first, last)  — fills a uint_least32_t random-access range
    //    - size()                 — returns the number of stored seed values
    template <typename S>
    concept seed_sequence =
        requires(S& s, uint_least32_t* p) {
            typename S::result_type;
            requires same_as<typename S::result_type, uint_least32_t>;
            { s.generate(p, p) };
            { s.size() };
        };

} // namespace MINIMAL_STD_NAMESPACE
