// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <fixed_string>
#include <format>
#include <format_formatters>

#include <ctype.h>
#include <stdio.h>

#include <random>

#include "../shared/process_isolation.h"

static unsigned __seven_bit_ascii_ctype_table[] = {
    _C, _C, _C, _C, _C, _C, _C, _C,
    _C, _C | _S | _B, _C | _S, _C | _S, _C | _S, _C | _S, _C, _C,
    _C, _C, _C, _C, _C, _C, _C, _C,
    _C, _C, _C, _C, _C, _C, _C, _C,
    _S | _B, _P, _P, _P, _P, _P, _P, _P,
    _P, _P, _P, _P, _P, _P, _P, _P,
    _N | _X, _N | _X, _N | _X, _N | _X, _N | _X, _N | _X, _N | _X, _N | _X,
    _N | _X, _N | _X, _P, _P, _P, _P, _P, _P,
    _P, _U | _X, _U | _X, _U | _X, _U | _X, _U | _X, _U | _X, _U,
    _U, _U, _U, _U, _U, _U, _U, _U,
    _U, _U, _U, _U, _U, _U, _U, _U,
    _U, _U, _U, _P, _P, _P, _P, _P,
    _P, _L | _X, _L | _X, _L | _X, _L | _X, _L | _X, _L | _X, _L,
    _L, _L, _L, _L, _L, _L, _L, _L,
    _L, _L, _L, _L, _L, _L, _L, _L,
    _L, _L, _L, _P, _P, _P, _P, _C};

extern unsigned *__get_ctype_table()
{
    return __seven_bit_ascii_ctype_table;
}

namespace
{

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (FormatTests)
    {
    };
#pragma GCC diagnostic pop

    TEST(FormatTests, EdgeCases)
    {
        minstd::fixed_string<256> formatted_string;

        //  Empty format string and no arguments

        STRCMP_EQUAL("", minstd::format(formatted_string, "").c_str());
        STRCMP_EQUAL("This is a test: \n", minstd::format(formatted_string, "This is a test: \n").c_str());

        //  Case of no closing brace

        STRCMP_EQUAL("This is a test: ", minstd::format(formatted_string, "This is a test: {\n", (uint32_t)423).c_str());

        //  Format string containing only a colon

        STRCMP_EQUAL("This is a test: 423\n", minstd::format(formatted_string, "This is a test: {:}\n", (uint32_t)423).c_str());
        
        //  Alignment without width
        
        STRCMP_EQUAL("This is a test: of alignment   \n", minstd::format(formatted_string, "This is a test: {:<} {:12}\n", "of", "alignment").c_str());

        //  Alt has no effect on integers without a type specifier

        STRCMP_EQUAL("No Effect: 48", minstd::format(formatted_string, "No Effect: {:#}", (int16_t)48).c_str());

        //  Zero fill has not effect without a width and is ignored when an alignment character is supplied

        STRCMP_EQUAL("This is a test: 423 of no effect zero prefixing", minstd::format(formatted_string, "This is a test: {:0} {}", 423, "of no effect zero prefixing").c_str());
        STRCMP_EQUAL("This is a test:    423 of no effect zero prefixing", minstd::format(formatted_string, "This is a test: {:>06} {}", 423, "of no effect zero prefixing").c_str());

        //  Menaingless type specifier

        STRCMP_EQUAL("Meaningless type: 47", minstd::format(formatted_string, "Meaningless type: {:q}", 47).c_str());
    }

