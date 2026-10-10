// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>
#include <random>

#include <stdint.h>

#include "../shared/process_isolation.h"

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(RandomTests)
    {
    };
#pragma GCC diagnostic pop

    TEST(RandomTests, UniformIntDistributionFullUnsignedRange)
    {
        //  range + 1 wraps to 0 for the full 64-bit range, and the threshold divided by it.
        CHECK(minstd::pmr::test::runs_to_completion([]
                                                    {
                                                        minstd::xoroshiro128_plus_plus engine(12345);
                                                        minstd::uniform_int_distribution<uint64_t> distribution; //  default: [0, UINT64_MAX]
                                                        bool saw_high_bit = false;
                                                        for (int i = 0; i < 64; i++) saw_high_bit |= (distribution(engine) >> 63) != 0;
                                                        if (!saw_high_bit) _exit(1); })); //  before: child dies with SIGFPE
    }

    TEST(RandomTests, UniformIntDistributionFullSignedRange)
    {
        CHECK(minstd::pmr::test::runs_to_completion([]
                                                    {
                                                        minstd::xoroshiro128_plus_plus engine(99);
                                                        minstd::uniform_int_distribution<int64_t> distribution(INT64_MIN, INT64_MAX);
                                                        bool saw_negative = false;
                                                        bool saw_positive = false;
                                                        for (int i = 0; i < 64; i++)
                                                        {
                                                            const int64_t value = distribution(engine);
                                                            saw_negative |= value < 0;
                                                            saw_positive |= value > 0;
                                                        }
                                                        if (!saw_negative || !saw_positive) _exit(1); })); //  before: child dies with SIGFPE
    }

    TEST(RandomTests, UniformIntDistributionStaysInRange)
    {
        minstd::xoroshiro128_plus_plus engine(42);
        minstd::uniform_int_distribution<int32_t> distribution(-5, 5);

        bool seen[11] = {};

        for (int i = 0; i < 10000; i++)
        {
            const int32_t value = distribution(engine);
            CHECK(value >= -5 && value <= 5);
            seen[value + 5] = true;
        }

        for (bool value_seen : seen)
        {
            CHECK_TRUE(value_seen);
        }

        minstd::uniform_int_distribution<int64_t> wide(INT64_MIN + 1, INT64_MAX - 1); //  range + 1 just short of 2^64

        for (int i = 0; i < 1000; i++)
        {
            const int64_t value = wide(engine);
            CHECK(value > INT64_MIN && value < INT64_MAX);
        }
    }
}
