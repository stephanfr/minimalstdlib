// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//
//  Differential tests for formatting and parsing.  Random format specs and values are rendered by minstd::format and
//      by a reference built from the host's snprintf, which supplies the digits, with std::format's rules for sign,
//      prefix, fill, alignment and zero padding applied around them.  from_chars is compared with a direct reference
//      parser that accumulates in 128 bits.
//

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <charconv>
#include <fixed_string>
#include <format>
#include <limits>
#include <random>

#include <stdint.h>
#include <stdio.h>

namespace
{
    size_t strlen_like(const char *s);

    struct spec
    {
        char fill = 0;    //  0: default
        char align = 0;   //  '<', '>', '^' or 0
        char sign = 0;    //  '+', '-', ' ' or 0
        bool alt = false;
        bool zero = false;
        unsigned width = 0;
        char type = 0;    //  integers: d x X o b B or 0
        int precision = -1;

        void text(char *out, size_t size) const
        {
            char *p = out;
            *p++ = '{';
            *p++ = ':';

            if (align != 0)
            {
                if (fill != 0)
                {
                    *p++ = fill;
                }
                *p++ = align;
            }

            if (sign != 0)
            {
                *p++ = sign;
            }

            if (alt)
            {
                *p++ = '#';
            }

            if (zero)
            {
                *p++ = '0';
            }

            p += (width != 0) ? snprintf(p, size - (p - out), "%u", width) : 0;
            p += (precision >= 0) ? snprintf(p, size - (p - out), ".%d", precision) : 0;

            if (type != 0)
            {
                *p++ = type;
            }

            *p++ = '}';
            *p = 0;
        }
    };

    //  std::format's padding: zero fill goes between sign/prefix and digits (and is ignored with an explicit
    //      alignment or for inf/nan); otherwise the fill goes before, after or around the whole field.

    void apply_padding(char *out, size_t size, const char *sign, const char *prefix, const char *digits, const spec &s, char default_align, bool finite)
    {
        const size_t body = strlen_like(sign) + strlen_like(prefix) + strlen_like(digits);
        const size_t fill_count = (s.width > body) ? s.width - body : 0;

        size_t before = 0, zeros = 0, after = 0;

        if (s.zero && (s.align == 0) && finite)
        {
            zeros = fill_count;
        }
        else
        {
            const char align = (s.align != 0) ? s.align : default_align;

            if (align == '<')
            {
                after = fill_count;
            }
            else if (align == '^')
            {
                before = fill_count / 2;
                after = fill_count - before;
            }
            else
            {
                before = fill_count;
            }
        }

        const char fill = ((s.fill != 0) && (s.align != 0)) ? s.fill : ' ';

        size_t n = 0;
        auto put = [&](char c)
        {
            if (n + 1 < size)
            {
                out[n++] = c;
            }
        };

        for (size_t i = 0; i < before; i++) put(fill);
        for (const char *c = sign; *c; c++) put(*c);
        for (const char *c = prefix; *c; c++) put(*c);
        for (size_t i = 0; i < zeros; i++) put('0');
        for (const char *c = digits; *c; c++) put(*c);
        for (size_t i = 0; i < after; i++) put(fill);

        out[n] = 0;
    }

    size_t strlen_like(const char *s)
    {
        size_t n = 0;
        while (s[n] != 0)
        {
            n++;
        }
        return n;
    }

    bool same_text(const char *a, const char *b)
    {
        while ((*a != 0) && (*a == *b))
        {
            a++;
            b++;
        }
        return *a == *b;
    }

    void reference_integer(char *out, size_t size, bool negative, unsigned long long magnitude, const spec &s)
    {
        char digits[80];

        switch (s.type)
        {
        case 'x': snprintf(digits, sizeof(digits), "%llx", magnitude); break;
        case 'X': snprintf(digits, sizeof(digits), "%llX", magnitude); break;
        case 'o': snprintf(digits, sizeof(digits), "%llo", magnitude); break;
        case 'b':
        case 'B':
        {
            char reversed[80];
            size_t n = 0;
            unsigned long long v = magnitude;
            do
            {
                reversed[n++] = static_cast<char>('0' + (v & 1));
                v >>= 1;
            } while (v != 0);

            for (size_t i = 0; i < n; i++)
            {
                digits[i] = reversed[n - 1 - i];
            }
            digits[n] = 0;
            break;
        }
        default: snprintf(digits, sizeof(digits), "%llu", magnitude); break;
        }

        const char *sign = negative ? "-" : (s.sign == '+') ? "+" : (s.sign == ' ') ? " " : "";

        const char *prefix = "";
        if (s.alt)
        {
            switch (s.type)
            {
            case 'x': prefix = "0x"; break;
            case 'X': prefix = "0X"; break;
            case 'b': prefix = "0b"; break;
            case 'B': prefix = "0B"; break;
            case 'o': prefix = (magnitude != 0) ? "0" : ""; break;
            default: break;
            }
        }

        apply_padding(out, size, sign, prefix, digits, s, '>', true);
    }

