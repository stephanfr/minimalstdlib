// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <stdint.h>

#include <minstdconfig.h>

#include <limits>

namespace
{

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(NumericLimitsTests)
    {};
#pragma GCC diagnostic pop

    // -----------------------------------------------------------------------
    //  is_specialized
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, IsSpecializedFalseForUnknownType)
    {
        struct unknown_type {};
        CHECK_FALSE(minstd::numeric_limits<unknown_type>::is_specialized);
    }

    TEST(NumericLimitsTests, IsSpecializedTrueForIntegerTypes)
    {
        CHECK_TRUE(minstd::numeric_limits<int8_t>::is_specialized);
        CHECK_TRUE(minstd::numeric_limits<uint8_t>::is_specialized);
        CHECK_TRUE(minstd::numeric_limits<int16_t>::is_specialized);
        CHECK_TRUE(minstd::numeric_limits<uint16_t>::is_specialized);
        CHECK_TRUE(minstd::numeric_limits<int32_t>::is_specialized);
        CHECK_TRUE(minstd::numeric_limits<uint32_t>::is_specialized);
        CHECK_TRUE(minstd::numeric_limits<int64_t>::is_specialized);
        CHECK_TRUE(minstd::numeric_limits<uint64_t>::is_specialized);
    }

    TEST(NumericLimitsTests, IsSpecializedTrueForFloatTypes)
    {
        CHECK_TRUE(minstd::numeric_limits<float>::is_specialized);
        CHECK_TRUE(minstd::numeric_limits<double>::is_specialized);
    }

    // -----------------------------------------------------------------------
    //  is_integer
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, IsIntegerTrueForIntegerTypes)
    {
        CHECK_TRUE(minstd::numeric_limits<int8_t>::is_integer);
        CHECK_TRUE(minstd::numeric_limits<uint8_t>::is_integer);
        CHECK_TRUE(minstd::numeric_limits<int16_t>::is_integer);
        CHECK_TRUE(minstd::numeric_limits<uint16_t>::is_integer);
        CHECK_TRUE(minstd::numeric_limits<int32_t>::is_integer);
        CHECK_TRUE(minstd::numeric_limits<uint32_t>::is_integer);
        CHECK_TRUE(minstd::numeric_limits<int64_t>::is_integer);
        CHECK_TRUE(minstd::numeric_limits<uint64_t>::is_integer);
    }

    TEST(NumericLimitsTests, IsIntegerFalseForFloatTypes)
    {
        CHECK_FALSE(minstd::numeric_limits<float>::is_integer);
        CHECK_FALSE(minstd::numeric_limits<double>::is_integer);
    }

    // -----------------------------------------------------------------------
    //  is_signed
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, IsSignedCorrectForSignedTypes)
    {
        CHECK_TRUE(minstd::numeric_limits<int8_t>::is_signed);
        CHECK_TRUE(minstd::numeric_limits<int16_t>::is_signed);
        CHECK_TRUE(minstd::numeric_limits<int32_t>::is_signed);
        CHECK_TRUE(minstd::numeric_limits<int64_t>::is_signed);
        CHECK_TRUE(minstd::numeric_limits<float>::is_signed);
        CHECK_TRUE(minstd::numeric_limits<double>::is_signed);
    }

    TEST(NumericLimitsTests, IsSignedFalseForUnsignedTypes)
    {
        CHECK_FALSE(minstd::numeric_limits<uint8_t>::is_signed);
        CHECK_FALSE(minstd::numeric_limits<uint16_t>::is_signed);
        CHECK_FALSE(minstd::numeric_limits<uint32_t>::is_signed);
        CHECK_FALSE(minstd::numeric_limits<uint64_t>::is_signed);
    }

    // -----------------------------------------------------------------------
    //  is_exact
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, IsExactTrueForIntegerTypes)
    {
        CHECK_TRUE(minstd::numeric_limits<int8_t>::is_exact);
        CHECK_TRUE(minstd::numeric_limits<uint8_t>::is_exact);
        CHECK_TRUE(minstd::numeric_limits<int32_t>::is_exact);
        CHECK_TRUE(minstd::numeric_limits<uint64_t>::is_exact);
    }

    TEST(NumericLimitsTests, IsExactFalseForFloatTypes)
    {
        CHECK_FALSE(minstd::numeric_limits<float>::is_exact);
        CHECK_FALSE(minstd::numeric_limits<double>::is_exact);
    }

    // -----------------------------------------------------------------------
    //  is_modulo
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, IsModuloTrueForUnsignedTypes)
    {
        CHECK_TRUE(minstd::numeric_limits<uint8_t>::is_modulo);
        CHECK_TRUE(minstd::numeric_limits<uint16_t>::is_modulo);
        CHECK_TRUE(minstd::numeric_limits<uint32_t>::is_modulo);
        CHECK_TRUE(minstd::numeric_limits<uint64_t>::is_modulo);
    }

    TEST(NumericLimitsTests, IsModuloFalseForSignedAndFloatTypes)
    {
        CHECK_FALSE(minstd::numeric_limits<int8_t>::is_modulo);
        CHECK_FALSE(minstd::numeric_limits<int16_t>::is_modulo);
        CHECK_FALSE(minstd::numeric_limits<int32_t>::is_modulo);
        CHECK_FALSE(minstd::numeric_limits<int64_t>::is_modulo);
        CHECK_FALSE(minstd::numeric_limits<float>::is_modulo);
        CHECK_FALSE(minstd::numeric_limits<double>::is_modulo);
    }

    // -----------------------------------------------------------------------
    //  radix
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, RadixIsCorrect)
    {
        CHECK_EQUAL(2, minstd::numeric_limits<int8_t>::radix);
        CHECK_EQUAL(2, minstd::numeric_limits<uint32_t>::radix);
        CHECK_EQUAL(2, minstd::numeric_limits<int64_t>::radix);
        CHECK_EQUAL(2, minstd::numeric_limits<float>::radix);
        CHECK_EQUAL(2, minstd::numeric_limits<double>::radix);
    }

    // -----------------------------------------------------------------------
    //  digits
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, DigitsCorrectForIntegerTypes)
    {
        CHECK_EQUAL(7,  minstd::numeric_limits<int8_t>::digits);
        CHECK_EQUAL(8,  minstd::numeric_limits<uint8_t>::digits);
        CHECK_EQUAL(15, minstd::numeric_limits<int16_t>::digits);
        CHECK_EQUAL(16, minstd::numeric_limits<uint16_t>::digits);
        CHECK_EQUAL(31, minstd::numeric_limits<int32_t>::digits);
        CHECK_EQUAL(32, minstd::numeric_limits<uint32_t>::digits);
        CHECK_EQUAL(63, minstd::numeric_limits<int64_t>::digits);
        CHECK_EQUAL(64, minstd::numeric_limits<uint64_t>::digits);
    }

    TEST(NumericLimitsTests, DigitsCorrectForFloatTypes)
    {
        CHECK_EQUAL(24, minstd::numeric_limits<float>::digits);
        CHECK_EQUAL(53, minstd::numeric_limits<double>::digits);
    }

    // -----------------------------------------------------------------------
    //  digits10
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, Digits10CorrectForIntegerTypes)
    {
        CHECK_EQUAL(2,  minstd::numeric_limits<int8_t>::digits10);
        CHECK_EQUAL(2,  minstd::numeric_limits<uint8_t>::digits10);
        CHECK_EQUAL(4,  minstd::numeric_limits<int16_t>::digits10);
        CHECK_EQUAL(4,  minstd::numeric_limits<uint16_t>::digits10);
        CHECK_EQUAL(9,  minstd::numeric_limits<int32_t>::digits10);
        CHECK_EQUAL(9,  minstd::numeric_limits<uint32_t>::digits10);
        CHECK_EQUAL(18, minstd::numeric_limits<int64_t>::digits10);
        CHECK_EQUAL(19, minstd::numeric_limits<uint64_t>::digits10);
    }

    TEST(NumericLimitsTests, Digits10CorrectForFloatTypes)
    {
        CHECK_EQUAL(6,  minstd::numeric_limits<float>::digits10);
        CHECK_EQUAL(15, minstd::numeric_limits<double>::digits10);
    }

    // -----------------------------------------------------------------------
    //  min() / max() — integer types
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, IntegerMinMaxCorrect)
    {
        CHECK_EQUAL(int8_t(-128),                    minstd::numeric_limits<int8_t>::min());
        CHECK_EQUAL(int8_t(127),                     minstd::numeric_limits<int8_t>::max());
        CHECK_EQUAL(uint8_t(0),                      minstd::numeric_limits<uint8_t>::min());
        CHECK_EQUAL(uint8_t(255U),                   minstd::numeric_limits<uint8_t>::max());
        CHECK_EQUAL(int16_t(-32768),                 minstd::numeric_limits<int16_t>::min());
        CHECK_EQUAL(int16_t(32767),                  minstd::numeric_limits<int16_t>::max());
        CHECK_EQUAL(uint16_t(0),                     minstd::numeric_limits<uint16_t>::min());
        CHECK_EQUAL(uint16_t(65535U),                minstd::numeric_limits<uint16_t>::max());
        CHECK_EQUAL(int32_t(-2147483647 - 1),        minstd::numeric_limits<int32_t>::min());
        CHECK_EQUAL(int32_t(2147483647),             minstd::numeric_limits<int32_t>::max());
        CHECK_EQUAL(uint32_t(0),                     minstd::numeric_limits<uint32_t>::min());
        CHECK_EQUAL(uint32_t(4294967295U),           minstd::numeric_limits<uint32_t>::max());
        CHECK_EQUAL(int64_t(-9223372036854775807LL - 1), minstd::numeric_limits<int64_t>::min());
        CHECK_EQUAL(int64_t(9223372036854775807LL),  minstd::numeric_limits<int64_t>::max());
        CHECK_EQUAL(uint64_t(0),                     minstd::numeric_limits<uint64_t>::min());
        CHECK_EQUAL(uint64_t(18446744073709551615ULL), minstd::numeric_limits<uint64_t>::max());
    }

    // -----------------------------------------------------------------------
    //  lowest() — equals min() for integers
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, LowestEqualsMinForIntegerTypes)
    {
        CHECK_EQUAL(minstd::numeric_limits<int8_t>::min(),   minstd::numeric_limits<int8_t>::lowest());
        CHECK_EQUAL(minstd::numeric_limits<uint8_t>::min(),  minstd::numeric_limits<uint8_t>::lowest());
        CHECK_EQUAL(minstd::numeric_limits<int16_t>::min(),  minstd::numeric_limits<int16_t>::lowest());
        CHECK_EQUAL(minstd::numeric_limits<uint16_t>::min(), minstd::numeric_limits<uint16_t>::lowest());
        CHECK_EQUAL(minstd::numeric_limits<int32_t>::min(),  minstd::numeric_limits<int32_t>::lowest());
        CHECK_EQUAL(minstd::numeric_limits<uint32_t>::min(), minstd::numeric_limits<uint32_t>::lowest());
        CHECK_EQUAL(minstd::numeric_limits<int64_t>::min(),  minstd::numeric_limits<int64_t>::lowest());
        CHECK_EQUAL(minstd::numeric_limits<uint64_t>::min(), minstd::numeric_limits<uint64_t>::lowest());
    }

    // -----------------------------------------------------------------------
    //  min() / max() / lowest() — float types
    //  Crucially: min() is the smallest POSITIVE normalised value,
    //             lowest() is the most negative finite value (-max()).
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, FloatMinIsPositive)
    {
        CHECK_TRUE(minstd::numeric_limits<float>::min() > 0.0f);
        CHECK_TRUE(minstd::numeric_limits<double>::min() > 0.0);
    }

    TEST(NumericLimitsTests, FloatMaxIsPositive)
    {
        CHECK_TRUE(minstd::numeric_limits<float>::max() > 0.0f);
        CHECK_TRUE(minstd::numeric_limits<double>::max() > 0.0);
    }

    TEST(NumericLimitsTests, FloatLowestIsNegativeMax)
    {
        // lowest() must equal -max() for IEEE 754 types
        CHECK_TRUE(minstd::numeric_limits<float>::lowest() < 0.0f);
        CHECK_TRUE(minstd::numeric_limits<float>::lowest() == -minstd::numeric_limits<float>::max());
        CHECK_TRUE(minstd::numeric_limits<double>::lowest() < 0.0);
        CHECK_TRUE(minstd::numeric_limits<double>::lowest() == -minstd::numeric_limits<double>::max());
    }

    TEST(NumericLimitsTests, FloatMinLessThanMax)
    {
        CHECK_TRUE(minstd::numeric_limits<float>::min() < minstd::numeric_limits<float>::max());
        CHECK_TRUE(minstd::numeric_limits<double>::min() < minstd::numeric_limits<double>::max());
    }

    // -----------------------------------------------------------------------
    //  epsilon()
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, EpsilonIsPositiveAndSmall)
    {
        CHECK_TRUE(minstd::numeric_limits<float>::epsilon() > 0.0f);
        CHECK_TRUE(minstd::numeric_limits<float>::epsilon() < 1.0f);
        CHECK_TRUE(minstd::numeric_limits<double>::epsilon() > 0.0);
        CHECK_TRUE(minstd::numeric_limits<double>::epsilon() < 1.0);
        // double epsilon must be smaller than float epsilon
        CHECK_TRUE(minstd::numeric_limits<double>::epsilon() < minstd::numeric_limits<float>::epsilon());
    }

    // -----------------------------------------------------------------------
    //  is_iec559
    // -----------------------------------------------------------------------

    TEST(NumericLimitsTests, IsIec559TrueForFloatTypes)
    {
        CHECK_TRUE(minstd::numeric_limits<float>::is_iec559);
        CHECK_TRUE(minstd::numeric_limits<double>::is_iec559);
    }

    TEST(NumericLimitsTests, IsIec559FalseForIntegerTypes)
    {
        CHECK_FALSE(minstd::numeric_limits<int32_t>::is_iec559);
        CHECK_FALSE(minstd::numeric_limits<uint64_t>::is_iec559);
    }

} // anonymous namespace

