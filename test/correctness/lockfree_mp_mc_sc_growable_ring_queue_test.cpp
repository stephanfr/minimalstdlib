// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <CppUTest/TestHarness.h>

#include <minstdconfig.h>

#include <lockfree/mp_mc_sc_growable_ring_queue>

#include <__memory_resource/memory_resource.h>
#include <__memory_resource/monotonic_buffer_resource.h>
#include <__memory_resource/polymorphic_allocator.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (MpMcScGrowableRingQueueTests)
    {
    };
#pragma GCC diagnostic pop

    constexpr size_t SCRATCH_BUFFER_SIZE = 1 << 22;
    char scratch_buffer[SCRATCH_BUFFER_SIZE];

    //  monotonic_buffer_resource is deliberately not thread-safe (matches
    //  std::pmr::monotonic_buffer_resource), so the multithreaded tests
    //  below need a resource backed directly by global new/delete instead.
    class new_delete_test_resource : public minstd::pmr::memory_resource
    {
    private:
        void *do_allocate(size_t bytes, size_t align) override
        {
            return ::operator new(bytes, static_cast<std::align_val_t>(align));
        }

        void do_deallocate(void *ptr, size_t, size_t align) override
        {
            ::operator delete(ptr, static_cast<std::align_val_t>(align));
        }

        bool do_is_equal(const minstd::pmr::memory_resource &other) const noexcept override
        {
            return this == &other;
        }
    };

    struct ring_test_element
    {
        uint32_t producer_ = 0;
        uint32_t sequence_ = 0;
    };

    template <bool MultiConsumer, size_t SegmentCount>
    struct ring_stress_harness
    {
        using queue_t = minstd::mp_mc_sc_growable_ring_queue<ring_test_element, MultiConsumer, SegmentCount>;
        using alloc_t = minstd::pmr::polymorphic_allocator<typename queue_t::allocator_type::value_type>;

        static constexpr size_t NUM_PRODUCERS = 6;
        static constexpr size_t NUM_CONSUMERS = MultiConsumer ? 3 : 1;
        static constexpr size_t ITEMS_PER_PRODUCER = 4000;
        static constexpr size_t TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

        queue_t *queue_ = nullptr;
        minstd::atomic<bool> start_{false};
        minstd::atomic<size_t> finished_producers_{0};
        minstd::atomic<size_t> popped_{0};

        uint8_t seen_[NUM_PRODUCERS][ITEMS_PER_PRODUCER] = {};
        pthread_mutex_t seen_mutex_ = PTHREAD_MUTEX_INITIALIZER;

        struct producer_args
        {
            ring_stress_harness *harness_;
            uint32_t producer_id_;
        };

        static void *produce(void *arg)
        {
            auto *args = static_cast<producer_args *>(arg);
            ring_stress_harness *h = args->harness_;
            uint32_t producer_id = args->producer_id_;
            delete args;

            while (!h->start_.load(minstd::memory_order_acquire))
            {
            }

            for (uint32_t i = 0; i < ITEMS_PER_PRODUCER; ++i)
            {
                ring_test_element element{producer_id, i};

                while (!h->queue_->push_back(element))
                {
                }
            }

            h->finished_producers_.fetch_add(1, minstd::memory_order_acq_rel);

            return nullptr;
        }

        static void *consume(void *arg)
        {
            auto *h = static_cast<ring_stress_harness *>(arg);

            while (!h->start_.load(minstd::memory_order_acquire))
            {
            }

            while (true)
            {
                ring_test_element element;

                if (h->queue_->pop_front(element))
                {
                    pthread_mutex_lock(&h->seen_mutex_);
                    uint8_t &slot = h->seen_[element.producer_][element.sequence_];
                    bool duplicate = (slot != 0);
                    slot = 1;
                    pthread_mutex_unlock(&h->seen_mutex_);

                    CHECK_FALSE(duplicate);

                    h->popped_.fetch_add(1, minstd::memory_order_acq_rel);
                }
                else if (h->finished_producers_.load(minstd::memory_order_acquire) == NUM_PRODUCERS &&
                         h->popped_.load(minstd::memory_order_acquire) == TOTAL_ITEMS)
                {
                    break;
                }
            }

            return nullptr;
        }

        void run(size_t idle_rotation_interval = 0)
        {
            new_delete_test_resource resource;
            alloc_t alloc(&resource);

            //  Deliberately small initial capacity relative to
            //  ITEMS_PER_PRODUCER, forcing many real growth cycles across
            //  every segment in the ring under contention.
            queue_t q(alloc, 8, 5, 4, idle_rotation_interval);
            queue_ = &q;

            pthread_t producers[NUM_PRODUCERS];
            pthread_t consumers[NUM_CONSUMERS];

            for (size_t i = 0; i < NUM_PRODUCERS; ++i)
            {
                auto *args = new producer_args{this, static_cast<uint32_t>(i)};
                pthread_create(&producers[i], nullptr, produce, args);
            }

            for (size_t i = 0; i < NUM_CONSUMERS; ++i)
            {
                pthread_create(&consumers[i], nullptr, consume, this);
            }

            start_.store(true, minstd::memory_order_release);

            for (size_t i = 0; i < NUM_PRODUCERS; ++i)
            {
                pthread_join(producers[i], nullptr);
            }

            for (size_t i = 0; i < NUM_CONSUMERS; ++i)
            {
                pthread_join(consumers[i], nullptr);
            }

            CHECK_EQUAL(TOTAL_ITEMS, popped_.load());

            for (size_t p = 0; p < NUM_PRODUCERS; ++p)
            {
                for (size_t s = 0; s < ITEMS_PER_PRODUCER; ++s)
                {
                    CHECK_EQUAL(1, seen_[p][s]);
                }
            }

            //  With SegmentCount segments each starting at capacity 8, the
            //  ring should have grown well past its starting total.
            CHECK_TRUE(q.capacity_estimate() > 8 * SegmentCount);
        }
    };
}

