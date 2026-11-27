// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "minstdconfig.h"

#include <__type_traits/integral_constant.h>
#include <__type_traits/is_destructible.h>

namespace MINIMAL_STD_NAMESPACE
{
#if __has_builtin(__is_trivially_destructible)
    template <class _Tp>
    inline constexpr bool is_trivially_destructible_v = __is_trivially_destructible(_Tp);
#else
    template <class _Tp>
    inline constexpr bool is_trivially_destructible_v = is_destructible_v<_Tp> && __has_trivial_destructor(_Tp);
#endif

    template <class _Tp>
    struct is_trivially_destructible : public integral_constant<bool, is_trivially_destructible_v<_Tp>>
    {
    };
} // namespace MINIMAL_STD_NAMESPACE
