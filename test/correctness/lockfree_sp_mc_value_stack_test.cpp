// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <lockfree/sp_mc_value_stack>

#include <pthread.h>
#include <stdint.h>
#include <atomic>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(SpMcValueStackTests)
    {
    };
#pragma GCC diagnostic pop

    //  -----------------------------------------------------------------------
    //  Single-threaded correctness
    //  -----------------------------------------------------------------------

    TEST(SpMcValueStackTests, EmptyOnConstruction)
    {
        minstd::lockfree::sp_mc_value_stack<uint16_t, 8> s;

        CHECK(s.empty());
        CHECK_EQUAL(0u, s.size());
    }

    TEST(SpMcValueStackTests, PushPopLifo)
    {
        minstd::lockfree::sp_mc_value_stack<uint32_t, 4> s;

        CHECK(s.push(10));
        CHECK(s.push(20));
        CHECK(s.push(30));
        CHECK_EQUAL(3u, s.size());
        CHECK(!s.empty());

        uint32_t v = 0;
        CHECK(s.pop(v));  CHECK_EQUAL(30u, v);
        CHECK(s.pop(v));  CHECK_EQUAL(20u, v);
        CHECK(s.pop(v));  CHECK_EQUAL(10u, v);
        CHECK(!s.pop(v));
        CHECK(s.empty());
    }

    TEST(SpMcValueStackTests, PushReturnsFalseWhenFull)
    {
        minstd::lockfree::sp_mc_value_stack<uint8_t, 2> s;

        CHECK(s.push(1));
        CHECK(s.push(2));
        CHECK(!s.push(3));
        CHECK_EQUAL(2u, s.size());
    }

    TEST(SpMcValueStackTests, PopReturnsFalseWhenEmpty)
    {
        minstd::lockfree::sp_mc_value_stack<int, 4> s;

        int v = -1;
        CHECK(!s.pop(v));
        CHECK_EQUAL(-1, v);
    }

    TEST(SpMcValueStackTests, FillDrainFillDrain)
    {
        static constexpr size_t CAP = 16;
        minstd::lockfree::sp_mc_value_stack<uint32_t, CAP> s;

        for (uint32_t round = 0; round < 3; round++)
        {
            for (uint32_t i = 0; i < CAP; i++)
            {
                CHECK(s.push(round * 100 + i));
            }
            CHECK(!s.push(99));

            for (uint32_t i = CAP; i > 0; i--)
            {
                uint32_t v = 0;
                CHECK(s.pop(v));
                CHECK_EQUAL(round * 100 + i - 1, v);
            }
            CHECK(s.empty());
        }
    }

    //  -----------------------------------------------------------------------
    //  Multi-consumer correctness: one producer, N consumer threads.
    //
    //  Protocol: producer pushes N real items then signals done; consumers
    //  drain until done && empty.  The generation counter in the state word
    //  prevents the double-pop ABA race that a plain count CAS suffers from.
    //  -----------------------------------------------------------------------

    static constexpr size_t MT_CONSUMERS = 4;
    static constexpr size_t MT_ITEMS     = 1024;
    static constexpr size_t MT_CAPACITY  = MT_ITEMS + 8;   // headroom

    struct consumer_args
    {
        minstd::lockfree::sp_mc_value_stack<uint32_t, MT_CAPACITY> *stack;
        minstd::atomic<bool>     done{false};
        minstd::atomic<uint64_t> checksum{0};
        minstd::atomic<uint32_t> count{0};
    };

    void *consumer_thread(void *arg)
    {
        consumer_args *a = static_cast<consumer_args *>(arg);

        while (!a->done.load(minstd::memory_order_acquire) || !a->stack->empty())
        {
            uint32_t v;
            if (a->stack->pop(v))
            {
                a->checksum.fetch_add(v, minstd::memory_order_relaxed);
                a->count.fetch_add(1,    minstd::memory_order_relaxed);
            }
        }

        return nullptr;
    }

    TEST(SpMcValueStackTests, MultiConsumerAllValuesPopped)
    {
        minstd::lockfree::sp_mc_value_stack<uint32_t, MT_CAPACITY> stack;

        consumer_args args;
        args.stack = &stack;

        pthread_t threads[MT_CONSUMERS];
        for (size_t i = 0; i < MT_CONSUMERS; i++)
        {
            pthread_create(&threads[i], nullptr, consumer_thread, &args);
        }

        uint64_t expected_checksum = 0;
        for (uint32_t i = 1; i <= MT_ITEMS; i++)
        {
            while (!stack.push(i)) {}
            expected_checksum += i;
        }

        args.done.store(true, minstd::memory_order_release);

        for (size_t i = 0; i < MT_CONSUMERS; i++)
        {
            pthread_join(threads[i], nullptr);
        }

        CHECK_EQUAL(static_cast<uint32_t>(MT_ITEMS), args.count.load());
        CHECK_EQUAL(expected_checksum, args.checksum.load());
        CHECK(stack.empty());
    }
}