TEST(MpMcScGrowableRingQueueTests, BasicSingleThreaded)
{
    minstd::pmr::monotonic_buffer_resource resource(scratch_buffer, SCRATCH_BUFFER_SIZE, nullptr);

    using queue_t = minstd::mp_mc_sc_growable_ring_queue<int>;
    minstd::pmr::polymorphic_allocator<queue_t::allocator_type::value_type> alloc(&resource);

    queue_t q(alloc, 4);

    CHECK_TRUE(q.empty());

    CHECK_TRUE(q.push_back(1));
    CHECK_TRUE(q.push_back(2));
    CHECK_TRUE(q.push_back(3));

    CHECK_FALSE(q.empty());

    int v = 0;

    CHECK_TRUE(q.pop_front(v));
    CHECK_EQUAL(1, v);
    CHECK_TRUE(q.pop_front(v));
    CHECK_EQUAL(2, v);
    CHECK_TRUE(q.pop_front(v));
    CHECK_EQUAL(3, v);

    CHECK_TRUE(q.empty());
    CHECK_FALSE(q.pop_front(v));
}

//  Default SegmentCount == 2: exactly two segments must be exhausted before
//  push_back reports "full", and draining a single item is not enough to
//  reopen a life -- only a FULL drain regrows a segment.
TEST(MpMcScGrowableRingQueueTests, BothSegmentsFullReturnsFalse)
{
    minstd::pmr::monotonic_buffer_resource resource(scratch_buffer, SCRATCH_BUFFER_SIZE, nullptr);

    using queue_t = minstd::mp_mc_sc_growable_ring_queue<int>;
    minstd::pmr::polymorphic_allocator<queue_t::allocator_type::value_type> alloc(&resource);

    queue_t q(alloc, 8);

    int pushed = 0;

    while (q.push_back(pushed))
    {
        ++pushed;
    }

    CHECK_EQUAL(16, pushed); // 8 + 8: both SegmentCount==2 segments exhausted
    CHECK_FALSE(q.push_back(9999));

    int v = -1;

    CHECK_TRUE(q.pop_front(v));
    CHECK_EQUAL(0, v);

    //  One pop does not reopen a life -- the drained segment is not FULLY
    //  drained yet.
    CHECK_FALSE(q.push_back(9999));

    for (int i = 0; i < 7; ++i)
    {
        CHECK_TRUE(q.pop_front(v));
    }

    //  Segment now fully drained and regrown -- room again.
    CHECK_TRUE(q.push_back(9999));
}

//  SegmentCount raised from the default 2 to 5: should absorb 5x the
//  per-segment initial capacity before push_back ever returns false, and
//  every item pushed across all 5 segments must still come back out in the
//  same order it went in (single-threaded: no ambiguity about which
//  producer claimed which slot) -- confirms the ring generalization walks
//  every segment on both the push and the pop side, not just the first two.
TEST(MpMcScGrowableRingQueueTests, DeeperRingAbsorbsProportionallyMoreBurst)
{
    minstd::pmr::monotonic_buffer_resource resource(scratch_buffer, SCRATCH_BUFFER_SIZE, nullptr);

    using queue_t = minstd::mp_mc_sc_growable_ring_queue<int, false, 5>;
    minstd::pmr::polymorphic_allocator<queue_t::allocator_type::value_type> alloc(&resource);

    queue_t q(alloc, 8);

    int pushed = 0;

    while (q.push_back(pushed))
    {
        ++pushed;
    }

    CHECK_EQUAL(40, pushed); // 5 segments x capacity 8 each
    CHECK_FALSE(q.push_back(9999));

    for (int expected = 0; expected < 40; ++expected)
    {
        int v = -1;

        CHECK_TRUE(q.pop_front(v));
        CHECK_EQUAL(expected, v);
    }

    CHECK_TRUE(q.empty());
}

//  A single segment can still be exhausted (or fully drained) exactly as
//  before -- the ring generalization must reduce to identical behavior when
//  SegmentCount is left at its default of 2.
TEST(MpMcScGrowableRingQueueTests, DefaultSegmentCountMatchesExplicitTwo)
{
    minstd::pmr::monotonic_buffer_resource resource(scratch_buffer, SCRATCH_BUFFER_SIZE, nullptr);

    using default_queue_t = minstd::mp_mc_sc_growable_ring_queue<int>;
    using explicit_queue_t = minstd::mp_mc_sc_growable_ring_queue<int, false, 2>;

    CHECK_TRUE((minstd::is_same<default_queue_t, explicit_queue_t>::value));
}

