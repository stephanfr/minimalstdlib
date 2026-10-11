// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <stdint.h>
#include <minstdconfig.h>
#include <charconv>

#include "../shared/process_isolation.h"

namespace
{

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(FromCharsTests)
    {};
#pragma GCC diagnostic pop

    // -----------------------------------------------------------------------
    // Unsigned decimal
    // -----------------------------------------------------------------------

    TEST(FromCharsTests, Uint32BasicDecimal)
    {
        uint32_t v = 0;
        const char *s = "12345";
        auto [ptr, ec] = minstd::from_chars(s, s + 5, v);
        CHECK_EQUAL(12345U, v);
        CHECK(ec == minstd::errc{});
        CHECK_EQUAL(s + 5, ptr);
    }

    TEST(FromCharsTests, Uint32Zero)
    {
        uint32_t v = 99;
        const char *s = "0";
        auto [ptr, ec] = minstd::from_chars(s, s + 1, v);
        CHECK_EQUAL(0U, v);
        CHECK(ec == minstd::errc{});
    }

    TEST(FromCharsTests, Uint32WithTrailingChars)
    {
        uint32_t v = 0;
        const char *s = "42abc";
        auto [ptr, ec] = minstd::from_chars(s, s + 5, v);
        CHECK_EQUAL(42U, v);
        CHECK(ec == minstd::errc{});
        CHECK_EQUAL(s + 2, ptr);  // stopped before 'a'
    }

    TEST(FromCharsTests, Uint32Max)
    {
        uint32_t v = 0;
        const char *s = "4294967295";
        auto [ptr, ec] = minstd::from_chars(s, s + 10, v);
        CHECK_EQUAL(4294967295U, v);
        CHECK(ec == minstd::errc{});
    }

    TEST(FromCharsTests, Uint32Overflow)
    {
        uint32_t v = 0;
        const char *s = "4294967296";  // max+1
        auto [ptr, ec] = minstd::from_chars(s, s + 10, v);
        CHECK(ec == minstd::errc::value_too_large);
        CHECK_EQUAL(s + 10, ptr);  // as std: past all the digits
        CHECK_EQUAL(0u, v);        // value unchanged on error
    }

    TEST(FromCharsTests, Uint64Max)
    {
        uint64_t v = 0;
        const char *s = "18446744073709551615";
        auto [ptr, ec] = minstd::from_chars(s, s + 20, v);
        CHECK_EQUAL(18446744073709551615ULL, v);
        CHECK(ec == minstd::errc{});
    }

    // -----------------------------------------------------------------------
    // Unsigned hex
    // -----------------------------------------------------------------------

    TEST(FromCharsTests, Uint32Hex)
    {
        uint32_t v = 0;
        const char *s = "1A2B";
        auto [ptr, ec] = minstd::from_chars(s, s + 4, v, 16);
        CHECK_EQUAL(0x1A2Bu, v);
        CHECK(ec == minstd::errc{});
    }

    TEST(FromCharsTests, Uint32HexLowercase)
    {
        uint32_t v = 0;
        const char *s = "deadbeef";
        auto [ptr, ec] = minstd::from_chars(s, s + 8, v, 16);
        CHECK_EQUAL(0xDEADBEEFu, v);
        CHECK(ec == minstd::errc{});
    }

    // -----------------------------------------------------------------------
    // Signed decimal
    // -----------------------------------------------------------------------

    TEST(FromCharsTests, Int32Positive)
    {
        int32_t v = 0;
        const char *s = "9876";
        auto [ptr, ec] = minstd::from_chars(s, s + 4, v);
        CHECK_EQUAL(9876, v);
        CHECK(ec == minstd::errc{});
    }

    TEST(FromCharsTests, Int32Negative)
    {
        int32_t v = 0;
        const char *s = "-9876";
        auto [ptr, ec] = minstd::from_chars(s, s + 5, v);
        CHECK_EQUAL(-9876, v);
        CHECK(ec == minstd::errc{});
    }

    TEST(FromCharsTests, Int32MinValue)
    {
        int32_t v = 0;
        const char *s = "-2147483648";
        auto [ptr, ec] = minstd::from_chars(s, s + 11, v);
        CHECK_EQUAL(-2147483647 - 1, v);
        CHECK(ec == minstd::errc{});
    }

