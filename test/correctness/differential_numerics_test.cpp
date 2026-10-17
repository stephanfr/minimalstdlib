// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//
//  Differential tests for the numeric utilities: numeric_limits against the compiler's predefined macros, rotl/rotr
//      against a bit-by-bit reference, and uniform_int_distribution against its contract (every result in range, and
//      a chi-square test of uniformity over small ranges, including with an engine narrower than the range).
//

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <bit>
#include <limits>
#include <random>

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "../shared/process_isolation.h"

namespace
{
    TEST_GROUP(DifferentialNumericsTests){};

    template <typename T>
    void check_integer_limits(const char *name, long double min, long double max, int digits, bool is_signed)
    {
        using limits = minstd::numeric_limits<T>;

        char text[160];
        snprintf(text, sizeof(text), "numeric_limits<%s>", name);

        CHECK_TEXT(limits::is_specialized, text);
        CHECK_TEXT(limits::is_integer, text);
        CHECK_TEXT(limits::is_signed == is_signed, text);
        CHECK_TEXT(static_cast<long double>(limits::min()) == min, text);
        CHECK_TEXT(static_cast<long double>(limits::max()) == max, text);
        CHECK_TEXT(static_cast<long double>(limits::lowest()) == min, text);
        CHECK_TEXT(limits::digits == digits, text);
        CHECK_TEXT(limits::is_exact, text);
        CHECK_TEXT(limits::is_modulo == !is_signed, text); //  unsigned arithmetic wraps; signed overflow is undefined
        CHECK_TEXT(limits::radix == 2, text);
        CHECK_TEXT(!limits::is_iec559, text);

        //  digits10: the decimal digits representable without change, floor(digits * log10(2))
        CHECK_TEXT(limits::digits10 == static_cast<int>(digits * 0.30102999566398119521L), text);
    }

    TEST(DifferentialNumericsTests, IntegerLimitsMatchThePredefinedMacros)
    {
        check_integer_limits<signed char>("signed char", -__SCHAR_MAX__ - 1, __SCHAR_MAX__, 7, true);
        check_integer_limits<unsigned char>("unsigned char", 0, 2 * (long double)__SCHAR_MAX__ + 1, 8, false);
        check_integer_limits<short>("short", -__SHRT_MAX__ - 1, __SHRT_MAX__, 15, true);
        check_integer_limits<unsigned short>("unsigned short", 0, 2 * (long double)__SHRT_MAX__ + 1, 16, false);
        check_integer_limits<int>("int", -(long double)__INT_MAX__ - 1, __INT_MAX__, 31, true);
        check_integer_limits<unsigned int>("unsigned int", 0, 2 * (long double)__INT_MAX__ + 1, 32, false);
        check_integer_limits<long>("long", -(long double)__LONG_MAX__ - 1, __LONG_MAX__, 63, true);
        check_integer_limits<unsigned long>("unsigned long", 0, 2 * (long double)__LONG_MAX__ + 1, 64, false);
        check_integer_limits<long long>("long long", -(long double)__LONG_LONG_MAX__ - 1, __LONG_LONG_MAX__, 63, true);
        check_integer_limits<unsigned long long>("unsigned long long", 0, 2 * (long double)__LONG_LONG_MAX__ + 1, 64, false);
        check_integer_limits<char16_t>("char16_t", 0, 65535, 16, false);
        check_integer_limits<char32_t>("char32_t", 0, 4294967295.0L, 32, false);

        //  char's signedness is the target's: compare with the macro the compiler sets
#if defined(__CHAR_UNSIGNED__)
        check_integer_limits<char>("char", 0, 255, 8, false);
#else
        check_integer_limits<char>("char", -128, 127, 7, true);
#endif

        CHECK(minstd::numeric_limits<bool>::is_specialized);
        CHECK_EQUAL(1, minstd::numeric_limits<bool>::digits);
        CHECK(minstd::numeric_limits<bool>::max() == true);
        CHECK(minstd::numeric_limits<bool>::min() == false);

        //  cv-qualified types share the unqualified specialization
        CHECK(minstd::numeric_limits<const volatile int>::max() == __INT_MAX__);
    }