TEST(MpMcScGrowableRingQueueTests, RingUnderContentionSingleConsumer)
{
    ring_stress_harness<false, 4> harness;
    harness.run();
}

TEST(MpMcScGrowableRingQueueTests, RingUnderContentionMultiConsumer)
{
    ring_stress_harness<true, 4> harness;
    harness.run();
}

//  idle_rotation_interval left at its default of 0: capacity must only ever
//  grow, exactly as before this feature existed, even when a segment is left
//  running well under 50% loaded indefinitely.
TEST(MpMcScGrowableRingQueueTests, IdleRotationDisabledByDefault)
{
    minstd::pmr::monotonic_buffer_resource resource(scratch_buffer, SCRATCH_BUFFER_SIZE, nullptr);

    using queue_t = minstd::mp_mc_sc_growable_ring_queue<int>;
    minstd::pmr::polymorphic_allocator<queue_t::allocator_type::value_type> alloc(&resource);

    queue_t q(alloc, 100);

    for (int i = 0; i < 20; ++i)
    {
        CHECK_TRUE(q.push_back(i));
    }

    for (int i = 0; i < 20; ++i)
    {
        int v = -1;

        CHECK_TRUE(q.pop_front(v));
        CHECK_EQUAL(i, v);
    }

    //  Segment never got anywhere near full (20 of 100), and with rotation
    //  disabled it must never have been capped or shrunk.
    CHECK_EQUAL(200, q.capacity_estimate()); // 100 + 100, both segments untouched
}

//  A segment left running well under 50% loaded should get its life closed
//  early and reopen SMALLER, once idle_rotation_interval successful pops
//  have occurred -- without ever needing to be completely refilled to its
//  (currently much larger) declared capacity first.
TEST(MpMcScGrowableRingQueueTests, IdleRotationShrinksUnderusedSegment)
{
    minstd::pmr::monotonic_buffer_resource resource(scratch_buffer, SCRATCH_BUFFER_SIZE, nullptr);

    using queue_t = minstd::mp_mc_sc_growable_ring_queue<int>;
    minstd::pmr::polymorphic_allocator<queue_t::allocator_type::value_type> alloc(&resource);

    //  initial_capacity=100 (both segments), default growth 5/4, check for
    //  idle rotation every 4th successful pop.
    queue_t q(alloc, 100, 5, 4, 4);

    CHECK_EQUAL(200, q.capacity_estimate()); // 100 + 100 to start

    //  Push only 20 of segment 0's 100 slots -- well under the 50%
    //  threshold -- then stop. No overflow, no organic regrow trigger.
    for (int i = 0; i < 20; ++i)
    {
        CHECK_TRUE(q.push_back(i));
    }

    //  Drain all 20. On the 4th, 8th, 12th, 16th, and 20th pops (every
    //  idle_rotation_interval-th one), the queue opportunistically checks
    //  whether the active segment (still segment 0, at 20/100 = 20% loaded)
    //  should be capped. The first such check that lands while a claimed-
    //  but-not-yet-consumed backlog still remains closes segment 0's life
    //  early at capacity_=20; the final pop then completes that (now
    //  smaller) life and triggers a real regrow, shrinking it further via
    //  the inverse growth ratio (20 * 4 / 5 = 16) rather than growing.
    for (int expected = 0; expected < 20; ++expected)
    {
        int v = -1;

        CHECK_TRUE(q.pop_front(v));
        CHECK_EQUAL(expected, v);
    }

    //  Segment 0 must have reopened SMALLER than its original 100 -- total
    //  capacity is now segment 0's shrunk size plus segment 1's untouched
    //  100. If shrinking never triggered, this would still read 200.
    size_t final_capacity = q.capacity_estimate();

    CHECK_TRUE(final_capacity < 200);
    CHECK_TRUE(final_capacity >= 100); // segment 1 alone still accounts for 100

    //  The queue must still be fully, correctly usable afterward.
    for (int i = 100; i < 120; ++i)
    {
        CHECK_TRUE(q.push_back(i));
    }

    for (int expected = 100; expected < 120; ++expected)
    {
        int v = -1;

        CHECK_TRUE(q.pop_front(v));
        CHECK_EQUAL(expected, v);
    }
}

//  Same shrink mechanism, but under real multi-producer/multi-consumer
//  contention (both MultiConsumer configurations), to catch any concurrency
//  bug in the shrink-vs-normal-completion arbitration (segment::retire_claimed_)
//  that a single-threaded test can't exercise.
TEST(MpMcScGrowableRingQueueTests, RingUnderContentionWithIdleRotationSingleConsumer)
{
    ring_stress_harness<false, 4> harness;
    harness.run(500);
}

TEST(MpMcScGrowableRingQueueTests, RingUnderContentionWithIdleRotationMultiConsumer)
{
    ring_stress_harness<true, 4> harness;
    harness.run(500);
}
