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

namespace
{
    //  A 32-bit engine (xorshift), narrower than a 64-bit distribution range.
    struct engine32
    {
        using result_type = uint32_t;
        static constexpr result_type min() { return 0; }
        static constexpr result_type max() { return 0xFFFFFFFFu; }

        uint64_t state = 88172645463325252ULL;

        result_type operator()()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<uint32_t>(state);
        }
    };
}

TEST_GROUP(RandomWideRanges)
{
};

TEST(RandomWideRanges, NarrowEngineCoversAWideRange)
{
    //  The rejection threshold assumed the engine was at least as wide as the range: (0, 2^40) returned only
    //      values in [4278190080, 2^32), and (0, 2^64 - 2) never returned.
    engine32 engine;
    minstd::uniform_int_distribution<uint64_t> wide(0, uint64_t{1} << 40);

    uint64_t smallest = UINT64_MAX;
    uint64_t largest = 0;
    size_t above_32_bits = 0;

    for (int i = 0; i < 20000; ++i)
    {
        const uint64_t v = wide(engine);
        CHECK(v <= (uint64_t{1} << 40));
        smallest = (v < smallest) ? v : smallest;
        largest = (v > largest) ? v : largest;
        above_32_bits += (v > 0xFFFFFFFFull) ? 1 : 0;
    }

    CHECK(smallest < (uint64_t{1} << 32));        //  low values appear
    CHECK(largest > (uint64_t{1} << 39));         //  and the top half of the range
    CHECK(above_32_bits > 19000);                 //  ~99.6% of [0, 2^40] lies above 2^32

    const bool returns = minstd::pmr::test::runs_to_completion([]
                                                               {
                                                                   engine32 e;
                                                                   minstd::uniform_int_distribution<uint64_t> nearly_full(0, UINT64_MAX - 1);
                                                                   for (int i = 0; i < 1000; ++i)
                                                                   {
                                                                       (void)nearly_full(e);
                                                                   }
                                                               });
    CHECK_TRUE(returns);
}

TEST(RandomWideRanges, SeedSeqMatchesTheStandardAlgorithm)
{
    //  Reference values from std::seed_seq (libstdc++); the previous implementation differed in every word.
    uint32_t out[4];
    minstd::seed_seq three{1u, 2u, 3u};
    three.generate(out, out + 4);
    CHECK_EQUAL(2494033729u, out[0]);
    CHECK_EQUAL(3915881101u, out[1]);
    CHECK_EQUAL(1602617867u, out[2]);
    CHECK_EQUAL(764004082u, out[3]);

    uint32_t one[1];
    minstd::seed_seq five{0xdeadbeefu, 7u, 0u, 99u, 1234567u};
    five.generate(one, one + 1);
    CHECK_EQUAL(1160567067u, one[0]);

    uint32_t eight[8];
    minstd::seed_seq empty;
    empty.generate(eight, eight + 8);
    CHECK_EQUAL(3071959997u, eight[0]);
    CHECK_EQUAL(2235109613u, eight[7]);
}

TEST_GROUP(XoroshiroFork){};

TEST(XoroshiroFork, SuccessiveForksDoNotOverlap)
{
    //  fork() stepped the parent only 128 times and returned a copy jumped 2^64 ahead, so a second fork returned
    //      the first fork's stream shifted by 128 outputs.
    minstd::xoroshiro128_plus_plus parent(2024);
    const minstd::xoroshiro128_plus_plus original = parent;

    minstd::xoroshiro128_plus_plus first = parent.fork();
    minstd::xoroshiro128_plus_plus second = parent.fork();

    CHECK(first == original); //  the first fork starts where the parent was
    CHECK(first != second);
    CHECK(second != parent);

    constexpr int SAMPLES = 1024;
    static uint64_t first_values[SAMPLES];

    for (auto &v : first_values)
    {
        v = first();
    }

    //  None of the second fork's first outputs (well past the old 128-step shift) appears in the first fork's

    for (int i = 0; i < SAMPLES + 256; i++)
    {
        const uint64_t v = second();

        for (const uint64_t f : first_values)
        {
            CHECK(v != f);
        }
    }
}
