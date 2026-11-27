// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

namespace MINIMAL_STD_NAMESPACE
{
    //  Asks a wrapper type to construct its value from the arguments that follow.

    struct in_place_t
    {
        explicit in_place_t() = default;
    };

    inline constexpr in_place_t in_place{};
} // namespace MINIMAL_STD_NAMESPACE
