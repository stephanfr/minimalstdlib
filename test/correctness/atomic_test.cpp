// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>
#include <atomic>
#include <new>

namespace
{
    TEST_GROUP(AtomicTests){};

    //  Compile-time: the failure order is never release or acq_rel.
    static_assert(minstd::atomic<uint32_t>::failure_order_for(minstd::memory_order_acq_rel) == minstd::memory_order_acquire);
    static_assert(minstd::atomic<uint32_t>::failure_order_for(minstd::memory_order_release) == minstd::memory_order_relaxed);
    static_assert(minstd::atomic<uint32_t>::failure_order_for(minstd::memory_order_seq_cst) == minstd::memory_order_seq_cst);
    static_assert(minstd::atomic<uint32_t>::failure_order_for(minstd::memory_order_acquire) == minstd::memory_order_acquire);
    static_assert(minstd::atomic<uint32_t>::failure_order_for(minstd::memory_order_relaxed) == minstd::memory_order_relaxed);

    TEST(AtomicTests, SingleOrderCompareExchangeWithReleaseOrders)
    {
        //  Before the fix this file emits -Winvalid-memory-model (an error with -Werror=invalid-memory-model).
        minstd::atomic<uint32_t> value{5};

        uint32_t expected = 4;
        CHECK_FALSE(value.compare_exchange_strong(expected, 9, minstd::memory_order_acq_rel));
        CHECK_EQUAL(5u, expected); //  a failed CAS reports the current value

        CHECK_TRUE(value.compare_exchange_strong(expected, 9, minstd::memory_order_release));
        CHECK_EQUAL(9u, value.load());

        expected = 9;
        while (!value.compare_exchange_weak(expected, 11, minstd::memory_order_acq_rel))
        {
        }
        CHECK_EQUAL(11u, value.load());

        expected = 1;
        CHECK_FALSE(value.compare_exchange_weak(expected, 2, minstd::memory_order_release));
        CHECK_EQUAL(11u, expected);
    }

    TEST(AtomicTests, PointerArithmeticCountsElements)
    {
        //  fetch_add on atomic<T *> took a T * argument (so it did not compile with a count), and the builtin adds
        //      bytes, not elements.
        static uint64_t values[8] = {};

        minstd::atomic<uint64_t *> cursor(&values[0]);

        POINTERS_EQUAL(&values[0], cursor.fetch_add(2));
        POINTERS_EQUAL(&values[2], cursor.load());

        cursor += 3;
        POINTERS_EQUAL(&values[5], cursor.load());

        ++cursor;
        POINTERS_EQUAL(&values[6], cursor.load());

        POINTERS_EQUAL(&values[6], cursor.fetch_sub(4));
        POINTERS_EQUAL(&values[2], cursor.load());

        cursor--;
        POINTERS_EQUAL(&values[1], cursor.load());
    }

    TEST(AtomicTests, DefaultConstructedIsValueInitialised)
    {
        //  C++20 value-initialises a default-constructed atomic; this one used to hold whatever the storage held

        alignas(minstd::atomic<uint64_t>) unsigned char storage[sizeof(minstd::atomic<uint64_t>)];

        for (auto &b : storage)
        {
            b = 0xA5;
        }

        auto *value = new (storage) minstd::atomic<uint64_t>();

        CHECK_EQUAL(0u, value->load());

        value->~atomic();
    }
}
