// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <__concepts/same_as.h>

namespace MINIMAL_STD_NAMESPACE
{
    template <typename Generator>
    concept uniform_random_bit_generator =
        requires(Generator gen) {
            typename Generator::result_type;
            { Generator::min() } -> same_as<typename Generator::result_type>;
            { Generator::max() } -> same_as<typename Generator::result_type>;
            { gen() } -> same_as<typename Generator::result_type>;
        } &&
        Generator::min() < Generator::max();

} // namespace MINIMAL_STD_NAMESPACE