    TEST(FromCharsTests, Int32MaxValue)
    {
        int32_t v = 0;
        const char *s = "2147483647";
        auto [ptr, ec] = minstd::from_chars(s, s + 10, v);
        CHECK_EQUAL(2147483647, v);
        CHECK(ec == minstd::errc{});
    }

    TEST(FromCharsTests, Int32Overflow)
    {
        int32_t v = 0;
        const char *s = "2147483648";  // max+1
        auto [ptr, ec] = minstd::from_chars(s, s + 10, v);
        CHECK(ec == minstd::errc::value_too_large);
        CHECK_EQUAL(s + 10, ptr);
    }

    TEST(FromCharsTests, Int32NegativeOverflow)
    {
        int32_t v = 0;
        const char *s = "-2147483649";  // min-1
        auto [ptr, ec] = minstd::from_chars(s, s + 11, v);
        CHECK(ec == minstd::errc::value_too_large);
        CHECK_EQUAL(s + 11, ptr);
    }

    // -----------------------------------------------------------------------
    // Invalid input
    // -----------------------------------------------------------------------

    TEST(FromCharsTests, EmptyRange)
    {
        uint32_t v = 0;
        const char *s = "42";
        auto [ptr, ec] = minstd::from_chars(s, s, v);  // empty range
        CHECK(ec == minstd::errc::invalid_argument);
        CHECK_EQUAL(s, ptr);
    }

    TEST(FromCharsTests, NoDigits)
    {
        uint32_t v = 0;
        const char *s = "abc";
        auto [ptr, ec] = minstd::from_chars(s, s + 3, v);
        CHECK(ec == minstd::errc::invalid_argument);
        CHECK_EQUAL(s, ptr);
    }

    TEST(FromCharsTests, NegativeSignOnly)
    {
        int32_t v = 0;
        const char *s = "-";
        auto [ptr, ec] = minstd::from_chars(s, s + 1, v);
        CHECK(ec == minstd::errc::invalid_argument);
        CHECK_EQUAL(s, ptr);
    }

    // -----------------------------------------------------------------------
    // Base 2 (binary)
    // -----------------------------------------------------------------------

    TEST(FromCharsTests, Uint32Binary)
    {
        uint32_t v = 0;
        const char *s = "1011";
        auto [ptr, ec] = minstd::from_chars(s, s + 4, v, 2);
        CHECK_EQUAL(11U, v);
        CHECK(ec == minstd::errc{});
    }

    // -----------------------------------------------------------------------
    // Smaller types
    // -----------------------------------------------------------------------

    TEST(FromCharsTests, Uint8Max)
    {
        uint8_t v = 0;
        const char *s = "255";
        auto [ptr, ec] = minstd::from_chars(s, s + 3, v);
        CHECK_EQUAL(255U, v);
        CHECK(ec == minstd::errc{});
    }

    TEST(FromCharsTests, Uint8Overflow)
    {
        uint8_t v = 0;
        const char *s = "256";
        auto [ptr, ec] = minstd::from_chars(s, s + 3, v);
        CHECK(ec == minstd::errc::value_too_large);
    }

    TEST(FromCharsTests, Int8Min)
    {
        int8_t v = 0;
        const char *s = "-128";
        auto [ptr, ec] = minstd::from_chars(s, s + 4, v);
        CHECK_EQUAL(-128, v);
        CHECK(ec == minstd::errc{});
    }

    TEST(FromCharsTests, InvalidBaseIsRejected)
    {
        //  base 0 divided by zero computing the overflow limits; bases above 36 have no digit set.
        CHECK(minstd::pmr::test::runs_to_completion([]
                                                    {
                                                        const char text[] = "12";
                                                        int32_t value = 0;
                                                        uint32_t uvalue = 0;
                                                        if (minstd::from_chars(text, text + 2, value, 0).ec != minstd::errc::invalid_argument) _exit(1);
                                                        if (minstd::from_chars(text, text + 2, uvalue, 1).ec != minstd::errc::invalid_argument) _exit(2);
                                                        if (minstd::from_chars(text, text + 2, uvalue, 37).ec != minstd::errc::invalid_argument) _exit(3); })); //  before: SIGFPE
    }

    TEST(FromCharsTests, OverflowConsumesAllDigits)
    {
        const char text[] = "99999999999x";
        int32_t value = 7;

        auto result = minstd::from_chars(text, text + 12, value);

        CHECK(result.ec == minstd::errc::value_too_large);
        CHECK(result.ptr == text + 11); //  before: text
        CHECK_EQUAL(7, value);
    }
} // anonymous namespace
