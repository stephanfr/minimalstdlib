// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  Differential fuzzing of from_chars: every integer type against a reference parser that accumulates in 128 bits.
//      The first byte picks the base; a mismatch aborts.

#include <minstdconfig.h>

#include <charconv>
#include <limits>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace
{
    template <typename T>
    void reference(const char *first, const char *last, int base, const char *&ptr, int &outcome, T &value)
    {
        const char *p = first;
        bool negative = false;

        if (minstd::numeric_limits<T>::is_signed && (p < last) && (*p == '-'))
        {
            negative = true;
            p++;
        }

        const char *digits_start = p;
        unsigned __int128 accumulated = 0;
        bool overflow = false;
        const unsigned __int128 limit = negative ? static_cast<unsigned __int128>(-(static_cast<__int128>(minstd::numeric_limits<T>::min())))
                                                 : static_cast<unsigned __int128>(minstd::numeric_limits<T>::max());

        for (; p < last; p++)
        {
            int d;
            const char c = *p;
            if ((c >= '0') && (c <= '9')) d = c - '0';
            else if ((c >= 'a') && (c <= 'z')) d = c - 'a' + 10;
            else if ((c >= 'A') && (c <= 'Z')) d = c - 'A' + 10;
            else break;

            if (d >= base)
            {
                break;
            }

            if (!overflow)
            {
                accumulated = accumulated * base + d;
                overflow = accumulated > limit;
            }
        }

        if (p == digits_start)
        {
            ptr = first;
            outcome = 1;
            return;
        }

        ptr = p;
        outcome = overflow ? 2 : 0;

        if (!overflow)
        {
            value = negative ? static_cast<T>(-static_cast<__int128>(accumulated)) : static_cast<T>(accumulated);
        }
    }

    template <typename T>
    void check(const char *first, const char *last, int base)
    {
        T got = static_cast<T>(0x5A);
        const auto result = minstd::from_chars(first, last, got, base);

        T want = static_cast<T>(0x5A);
        const char *want_ptr = nullptr;
        int want_outcome = 0;
        reference<T>(first, last, base, want_ptr, want_outcome, want);

        const int got_outcome = (result.ec == minstd::errc{}) ? 0 : (result.ec == minstd::errc::invalid_argument) ? 1 : 2;

        if ((got_outcome != want_outcome) || (result.ptr != want_ptr) || (got != want))
        {
            abort();
        }
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
    {
        return 0;
    }

    const int base = 2 + (data[0] % 35);
    const char *first = reinterpret_cast<const char *>(data + 1);
    const char *last = reinterpret_cast<const char *>(data + size);

    check<int8_t>(first, last, base);
    check<uint8_t>(first, last, base);
    check<int16_t>(first, last, base);
    check<uint16_t>(first, last, base);
    check<int32_t>(first, last, base);
    check<uint32_t>(first, last, base);
    check<int64_t>(first, last, base);
    check<uint64_t>(first, last, base);

    return 0;
}
