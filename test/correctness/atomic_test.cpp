// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>
#include <atomic>

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
}
