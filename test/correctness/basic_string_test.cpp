// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//  Tests targeting basic_string<char> methods directly via base-class references.
//  basic_string<T> cannot be constructed directly; fixed_string is used as the
//  concrete vehicle, but every method under test is invoked through a
//  minstd::string& or minstd::string const& reference so that coverage is
//  attributed to the basic_string<char> definition in the header.

#include <CppUTest/TestHarness.h>

#include <fixed_string>

namespace
{

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(BasicStringTests){};
#pragma GCC diagnostic pop

    // -----------------------------------------------------------------------
    // Helpers that accept only the base-class reference so the compiler
    // cannot optimise away the virtual/non-virtual dispatch.
    // -----------------------------------------------------------------------

    static const char *call_c_str(const minstd::string &s)
    {
        return s.c_str();
    }

    static const char *call_implicit_cast(const minstd::string &s)
    {
        return static_cast<const char *>(s);
    }

    static size_t call_size(const minstd::string &s)
    {
        return s.size();
    }

    static size_t call_length(const minstd::string &s)
    {
        return s.length();
    }

    static bool call_empty(const minstd::string &s)
    {
        return s.empty();
    }

    static void call_clear(minstd::string &s)
    {
        s.clear();
    }

    static char call_index_const(const minstd::string &s, size_t pos)
    {
        return s[pos];
    }

    static char call_index_mutable(minstd::string &s, size_t pos)
    {
        return s[pos];
    }

    static bool call_push_back(minstd::string &s, char c)
    {
        return s.push_back(c);
    }

    static size_t call_find_char(const minstd::string &s, char c, size_t pos = 0)
    {
        return s.find(c, pos);
    }

    static size_t call_find_cstr(const minstd::string &s, const char *needle, size_t pos = 0)
    {
        return s.find(needle, pos);
    }

    static size_t call_find_last_of(const minstd::string &s, char c, size_t pos = minstd::string::npos)
    {
        return s.find_last_of(c, pos);
    }

    static minstd::string &call_append_cstr(minstd::string &s, const char *value)
    {
        return (s += value);
    }

    // -----------------------------------------------------------------------
    // Tests
    // -----------------------------------------------------------------------

    TEST(BasicStringTests, CStrAndImplicitCast)
    {
        minstd::fixed_string<32> concrete = "hello";
        minstd::string &s = concrete;

        STRCMP_EQUAL("hello", call_c_str(s));
        STRCMP_EQUAL("hello", call_implicit_cast(s));
    }

    TEST(BasicStringTests, DefaultConstructor)
    {
        //  Exercises the 3-argument protected constructor via fixed_string default ctor.
        minstd::fixed_string<16> concrete;
        minstd::string &s = concrete;

        CHECK(call_empty(s));
        CHECK_EQUAL(0u, call_size(s));
        CHECK_EQUAL(0u, call_length(s));
        STRCMP_EQUAL("", call_c_str(s));
    }

    TEST(BasicStringTests, SizeAndLength)
    {
        minstd::fixed_string<64> concrete = "abcdef";
        minstd::string &s = concrete;

        CHECK_EQUAL(6u, call_size(s));
        CHECK_EQUAL(6u, call_length(s));
    }

    TEST(BasicStringTests, Empty)
    {
        minstd::fixed_string<32> concrete;
        minstd::string &s = concrete;

        CHECK(call_empty(s));

        concrete = "nonempty";
        CHECK(!call_empty(s));
    }

    TEST(BasicStringTests, Clear)
    {
        minstd::fixed_string<32> concrete = "clear me";
        minstd::string &s = concrete;

        CHECK(!call_empty(s));
        call_clear(s);
        CHECK(call_empty(s));
        CHECK_EQUAL(0u, call_size(s));
        STRCMP_EQUAL("", call_c_str(s));
    }

