// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <__concepts/arithmetic.h>
#include <limits>

namespace MINIMAL_STD_NAMESPACE
{
    template <__minstdlib_unsigned_integer _Tp>
    [[nodiscard]] _MINIMAL_STD_HIDDEN constexpr _Tp rotl(_Tp __x, int __shift) noexcept
    {
        constexpr int __width = numeric_limits<_Tp>::digits;

        const int __rotation = __shift % __width;

        if (__rotation == 0)
            return __x;

        if (__rotation > 0)
            return static_cast<_Tp>((__x << __rotation) | (__x >> (__width - __rotation)));

        const int __normalized = -__rotation;
        return static_cast<_Tp>((__x >> __normalized) | (__x << (__width - __normalized)));
    }

    template <__minstdlib_unsigned_integer _Tp>
    [[nodiscard]] _MINIMAL_STD_HIDDEN constexpr _Tp rotr(_Tp __x, int __shift) noexcept
    {
        constexpr int __width = numeric_limits<_Tp>::digits;

        const int __rotation = __shift % __width;

        if (__rotation == 0)
            return __x;

        if (__rotation > 0)
            return static_cast<_Tp>((__x >> __rotation) | (__x << (__width - __rotation)));

        const int __normalized = -__rotation;
        return static_cast<_Tp>((__x << __normalized) | (__x >> (__width - __normalized)));
    }
} // namespace MINIMAL_STD_NAMESPACE