    template <typename T>
    void check_floating_categories(const char *name)
    {
        using limits = minstd::numeric_limits<T>;

        CHECK_TEXT(limits::is_specialized, name);
        CHECK_TEXT(!limits::is_integer, name);
        CHECK_TEXT(limits::is_signed, name);
        CHECK_TEXT(!limits::is_exact, name);
        CHECK_TEXT(!limits::is_modulo, name);
        CHECK_TEXT(limits::radix == __FLT_RADIX__, name);
        CHECK_TEXT(limits::is_iec559, name); //  the targets minstd supports use IEEE 754 float and double
    }

    struct not_arithmetic
    {
    };

    TEST(DifferentialNumericsTests, FloatingLimitsMatchThePredefinedMacros)
    {
        check_floating_categories<float>("float");
        check_floating_categories<double>("double");

        CHECK(!minstd::numeric_limits<not_arithmetic>::is_specialized);

        CHECK(minstd::numeric_limits<float>::max() == __FLT_MAX__);
        CHECK(minstd::numeric_limits<float>::min() == __FLT_MIN__);
        CHECK(minstd::numeric_limits<float>::lowest() == -__FLT_MAX__);
        CHECK(minstd::numeric_limits<float>::epsilon() == __FLT_EPSILON__);
        CHECK_EQUAL(__FLT_MANT_DIG__, minstd::numeric_limits<float>::digits);
        CHECK_EQUAL(__FLT_DIG__, minstd::numeric_limits<float>::digits10);

        CHECK(minstd::numeric_limits<double>::max() == __DBL_MAX__);
        CHECK(minstd::numeric_limits<double>::min() == __DBL_MIN__);
        CHECK(minstd::numeric_limits<double>::lowest() == -__DBL_MAX__);
        CHECK(minstd::numeric_limits<double>::epsilon() == __DBL_EPSILON__);
        CHECK_EQUAL(__DBL_MANT_DIG__, minstd::numeric_limits<double>::digits);
        CHECK_EQUAL(__DBL_DIG__, minstd::numeric_limits<double>::digits10);

        CHECK(minstd::numeric_limits<long double>::max() == __LDBL_MAX__);
        CHECK(minstd::numeric_limits<long double>::min() == __LDBL_MIN__);
        CHECK(minstd::numeric_limits<long double>::epsilon() == __LDBL_EPSILON__);
        CHECK_EQUAL(__LDBL_MANT_DIG__, minstd::numeric_limits<long double>::digits);
    }

    //
    //  rotl / rotr against a bit-by-bit rotation
    //

    template <typename T>
    T reference_rotl(T x, int shift)
    {
        constexpr int N = static_cast<int>(sizeof(T) * 8);
        const int r = ((shift % N) + N) % N; //  a negative shift rotates the other way, as std::rotl specifies
        T result = 0;

        for (int bit = 0; bit < N; bit++)
        {
            if ((x >> bit) & 1)
            {
                result = static_cast<T>(result | (static_cast<T>(1) << ((bit + r) % N)));
            }
        }

        return result;
    }

    template <typename T>
    void check_rotations(const char *name, uint64_t seed)
    {
        minstd::xoroshiro128_plus_plus engine(seed);

        for (int i = 0; i < 20000; i++)
        {
            const T x = static_cast<T>(engine());
            const int shift = static_cast<int>(engine() % 400) - 200;

            if ((minstd::rotl(x, shift) != reference_rotl(x, shift)) || (minstd::rotr(x, shift) != reference_rotl(x, -shift)))
            {
                char text[160];
                snprintf(text, sizeof(text), "%s rotation of 0x%llx by %d differs", name, static_cast<unsigned long long>(x), shift);
                FAIL(text);
            }
        }
    }

    TEST(DifferentialNumericsTests, RotationsMatchABitwiseReference)
    {
        check_rotations<uint8_t>("uint8_t", 1);
        check_rotations<uint16_t>("uint16_t", 2);
        check_rotations<uint32_t>("uint32_t", 3);
        check_rotations<uint64_t>("uint64_t", 4);
        check_rotations<unsigned long long>("unsigned long long", 5);
    }