    TEST(BasicStringTests, IndexOperatorConst)
    {
        minstd::fixed_string<32> concrete = "xyz";
        const minstd::string &s = concrete;

        CHECK_EQUAL('x', call_index_const(s, 0));
        CHECK_EQUAL('y', call_index_const(s, 1));
        CHECK_EQUAL('z', call_index_const(s, 2));
        //  Out-of-bounds returns NUL character
        CHECK_EQUAL('\0', call_index_const(s, 100));
    }

    TEST(BasicStringTests, IndexOperatorMutable)
    {
        minstd::fixed_string<32> concrete = "xyz";
        minstd::string &s = concrete;

        CHECK_EQUAL('x', call_index_mutable(s, 0));
        CHECK_EQUAL('y', call_index_mutable(s, 1));
        CHECK_EQUAL('z', call_index_mutable(s, 2));
        //  Out-of-bounds returns NUL character
        CHECK_EQUAL('\0', call_index_mutable(s, 100));
    }

    TEST(BasicStringTests, PushBack)
    {
        minstd::fixed_string<8> concrete = "abc";
        minstd::string &s = concrete;

        CHECK(call_push_back(s, 'd'));
        CHECK(call_push_back(s, 'e'));
        STRCMP_EQUAL("abcde", call_c_str(s));
        CHECK_EQUAL(5u, call_size(s));

        //  Push NUL — should succeed but not extend the string
        CHECK(call_push_back(s, '\0'));
        CHECK_EQUAL(5u, call_size(s));

        //  Fill to capacity (max_size == 8 for fixed_string<8>)
        call_push_back(s, 'f');
        call_push_back(s, 'g');
        call_push_back(s, 'h');

        //  One more should fail (at capacity)
        CHECK(!call_push_back(s, 'i'));
    }

    TEST(BasicStringTests, FindChar)
    {
        minstd::fixed_string<64> concrete = "abcabc";
        minstd::string &s = concrete;

        //  First occurrence
        CHECK_EQUAL(0u, call_find_char(s, 'a'));
        CHECK_EQUAL(2u, call_find_char(s, 'c'));

        //  Search from offset
        CHECK_EQUAL(3u, call_find_char(s, 'a', 1));
        CHECK_EQUAL(5u, call_find_char(s, 'c', 3));

        //  Not found
        CHECK_EQUAL(minstd::string::npos, call_find_char(s, 'z'));
    }

    TEST(BasicStringTests, AppendCStr)
    {
        minstd::fixed_string<32> concrete = "hello";
        minstd::string &s = concrete;

        call_append_cstr(s, " world");
        STRCMP_EQUAL("hello world", call_c_str(s));
        CHECK_EQUAL(11u, call_size(s));

        //  Append empty string — no change
        call_append_cstr(s, "");
        CHECK_EQUAL(11u, call_size(s));
    }

    TEST(BasicStringTests, AppendCStrTruncation)
    {
        //  fixed_string<8> holds max 8 characters
        minstd::fixed_string<8> concrete = "abc";
        minstd::string &s = concrete;

        call_append_cstr(s, "defghijklmn");   //  would exceed capacity

        CHECK_EQUAL(8u, call_size(s));
        STRCMP_EQUAL("abcdefgh", call_c_str(s));
    }

    TEST(BasicStringTests, FindCStringEmptyNeedleReturnsPos)
    {
        minstd::fixed_string<16> concrete = "abc";
        minstd::string &s = concrete;

        CHECK_EQUAL(0u, call_find_cstr(s, "", 0));
        CHECK_EQUAL(2u, call_find_cstr(s, "", 2));
        CHECK_EQUAL(3u, call_find_cstr(s, "", 3));
        CHECK_EQUAL(minstd::string::npos, call_find_cstr(s, "", 4));
    }

    TEST(BasicStringTests, FindLastOfDoesNotMatchTerminator)
    {
        minstd::fixed_string<16> concrete = "abc";
        minstd::string &s = concrete;

        CHECK_EQUAL(minstd::string::npos, call_find_last_of(s, '\0'));
    }

} // namespace
