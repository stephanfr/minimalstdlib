// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <lockfree/intrusive_tagged_stack>
#include <lockfree/tagged_ptr>

//
//  The tagged pointer codec under each MINIMAL_STD_POINTER_TOP_BITS rule.  The pointers are never
//      dereferenced, so any address can be encoded.
//

namespace
{
    TEST_GROUP (TaggedPtrTests)
    {
    };

    struct node
    {
    };

    using minstd::lockfree::pointer_top_bits;

    template <pointer_top_bits RULE>
    using codec = minstd::lockfree::tagged_ptr<node, uint16_t, RULE>;

    template <pointer_top_bits RULE>
    uintptr_t round_trip(uintptr_t address, uint16_t counter)
    {
        auto packed = codec<RULE>::pack(reinterpret_cast<node *>(address), counter);

        CHECK_EQUAL(counter, codec<RULE>::unpack_counter(packed));

        return reinterpret_cast<uintptr_t>(codec<RULE>::unpack_ptr(packed));
    }

    constexpr uintptr_t LOW = 0x0000'1234'5678'9ab0ULL;              //  user space / identity mapped
    constexpr uintptr_t LOW_BIT_47 = 0x0000'8000'0000'1000ULL;       //  low half, bit 47 set (TTBR0 with 48-bit VAs)
    constexpr uintptr_t HIGH = 0xFFFF'8000'0001'2340ULL;             //  canonical upper half
    constexpr uintptr_t HIGH_BIT_47_CLEAR = 0xFFFF'0000'0001'2340ULL; //  TTBR1 with 48-bit VAs, bit 47 clear

    TEST(TaggedPtrTests, ZeroRuleRestoresLowAddresses)
    {
        CHECK_EQUAL(LOW, round_trip<pointer_top_bits::zero>(LOW, 7));
        CHECK_EQUAL(LOW_BIT_47, round_trip<pointer_top_bits::zero>(LOW_BIT_47, 0xFFFF));
        CHECK_EQUAL(uintptr_t{0}, round_trip<pointer_top_bits::zero>(0, 3));
    }

    TEST(TaggedPtrTests, OnesRuleRestoresUpperHalfAddresses)
    {
        CHECK_EQUAL(HIGH, round_trip<pointer_top_bits::ones>(HIGH, 7));
        CHECK_EQUAL(HIGH_BIT_47_CLEAR, round_trip<pointer_top_bits::ones>(HIGH_BIT_47_CLEAR, 0xFFFF)); //  wrong under sign_extend_47
    }

    TEST(TaggedPtrTests, SignExtend47RestoresCanonicalAddresses)
    {
        CHECK_EQUAL(LOW, round_trip<pointer_top_bits::sign_extend_47>(LOW, 7));
        CHECK_EQUAL(HIGH, round_trip<pointer_top_bits::sign_extend_47>(HIGH, 0xFFFF));

        //  The documented limit: an upper-half address with bit 47 clear decodes into the lower half

        CHECK_EQUAL(HIGH_BIT_47_CLEAR & 0x0000'FFFF'FFFF'FFFFULL, round_trip<pointer_top_bits::sign_extend_47>(HIGH_BIT_47_CLEAR, 1));
    }

    TEST(TaggedPtrTests, CounterIncrementKeepsThePointer)
    {
        auto packed = codec<pointer_top_bits::ones>::pack(reinterpret_cast<node *>(HIGH), 0xFFFF);
        auto incremented = codec<pointer_top_bits::ones>::increment_counter(packed);

        CHECK_EQUAL(0, codec<pointer_top_bits::ones>::unpack_counter(incremented)); //  wraps
        CHECK_EQUAL(HIGH, reinterpret_cast<uintptr_t>(codec<pointer_top_bits::ones>::unpack_ptr(incremented)));
    }

    TEST(TaggedPtrTests, DefaultRuleFollowsTheBuildSetting)
    {
        //  The default codec follows MINIMAL_STD_POINTER_TOP_BITS; the hosted test build leaves it at ZERO (its
        //      earlier behaviour, no sign extension), and test/run_config_matrix.sh overrides it

        CHECK(minstd::lockfree::default_pointer_top_bits == static_cast<pointer_top_bits>(MINIMAL_STD_POINTER_TOP_BITS));

#if !defined(MINIMAL_STD_CONFIG_MATRIX)
        CHECK(minstd::lockfree::default_pointer_top_bits == pointer_top_bits::zero);
#endif
    }

    TEST(TaggedPtrTests, NullRoundTripsUnderEveryRule)
    {
        //  Under ONES, null decoded as 0xFFFF0000'00000000: empty stacks then looked non-empty and were dereferenced

        CHECK_EQUAL(uintptr_t{0}, round_trip<pointer_top_bits::ones>(0, 0));
        CHECK_EQUAL(uintptr_t{0}, round_trip<pointer_top_bits::ones>(0, 0xFFFF));
        CHECK_EQUAL(uintptr_t{0}, round_trip<pointer_top_bits::sign_extend_47>(0, 0x1234));
        CHECK_EQUAL(uintptr_t{0}, round_trip<pointer_top_bits::zero>(0, 0xFFFF));

        POINTERS_EQUAL(nullptr, codec<pointer_top_bits::ones>::unpack_ptr(codec<pointer_top_bits::ones>::make(nullptr)));
    }

    TEST(TaggedPtrTests, EmptyStackUnderTheOnesRule)
    {
        //  The single arena's free lists are intrusive_tagged_stacks headed by make(nullptr); popping an empty one
        //      under ONES followed the bogus decoded pointer.

        struct stack_node
        {
            stack_node *next_;
        };

        struct adapter
        {
            stack_node *to_node(stack_node *node) const { return node; }
            stack_node *to_link(stack_node *node) const { return node; }
        };

        using ones_codec = minstd::lockfree::tagged_ptr<stack_node, uint16_t, pointer_top_bits::ones>;
        using stack_type = minstd::lockfree::intrusive_tagged_stack<stack_node, ones_codec, stack_node *, &stack_node::next_>;

        minstd::atomic<uint64_t> head{ones_codec::make(nullptr)};

        auto no_backoff = [](size_t &) {};

        POINTERS_EQUAL(nullptr, stack_type::pop(head, adapter{}, no_backoff));
        POINTERS_EQUAL(nullptr, stack_type::steal_all(head));
    }
}