    spec random_integer_spec(minstd::xoroshiro128_plus_plus &engine)
    {
        static const char types[] = {0, 'd', 'x', 'X', 'o', 'b', 'B'};
        static const char aligns[] = {0, 0, '<', '>', '^'};
        static const char signs[] = {0, 0, '+', '-', ' '};
        static const char fills[] = {0, '*', '_', '0', '<'};

        spec s;
        s.type = types[engine() % sizeof(types)];
        s.align = aligns[engine() % sizeof(aligns)];
        s.fill = (s.align != 0) ? fills[engine() % sizeof(fills)] : 0;
        s.sign = signs[engine() % sizeof(signs)];
        s.alt = (engine() % 3) == 0;
        s.zero = (s.align == 0) && ((engine() % 3) == 0);
        s.width = ((engine() % 2) == 0) ? 0 : static_cast<unsigned>(engine() % 30);

        return s;
    }

    template <typename T>
    T random_value(minstd::xoroshiro128_plus_plus &engine)
    {
        switch (engine() % 6)
        {
        case 0: return minstd::numeric_limits<T>::min();
        case 1: return minstd::numeric_limits<T>::max();
        case 2: return static_cast<T>(0);
        case 3: return static_cast<T>(engine() % 100);
        default: return static_cast<T>(engine());
        }
    }

    template <typename T>
    void check_integers(const char *type_name, uint64_t seed)
    {
        minstd::xoroshiro128_plus_plus engine(seed);
        minstd::fixed_string<128> buffer;

        for (int i = 0; i < 20000; i++)
        {
            const spec s = random_integer_spec(engine);
            const T value = random_value<T>(engine);

            char format_text[48];
            s.text(format_text, sizeof(format_text));

            const bool negative = value < 0;
            const unsigned long long magnitude = negative ? (0ULL - static_cast<unsigned long long>(static_cast<long long>(value)))
                                                          : static_cast<unsigned long long>(value);

            char expected[160];
            reference_integer(expected, sizeof(expected), negative, magnitude, s);

            minstd::format(buffer, format_text, value);

            if (!same_text(buffer.c_str(), expected))
            {
                char text[400];
                snprintf(text, sizeof(text), "%s %s of %lld: got \"%s\" expected \"%s\"", type_name, format_text,
                         static_cast<long long>(value), buffer.c_str(), expected);
                FAIL(text);
            }
        }
    }

    TEST_GROUP(DifferentialFormatTests){};

    TEST(DifferentialFormatTests, IntegersMatchTheStdRules)
    {
        check_integers<int8_t>("int8_t", 1);
        check_integers<uint8_t>("uint8_t", 2);
        check_integers<int16_t>("int16_t", 3);
        check_integers<uint16_t>("uint16_t", 4);
        check_integers<int32_t>("int32_t", 5);
        check_integers<uint32_t>("uint32_t", 6);
        check_integers<int64_t>("int64_t", 7);
        check_integers<uint64_t>("uint64_t", 8);
    }

    //  Magnitudes from 1e-6 to below 2^64 (above it only 17 significant digits are kept, the documented limit), with
    //      ties and near-ties made common by short decimal mantissas, and full 53-bit mantissas for the rest

    double wide_magnitude(minstd::xoroshiro128_plus_plus &engine)
    {
        while (true)
        {
            const bool short_mantissa = (engine() % 2) == 0;
            const int decimal_exponent = static_cast<int>(engine() % 25) - 6;
            const uint64_t mantissa = short_mantissa ? (engine() % 100000) : (engine() >> 11);

            double value = static_cast<double>(mantissa) / (short_mantissa ? 1000.0 : 9007199254740992.0);

            for (int e = 0; e < decimal_exponent; e++)
            {
                value *= 10;
            }

            for (int e = 0; e > decimal_exponent; e--)
            {
                value /= 10;
            }

            if (value < 1.8e19)
            {
                return value;
            }
        }
    }