    //
    //  uniform_int_distribution: range and uniformity
    //

    //  An engine of only 16 bits, to exercise ranges wider than the engine
    struct narrow_engine
    {
        using result_type = uint16_t;

        static constexpr result_type min() { return 0; }
        static constexpr result_type max() { return 0xFFFF; }

        minstd::xoroshiro128_plus_plus inner{777};

        result_type operator()() { return static_cast<result_type>(inner() >> 48); }
    };

    //  Returns false (after printing why) on a value out of range or a failed chi-square.  Runs in a child process
    //      with a time limit, so a generator that loops forever fails the test instead of hanging the suite.

    template <typename engine_type, typename T>
    bool uniform(engine_type &engine, T a, T b, size_t buckets, const char *name)
    {
        //  Chi-square over `buckets` equal slices of [a, b]; the 0.1% critical value for up to 63 degrees of freedom
        //      is below 2 * buckets + 40, a loose bound that a biased generator still fails by a wide margin.

        minstd::uniform_int_distribution<T> distribution(a, b);

        constexpr size_t SAMPLES = 200000;
        size_t counts[64] = {};

        const long double span = static_cast<long double>(b) - static_cast<long double>(a) + 1;

        for (size_t i = 0; i < SAMPLES; i++)
        {
            const T v = distribution(engine);

            if ((v < a) || (v > b))
            {
                fprintf(stderr, "%s: %lld outside [%lld, %lld]\n", name, (long long)v, (long long)a, (long long)b);
                return false;
            }

            const size_t bucket = static_cast<size_t>((static_cast<long double>(v) - static_cast<long double>(a)) * buckets / span);
            counts[bucket < buckets ? bucket : buckets - 1]++;
        }

        const double expected = static_cast<double>(SAMPLES) / buckets;
        double chi_square = 0;

        for (size_t i = 0; i < buckets; i++)
        {
            const double d = static_cast<double>(counts[i]) - expected;
            chi_square += d * d / expected;
        }

        if (chi_square > 2.0 * buckets + 40)
        {
            fprintf(stderr, "%s: chi-square %.1f over %zu buckets\n", name, chi_square, buckets);
            return false;
        }

        return true;
    }

    template <typename engine_type, typename T>
    void check_uniformity(T a, T b, size_t buckets, const char *name)
    {
        const bool completed = minstd::pmr::test::runs_to_completion([&]
                                                                     {
                                                                         engine_type engine;
                                                                         if (!uniform(engine, a, b, buckets, name)) _exit(1); },
                                                                     20);
        CHECK_TEXT(completed, name);
    }

    struct wide_engine : minstd::xoroshiro128_plus_plus
    {
        wide_engine() : minstd::xoroshiro128_plus_plus(4242) {}
    };

    TEST(DifferentialNumericsTests, UniformIntIsInRangeAndUniform)
    {
        check_uniformity<wide_engine>(0, 9, 10, "[0, 9], 64-bit engine");
        check_uniformity<wide_engine>(-50, 49, 20, "[-50, 49], 64-bit engine");
        check_uniformity<wide_engine>(static_cast<uint64_t>(0), UINT64_MAX, 64, "full uint64 range");
        check_uniformity<wide_engine>(INT64_MIN, INT64_MAX, 64, "full int64 range");

        check_uniformity<narrow_engine>(0, 6, 7, "[0, 6], 16-bit engine");
        check_uniformity<narrow_engine>(static_cast<uint32_t>(0), static_cast<uint32_t>(1000000), 50, "[0, 1e6], 16-bit engine");
        check_uniformity<narrow_engine>(static_cast<uint64_t>(0), static_cast<uint64_t>(1) << 40, 64, "[0, 2^40], 16-bit engine");
        check_uniformity<narrow_engine>(static_cast<uint64_t>(0), UINT64_MAX - 1, 64, "[0, 2^64 - 2], 16-bit engine");
    }
}
