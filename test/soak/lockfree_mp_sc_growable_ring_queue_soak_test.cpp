// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>
#include <minstdconfig.h>

#include <lockfree/mp_sc_growable_ring_queue>

#include <stdint.h>
#include <stdlib.h>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP(MpScGrowableRingQueueSoakTests)
    {
    };
#pragma GCC diagnostic pop

    template <typename T>
    class malloc_allocator : public minstd::allocator<T>
    {
    public:
        size_t max_size() const noexcept override { return size_t{1} << 20; }
        T *allocate(size_t n) override { return static_cast<T *>(malloc(sizeof(T) * n)); }
        void deallocate(T *p, size_t) override { free(p); }
    };

    TEST(MpScGrowableRingQueueSoakTests, RetriesOnFullQueueNeverOverwriteUnconsumedSlots)
    {
        //  Every failed push still increments the segment's 32-bit position.  After 2^32 failed attempts the
        //      position carried into the capacity field, idx < capacity passed again, and producers overwrote
        //      unconsumed slots.  Takes a minute or so: 2^32 pushes against a stalled consumer.

        using queue_type = minstd::mp_sc_growable_ring_queue<uint32_t>;

        malloc_allocator<queue_type::slot_type> allocator;
        queue_type queue(allocator, 4);

        uint32_t pushed = 0;
        while (queue.push_back(pushed)) //  fill both segments
        {
            pushed++;
        }

        CHECK_EQUAL(8u, pushed);

        uint64_t accepted_while_full = 0;
        for (uint64_t i = 0; i < (uint64_t{1} << 32) + 16; i++) //  a producer spinning on a stalled consumer
        {
            if (queue.push_back(0xDEAD0000u))
            {
                accepted_while_full++;
            }
        }

        CHECK_EQUAL(0u, accepted_while_full); //  before: > 0 once the position carries into the capacity

        uint32_t value = 0;
        for (uint32_t i = 0; i < pushed; i++)
        {
            CHECK_TRUE(queue.pop_front(value));
            CHECK_EQUAL(i, value);
        }
    }
}