    TEST(DifferentialFormatTests, FloatsWithSignsAndPaddingMatchPrintf)
    {
        minstd::xoroshiro128_plus_plus engine(99);
        minstd::fixed_string<128> buffer;

        static const char aligns[] = {0, 0, '<', '>', '^'};
        static const char signs[] = {0, 0, '+', '-', ' '};

        for (int i = 0; i < 50000; i++)
        {
            spec s;
            s.align = aligns[engine() % sizeof(aligns)];
            s.fill = (s.align != 0) ? "*_#"[engine() % 3] : 0;
            s.sign = signs[engine() % sizeof(signs)];
            s.zero = (s.align == 0) && ((engine() % 3) == 0);
            s.width = ((engine() % 2) == 0) ? 0 : static_cast<unsigned>(engine() % 30);
            s.precision = static_cast<int>(engine() % 20); //  up to 19 decimals are exact, as printf gives them

            double value;
            switch (engine() % 12)
            {
            case 0: value = __builtin_inf(); break;
            case 1: value = -__builtin_inf(); break;
            case 2: value = __builtin_nan(""); break;
            case 3: value = -0.0; break;
            case 4: value = static_cast<double>(engine() % 1000) / 8.0; break; //  exact ties
            case 5:
            case 6:
                value = wide_magnitude(engine);
                break;
            default:
                value = static_cast<double>(static_cast<int64_t>(engine() % 2000000) - 1000000) / 997.0;
                break;
            }

            if ((engine() % 2) == 0)
            {
                value = -value;
            }

            char format_text[48];
            s.text(format_text, sizeof(format_text));

            //  printf supplies the digits (and "-" for negatives, "inf", "nan"); the std rules add the rest

            char printed[96];
            snprintf(printed, sizeof(printed), "%.*f", s.precision, value);

            const bool finite = (value == value) && (value != __builtin_inf()) && (value != -__builtin_inf());
            const bool negative = __builtin_signbit(value);
            const char *digits = (printed[0] == '-') ? printed + 1 : printed;
            const char *sign = negative ? "-" : (s.sign == '+') ? "+" : (s.sign == ' ') ? " " : "";

            char expected[160];
            apply_padding(expected, sizeof(expected), sign, "", digits, s, '>', finite);

            minstd::format(buffer, format_text, value);

            if (!same_text(buffer.c_str(), expected))
            {
                char text[400];
                snprintf(text, sizeof(text), "%s of %.17g: got \"%s\" expected \"%s\"", format_text, value, buffer.c_str(), expected);
                FAIL(text);
            }
        }
    }

    //
    //  from_chars against a 128-bit accumulating reference
    //

    template <typename T>
    void reference_from_chars(const char *first, const char *last, int base, const char *&ptr, int &outcome, T &value)
    {
        //  outcome: 0 success, 1 invalid argument, 2 out of range

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

        if (overflow)
        {
            outcome = 2;
            return;
        }

        outcome = 0;
        value = negative ? static_cast<T>(-static_cast<__int128>(accumulated)) : static_cast<T>(accumulated);
    }

    template <typename T>
    void check_from_chars(const char *type_name, uint64_t seed)
    {
        minstd::xoroshiro128_plus_plus engine(seed);
        static const char alphabet[] = "0123456789abcdefzZ-+ x";

        for (int i = 0; i < 50000; i++)
        {
            const int base = ((engine() % 3) == 0) ? static_cast<int>(2 + engine() % 35) : 10;

            char text[40];
            const size_t length = engine() % 30;
            for (size_t j = 0; j < length; j++)
            {
                //  Mostly digits valid in base 10, with some letters, signs and stray characters
                text[j] = ((engine() % 4) != 0) ? static_cast<char>('0' + engine() % 10) : alphabet[engine() % (sizeof(alphabet) - 1)];
            }
            text[length] = 0;

            const T sentinel = static_cast<T>(0x5A);

            T got = sentinel;
            const auto result = minstd::from_chars(text, text + length, got, base);

            T want = sentinel;
            const char *want_ptr = nullptr;
            int want_outcome = 0;
            reference_from_chars<T>(text, text + length, base, want_ptr, want_outcome, want);

            const int got_outcome = (result.ec == minstd::errc{}) ? 0 : (result.ec == minstd::errc::invalid_argument) ? 1 : 2;

            if ((got_outcome != want_outcome) || (result.ptr != want_ptr) || (got != want))
            {
                char message[300];
                snprintf(message, sizeof(message), "%s from_chars(\"%s\", base %d): outcome %d ptr +%ld value %lld; reference %d +%ld %lld",
                         type_name, text, base, got_outcome, static_cast<long>(result.ptr - text), static_cast<long long>(got),
                         want_outcome, static_cast<long>(want_ptr - text), static_cast<long long>(want));
                FAIL(message);
            }
        }
    }

    TEST(DifferentialFormatTests, FromCharsMatchesAReferenceParser)
    {
        check_from_chars<int8_t>("int8_t", 11);
        check_from_chars<uint8_t>("uint8_t", 12);
        check_from_chars<int16_t>("int16_t", 13);
        check_from_chars<uint16_t>("uint16_t", 14);
        check_from_chars<int32_t>("int32_t", 15);
        check_from_chars<uint32_t>("uint32_t", 16);
        check_from_chars<int64_t>("int64_t", 17);
        check_from_chars<uint64_t>("uint64_t", 18);
    }
}