//  The integer types without a fixed-width typedef fell back to the unspecialised template (digits 0, max 0).
TEST_GROUP(NumericLimitsOtherIntegerTypes)
{
};

TEST(NumericLimitsOtherIntegerTypes, LongLongAndCharacterTypes)
{
    CHECK_EQUAL(64, minstd::numeric_limits<unsigned long long>::digits);
    CHECK(minstd::numeric_limits<unsigned long long>::max() == 18446744073709551615ULL);
    CHECK(minstd::numeric_limits<long long>::max() == 9223372036854775807LL);
    CHECK(minstd::numeric_limits<long long>::min() == (-9223372036854775807LL - 1));
    CHECK_TRUE(minstd::numeric_limits<long long>::is_specialized);
    CHECK_TRUE(minstd::numeric_limits<unsigned long long>::is_modulo);

    CHECK_EQUAL(static_cast<int>(__SCHAR_MAX__ == 127 ? 127 : 0), static_cast<int>(minstd::numeric_limits<signed char>::max()));
    CHECK_EQUAL(65535, static_cast<int>(minstd::numeric_limits<char16_t>::max()));
    CHECK(minstd::numeric_limits<char32_t>::max() == 4294967295u);
    CHECK_TRUE(minstd::numeric_limits<char>::is_specialized);
    CHECK_EQUAL(minstd::numeric_limits<char>::is_signed ? 127 : 255, static_cast<int>(minstd::numeric_limits<char>::max()));

    CHECK_TRUE(minstd::numeric_limits<bool>::max());
    CHECK_FALSE(minstd::numeric_limits<bool>::min());
    CHECK_EQUAL(1, minstd::numeric_limits<bool>::digits);

    CHECK_EQUAL(32, minstd::numeric_limits<const unsigned int>::digits);
    CHECK(minstd::numeric_limits<long double>::max() > 0.0L);
}
