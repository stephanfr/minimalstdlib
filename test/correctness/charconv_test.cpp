// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <stdint.h>
#include <minstdconfig.h>
#include <charconv>

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
        CHECK_EQUAL(s, ptr);  // ptr unchanged on error
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
        CHECK_EQUAL(s, ptr);
    }

    TEST(FromCharsTests, Int32NegativeOverflow)
    {
        int32_t v = 0;
        const char *s = "-2147483649";  // min-1
        auto [ptr, ec] = minstd::from_chars(s, s + 11, v);
        CHECK(ec == minstd::errc::value_too_large);
        CHECK_EQUAL(s, ptr);
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

} // anonymous namespace
