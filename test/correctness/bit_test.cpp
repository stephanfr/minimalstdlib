// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <stdint.h>

#include <bit>

template <typename _Tp>
concept __has_rotl = requires(_Tp value)
{
    minstd::rotl(value, 1);
};

template <typename _Tp>
concept __has_rotr = requires(_Tp value)
{
    minstd::rotr(value, 1);
};

static_assert(minstd::rotl<uint32_t>(uint32_t{0x12345678u}, 8) == uint32_t{0x34567812u});
static_assert(minstd::rotr<uint32_t>(uint32_t{0x12345678u}, 8) == uint32_t{0x78123456u});
static_assert(!__has_rotl<int32_t>);
static_assert(!__has_rotr<int32_t>);
static_assert(!__has_rotl<bool>);
static_assert(!__has_rotr<bool>);

namespace
{

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(BitTests)
    {};
#pragma GCC diagnostic pop

    TEST(BitTests, RotlAndRotrForUint8)
    {
        const uint8_t value = static_cast<uint8_t>(0x81u);

        CHECK_TRUE(minstd::rotl<uint8_t>(value, 1) == static_cast<uint8_t>(0x03u));
        CHECK_TRUE(minstd::rotr<uint8_t>(value, 1) == static_cast<uint8_t>(0xC0u));
    }

    TEST(BitTests, ShiftNormalizationForUint16)
    {
        const uint16_t value = static_cast<uint16_t>(0x1234u);

        CHECK_TRUE(minstd::rotl<uint16_t>(value, 20) == static_cast<uint16_t>(0x2341u));
        CHECK_TRUE(minstd::rotr<uint16_t>(value, 20) == static_cast<uint16_t>(0x4123u));
    }

    TEST(BitTests, ZeroAndFullWidthShifts)
    {
        const uint32_t value = 0x89abcdefu;

        CHECK_TRUE(minstd::rotl<uint32_t>(value, 0) == value);
        CHECK_TRUE(minstd::rotl<uint32_t>(value, 32) == value);
        CHECK_TRUE(minstd::rotr<uint32_t>(value, 0) == value);
        CHECK_TRUE(minstd::rotr<uint32_t>(value, 64) == value);
    }

    TEST(BitTests, NegativeShiftBehavior)
    {
        const uint32_t value = 0xdeadbeefu;

        CHECK_TRUE(minstd::rotl<uint32_t>(value, -5) == minstd::rotr<uint32_t>(value, 5));
        CHECK_TRUE(minstd::rotr<uint32_t>(value, -9) == minstd::rotl<uint32_t>(value, 9));
    }

    TEST(BitTests, RotlRotrAreInverseOperations)
    {
        const uint64_t value = 0x0123456789abcdefULL;

        for (int shift = -130; shift <= 130; ++shift)
        {
            CHECK_TRUE(minstd::rotr<uint64_t>(minstd::rotl<uint64_t>(value, shift), shift) == value);
            CHECK_TRUE(minstd::rotl<uint64_t>(minstd::rotr<uint64_t>(value, shift), shift) == value);
        }
    }
} // namespace