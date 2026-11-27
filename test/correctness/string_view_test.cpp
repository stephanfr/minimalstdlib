// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <fixed_string>
#include <format>
#include <string_view>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (StringViewConstructionTests)
    {
    };

    TEST_GROUP (StringViewAccessTests)
    {
    };
#pragma GCC diagnostic pop

    minstd::string_view Echo(minstd::string_view text)
    {
        return text;
    }

    TEST(StringViewConstructionTests, DefaultIsEmpty)
    {
        const minstd::string_view empty;

        CHECK_TRUE(empty.empty());
        CHECK_EQUAL(0, empty.length());
        CHECK_TRUE(empty == "");
    }

    TEST(StringViewConstructionTests, LiteralsConvertImplicitly)
    {
        CHECK_EQUAL(5, Echo("hello").length());
        CHECK_TRUE(Echo("hello") == "hello");
        CHECK_FALSE(Echo("hello") == "help!");
        CHECK_FALSE(Echo("hello") == "hell");
    }

    TEST(StringViewConstructionTests, ArraysStopAtTheirFirstNulAndNeverPassTheirEnd)
    {
        const char shorter[16] = "abc";
        const char unterminated[4] = {'w', 'x', 'y', 'z'};

        CHECK_EQUAL(3, minstd::string_view(shorter).length());
        CHECK_EQUAL(4, minstd::string_view(unterminated).length());
        CHECK_TRUE(minstd::string_view(unterminated) == "wxyz");
    }

    TEST(StringViewConstructionTests, CPointersAreExplicitAndNullIsEmpty)
    {
        const char *text = "pointer";
        const char *nothing = nullptr;

        CHECK_EQUAL(7, minstd::string_view(text).length());
        CHECK_TRUE(minstd::string_view(nothing).empty());
        CHECK_TRUE(minstd::string_view(nothing, 12).empty());
        CHECK_EQUAL(3, minstd::string_view(text, 3).length());
    }

    TEST(StringViewConstructionTests, StringsConvertImplicitly)
    {
        const minstd::fixed_string<32> owned("owned text");

        CHECK_EQUAL(10, Echo(owned).length());
        CHECK_TRUE(Echo(owned) == "owned text");
    }

    TEST(StringViewAccessTests, IndexingPastTheEndGivesNul)
    {
        const minstd::string_view text("ab");

        CHECK_EQUAL('a', text[0]);
        CHECK_EQUAL('b', text[1]);
        CHECK_EQUAL('\0', text[2]);
        CHECK_EQUAL('\0', text[1000]);
    }

    TEST(StringViewAccessTests, SubstrClampsToTheView)
    {
        const minstd::string_view text("abcdef");

        CHECK_TRUE(text.substr(2, 3) == "cde");
        CHECK_TRUE(text.substr(4) == "ef");
        CHECK_TRUE(text.substr(4, 100) == "ef");
        CHECK_TRUE(text.substr(6).empty());
        CHECK_TRUE(text.substr(100).empty());
    }

    TEST(StringViewAccessTests, FindReportsTheFirstMatchOrNpos)
    {
        const minstd::string_view text("a.b.c");

        CHECK_EQUAL(1, text.find('.'));
        CHECK_EQUAL(3, text.find('.', 2));
        CHECK_EQUAL(minstd::string_view::npos, text.find('x'));
        CHECK_EQUAL(minstd::string_view::npos, text.find('.', 100));
    }

    TEST(StringViewAccessTests, FormatsLikeAString)
    {
        minstd::fixed_string<64> buffer;
        const char unterminated[3] = {'x', 'y', 'z'};

        minstd::format(buffer, "[{}] [{:>5}] [{}]", minstd::string_view("abc"), minstd::string_view("ab"), minstd::string_view(unterminated));

        STRCMP_EQUAL("[abc] [   ab] [xyz]", buffer.c_str());
    }
} // namespace