    TEST(FormatTests, Chars)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: of format()", minstd::format(formatted_string, "This is a test: {}{} format()", 'o', 'f').c_str());
    }

    TEST(FormatTests, CharStrings)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: of format()", minstd::format(formatted_string, "This is a test: {} {}", "of", "format()").c_str());
    }

    TEST(FormatTests, STDStrings)
    {
        minstd::fixed_string<256> formatted_string;

        const minstd::fixed_string<64> arg1("that");

        STRCMP_EQUAL("This is a test: that worked\n", minstd::format(formatted_string, "This is a test: {} {}\n", arg1, minstd::fixed_string<64>("worked")));
    }

    TEST(FormatTests, 8BitSignedAndUnsignedInts)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: -123", minstd::format(formatted_string, "This is a test: {}", (int8_t)-123).c_str());
        STRCMP_EQUAL("This is a test: 255\n", minstd::format(formatted_string, "This is a test: {}\n", (uint8_t)255).c_str());
    }

    TEST(FormatTests, 16BitSignedAndUnsignedInts)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: -32000", minstd::format(formatted_string, "This is a test: {}", (int16_t)-32000).c_str());
        STRCMP_EQUAL("This is a test: 65535\n", minstd::format(formatted_string, "This is a test: {}\n", (uint16_t)65535).c_str());
    }

    TEST(FormatTests, 32BitSignedAndUnsignedInts)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: -123", minstd::format(formatted_string, "This is a test: {}", (int32_t)-123).c_str());
        STRCMP_EQUAL("This is a test: 423\n", minstd::format(formatted_string, "This is a test: {}\n", (uint32_t)423).c_str());
    }

    TEST(FormatTests, 64BitSignedAndUnsignedInts)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: -123", minstd::format(formatted_string, "This is a test: {}", (int64_t)-123).c_str());
        STRCMP_EQUAL("This is a test: 423\n", minstd::format(formatted_string, "This is a test: {}\n", (uint64_t)423).c_str());
    }

    TEST(FormatTests, Bools)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: true false\n", minstd::format(formatted_string, "This is a test: {} {}\n", true, false).c_str());
    }

    TEST(FormatTests, Floats)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: 12.3400 -56.7800\n", minstd::format(formatted_string, "This is a test: {} {}\n", (float)12.34, (float)-56.78).c_str());
        STRCMP_EQUAL("This is a test: 12.0000 -56.0000\n", minstd::format(formatted_string, "This is a test: {} {}\n", (float)12, (float)-56).c_str());
    }

    TEST(FormatTests, Doubles)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: 12.3400 -56.7800\n", minstd::format(formatted_string, "This is a test: {} {}\n", (double)12.34, (double)-56.78).c_str());
    }

    TEST(FormatTests, Pointer)
    {
        minstd::fixed_string<256> formatted_string;
        void *pointer = (void *)0x00000000000a3348;

        STRNCMP_EQUAL("This is a test: 0X00000000000A3348", minstd::format(formatted_string, "This is a test: {}", pointer).c_str(), 32);
        STRNCMP_EQUAL("This is a test: 0X00000000000A3348", minstd::format(formatted_string, "This is a test: {:p}", pointer).c_str(), 32);
    }

    TEST(FormatTests, PositionalArguments)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: 6789 423 6789 6789\n", minstd::format(formatted_string, "This is a test: {1} {0} {1} {1}\n", (uint32_t)423, (uint32_t)6789, "Should not be this argument").c_str());
    }

    TEST(FormatTests, AlignAndFill)
    {
        minstd::fixed_string<256> formatted_string;

        //  String alignment and fill

        STRCMP_EQUAL("This is a test: of     alignment   \n", minstd::format(formatted_string, "This is a test: {:<6} {:12}\n", "of", "alignment").c_str());
        STRCMP_EQUAL("This is a test:     of alignment   \n", minstd::format(formatted_string, "This is a test: {:>6} {:<12}\n", "of", "alignment").c_str());
        STRCMP_EQUAL("This is a test:   of   -alignment--\n", minstd::format(formatted_string, "This is a test: {:^6} {:-^12}\n", "of", "alignment").c_str());
        STRCMP_EQUAL("This is a test: **of** alignment\n", minstd::format(formatted_string, "This is a test: {:*^6} {:-^6}\n", "of", "alignment").c_str());

        //  Integer alignment and fill

        STRCMP_EQUAL("This is a test: 423***    6789\n", minstd::format(formatted_string, "This is a test: {:*<6} {:7}\n", (uint32_t)423, (int32_t)6789).c_str());
        STRCMP_EQUAL("This is a test: *423**  6789\n", minstd::format(formatted_string, "This is a test: {:*^6} {:>5}\n", (uint32_t)423, (int32_t)6789).c_str());
        STRCMP_EQUAL("This is a test: 7654     \n", minstd::format(formatted_string, "This is a test: {:<9}\n", (uint32_t)7654).c_str());
        STRCMP_EQUAL("This is a test: 50000 00600 00007", minstd::format(formatted_string, "This is a test: {:0<5} {:0^5} {:0>5}", 5, 6, 7).c_str());
    }

    TEST(FormatTests, Sign)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: +423 of sign specification", minstd::format(formatted_string, "This is a test: {:+} {}", 423, "of sign specification").c_str());
        STRCMP_EQUAL("This is a test:  423 of sign specification", minstd::format(formatted_string, "This is a test: {: } {}", 423, "of sign specification").c_str());
        STRCMP_EQUAL("This is a test: 423 of sign specification", minstd::format(formatted_string, "This is a test: {:-} {}", 423, "of sign specification").c_str());

        STRCMP_EQUAL("This is a test:   +423 of sign specification", minstd::format(formatted_string, "This is a test: {:+6} {}", 423, "of sign specification").c_str());
        STRCMP_EQUAL("This is a test:    423 of sign specification", minstd::format(formatted_string, "This is a test: {: 6} {}", 423, "of sign specification").c_str());
        STRCMP_EQUAL("This is a test:    423 of sign specification", minstd::format(formatted_string, "This is a test: {:-6} {}", 423, "of sign specification").c_str());

        STRCMP_EQUAL("This is a test: +00423 of sign specification", minstd::format(formatted_string, "This is a test: {:+06d} {}", 423, "of sign specification").c_str());
    }

    TEST(FormatTests, ZeroPrefix)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("This is a test: 000423 of zero prefixing", minstd::format(formatted_string, "This is a test: {:06} {}", 423, "of zero prefixing").c_str());
    
        STRCMP_EQUAL("0x00a02082", minstd::format(formatted_string, "{:#010x}", 0x00a02082).c_str());
    }

    TEST(FormatTests, Precision)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("Floating Point Precision Test: 12.3400 -56.78000\n", minstd::format(formatted_string, "Floating Point Precision Test: {:.4} {:.5}\n", (float)12.34, (double)-56.78).c_str());
        STRCMP_EQUAL("Floating Point Formatting Test: |12.3400  | |  -56.78000|\n", minstd::format(formatted_string, "Floating Point Formatting Test: |{:<9.4}| |{:>11.5}|\n", (float)12.34, (double)-56.78).c_str());
    }

    TEST(FormatTests, IntegerFormatting)
    {
        minstd::fixed_string<256> formatted_string;

        //  Hexadecimal formatting

        STRCMP_EQUAL("Hex Format: 2f", minstd::format(formatted_string, "Hex Format: {:x}", (int16_t)47).c_str());
        STRCMP_EQUAL("Hex Format: 0x30", minstd::format(formatted_string, "Hex Format: {:#x}", (int16_t)48).c_str());
        STRCMP_EQUAL("Hex Format: 2F", minstd::format(formatted_string, "Hex Format: {:X}", (uint16_t)47).c_str());
        STRCMP_EQUAL("Hex Format: 0X2E", minstd::format(formatted_string, "Hex Format: {:#X}", (uint16_t)46).c_str());

        STRCMP_EQUAL("Hex Format: +0X2E", minstd::format(formatted_string, "Hex Format: {:+#X}", (int16_t)46).c_str());
        STRCMP_EQUAL("Hex Format:  0X2E", minstd::format(formatted_string, "Hex Format: {: #X}", (int16_t)46).c_str());
        STRCMP_EQUAL("Hex Format: 0X2E", minstd::format(formatted_string, "Hex Format: {:-#X}", (int16_t)46).c_str());
        STRCMP_EQUAL("Hex Format: -2f", minstd::format(formatted_string, "Hex Format: {:x}", (int16_t)-47).c_str());
        STRCMP_EQUAL("Hex Format: -0x30", minstd::format(formatted_string, "Hex Format: {:#x}", (int16_t)-48).c_str());
        STRCMP_EQUAL("Hex Format: -0x30", minstd::format(formatted_string, "Hex Format: {:-#x}", (int16_t)-48).c_str());

        //  Binary formatting

        STRCMP_EQUAL("Binary Format: 101111", minstd::format(formatted_string, "Binary Format: {:b}", (int16_t)47).c_str());
        STRCMP_EQUAL("Binary Format: 0b110000", minstd::format(formatted_string, "Binary Format: {:#b}", (int16_t)48).c_str());
        STRCMP_EQUAL("Binary Format: 101111", minstd::format(formatted_string, "Binary Format: {:B}", (uint16_t)47).c_str());
        STRCMP_EQUAL("Binary Format: 0B101110", minstd::format(formatted_string, "Binary Format: {:#B}", (uint16_t)46).c_str());

        STRCMP_EQUAL("Binary Format: -101111", minstd::format(formatted_string, "Binary Format: {:b}", (int16_t)-47).c_str());
        STRCMP_EQUAL("Binary Format: -0b110000", minstd::format(formatted_string, "Binary Format: {:#b}", (int16_t)-48).c_str());

        //  Octal formatting

        STRCMP_EQUAL("Octal Format: 57", minstd::format(formatted_string, "Octal Format: {:o}", (int16_t)47).c_str());
        STRCMP_EQUAL("Octal Format: 060", minstd::format(formatted_string, "Octal Format: {:#o}", (uint16_t)48).c_str());

        STRCMP_EQUAL("Octal Format: -57", minstd::format(formatted_string, "Octal Format: {:o}", (int16_t)-47).c_str());
        STRCMP_EQUAL("Octal Format: -060", minstd::format(formatted_string, "Octal Format: {:#o}", (int16_t)-48).c_str());
    }

    TEST(FormatTests, Errors)
    {
        minstd::fixed_string<256> formatted_string;

        //  Format string with an invalid positional argument

        STRCMP_EQUAL("This is a test: {Invalid format string: '.5'}\n", minstd::format(formatted_string, "This is a test: {.5}\n", (uint32_t)423).c_str());

        //  Argument Id out of range

        STRCMP_EQUAL("This is a test: {Argument position out of range} of            \n", minstd::format(formatted_string, "This is a test: {2:<6} {:14}\n", "of", "bad alignment").c_str());

        //  Two fill characters before alignment symbol or fill of '{' or '}'

        STRCMP_EQUAL("This is a test: {Invalid format string: ':**<6'} bad alignment \n", minstd::format(formatted_string, "This is a test: {:**<6} {:14}\n", "of", "bad alignment").c_str());
        STRCMP_EQUAL("This is a test: {Invalid format string: ':{<6'} bad alignment \n", minstd::format(formatted_string, "This is a test: {:{<6} {:14}\n", "of", "bad alignment").c_str());
        //  '}' cannot be a fill character: "{:}" is an empty spec, and "<6}" that follows is literal text

        STRCMP_EQUAL("This is a test: of<6} bad alignment \n", minstd::format(formatted_string, "This is a test: {:}<6} {:14}\n", "of", "bad alignment").c_str());
    }

    TEST(FormatTests, MostNegativeIntegers)
    {
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("-9223372036854775808", minstd::format(formatted_string, "{}", (int64_t)INT64_MIN).c_str());
        STRCMP_EQUAL("-2147483648", minstd::format(formatted_string, "{}", (int32_t)INT32_MIN).c_str());
        STRCMP_EQUAL("-32768", minstd::format(formatted_string, "{}", (int16_t)INT16_MIN).c_str());
        STRCMP_EQUAL("-128", minstd::format(formatted_string, "{}", (int8_t)INT8_MIN).c_str());
    }

    TEST(FormatTests, ClosingBraceFollowedByAlignmentCharacter)
    {
        //  Any '}' followed by '<', '>' or '^' was rejected as a fill character, so "<{}>" could not be formatted.
        minstd::fixed_string<256> formatted_string;

        STRCMP_EQUAL("<abc>", minstd::format(formatted_string, "<{}>", "abc").c_str()); //  before: <{Invalid format string: '}>'}
        STRCMP_EQUAL("x=1>0", minstd::format(formatted_string, "x={}>0", (int32_t)1).c_str());
        STRCMP_EQUAL("[  7]^", minstd::format(formatted_string, "[{:>3}]^", (int32_t)7).c_str());

        //  "{:}" is an empty spec, so a following '<' is literal text, as in std::format
        STRCMP_EQUAL("a<6} x", minstd::format(formatted_string, "{:}<6} {}", "a", "x").c_str());
        STRCMP_EQUAL("x<br>", minstd::format(formatted_string, "{:}<br>", "x").c_str()); //  before: rejected
    }

    TEST(FormatTests, SignedLeftAndCenterAlignmentHonourWidth)
    {
        //  The trailing-fill pass counted the sign twice (it is already in the buffer by then).
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("-1   ", minstd::format(buffer, "{:<5}", (int32_t)-1).c_str()); //  before: "-1  "
        STRCMP_EQUAL(" -1  ", minstd::format(buffer, "{:^5}", (int32_t)-1).c_str());
        STRCMP_EQUAL("+7   ", minstd::format(buffer, "{:<+5}", (int32_t)7).c_str());
        STRCMP_EQUAL("-2.50  ", minstd::format(buffer, "{:<7.2}", -2.5).c_str());
        STRCMP_EQUAL("7    ", minstd::format(buffer, "{:<5}", (int32_t)7).c_str()); //  unsigned case unchanged
        STRCMP_EQUAL("   -1", minstd::format(buffer, "{:>5}", (int32_t)-1).c_str());
    }

    TEST(FormatTests, FloatingPointEdgeCases)
    {
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("0.5000", minstd::format(buffer, "{}", 0.5).c_str());                   //  before: ".5000"
        STRCMP_EQUAL("-0.2500", minstd::format(buffer, "{}", -0.25).c_str());                //  before: "-.2500"
        STRCMP_EQUAL("0.5000000000", minstd::format(buffer, "{:.10}", 0.5).c_str());         //  before: zeros before the 5
        STRCMP_EQUAL("1.2500000000", minstd::format(buffer, "{:.10}", 1.25).c_str());
        STRCMP_EQUAL("3", minstd::format(buffer, "{:.0}", 3.0).c_str());                     //  before: "3."
        STRCMP_EQUAL("12.75", minstd::format(buffer, "{:.2}", 12.75).c_str());
    }

    TEST(FormatTests, EscapedBraces)
    {
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("{literal}", minstd::format(buffer, "{{literal}}").c_str());
        STRCMP_EQUAL("{7}", minstd::format(buffer, "{{{}}}", (int32_t)7).c_str());
        STRCMP_EQUAL("a{b}c 1 }", minstd::format(buffer, "a{{b}}c {} }}", (int32_t)1).c_str());
        STRCMP_EQUAL("end{", minstd::format(buffer, "end{{").c_str());
    }

    TEST(FormatTests, FloatsRoundAtThePrecision)
    {
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("1.15", minstd::format(buffer, "{:.2}", 1.15).c_str());              //  before: "1.14"
        STRCMP_EQUAL("2.0000", minstd::format(buffer, "{}", 1.99999).c_str());            //  before: "1.9999"
        STRCMP_EQUAL("-2.0000", minstd::format(buffer, "{}", -1.99999).c_str());
        STRCMP_EQUAL("10", minstd::format(buffer, "{:.0}", 9.7).c_str());
    }

    TEST(FormatTests, FloatsOutsideTheIntegerRange)
    {
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("inf -inf", minstd::format(buffer, "{} {}", __builtin_inf(), -__builtin_inf()).c_str());
        STRCMP_EQUAL("nan", minstd::format(buffer, "{}", __builtin_nan("")).c_str());
        STRCMP_EQUAL("  inf", minstd::format(buffer, "{:5}", __builtin_inff()).c_str());
        STRCMP_EQUAL("2000000000000000.0000", minstd::format(buffer, "{}", 2e15).c_str());
        STRCMP_EQUAL("100000000000000000000.00", minstd::format(buffer, "{:.2}", 1e20).c_str());
        STRCMP_EQUAL("-30000000000000000000000000000000.0", minstd::format(buffer, "{:.1}", -3e31).c_str());
    }

    TEST(FormatTests, FullBufferKeepsTheSignAndLeadingDigits)
    {
        minstd::fixed_string<8> buffer;

        //  Before the fix the digits were written least significant first, so a full buffer kept "87654321"

        STRCMP_EQUAL("-1234567", minstd::format(buffer, "{}", (int32_t)-12345678).c_str());
        STRCMP_EQUAL("0xabcdef", minstd::format(buffer, "{:#x}", (uint64_t)0xabcdef0123ULL).c_str());
    }

    TEST(FormatTests, UnsignedHonoursTheSignOption)
    {
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("+5", minstd::format(buffer, "{:+}", (uint32_t)5).c_str());           //  before: "5"
        STRCMP_EQUAL(" 5", minstd::format(buffer, "{: }", (uint64_t)5).c_str());
        STRCMP_EQUAL("+0x0a", minstd::format(buffer, "{:+#05x}", (uint8_t)10).c_str());
    }

    TEST(FormatTests, AlternateOctalZero)
    {
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("0", minstd::format(buffer, "{:#o}", (uint32_t)0).c_str());           //  before: "00"
        STRCMP_EQUAL("010", minstd::format(buffer, "{:#o}", (uint32_t)8).c_str());
        STRCMP_EQUAL("0x0", minstd::format(buffer, "{:#x}", (uint32_t)0).c_str());
    }

    TEST(FormatTests, FillThatIsAnAlignmentCharacter)
    {
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("ab<<<", minstd::format(buffer, "{:<<5}", "ab").c_str());
        STRCMP_EQUAL(">>>ab", minstd::format(buffer, "{:>>5}", "ab").c_str());
        STRCMP_EQUAL("^^7^^", minstd::format(buffer, "{:^^5}", (int32_t)7).c_str());
    }

    TEST(FormatTests, StringPrecisionTruncates)
    {
        minstd::fixed_string<128> buffer;
        minstd::fixed_string<16> hello("hello");

        STRCMP_EQUAL("he", minstd::format(buffer, "{:.2}", "hello").c_str());              //  before: "hello"
        STRCMP_EQUAL("hel  |", minstd::format(buffer, "{:5.3}|", hello).c_str());
        STRCMP_EQUAL("hello", minstd::format(buffer, "{:.9}", "hello").c_str());
    }

    TEST(FormatTests, OversizedIdsAndWidthsAreInvalid)
    {
        minstd::fixed_string<128> buffer;

        //  4294967296 wrapped to argument 0 before the fix

        STRCMP_EQUAL("{Invalid format string: '4294967296'}", minstd::format(buffer, "{4294967296}", (int32_t)1).c_str());
        STRCMP_EQUAL("{Invalid format string: ':4294967297'}", minstd::format(buffer, "{:4294967297}", (int32_t)1).c_str());
        STRCMP_EQUAL("{Invalid format string: ':.99999999999'}", minstd::format(buffer, "{:.99999999999}", 1.0).c_str());
    }

    TEST(FormatTests, FloatsKeepTheirFractionUpTo2To64)
    {
        minstd::fixed_string<128> buffer;

        //  Values above 1.8e19 / 10^precision took a path that dropped the fraction and rounded the integer part

        STRCMP_EQUAL("200000000000.75000000", minstd::format(buffer, "{:.8}", 200000000000.75).c_str()); //  before: ...001.00000000
        STRCMP_EQUAL("100000000000000.500000", minstd::format(buffer, "{:.6}", 1e14 + 0.5).c_str());
        STRCMP_EQUAL("2251799813685248.5000", minstd::format(buffer, "{}", 2251799813685248.5).c_str());
        STRCMP_EQUAL("18446744073709549568.00", minstd::format(buffer, "{:.2}", 18446744073709549568.0).c_str()); //  largest double below 2^64
    }

    TEST(FormatTests, FloatsComputeUpTo19ExactDecimals)
    {
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("0.3333333333", minstd::format(buffer, "{:.10}", 1.0 / 3).c_str()); //  before: 0.3333333300

        //  The digits are those of the exact binary value, as printf and std::format give them

        STRCMP_EQUAL("0.10000000000000001", minstd::format(buffer, "{:.17}", 0.1).c_str());
        STRCMP_EQUAL("0.29999999999999999", minstd::format(buffer, "{:.17}", 0.3).c_str());
        STRCMP_EQUAL("0.12345678901234499740", minstd::format(buffer, "{:.20}", 0.123456789012345).c_str()); //  19 exact, then 0
    }

    TEST(FormatTests, FloatsRoundNearTiesByTheExactValue)
    {
        //  The tie test used a rounded double product, so values just above or below a tie rounded the wrong way

        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("0.01", minstd::format(buffer, "{:.2}", 0.005).c_str()); //  just above the tie; was 0.00
        STRCMP_EQUAL("0.001", minstd::format(buffer, "{:.3}", 0.0005).c_str());
        STRCMP_EQUAL("0.03", minstd::format(buffer, "{:.2}", 0.025).c_str());  //  just above; was 0.02
        STRCMP_EQUAL("0.01", minstd::format(buffer, "{:.2}", 0.015).c_str());  //  just below; was 0.02
    }

    TEST(FormatTests, FloatsMatchPrintf)
    {
        //  Differential check against the host's printf, which rounds the exact binary value correctly

        minstd::fixed_string<96> buffer;

        minstd::xoroshiro128_plus_plus engine(20261016);

        size_t mismatches = 0;
        char expected[96];
        char first_mismatch[160] = "";

        for (int i = 0; i < 100000; i++)
        {
            //  Magnitudes from 1e-6 to 1e18, with ties and near-ties made common by short decimal mantissas

            const int decimal_exponent = static_cast<int>(engine() % 25) - 6;
            const uint64_t mantissa = (i % 2 == 0) ? (engine() % 100000) : (engine() >> 11);

            double value = static_cast<double>(mantissa) / ((i % 2 == 0) ? 1000.0 : 9007199254740992.0);

            for (int e = 0; e < decimal_exponent; e++)
            {
                value *= 10;
            }

            for (int e = 0; e > decimal_exponent; e--)
            {
                value /= 10;
            }

            if (value >= 1.8e19)
            {
                continue; //  above 2^64 only the leading 17 digits are kept (the documented limit)
            }

            if (engine() & 1)
            {
                value = -value;
            }

            const unsigned precision = static_cast<unsigned>(engine() % 20);

            snprintf(expected, sizeof(expected), "%.*f", static_cast<int>(precision), value);

            char spec_text[16];
            snprintf(spec_text, sizeof(spec_text), "{:.%u}", precision);

            minstd::format(buffer, spec_text, value);

            if (!(buffer == expected))
            {
                if (mismatches++ == 0)
                {
                    snprintf(first_mismatch, sizeof(first_mismatch), "%.17g at .%u: got %s expected %s", value, precision, buffer.c_str(), expected);
                }
            }
        }

        if (mismatches != 0)
        {
            FAIL(first_mismatch);
        }

    }

    TEST(FormatTests, FloatTiesRoundToEven)
    {
        minstd::fixed_string<128> buffer;

        //  Exact ties round to even, as std::format does

        STRCMP_EQUAL("2", minstd::format(buffer, "{:.0}", 2.5).c_str());     //  before: 3
        STRCMP_EQUAL("4", minstd::format(buffer, "{:.0}", 3.5).c_str());
        STRCMP_EQUAL("0.12", minstd::format(buffer, "{:.2}", 0.125).c_str()); //  before: 0.13
        STRCMP_EQUAL("0.38", minstd::format(buffer, "{:.2}", 0.375).c_str());
        STRCMP_EQUAL("1.15", minstd::format(buffer, "{:.2}", 1.15).c_str());
    }

    TEST(FormatTests, FloatSignsAndNonFinitePadding)
    {
        minstd::fixed_string<128> buffer;

        STRCMP_EQUAL("-0.0000", minstd::format(buffer, "{}", -0.0).c_str()); //  before: 0.0000
        STRCMP_EQUAL("  inf", minstd::format(buffer, "{:05}", __builtin_inf()).c_str()); //  before: 00inf
        STRCMP_EQUAL(" -inf", minstd::format(buffer, "{:05}", -__builtin_inf()).c_str());
        STRCMP_EQUAL("-0012.50", minstd::format(buffer, "{:08.2}", -12.5).c_str());
    }

    TEST(FormatTests, HugeWidthsAndPrecisionsStopAtTheBuffer)
    {
        //  Fill loops ran once per character even on a full buffer: about 4e9 iterations for these.  An optimised
        //      host build can drop the no-op loop, so this guards the behaviour rather than reproducing the stall.
        minstd::fixed_string<16> buffer;

        CHECK_TRUE(minstd::pmr::test::runs_to_completion([]
                                                         {
                                                             minstd::fixed_string<16> b;
                                                             minstd::format(b, "{:4000000000}", (int32_t)1);
                                                             minstd::format(b, "{:.4000000000}", 1.0);
                                                             minstd::format(b, "{:>4000000000}", "x");
                                                             if (b.size() != 16) _exit(1); },
                                                         2));

        //  The buffer fills with padding and the rest is truncated, as for any output too long for the buffer

        STRCMP_EQUAL("                ", minstd::format(buffer, "{:4000000000}", (int32_t)1).c_str());
        STRCMP_EQUAL("1.00000000000000", minstd::format(buffer, "{:.4000000000}", 1.0).c_str());
    }
}
